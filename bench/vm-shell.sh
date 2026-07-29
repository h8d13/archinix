#!/bin/sh -e
# Where a commit's time goes OUTSIDE the store.
#
# bench/vm-commit.sh times the whole nixgen-commit; the in-box numbers
# put import at 1.6-2.8 s and optimise at 0.1-0.3 s against a 3.2-5.4 s
# commit, so something between one and two seconds of every commit is
# neither. This splits that remainder.
#
# It is not obvious a priori that any of it should exist. NAR keeps a
# file's type, its contents and the executable bit, and nothing else:
# no mode, no uid/gid, no xattrs, no ACLs, no capabilities, no chattr
# flags. That is deliberate, and it is what lets two trees differing
# only in ownership hash to the same store path, which is what makes
# dedup work at all. But a bootable rootfs needs /etc/shadow at 0600
# and setcap binaries with their capabilities, so nixgen-savemeta
# captures that metadata as ordinary files inside the tree
# (etc/nixgen/{perms,caps,acls,xattrs,attrs}) before the dump, where it
# becomes content: hashed, deduped, and rolled back with the
# generation. That capture has to precede the import, and it is the
# bulk of what this measures.
#
# The probes are the interesting part: getcap, getfacl, getfattr and
# lsattr have no bulk form, so they run once per path, and lsattr -d
# opens every inode. Against an overlayfs merged view whose lower is
# squashfs, that is not the same as running it on the host.
#
# usage: vm-shell.sh [label]
# env: CPUS, RAM, PKGS   (PKGS default: the driver-heavy shape)
cd "$(dirname "$0")/.."

LABEL=${1:-shell}
PKGS=${PKGS-linux-firmware linux-headers sway}
CPUS=${CPUS:-4} RAM=${RAM:-6G}
export CPUS RAM
SERIAL_CMD_TIMEOUT=${SERIAL_CMD_TIMEOUT:-900}
export SERIAL_CMD_TIMEOUT

mkdir -p build/tmp
LOG=build/tmp/vm-shell-$LABEL.log

# Same trick as vm-commit.sh: the timing helper goes over the wire
# base64'd, because serial-sh.py sends one line and waits for a prompt.
HELPER=$(printf '%s\n' \
	'P=$1; shift' \
	'S=$(date +%s%N)' \
	'"$@" > /dev/null 2>&1' \
	'E=$(date +%s%N)' \
	'echo SHELLLINE $P ms=$(( (E - S) / 1000000 ))' \
	| base64 -w0)

# One walk per probe, so each manifest's cost is attributable. These
# mirror nixgen-savemeta's own pipelines; keep them in step with it.
WALKS=$(printf '%s\n' \
	'cd /run/s' \
	'TAB=$(printf "\t")' \
	't() { N=$1; shift; S=$(date +%s%N); eval "$@" > /dev/null 2>&1; E=$(date +%s%N); echo SHELLLINE $N ms=$(( (E - S) / 1000000 )); }' \
	't walk_bare "find . -mindepth 1 -xdev -printf \"%y${TAB}%p\\0\" > /tmp/list"' \
	't sort_two "cut -z -f2- /tmp/list | LC_ALL=C sort -z > /tmp/fd"' \
	't probe_caps "grep -z \"^f${TAB}\" /tmp/list | cut -z -f2- | LC_ALL=C sort -z | xargs -0 -r getcap"' \
	't probe_acls "xargs -0 -r getfacl -sPp --numeric < /tmp/fd"' \
	't probe_xattr "xargs -0 -r getfattr -h -d -m \"^(user|trusted)\\.\" < /tmp/fd"' \
	't probe_attrs "xargs -0 -r lsattr -d < /tmp/fd"' \
	'echo ENTRIES=$(tr -cd "\\0" < /tmp/fd | wc -c)' \
	| base64 -w0)

set -- "echo $HELPER | base64 -d > /tmp/t.sh"
set -- "$@" "echo $WALKS | base64 -d > /tmp/walks.sh"
if [ -n "$PKGS" ]; then
	set -- "$@" "pacman -Sy --noconfirm --needed $PKGS > /tmp/pac.log 2>&1; tail -1 /tmp/pac.log"
fi
set -- "$@" "du -sm /usr | tail -1"
# the snapshot nixgen-commit imports: same bind, same rprivate
set -- "$@" "mkdir -p /run/s && mount --bind / /run/s && mount --make-rprivate /run/s && echo SNAP_OK"
set -- "$@" "sh /tmp/t.sh savemeta_total nixgen-savemeta /run/s"
set -- "$@" "sh /tmp/walks.sh"

echo "== $LABEL  CPUS=$CPUS RAM=$RAM PKGS=${PKGS:-none}"
arch/tests/serial-sh.py iso "$@" 2>&1 | tee "$LOG"

echo
echo "--- $LABEL (ms)"
grep -a "^SHELLLINE" "$LOG" | sed 's/^SHELLLINE //' || echo "no SHELLLINE in $LOG"
grep -a "^ENTRIES=" "$LOG" || :
