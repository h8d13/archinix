#!/usr/bin/env python3
# Why the import counter stutters, in real time, on one timeline.
#
# The counter ("importing: N") is painted by AsyncFileHasher::end(), on
# the per-file hash thread. That thread only advances when the restore
# thread hands it another file. So a frozen counter is never the hasher
# being slow: it is the restore thread having stopped producing. This
# puts both on the same clock and shows what the restore thread was
# doing while the number sat still.
#
# strace -ttt gives absolute epoch timestamps and -T the duration of
# each call, so write() spans line up directly with the pty timestamps
# of the counter repaints. Both clocks are time.time().
#
# Reading the output: a gap where `in write()` accounts for nearly all
# the wall time is the kernel throttling the writer (balance_dirty_
# pages); a gap with little write() time is the pipeline blocked
# somewhere else and worth a different trace.
#
# usage: trace-stall.py <store> <name> <dir>    env: MEMMAX, GAP
import os
import pty
import re
import select
import subprocess
import sys
import time

if len(sys.argv) != 4:
	sys.exit(f"usage: {sys.argv[0]} <store> <name> <dir>")

WRITES = "build/tmp/trace-stall-writes.txt"
GAP = float(os.environ.get("GAP", "0.25"))

cmd = ["strace", "-f", "-ttt", "-T", "-e", "trace=write", "-o", WRITES,
       "build/tmp/import-dir", *sys.argv[1:]]
if memmax := os.environ.get("MEMMAX"):
	cmd = ["systemd-run", "--user", "--scope", "-q",
	       "-p", f"MemoryMax={memmax}", "-p", f"MemoryHigh={memmax}",
	       f"--setenv=LD_LIBRARY_PATH={os.environ.get('LD_LIBRARY_PATH','')}",
	       *cmd]

primary, secondary = pty.openpty()
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=secondary)
os.close(secondary)

paints, buf, last = [], "", -1
pat = re.compile(r"importing: (\d+)")
while True:
	r, _, _ = select.select([primary], [], [], 0.2)
	if not r:
		if p.poll() is not None:
			break
		continue
	try:
		chunk = os.read(primary, 65536)
	except OSError:
		break
	if not chunk:
		break
	now = time.time()
	buf += chunk.decode("utf8", "replace")
	end = 0
	for m in pat.finditer(buf):
		n = int(m.group(1)); end = m.end()
		if n > last:
			last = n
			paints.append((now, n))
	buf = buf[end:][-64:]
p.wait()

if len(paints) < 3:
	sys.exit("no counter paints captured (is stderr a tty? did the import run?)")

# tid, start, dur -- strace writes the duration at the end of the line
wr = []
lp = re.compile(r"^(\d+)\s+(\d+\.\d+)\s+write\(.*<(\d+\.\d+)>\s*$")
with open(WRITES) as f:
	for line in f:
		m = lp.match(line)
		if m:
			wr.append((int(m.group(1)), float(m.group(2)), float(m.group(3))))
wr.sort(key=lambda x: x[1])

t0, tn = paints[0][0], paints[-1][0]
print(f"\nimport: {paints[-1][1]} files, {tn - t0:.2f} s, {len(paints)} counter paints, "
      f"{len(wr)} write() calls", file=sys.stderr)

def writes_in(a, b):
	sel = [w for w in wr if w[1] < b and w[1] + w[2] > a]
	busy = sum(min(w[1] + w[2], b) - max(w[1], a) for w in sel)
	return sel, busy

gaps = []
for i in range(1, len(paints)):
	dt = paints[i][0] - paints[i - 1][0]
	if dt >= GAP:
		gaps.append((dt, paints[i - 1][0], paints[i][0],
			     paints[i][1] - paints[i - 1][1]))
gaps.sort(reverse=True)

stalled = sum(g[0] for g in gaps)
print(f"counter frozen >= {GAP}s: {len(gaps)} times, {stalled:.2f} s "
      f"({100 * stalled / (tn - t0):.0f}% of the import)\n", file=sys.stderr)

print(f"{'freeze':>8} {'files':>6} {'writes':>7} {'in write()':>11} {'longest':>9}  thread",
      file=sys.stderr)
for dt, a, b, nf in gaps[:10]:
	sel, busy = writes_in(a, b)
	longest = max(sel, key=lambda w: w[2]) if sel else None
	print(f"{dt:7.2f}s {nf:6d} {len(sel):7d} {busy:10.2f}s "
	      f"{(longest[2] if longest else 0):8.2f}s  "
	      f"tid {longest[0] if longest else '-'}", file=sys.stderr)

sel, busy = writes_in(t0, tn)
print(f"\nwhole import: {busy:.2f}s of {tn - t0:.2f}s inside write() "
      f"({100 * busy / (tn - t0):.0f}%)", file=sys.stderr)
outside = busy - sum(writes_in(a, b)[1] for _, a, b, _ in gaps)
print(f"  during freezes: {busy - outside:.2f}s   while the counter moved: {outside:.2f}s",
      file=sys.stderr)
by_tid = {}
for tid, _, d in wr:
	by_tid[tid] = by_tid.get(tid, 0) + d
print("  write() time by thread: "
      + ", ".join(f"tid {t}: {v:.2f}s" for t, v in sorted(by_tid.items(), key=lambda x: -x[1])),
      file=sys.stderr)
