#!/usr/bin/env python3
# Refined dead-code estimate for libutil.
# Fixes two blind spots of naive nm -u analysis:
#  1) intra-TU calls: use objdump -r relocations, not just undefined syms.
#  2) virtual methods: never referenced by name (dispatched via vtable), so
#     a used vtable/ctor/typeinfo marks the whole class live.
import glob
import re
import subprocess
from collections import defaultdict

UTIL = "build/libutil/libnixutil.so.2.36.0.p"
CONSUMERS = (glob.glob("build/libstore/libnixstore.so.2.36.0.p/*.o")
	     + [b for b in glob.glob("build/*")
		if subprocess.run(["file", "-b", b], capture_output=True,
				  text=True).stdout.startswith("ELF")])

def defined(obj):
	out = subprocess.run(["nm", "-g", "--defined-only", obj],
			     capture_output=True, text=True, check=True)
	return {p[-1] for p in (l.split() for l in out.stdout.splitlines())
		if len(p) >= 3 and p[-2] in "TDBRi"}

def refs(obj):
	# relocations catch intra-TU references too
	out = subprocess.run(["objdump", "-rR", obj],
			     capture_output=True, text=True, check=True)
	r = set()
	for line in out.stdout.splitlines():
		p = line.split()
		if len(p) >= 3 and p[0].startswith("0") and "R_" in p[1]:
			r.add(p[2].split("-0x")[0].split("+0x")[0])
	if not r:  # stripped/dynamic-only: fall back to undefined syms
		out = subprocess.run(["nm", "-Du", obj], capture_output=True,
				     text=True, check=True)
		r = {l.split()[-1] for l in out.stdout.splitlines() if l.split()}
	return r

util_objs = sorted(glob.glob(UTIL + "/*.o"))
owner, defs, uses = {}, {}, {}
for o in util_objs:
	tu = o.split("/")[-1][:-2]
	defs[tu] = defined(o)
	uses[tu] = refs(o)
	for s in defs[tu]:
		owner.setdefault(s, tu)

# class key -> all syms of that class (member fns, vtable, typeinfo)
def class_key(sym):
	m = re.match(r"_ZT[VITS](N.+E)$", sym)
	if m:
		return m.group(1)[1:-1]
	return None

class_syms = defaultdict(set)
for s in owner:
	k = class_key(s)
	if k:
		class_syms[k].add(s)
for s in owner:                       # attach members to their class
	for k in class_syms:
		if s.startswith("_ZN" + k) or s.startswith("_ZNK" + k) \
		   or s.startswith("_ZTh") or s.startswith("_ZTv"):
			if k in s:
				class_syms[k].add(s)

def expand(sym):
	out = {sym}
	for k, group in class_syms.items():
		if sym in group:
			out |= group
	return out

roots = set()
for o in CONSUMERS:
	roots |= refs(o)

used_syms = set()
worklist = [s for s in roots if s in owner]
while worklist:
	s = worklist.pop()
	for e in expand(s):
		if e in used_syms or e not in owner:
			continue
		used_syms.add(e)
		tu = owner[e]
		# a TU is pulled in whole at link time (no -ffunction-sections
		# gc here), so all its refs become live
		for r in uses[tu]:
			if r in owner and r not in used_syms:
				worklist.append(r)

live_tus = {owner[s] for s in used_syms}
names = {o.split("/")[-1][:-2] for o in util_objs}

def sloc(tu):
	f = tu.replace("unix_", "unix/").replace("linux_", "linux/")
	p = "src/libutil/" + f
	try:
		return sum(1 for _ in open(p, encoding="utf-8"))
	except OSError:
		return 0

print("=== dead TUs ===")
for t in sorted(names - live_tus):
	print("  %-40s %4d lines" % (t, sloc(t)))
print()
print("=== live TUs: unreferenced strong syms (after vtable expansion) ===")
tot_d = tot_u = 0
rows = []
for t in sorted(live_tus):
	d = defs[t]
	u = d - used_syms
	tot_d += len(d)
	tot_u += len(u)
	rows.append((len(u) / max(len(d), 1), t, len(u), len(d), u))
for frac, t, nu, nd, u in sorted(rows, reverse=True):
	print("%-38s %3d/%-3d unused (%2d%%) %5d lines" % (
		t, nu, nd, frac * 100, sloc(t)))
print()
print("total strong syms in live TUs: %d, unreferenced: %d (%d%%)" % (
	tot_d, tot_u, 100 * tot_u / tot_d))
print()
for frac, t, nu, nd, u in sorted(rows, reverse=True):
	if not u:
		continue
	dem = subprocess.run(["c++filt"], input="\n".join(sorted(u)),
			     capture_output=True, text=True).stdout.splitlines()
	print("-- %s" % t)
	for s in dem:
		print("     " + s)
