#!/bin/sh -e
# How long can nixgen-perms.service be? Measured inside a booted box
# over the serial console, against generations whose metadata manifests
# grow class by class.
#
# The service is Type=oneshot ExecStart=nixgen-restmeta (setup-boot.sh),
# so its duration is restmeta's wall time over
# etc/nixgen/{perms,caps,acls,xattrs,attrs} of the generation being
# booted. What decides that time is exec count, not manifest size: mode
# and ownership rows batch per distinct value, ACLs and xattrs restore
# in one call, and caps stay one exec per row (setcap takes <caps>
# <file> pairs). So the sweep is per class rather than per megabyte --
# a class that forks per row is a different curve, not a bigger one.
# That asymmetry is what this found: ownership rows were per-row until
# 100k of them measured 144 s of boot here. See BASELINE.
#
# Rebooting into successively larger generations is not available over
# serial: GRUB is 'set default=0' (nixgen-setup) and its menu goes to
# the VGA that SERIAL=on removes. Instead each committed generation is
# staged exactly as boot stages it -- overlay on the store disk, fresh
# tmpfs upper, metacopy=on -- and restmeta timed against that view.
# nixgen-switch already does this staging for real; the anchor phase
# below checks the staged number against the actual service delta
# systemd recorded for this boot.
#
# The ISO carries the store binaries and the nixgen scripts, so this
# measures whatever arch/iso/mkiso.sh last built.
#
# usage: vm-perms.sh [label]
# env: SIZES        row counts for the batched classes
#      SLOW_SIZES   row counts for the exec-per-row classes
#      CLASSES      subset of: mode own caps acls xattrs attrs
#      CACHE,IOPS,BPS,CPUS,RAM   see uefi-vm.sh
#      STORE, STORESIZE, KEEPSTORE   bench store disk
cd "$(dirname "$0")/.."

LABEL=${1:-perms}
SIZES=${SIZES:-"2000 20000"}
SLOW_SIZES=${SLOW_SIZES:-"1000 5000"}
CLASSES=${CLASSES:-"mode own caps acls xattrs attrs"}
# its own disk, freshly formatted: base is only 'base' against an empty
# store, and a bench has no business writing to the store disk someone
# is developing against
STORE=${STORE:-$PWD/build/vm/nixstore-perms.img}
export STORE
if [ "${KEEPSTORE:-0}" = 0 ]; then
	arch/iso/mkstoredisk.sh "$STORE" "${STORESIZE:-16G}" > /dev/null
fi
# a commit per data point, each preceded by planting up to 20k paths
SERIAL_CMD_TIMEOUT=${SERIAL_CMD_TIMEOUT:-900}
export SERIAL_CMD_TIMEOUT

mkdir -p build/tmp
LOG=build/tmp/vm-perms-$LABEL.log

# The in-box script goes over the wire base64'd and in chunks:
# serial-sh.py sends one line and waits for a prompt (so no heredoc),
# and the tty line discipline drops anything past ~4 KiB in one line.
B64=$(base64 -w0 bench/perms-inbox.sh)
set -- "rm -f /tmp/pb.b64"
for chunk in $(echo "$B64" | fold -w 1000); do
	set -- "$@" "echo -n $chunk >> /tmp/pb.b64"
done
set -- "$@" "base64 -d < /tmp/pb.b64 > /tmp/pb.sh"

# anchor: what systemd actually charged the service on THIS boot, next
# to the manifest it replayed. The staged base number below has to land
# near it or the staging is not modelling the boot
set -- "$@" "systemctl show nixgen-perms.service -p InactiveExitTimestampMonotonic -p ActiveEnterTimestampMonotonic"
set -- "$@" "wc -l /etc/nixgen/perms /etc/nixgen/caps /etc/nixgen/acls /etc/nixgen/xattrs /etc/nixgen/attrs"

# base first: every later row count is base + N, so the per-row cost is
# a subtraction rather than an intercept guess
set -- "$@" "sh /tmp/pb.sh base 0"
for c in $CLASSES; do
	case $c in
	caps | attrs) sizes=$SLOW_SIZES ;;
	*) sizes=$SIZES ;;
	esac
	for n in $sizes; do
		set -- "$@" "sh /tmp/pb.sh $c $n"
	done
done
set -- "$@" "nixgen-listid"

echo "== $LABEL  CLASSES='$CLASSES' SIZES='$SIZES' SLOW_SIZES='$SLOW_SIZES'"
echo "   CACHE=${CACHE:-writeback} IOPS=${IOPS:-0} BPS=${BPS:-0} CPUS=${CPUS:-6}"
arch/tests/serial-sh.py iso "$@" 2>&1 | tee "$LOG"

echo
echo "--- $LABEL"
grep -a "^PERMSLINE" "$LOG" | sed "s/^PERMSLINE/$LABEL/" || {
	echo "no PERMSLINE in $LOG (commit failed? see the log)"
	exit 1
}
