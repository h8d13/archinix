#!/bin/sh -e
# Runs inside the box; sent over serial by bench/vm-commit.sh, which is
# where the reasoning lives. Times one commit phase and charges it both
# the store disk's sector deltas (/sys/block/vda/stat fields 3 and 7)
# and what the kernel says it cost in memory.
#
# Memory comes from a transient cgroup scope, not from the tool's own
# RSS: nixgen-commit is a shell script driving import-dir, and the RAM
# that decides whether a commit survives on a small box is the whole
# tree's charge, page cache included (the overlay upper is a tmpfs, so
# the pages the commit reads and the pages it dirties compete for the
# same RAM). Two numbers, because the peak alone cannot tell them
# apart:
#   mem_peak   memory.peak: kernel high-water for the scope, anon +
#              page cache the commit pulled in or dirtied
#   anon_peak  sampled max of memory.stat anon: what the tools HELD.
#              This is the one that must stay flat as trees grow; a
#              mem_peak near RAM is expected, cache is reclaimable.
# usage: commit-inbox.sh <phase> <cmd...>
#        commit-inbox.sh --inner <cmd...>   (re-exec inside the scope)
STOP=/tmp/ci-stop
ANON=/tmp/ci-anon
PEAK=/tmp/ci-peak

if [ "$1" = --inner ]; then
	shift
	C=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)
	rm -f "$STOP"
	echo 0 > "$ANON"
	# 50 ms: a commit runs seconds to minutes and grows its tables
	# gradually, so this catches the shape. A spike shorter than one
	# tick is invisible to anon_peak -- mem_peak is the kernel's own
	# high-water and does see it.
	(
		M=0
		while [ ! -e "$STOP" ]; do
			A=$(awk '/^anon /{print $2}' "$C/memory.stat")
			if [ -n "$A" ] && [ "$A" -gt "$M" ]; then
				M=$A
				echo "$M" > "$ANON"
			fi
			sleep 0.05
		done
	) &
	SAMPLER=$!

	RC=0
	"$@" || RC=$?

	touch "$STOP"
	wait "$SAMPLER"
	# read the peak while still inside the scope: the cgroup and its
	# counters are removed the moment this shell exits
	if [ -r "$C/memory.peak" ]; then
		cat "$C/memory.peak" > "$PEAK"
	else
		# pre-5.19 kernel: no high-water file to read
		echo 0 > "$PEAK"
	fi
	exit "$RC"
fi

PHASE=$1
shift
read -r _ _ RD0 _ _ _ WR0 _ < /sys/block/vda/stat
S=$(date +%s%N)
systemd-run --scope -q -p MemoryAccounting=yes sh "$0" --inner "$@"
E=$(date +%s%N)
read -r _ _ RD1 _ _ _ WR1 _ < /sys/block/vda/stat

echo "BENCHLINE $PHASE wall_ms=$(( (E - S) / 1000000 ))" \
	"rd_mib=$(( (RD1 - RD0) / 2048 )) wr_mib=$(( (WR1 - WR0) / 2048 ))" \
	"mem_peak_mib=$(( $(cat "$PEAK") / 1048576 ))" \
	"anon_peak_mib=$(( $(cat "$ANON") / 1048576 ))"
