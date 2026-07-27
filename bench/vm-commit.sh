#!/bin/sh -e
# Commit benchmark inside a booted box, over the serial console.
#
# Every number in BASELINE up to now was measured on the host, against
# a tree on an NVMe with the page cache in whatever state the previous
# run left it. That measures the developer's laptop. This measures the
# thing that ships: nixgen-commit, snapshotting a real running root
# onto a real store disk, through the initramfs-mounted store, with
# whatever media the drive knobs say.
#
# Three phases, which are the three shapes a store sees:
#   base   first commit onto a fresh store   (nothing to dedup)
#   pkgs   commit after installing packages  (mostly dup + new content)
#   noop   commit again, nothing changed     (everything dedups)
# The middle one is the real update shape and the reason for PKGS:
# synthetic trees have to guess a size distribution and a dedup
# topology, and guessing it wrong is easy (an earlier generator here
# produced 26 duplicate files where it meant to produce 20,427). A
# package set has the real one, including how it overlaps the previous
# generation, which is the part that actually decides commit cost.
#
# Media is the variable that matters most and the one a host bench
# cannot reach: see uefi-vm.sh's CACHE/IOPS/BPS. Sweep them here.
#
# The ISO carries the store binaries, so this measures whatever
# arch/iso/mkiso.sh last built -- rebuild it to measure a code change.
#
# usage: vm-commit.sh [label]
# env: CACHE, IOPS, BPS   drive knobs (see uefi-vm.sh)
#      CPUS, RAM          VM resources
#      PKGS               packages to install for the middle phase
#                         (default sway; empty skips that phase)
cd "$(dirname "$0")/.."

LABEL=${1:-vm}
PKGS=${PKGS-sway}
# Its own disk, freshly formatted per run: the 'base' phase only means
# anything against an empty store, and a bench has no business writing
# to the store disk someone is developing against. STORE= to override.
STORE=${STORE:-$PWD/build/vm/nixstore-bench.img}
export STORE
if [ "${KEEPSTORE:-0}" = 0 ]; then
	arch/iso/mkstoredisk.sh "$STORE" "${STORESIZE:-16G}" > /dev/null
fi
# pacman downloads and a multi-GiB commit both outrun the interactive
# default; the store disk is throttled on purpose in some runs
SERIAL_CMD_TIMEOUT=${SERIAL_CMD_TIMEOUT:-900}
export SERIAL_CMD_TIMEOUT

mkdir -p build/tmp
LOG=build/tmp/vm-commit-$LABEL.log

# The in-box helper: time a command and charge it the store disk's
# sector deltas (/sys/block/vda/stat fields 3 and 7). It goes over the
# wire base64'd: serial-sh.py sends one line and waits for a prompt, so
# a heredoc would hang, and hand-escaping a nested quoting mess through
# two shells is how you end up debugging the harness instead of the
# store.
HELPER=$(printf '%s\n' \
	'P=$1; shift' \
	'read -r _ _ RD0 _ _ _ WR0 _ < /sys/block/vda/stat' \
	'S=$(date +%s%N)' \
	'"$@"' \
	'E=$(date +%s%N)' \
	'read -r _ _ RD1 _ _ _ WR1 _ < /sys/block/vda/stat' \
	'echo BENCHLINE $P wall_ms=$(( (E - S) / 1000000 )) rd_mib=$(( (RD1 - RD0) / 2048 )) wr_mib=$(( (WR1 - WR0) / 2048 ))' \
	| base64 -w0)

set -- "echo $HELPER | base64 -d > /tmp/bench.sh"
set -- "$@" "sh /tmp/bench.sh base nixgen-commit base"
if [ -n "$PKGS" ]; then
	# -Sy not -Syu: a full upgrade rewrites the whole root and turns
	# the phase into a second 'base' instead of an incremental update
	set -- "$@" "pacman -Sy --noconfirm --needed $PKGS > /tmp/pac.log 2>&1; tail -2 /tmp/pac.log"
	set -- "$@" "du -sm /usr | tail -1"
	set -- "$@" "sh /tmp/bench.sh pkgs nixgen-commit pkgs"
fi
set -- "$@" "sh /tmp/bench.sh noop nixgen-commit noop"
set -- "$@" "nixgen-listid"

echo "== $LABEL  CACHE=${CACHE:-writeback} IOPS=${IOPS:-0} BPS=${BPS:-0} CPUS=${CPUS:-6} PKGS=${PKGS:-none}"
arch/tests/serial-sh.py iso "$@" 2>&1 | tee "$LOG"

echo
echo "--- $LABEL"
grep -a "^BENCHLINE" "$LOG" | sed "s/^BENCHLINE/$LABEL/" || {
	echo "no BENCHLINE in $LOG (commit failed? see the log)"
	exit 1
}
