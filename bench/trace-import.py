#!/usr/bin/env python3
# Measure the import's *stutter*, not just its wall clock. Runs
# import-dir on a pty (progressTick is a no-op off a terminal) and
# timestamps every counter repaint. The counter advances from
# AsyncFileHasher's per-file end(), throttled to one paint per 80 ms,
# so on a pipeline that keeps moving the gaps sit at ~80 ms and the
# distribution has no tail. Every gap above that is the restore thread
# blocked -- in write() under writeback throttling, or waiting on a
# hash queue -- which is exactly what a user sees as the number
# freezing and then jumping.
# MEMMAX runs it in a transient cgroup scope: a box committing with a
# tmpfs overlay upper has no page cache to spare, so writeback
# throttling lands inside the import instead of arriving afterwards as
# one long sync. Unconstrained on a 31 GiB host it never lands at all,
# which is why a host bench cannot see this.
# usage: trace-import.py <store> <name> <dir>    env: MEMMAX
import os
import pty
import re
import select
import statistics
import subprocess
import sys
import time

if len(sys.argv) != 4:
	sys.exit(f"usage: {sys.argv[0]} <store> <name> <dir>")

cmd = ["build/tmp/import-dir", *sys.argv[1:]]
if memmax := os.environ.get("MEMMAX"):
	cmd = ["systemd-run", "--user", "--scope", "-q",
	       "-p", f"MemoryMax={memmax}", "-p", f"MemoryHigh={memmax}",
	       f"--setenv=LD_LIBRARY_PATH={os.environ.get('LD_LIBRARY_PATH', '')}",
	       *cmd]

primary, secondary = pty.openpty()
t0 = time.time()
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=secondary)
os.close(secondary)

paints = []		# (t, phase, count)
buf = ""
last = {}		# phase -> last count seen, so a partial read that
			# leaves a repaint split across two chunks is not
			# counted twice
pat = re.compile(r"(importing|optimising): (\d+)")
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
	now = time.time() - t0
	buf += chunk.decode("utf8", "replace")
	end = 0
	for m in pat.finditer(buf):
		phase, n = m.group(1), int(m.group(2))
		end = m.end()
		if n > last.get(phase, -1):
			last[phase] = n
			paints.append((now, phase, n))
	# keep only the unmatched tail: a repaint cut mid-number
	buf = buf[end:][-64:]
p.wait()

imp = [(t, n) for t, phase, n in paints if phase == "importing"]
if len(imp) < 3:
	sys.exit("no import paints captured")

gaps = [(imp[i][0] - imp[i - 1][0], imp[i][1] - imp[i - 1][1])
	for i in range(1, len(imp))]
secs = [g for g, _ in gaps]
print(f"import: {imp[-1][1]} files, {imp[-1][0] - imp[0][0]:.2f} s, "
      f"{len(imp)} paints", file=sys.stderr)
print(f"gap between paints: median {statistics.median(secs) * 1000:.0f} ms, "
      f"p90 {sorted(secs)[int(len(secs) * 0.9)] * 1000:.0f} ms, "
      f"max {max(secs) * 1000:.0f} ms", file=sys.stderr)
stall = [g for g in secs if g > 0.25]
print(f"paints over 250 ms: {len(stall)} "
      f"({sum(stall):.2f} s, {100 * sum(stall) / (imp[-1][0] - imp[0][0]):.0f}% of the import)",
      file=sys.stderr)
print("\nworst 10 gaps (s, files completed in that gap):", file=sys.stderr)
for g, n in sorted(gaps, reverse=True)[:10]:
	print(f"  {g:6.2f} s  {n:6d} files", file=sys.stderr)
