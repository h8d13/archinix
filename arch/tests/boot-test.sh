#!/bin/sh -e
# Smoke-boot build/nixarch.iso headless: default GRUB entry, serial
# console captured to build/tmp/serial.log, pass = autologin marker
# appears. qemu has no reason to exit on success, so the log is polled
# and qemu killed as soon as the marker lands (or after 180s, the fail
# case).
cd "$(dirname "$0")/../.."

LOG=build/tmp/serial.log
mkdir -p build/tmp
ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"
DISK=
[ -f build/vm/nixstore.img ] && \
	DISK="-drive file=build/vm/nixstore.img,format=raw,if=virtio"

rm -f "$LOG"
qemu-system-x86_64 $ACCEL -m 1536 $DISK \
	-cdrom build/nixarch.iso -boot d \
	-display none -no-reboot -serial "file:$LOG" &
QPID=$!

WAITED=0
while [ "$WAITED" -lt 180 ]; do
	grep -aq "NIXARCH BOOT OK" "$LOG" 2>/dev/null && break
	kill -0 "$QPID" 2>/dev/null || break
	sleep 1
	WAITED=$((WAITED + 1))
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

grep -a "NIXARCH BOOT OK" "$LOG" || {
	echo "FAIL: marker not in $LOG"; exit 1;
}
# the hook falls back to the medium when the copy fails, so a green
# boot alone does not say which path ran. Only a SERIAL=1 image puts
# that output here, hence the two separate failures
grep -aq ":: nixgen:" "$LOG" || {
	echo "FAIL: no store-hook output; rebuild SERIAL=1"; exit 1;
}
grep -a "boot medium released" "$LOG" || {
	echo "FAIL: store not copied to RAM"; exit 1;
}
echo "PASS (${WAITED}s)"
