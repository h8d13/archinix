#!/bin/sh -e
# End-to-end test of the persistent state path (nixgen-data + nixdata=)
# for both default manifest entries, /home and /var/log.
# Boot 1 (ISO + store disk + blank data disk): pre-seed a file into the
# RAM /home, format the data disk in the box (nixgen-data: mkfs deps
# install on demand, seed copies the running /home and /var/log over),
# commit a generation. Boot 2 (that generation, nixdata=NIXDATA): both
# paths must be the data filesystem, carry the seeded content, take new
# markers; a commit here must exclude them (non-recursive snapshot) and
# its entry must inherit nixdata= and be GC-rooted. Boot 3 (same, with
# the flag): markers persisted. Boot 4 (same generation, NO nixdata=,
# disk still attached): overlay /home, markers gone, seed file present
# from the lower: volatility is gated on the flag, not the disk.
cd "$(dirname "$0")/../.."

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"
LOG=build/tmp/state-test.log
SOCK=build/tmp/state-test.sock
DATA=build/tmp/state-data.img
mkdir -p build/tmp
rm -f "$LOG" "$SOCK" "$DATA"

drive() { python3 - "$SOCK" "$LOG" "$@" <<'PY'
import socket, sys, time

sock_path, log_path = sys.argv[1], sys.argv[2]
script = sys.argv[3:]           # expect [send expect]... [send]

s = socket.socket(socket.AF_UNIX)
deadline = time.time() + 30
while True:
	try:
		s.connect(sock_path)
		break
	except OSError:
		if time.time() > deadline:
			sys.exit("connect timeout on " + sock_path)
		time.sleep(0.5)
s.settimeout(1.0)
log = open(log_path, "ab")
buf = b""

def wait_for(pat, timeout):
	global buf
	end = time.time() + timeout
	while pat.encode() not in buf:
		if time.time() > end:
			sys.exit("timeout waiting for: " + pat)
		try:
			d = s.recv(4096)
		except TimeoutError:
			continue
		if not d:
			sys.exit("eof waiting for: " + pat)
		buf += d
		log.write(d)
		log.flush()
	for line in buf.decode(errors="replace").splitlines():
		if pat in line:
			print(line)
	buf = b""

wait_for(script[0], 300)
i = 1
while i < len(script):
	s.sendall(script[i].encode() + b"\n")
	if i + 1 >= len(script):
		break           # trailing send (e.g. poweroff), no expect
	wait_for(script[i + 1], 900)
	i += 2
PY
}

arch/iso/mkstoredisk.sh
truncate -s 2G "$DATA"

echo "--- boot 1: ISO, format the data disk in the box, commit"
qemu-system-x86_64 $ACCEL -m 2G -boot d -cdrom build/nixarch.iso \
	-drive file=build/vm/nixstore.img,format=raw,if=virtio \
	-drive file="$DATA",format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
# The generation name is read back on a line of its own, with the
# marker LAST. wait_for matches a substring of whatever has arrived so
# far and then prints only the lines carrying it, so neither end works:
# "committed: " can fire mid-name and capture nothing, and the name's
# own suffix matches import-dir's earlier "imported: /nix/store/<gen>"
# line, whose match clears the buffer before commit's line lands.
# nixgen-commit's trailing "(GRUB entry added, visible next boot)" used
# to be the pattern here and was dropped in f9fb8e3.
# The read-back is queued while commit still runs; bash executes it
# after, so the glob always resolves
OUT=$(drive "NIXARCH BOOT OK" \
	'mkdir -p /home/pre && echo seedmark > /home/pre/seedfile && echo PRE_OK' \
	"PRE_OK" \
	"nixgen-data /dev/vdb" \
	"type the device path to continue" \
	"/dev/vdb" \
	"label NIXDATA" \
	"nixgen-commit test-state" \
	"-test-state" \
	'echo "committed: $(basename $(ls -d /nixstoredev/nix/store/*-test-state))" GENLINE_"OK"' \
	"GENLINE_OK" \
	"poweroff") || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID
GEN=$(echo "$OUT" | sed -n 's/.*committed: \([^ ]*\).*/\1/p')
[ -n "$GEN" ] || { echo "FAIL: no generation name captured"; exit 1; }
echo "generation: $GEN"

rm -f build/tmp/state-vmlinuz build/tmp/state-initrd
debugfs -R "dump /nix/store/$GEN/boot/vmlinuz-linux build/tmp/state-vmlinuz" \
	build/vm/nixstore.img 2>/dev/null
