#!/usr/bin/env python3
# Link-reachability of libutil TUs/symbols from libstore + arch/ consumers.
# Roots = undefined syms of consumers; edges = undef syms of a libutil
# TU -> the TU that strongly defines them.
import glob
import subprocess
from collections import defaultdict

UTIL = "build/libutil/libnixutil.so.2.36.0.p"
CONSUMER_OBJS = glob.glob("build/libstore/libnixstore.so.2.36.0.p/*.o")
CONSUMER_BINS = [
	b for b in glob.glob("build/*")
	if subprocess.run(["file", "-b", b], capture_output=True,
			  text=True).stdout.startswith("ELF")
]

def defined(obj):
	out = subprocess.run(["nm", "-g", "--defined-only", obj],
			     capture_output=True, text=True, check=True)
	d = set()
	for line in out.stdout.splitlines():
		p = line.split()
		if len(p) >= 3 and p[-2] in "TDBRi":
			d.add(p[-1])
	return d

def undefined(obj, dyn=False):
	cmd = ["nm", "-u"] + (["-D"] if dyn else []) + [obj]
	out = subprocess.run(cmd, capture_output=True, text=True, check=True)
	return {l.split()[-1] for l in out.stdout.splitlines() if l.split()}

util_objs = sorted(glob.glob(UTIL + "/*.o"))
owner, defs, undefs = {}, {}, {}
for o in util_objs:
	name = o.split("/")[-1][:-2]
	defs[name] = defined(o)
	undefs[name] = undefined(o)
	for s in defs[name]:
		owner.setdefault(s, name)

roots = set()
for o in CONSUMER_OBJS:
	roots |= undefined(o)
for b in CONSUMER_BINS:
	roots |= undefined(b, dyn=True)

used = defaultdict(set)          # tu -> its own syms that are referenced
why = defaultdict(set)           # tu -> (referrer, sym)
queue = []
for s in roots:
	t = owner.get(s)
	if t:
		used[t].add(s)
		why[t].add(("consumer", s))
		queue.append(t)

reached = set()
while queue:
	tu = queue.pop()
	if tu in reached:
		continue
	reached.add(tu)
	for s in undefs[tu]:
		t = owner.get(s)
		if t and t != tu:
			used[t].add(s)
			why[t].add((tu, s))
			if t not in reached:
				queue.append(t)

def demangle(syms):
	if not syms:
		return []
	out = subprocess.run(["c++filt"], input="\n".join(syms),
			     capture_output=True, text=True).stdout
	return out.splitlines()

names = {o.split("/")[-1][:-2] for o in util_objs}
print("=== REACHED: used/defined strong syms ===")
for t in sorted(reached, key=lambda t: len(used[t]) / max(len(defs[t]), 1)):
	print("%-38s %3d/%-3d  refs: %s" % (
		t, len(used[t]), len(defs[t]),
		", ".join(sorted({w[0] for w in why[t]})[:5])))
print()
print("=== UNREACHED TUs ===")
for t in sorted(names - reached):
	print("  " + t)
print()
print("=== per-TU unused strong syms (functions only) ===")
for t in sorted(reached):
	dead = [s for s in demangle(sorted(defs[t] - used[t]))
		if "(" in s and "::~" not in s]
	if not dead:
		continue
	print("-- %s (%d unused)" % (t, len(dead)))
	for s in dead:
		print("     " + s)
