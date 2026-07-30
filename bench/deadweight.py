#!/usr/bin/env python3
# Join bench/deadweight.sh's per-consumer --print-gc-sections output into
# one report: sections discarded by every consumer link, with the symbol
# they hold, its size, and the source file that defines it.
# usage: deadweight.py <lean-prefix> <raw-file>
import collections
import os
import subprocess
import sys

prefix, raw = sys.argv[1], sys.argv[2]

# consumer -> set of (object, section); a section is dead only if every
# link dropped it (a tool that uses it keeps it alive for all of them)
per_consumer = collections.defaultdict(set)
for line in open(raw):
	parts = line.split()
	if len(parts) < 3:
		continue
	consumer, section, obj = parts[0], parts[1], parts[2]
	# the linker names archive members as 'libnixutil.a(source-path.cc.o)'
	member = obj[obj.index("(") + 1:-1] if "(" in obj else os.path.basename(obj)
	per_consumer[consumer].add((member, section))

if not per_consumer:
	sys.exit("no gc output: did the links run with --print-gc-sections?")

dead = set.intersection(*per_consumer.values())

# section -> (size, symbol) per archive member, from the archives
# themselves: the linker names sections, not sizes
sizes = {}
defined = set()
strong = set()
for lib in ("libnixutil.a", "libnixstore.a"):
	path = os.path.join(prefix, "lib", lib)
	members = subprocess.run(["ar", "t", path], capture_output=True,
				 text=True, check=True).stdout.split()
	for m in members:
		out = subprocess.run(["ar", "p", path, m],
				     capture_output=True, check=True).stdout
		tmp = f"/tmp/dw-{m}"
		open(tmp, "wb").write(out)
		sec = subprocess.run(["readelf", "-SW", tmp],
				     capture_output=True, text=True).stdout
		for line in sec.splitlines():
			line = line.strip()
			if not line.startswith("["):
				continue
			rest = line[line.index("]") + 1:].split()
			if len(rest) >= 5 and rest[0].startswith("."):
				try:
					sizes[(m, rest[0])] = int(rest[4], 16)
				except ValueError:
					pass
		nm = subprocess.run(["nm", "-g", "--defined-only", tmp],
				    capture_output=True, text=True).stdout
		for line in nm.splitlines():
			q = line.split()
			if len(q) >= 3:
				defined.add(q[-1])
				# comdat (weak) symbols are emitted per TU and
				# folded by the linker: dropping one copy says
				# nothing about the code, the surviving copy is
				# somebody else's
				if q[-2] in "TDBRi":
					strong.add(q[-1])
		os.unlink(tmp)

# the section name carries the symbol: .text._ZN3nix... etc
def sym_of(section):
	for pre in (".text.unlikely.", ".text.startup.", ".text.hot.", ".text.",
		    ".rodata.", ".data.rel.ro.", ".data.", ".bss."):
		if section.startswith(pre):
			# comdat sections carry their group in brackets
			return section[len(pre):].split("[")[0]
	return None

# mangled -> owning source file, from the shared build's objects
owner = {}
utilobjs = "build/libutil/libnixutil.so.2.36.0.p"
storeobjs = "build/libstore/libnixstore.so.2.36.0.p"
for d in (utilobjs, storeobjs):
	if not os.path.isdir(d):
		continue
	for o in os.listdir(d):
		if not o.endswith(".o"):
			continue
		out = subprocess.run(["nm", "-g", "--defined-only",
				      os.path.join(d, o)],
				     capture_output=True, text=True).stdout
		for line in out.splitlines():
			p = line.split()
			if len(p) >= 3:
				owner.setdefault(p[-1], o[:-2])

# GCC emits two bodies per constructor (C1/C2) and up to three per
# destructor (D0/D1/D2); a call site references one, so the linker drops
# the other alias from a perfectly live function. Only report a
# ctor/dtor when every one of its variants died.
def canonical(sym):
	for tag in ("C1E", "C2E", "C4E", "D0E", "D1E", "D2E"):
		i = sym.find(tag)
		if i > 0:
			return sym[:i] + tag[0] + "*E" + sym[i + 3:]
	return sym

dead_syms = {sym_of(sec) for _, sec in dead}
dead_syms.discard(None)
dead_syms = {s.split("[")[0] for s in dead_syms}
groups = collections.defaultdict(set)
for sym in defined:
	groups[canonical(sym)].add(sym)
alias_live = set()
for sym in list(dead_syms):
	family = groups.get(canonical(sym))
	if family and not family <= dead_syms:
		alias_live.add(sym)

# symbol -> the sections holding it, per archive member
copies = collections.defaultdict(set)
for (m, sec), size in sizes.items():
	sym = sym_of(sec)
	if sym:
		copies[sym].add((m, sec))
all_copies_dead = {sym for sym, locs in copies.items() if locs <= dead}

rows = []
comdat = []
for obj, section in dead:
	mangled = sym_of(section)
	if not mangled:
		continue
	# skip the C++ scaffolding that is never a deletion candidate
	if mangled.startswith(("_ZTV", "_ZTI", "_ZTS", "_ZGV", "_ZZ")):
		continue
	if mangled in alias_live:
		continue
	# a weak (comdat) symbol is emitted by every TU that instantiates it
	# and the linker folds them: dropping one copy proves nothing, so
	# require that every copy died
	if mangled not in strong and mangled not in all_copies_dead:
		continue
	size = sizes.get((obj, section), 0)
	# prelink merges every object into one, so a section from there has
	# no source file to point at: those are comdat template
	# instantiations (boost::format, std:: containers), which the
	# linker dedups anyway and which no source edit removes
	if "prelink" in obj and mangled not in owner:
		comdat.append(size)
		continue
	rows.append((size, obj, mangled, owner.get(mangled, obj)))

names = subprocess.run(["c++filt"], input="\n".join(r[2] for r in rows),
		       capture_output=True, text=True).stdout.splitlines()

by_file = collections.defaultdict(list)
for (size, obj, mangled, file), pretty in zip(rows, names):
	# template instantiations and std:: guts are not ours to delete
	if "3nix" not in mangled:
		continue
	by_file[file].append((size, pretty))

total = sum(s for v in by_file.values() for s, _ in v)
print("dead in every consumer link: %d symbols, %d bytes" % (
	sum(len(v) for v in by_file.values()), total))
print("consumers linked: %d" % len(per_consumer))
print("plus %d comdat/template sections with no source file of their own"
      % len(comdat))
print("skipped %d weak/comdat copies folded against a live one elsewhere"
      % len({sym_of(sec) for _, sec in dead} - strong - all_copies_dead - {None}))
print("skipped %d ctor/dtor aliases whose sibling variant is live"
      % len(alias_live))
print()
for file, items in sorted(by_file.items(), key=lambda kv: -sum(s for s, _ in kv[1])):
	print("%-34s %6d bytes  %3d symbols" % (
		file, sum(s for s, _ in items), len(items)))
print()
for file, items in sorted(by_file.items(), key=lambda kv: -sum(s for s, _ in kv[1])):
	print("== %s" % file)
	for size, pretty in sorted(items, reverse=True):
		print("   %6d  %s" % (size, pretty))