debugfs -R "dump /nix/store/$GEN/boot/initramfs-linux.img build/tmp/state-initrd" \
	build/vm/nixstore.img 2>/dev/null
[ -s build/tmp/state-vmlinuz ] && [ -s build/tmp/state-initrd ] \
	|| { echo "FAIL: kernel/initramfs not extracted from img"; exit 1; }

echo "--- boot 2: nixdata=NIXDATA, mounts + seed + markers + commit exclusion"
rm -f "$SOCK"
qemu-system-x86_64 $ACCEL -m 2G \
	-kernel build/tmp/state-vmlinuz -initrd build/tmp/state-initrd \
	-append "nixgen=$GEN nixsource=disk nixdata=NIXDATA console=ttyS0,115200" \
	-drive file=build/vm/nixstore.img,format=raw,if=virtio \
	-drive file="$DATA",format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
drive "NIXARCH BOOT OK" \
	'echo "homefs=$(findmnt -no FSTYPE -T /home)"' \
	"homefs=ext4" \
	'echo "logfs=$(findmnt -no FSTYPE -T /var/log)"' \
	"logfs=ext4" \
	'journalctl -b --no-pager | grep -qi "ordering cycle" || echo ORDER_"OK"' \
	"ORDER_OK" \
	'echo "seed=$(cat /home/pre/seedfile)"' \
	"seed=seedmark" \
	'[ -s /var/log/pacman.log ] && echo LOG_SEEDED' \
	"LOG_SEEDED" \
	'echo statemark > /home/pre/statemark && echo logmark > /var/log/logmark && echo MARKS_OK' \
	"MARKS_OK" \
	"nixgen-commit test-excl" \
	"-test-excl" \
	'E=$(basename "$(ls -d /nixstoredev/nix/store/*-test-excl)"); [ -f "/nixstoredev/nix/store/$E/home/pre/seedfile" ] && [ ! -e "/nixstoredev/nix/store/$E/home/pre/statemark" ] && [ ! -e "/nixstoredev/nix/store/$E/var/log/logmark" ] && echo EXCL_OK' \
	"EXCL_OK" \
	'[ -L "/nixstoredev/nix/var/nix/gcroots/$E" ] && echo ROOTED' \
	"ROOTED" \
	'grep -q "nixgen=$E .*nixdata=NIXDATA" /nixstoredev/entries.cfg && echo INHERIT_OK' \
	"INHERIT_OK" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID

echo "--- boot 3: nixdata=NIXDATA again, markers must persist"
rm -f "$SOCK"
qemu-system-x86_64 $ACCEL -m 2G \
	-kernel build/tmp/state-vmlinuz -initrd build/tmp/state-initrd \
	-append "nixgen=$GEN nixsource=disk nixdata=NIXDATA console=ttyS0,115200" \
	-drive file=build/vm/nixstore.img,format=raw,if=virtio \
	-drive file="$DATA",format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
drive "NIXARCH BOOT OK" \
	'echo "back=$(cat /home/pre/statemark)-$(cat /var/log/logmark)"' \
	"back=statemark-logmark" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID

echo "--- boot 4: no nixdata=, disk attached: volatile, flag-gated"
rm -f "$SOCK"
qemu-system-x86_64 $ACCEL -m 2G \
	-kernel build/tmp/state-vmlinuz -initrd build/tmp/state-initrd \
	-append "nixgen=$GEN nixsource=disk console=ttyS0,115200" \
	-drive file=build/vm/nixstore.img,format=raw,if=virtio \
	-drive file="$DATA",format=raw,if=virtio \
	-nic user,model=virtio-net-pci \
	-display none -no-reboot -serial "unix:$SOCK,server,nowait" &
QPID=$!
drive "NIXARCH BOOT OK" \
	'echo "homefs=$(findmnt -no FSTYPE -T /home)"' \
	"homefs=overlay" \
	'[ ! -e /home/pre/statemark ] && [ ! -e /var/log/logmark ] && [ -f /home/pre/seedfile ] && echo VOLATILE_OK' \
	"VOLATILE_OK" \
	"poweroff" > /dev/null || { kill $QPID 2>/dev/null; exit 1; }
wait $QPID
rm -f build/tmp/state-vmlinuz build/tmp/state-initrd "$DATA"

echo "PASS: /home and /var/log seeded onto the data disk, persisted" \
	"across reboots under nixdata=, excluded from commit (entry rooted," \
	"flag inherited), and volatile again without the flag"
