#!/usr/bin/env python3
# Per-function reachability inside libutil, from libstore + arch/ consumers.
# Attribution is by address range (no -ffunction-sections in this build), so
# each relocation is charged to the symbol whose [value,value+size) covers it.
# vtable objects are nodes too: their relocs are the virtual-dispatch edges.
import glob
import subprocess
from bisect import bisect_right
from collections import defaultdict

UTIL_DIR = "build/libutil/libnixutil.so.2.36.0.p"

def is_elf(p):
	return subprocess.run(["file", "-b", p], capture_output=True,
			      text=True).stdout.startswith("ELF")

CONSUMERS = ([o for o in glob.glob("build/libstore/libnixstore.so.2.36.0.p/*.o")]
	     + [b for b in glob.glob("build/*") if is_elf(b)]
	     + [b for b in glob.glob("build/*/*") if is_elf(b)
		and "libnixutil" not in b and UTIL_DIR not in b])

def sections(obj):
	out = subprocess.run(["readelf", "-SW", obj], capture_output=True,
			     text=True, check=True).stdout
	idx = {}
	for line in out.splitlines():
		line = line.strip()
		if not line.startswith("["):
			continue
		close = line.index("]")
		n = line[1:close].strip()
		rest = line[close + 1:].split()
		if n.isdigit() and rest:
			idx[int(n)] = rest[0]
	return idx

def symbols(obj):
	"""[(section_name, value, size, name, bind, type)]"""
	out = subprocess.run(["readelf", "-sW", obj], capture_output=True,
			     text=True, check=True).stdout
	secs = sections(obj)
	res = []
	for line in out.splitlines():
		p = line.split()
		if len(p) < 8 or not p[0].endswith(":"):
			continue
		value, size, typ, bind, vis, ndx = p[1], p[2], p[3], p[4], p[5], p[6]
		name = p[7] if len(p) > 7 else ""
		if not ndx.isdigit() or typ not in ("FUNC", "OBJECT"):
			continue
		try:
			res.append((secs.get(int(ndx), "?"), int(value, 16),
				    int(size), name, bind, typ))
		except ValueError:
			continue
	return res

def relocs(obj):
	"""[(section_name, offset, target_symbol)]"""
	out = subprocess.run(["readelf", "-rW", obj], capture_output=True,
			     text=True, check=True).stdout
	cur = None
	res = []
	for line in out.splitlines():
		if line.startswith("Relocation section"):
			nm = line.split("'")[1]
			cur = nm[5:] if nm.startswith(".rela") else nm[4:]
			continue
		p = line.split()
		if cur and len(p) >= 5 and len(p[0]) == 16:
			try:
				off = int(p[0], 16)
			except ValueError:
				continue
			tgt = p[4].split("+")[0].split("-")[0]
			res.append((cur, off, tgt))
	return res

# ---- build the libutil graph -------------------------------------------
owner = {}                       # global sym -> tu
strong = set()                   # GLOBAL (non-weak) sym names
node_edges = defaultdict(set)    # (tu, sym) -> set of target sym names
size_of = {}                     # (tu, sym) -> bytes
tu_syms = defaultdict(list)
alias = defaultdict(set)         # (tu, sym) -> syms at same address (C1/C2 etc)

for obj in sorted(glob.glob(UTIL_DIR + "/*.o")):
	tu = obj.split("/")[-1][:-2]
	syms = symbols(obj)
	# per-section sorted symbol table for range attribution
	bysec = defaultdict(list)
	for sec, val, size, name, bind, typ in syms:
		if not name:
			continue
		bysec[sec].append((val, size, name))
		if bind in ("GLOBAL", "WEAK") and size:
			owner.setdefault(name, tu)
		if bind == "GLOBAL" and size:
			strong.add(name)
		size_of[(tu, name)] = size
		tu_syms[tu].append(name)
	for sec in bysec:
		bysec[sec].sort()
	same_addr = defaultdict(set)
	for sec, val, size, name, bind, typ in syms:
		if name:
			same_addr[(sec, val)].add(name)
	for group in same_addr.values():
		for n in group:
			alias[(tu, n)] |= {(tu, g) for g in group}
	starts = {s: [v for v, _, _ in lst] for s, lst in bysec.items()}
	for sec, off, tgt in relocs(obj):
		lst = bysec.get(sec)
		if not lst:
			continue
		i = bisect_right(starts[sec], off) - 1
		if i < 0:
			continue
		val, size, name = lst[i]
		if off >= val + max(size, 1) and size:
			continue          # padding between symbols
		node_edges[(tu, name)].add(tgt)

roots = set()
for c in CONSUMERS:
	for _, _, tgt in relocs(c):
		roots.add(tgt)
	# dynamic executables: relocs name the imported syms already

live = set()                     # (tu, sym)
work = [(owner[s], s) for s in roots if s in owner]
while work:
	node = work.pop()
	if node in live:
		continue
	live.add(node)
	for a in alias.get(node, ()):
		if a not in live:
			work.append(a)
	tu = node[0]
	for tgt in node_edges.get(node, ()):
		if tgt in owner:
			nxt = (owner[tgt], tgt)
		elif (tu, tgt) in size_of:        # local/static in same TU
			nxt = (tu, tgt)
		else:
			continue
		if nxt not in live:
			work.append(nxt)

def sloc(tu):
	p = "src/libutil/" + tu.replace("unix_", "unix/")
	try:
		return sum(1 for _ in open(p, encoding="utf-8"))
	except OSError:
		return 0

print("consumers scanned: %d" % len(CONSUMERS))
print()
rows = []
for tu in sorted(tu_syms):
	tot = dead = 0
	dead_syms = []
	for s in set(tu_syms[tu]):
		sz = size_of[(tu, s)]
		# only first-party strong syms: weak/comdat template
		# instantiations (boost, std::) are deduped at link time and
		# would swamp the count
		if not sz or s not in strong or "3nix" not in s:
			continue
		tot += sz
		if (tu, s) not in live:
			dead += sz
			dead_syms.append((sz, s))
	rows.append((dead / tot if tot else 1.0, tu, dead, tot,
		     sorted(dead_syms, reverse=True), sloc(tu)))

print("%-38s %9s %9s %6s %6s" % ("TU", "dead B", "total B", "dead%", "lines"))
for frac, tu, dead, tot, ds, lines in sorted(rows, reverse=True):
	print("%-38s %9d %9d %5d%% %6d" % (tu, dead, tot, frac * 100, lines))
td = sum(r[2] for r in rows)
tt = sum(r[3] for r in rows)
print("\nTOTAL dead %d / %d bytes of code+data (%d%%)" % (td, tt, 100 * td / tt))
print()
for frac, tu, dead, tot, ds, lines in sorted(rows, reverse=True):
	if not ds:
		continue
	print("-- %s: %d dead syms" % (tu, len(ds)))
	dem = subprocess.run(["c++filt"], input="\n".join(s for _, s in ds),
			     capture_output=True, text=True).stdout.splitlines()
	for (sz, _), name in zip(ds, dem):
		print("   %6d  %s" % (sz, name))
