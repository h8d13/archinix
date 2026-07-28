#!/bin/sh -e
# Volatile state rows: the fourth category (see NOTES.md). Two halves,
# because the risk is in two places.
#
# Host: nixgen-statepaths, the one parser four call sites read rows
# through. It takes a manifest argument, so the row grammar is testable
# without a VM, and a tag leaking into the persistent list is exactly
# the bug that would hand nixgen-seedstate the word "volatile" as a
# path to create.
#
# Box: the generator's two backings, which are only real on a booted
# system. With a store disk the row must be a bind off that disk (the
# bytes never enter RAM and survive a rollback); without one, its own
# capped tmpfs (quarantined, not sharing the upper). Both must keep the
# cache out of committed generations, and neither may write to a path
# the commit/update scrubs do not know about -- pacman creates the
# first CacheDir it is given rather than falling through, so "it will
# land somewhere harmless" is not a thing to assume.
# usage: volatile-test.sh   (needs build/nixarch.iso; builds its own
#        store disk, never touches build/vm/nixstore.img)
cd "$(dirname "$0")/../.."

PARSE=arch/nixgen/nixgen-statepaths
STORE=$PWD/build/vm/nixstore-vol.img
LOG=build/tmp/volatile-test.log
mkdir -p build/tmp
rm -f "$LOG"

T=$(mktemp -d build/tmp/volatile.XXXXXX)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

echo "--- host: manifest row grammar"
printf '/home\n/var/log\n/var/cache/pacman/pkg\tvolatile\n' > "$T/m"
printf '/srv/big volatile=40%%\n#/commented\nbogus\n/bad\tnosuchtag\n' >> "$T/m"

P=$("$PARSE" persistent "$T/m")
V=$("$PARSE" volatile "$T/m" 2> "$T/err")

[ "$P" = "$(printf '/home\n/var/log')" ] \
	|| fail "persistent rows wrong: $P"
# a tag must never reach the persistent list: seedstate takes that
# list as argv and would mkdir whatever it finds
if echo "$P" | grep -q volatile; then
	fail "tag leaked into the persistent list"
fi
[ "$V" = "$(printf '/var/cache/pacman/pkg 25%%\n/srv/big 40%%')" ] \
	|| fail "volatile rows wrong: $V"
grep -q "unknown tag 'nosuchtag'" "$T/err" \
	|| fail "unknown tag not reported: $(cat "$T/err")"
# an unknown tag must be dropped, not demoted to persistent: that
# would put a cache on the data partition
if echo "$P" | grep -q '/bad'; then
	fail "unknown-tag row demoted to persistent"
fi
echo "OK   rows split by tag, default and per-row size, bad tag dropped"

[ -f build/nixarch.iso ] || fail "no build/nixarch.iso (arch/iso/mkiso.sh)"
arch/iso/mkstoredisk.sh "$STORE" 8G > /dev/null

echo "--- box: store disk attached, row must be backed by the disk"
# pacman -Syw needs the network; the ISO boots with user-mode NAT
STORE=$STORE SERIAL_CMD_TIMEOUT=600 arch/tests/serial-sh.py iso \
	'findmnt -no SOURCE,FSTYPE /var/cache/pacman/pkg' \
	'findmnt -no SOURCE /var/cache/pacman/pkg | grep -q "\[/cache/var/cache/pacman/pkg\]" && echo DISK_"BIND_OK"' \
	'journalctl -b --no-pager | grep -qi "ordering cycle" || echo ORDER_"OK"' \
	'pacman -Syw --noconfirm which > /tmp/p.log 2>&1; echo pacman_rc=$?; tail -2 /tmp/p.log' \
	'ls /nixstoredev/cache/var/cache/pacman/pkg | grep -q "^which-.*pkg.tar.zst$" && echo DISK_"CACHE_OK"' \
	'nixgen-commit vol-test > /tmp/c.log 2>&1; tail -1 /tmp/c.log' \
	'G=$(basename $(ls -d /nixstoredev/nix/store/*-vol-test)); [ -z "$(ls -A /nixstoredev/nix/store/$G/var/cache/pacman/pkg)" ] && [ -z "$(ls -A /nixstoredev/nix/store/$G/nixstoredev)" ] && echo EXCL_"OK"' \
	> "$LOG" 2>&1 || { cat "$LOG"; fail "disk phase died (see $LOG)"; }

for m in DISK_BIND_OK ORDER_OK DISK_CACHE_OK EXCL_OK; do
	grep -q "^$m" "$LOG" || { cat "$LOG"; fail "missing marker $m"; }
done
echo "OK   bound off the store disk, downloads land there, gen excludes it"

echo "--- box: no store disk, row must be its own capped tmpfs"
STORE=none SERIAL_CMD_TIMEOUT=600 arch/tests/serial-sh.py iso \
	'findmnt -no FSTYPE,OPTIONS /var/cache/pacman/pkg' \
	'findmnt -no FSTYPE /var/cache/pacman/pkg | grep -q tmpfs && echo TMPFS_"OK"' \
	'findmnt -no OPTIONS /var/cache/pacman/pkg | grep -q "size=" && echo CAP_"OK"' \
	'pacman -Syw --noconfirm which > /tmp/p.log 2>&1; echo pacman_rc=$?' \
	'ls /var/cache/pacman/pkg | grep -q "^which-.*pkg.tar.zst$" && echo TMPFS_"CACHE_OK"' \
	'[ ! -e /nixstoredev/cache ] && echo NO_"DISK_WRITE_OK"' \
	>> "$LOG" 2>&1 || { cat "$LOG"; fail "diskless phase died (see $LOG)"; }

for m in TMPFS_OK CAP_OK TMPFS_CACHE_OK NO_DISK_WRITE_OK; do
	grep -q "^$m" "$LOG" || { cat "$LOG"; fail "missing marker $m"; }
done
echo "OK   capped tmpfs diskless, nothing written under /nixstoredev"

rm -f "$STORE"
echo "PASS: volatile rows parse apart from persistent ones, ride the" \
	"store disk when there is one and a capped tmpfs when there is" \
	"not, and stay out of committed generations either way"
