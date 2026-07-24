#!/bin/sh -e
# End-to-end test of nixgen-update, driven over tests/serial-sh.py:
# one phase per boot, assertions on the host between phases, so a
# stall dies at the offending command with the console tail printed
# instead of deep inside an embedded expect script.
# boot 1 (ISO + fresh store disk): offline update in the box installs
# the kernel from a dated Arch Linux Archive snapshot: a real version
# change (direction is irrelevant to the machinery: new kernel files
# land in the overlay, the ALPM hook regenerates the initramfs, the
# new generation boots its own kernel). Archive use lives here only;
# stock generations track live mirrors. A no-op update must refuse to
# mint a generation.
# boot 2 (that generation direct-kernel from the store disk, no ISO:
# proves self-hosting; VM_CMD, uefi-vm.sh has no -kernel mode): boot
# marker, restored permissions, the new package, then the in-box
# lifecycle: remove refuses the running generation, a committed one
# removes cleanly (store path GC'd, GRUB entry pruned), soft-switch,
# getty toggles, metadata round trip, export/import ship cycle with
# adopt, and nixgen-setup onto a blank disk.
# boot 3 (that disk alone under OVMF): real firmware, real ESP, the
# in-box install path; nixdata= inherited, /home on the data fs.
# debugfs reads the ext4 img without root. In-box markers echo with
# split quotes (STORE_"GATED") so a wrapped command echo can never
# satisfy its own assertion.
cd "$(dirname "$0")/../.."

SERSH=arch/tests/serial-sh.py
L1=build/tmp/update-test-b1.log
L2=build/tmp/update-test-b2.log
L3=build/tmp/update-test-b3.log
mkdir -p build/tmp
rm -f "$L1" "$L2" "$L3"

ACCEL=
[ -w /dev/kvm ] && ACCEL="-accel kvm"

# -e: patterns may start with a dash (-test-sw)
need() {
	grep -aq -e "$2" "$1" || { echo "FAIL: missing '$2' in $1"; exit 1; }
}
# exact occurrence count: shared messages (remove-refusal, entry
# pruned) must each appear once per leg that triggers them
need_n() {
	N=$(grep -ac -e "$2" "$1")
	[ "$N" -eq "$3" ] || {
		echo "FAIL: want $3 of '$2' in $1, got $N"
		exit 1
	}
}

# the in-box update installs the kernel from a dated archive snapshot:
# mirrorlist swapped for one transaction (-Syy: the archive db is older
# than the cached live one, plain -Sy 304s and resolves against live;
# no download timeout: the archive throttles), then restored. \$repo
# survives the layers to let pacman expand it
#
# FAST=1: the archive leg is the wall-clock pole (throttled kernel
# download + the restore resync). Swap it for a local no-network
# mutation; kernel-diff assertions are skipped, every other leg
# (lifecycle, ship/adopt, setup, all three boots) runs unchanged.
if [ -n "$FAST" ]; then
	UPCMD="echo fastmark > /usr/bin/fastmark && chmod 755 /usr/bin/fastmark"
	TOOL=/usr/bin/fastmark
else
	PIN=$(date -d '-30 days' +%Y/%m/%d)
	UPCMD="mv /etc/pacman.d/mirrorlist /tmp/ml && echo \"Server = https://archive.archlinux.org/repos/$PIN/\\\$repo/os/\\\$arch\" > /etc/pacman.d/mirrorlist && pacman -Syy --noconfirm --disable-download-timeout linux tree && mv /tmp/ml /etc/pacman.d/mirrorlist && pacman -Syy --noconfirm"
	TOOL=/usr/bin/tree
fi

arch/iso/mkstoredisk.sh
echo "--- boot 1: ISO + fresh disk, offline kernel version change in the box"
SERIAL_CMD_TIMEOUT=1800 python3 "$SERSH" iso \
	"nixgen-update test-up '$UPCMD'" \
	"nixgen-update noop-up true" > "$L1"
need "$L1" "reboot to switch"
need "$L1" "no change"
NEWGEN=$(sed -n 's/.*updated: \([^ ]*\).*/\1/p' "$L1" | head -1)
[ -n "$NEWGEN" ] || { echo "FAIL: no generation name captured"; exit 1; }
echo "new generation: $NEWGEN"

# restored dir modes in the update sandbox = no canonical-mode complaints
if grep -aq "directory permissions differ" "$L1"; then
	echo "FAIL: pacman permission warnings in update sandbox"
	exit 1
fi

debugfs -R "cat /entries.cfg" build/vm/nixstore.img 2>/dev/null \
	| grep -q "nixgen=$NEWGEN" || { echo "FAIL: no GRUB entry on disk"; exit 1; }
echo "GRUB entry present on store disk"

# the ISO kernel must have been replaced by the snapshot one, and the
# ALPM hook must have rebuilt the initramfs (nixgen hook included)
rm -f build/tmp/iso-vmlinuz build/tmp/iso-initrd build/tmp/test-vmlinuz build/tmp/test-initrd
xorriso -osirrox on -indev build/nixarch.iso \
	-extract /boot/vmlinuz-linux build/tmp/iso-vmlinuz \
	-extract /boot/initramfs-linux.img build/tmp/iso-initrd
debugfs -R "dump /nix/store/$NEWGEN/boot/vmlinuz-linux build/tmp/test-vmlinuz" \
	build/vm/nixstore.img 2>/dev/null
debugfs -R "dump /nix/store/$NEWGEN/boot/initramfs-linux.img build/tmp/test-initrd" \
	build/vm/nixstore.img 2>/dev/null
[ -s build/tmp/test-vmlinuz ] && [ -s build/tmp/test-initrd ] \
	|| { echo "FAIL: kernel/initramfs not extracted from img"; exit 1; }
if [ -z "$FAST" ]; then
	cmp -s build/tmp/iso-vmlinuz build/tmp/test-vmlinuz \
		&& { echo "FAIL: kernel unchanged (archive install broken)"; exit 1; }
	cmp -s build/tmp/iso-initrd build/tmp/test-initrd \
		&& { echo "FAIL: initramfs not regenerated"; exit 1; }
	echo "kernel version changed, initramfs regenerated"
fi

echo "--- boot 2: new generation from store disk only (no ISO)"
rm -f build/tmp/install-test.img
truncate -s 6G build/tmp/install-test.img
VM_CMD="qemu-system-x86_64 $ACCEL -m 2G \
 -kernel build/tmp/test-vmlinuz -initrd build/tmp/test-initrd \
 -append 'nixgen=$NEWGEN console=ttyS0,115200' \
 -drive file=build/vm/nixstore.img,format=raw,if=virtio \
 -drive file=build/tmp/install-test.img,format=raw,if=virtio \
 -nic user,model=virtio-net-pci \
 -display none -no-reboot -serial mon:stdio"
export VM_CMD
# nixgen-setup downloads grub + mkfs deps in the box; soft-switch
# restarts the serial session (its command output ends at the fresh
# banner's prompt, which is exactly what the assertions read)
SERIAL_CMD_TIMEOUT=600 python3 "$SERSH" custom \
	'grep -o "nixgen=[^ ]*" /proc/cmdline' \
	'echo "vartmp=$(stat -c %a /var/tmp)"' \
	'pacman -Q 2>&1 >/dev/null | grep -q "duplicated database" || echo DB_"CLEAN"' \
	'echo "shadow=$(stat -c %a /etc/shadow)"' \
	'echo "storemode=$(stat -c %a /nixstoredev/nix/store)-$(stat -c %a /nixstore)"' \
	"echo \"tool=\$(command -v $(basename "$TOOL"))\"" \
	"nixgen-remove $NEWGEN" \
	"nixgen-commit test-rm" \
	'R=$(basename "$(ls -d /nixstoredev/nix/store/*-test-rm)"); nixgen-remove test-rm' \
	'[ -e "/nixstoredev/nix/store/$R" ] || echo STORE_PATH_"GONE"' \
	'loginctl list-sessions --no-legend | grep tty1 | grep -q root && echo AUTO_"TTY1"' \
	'systemctl start getty@tty2 && sleep 1 && loginctl list-sessions --no-legend | grep tty2 | grep -q root && echo AUTO_"TTY2"' \
	'useradd -m -u 1100 -U tuser && touch /etc/diffmark && ln -s /tmp /var/linktest && chown -h 977:977 /var/linktest && touch /etc/acltest && setfacl -m u:977:r /etc/acltest && nixgen-commit test-sw' \
	"nixgen-switch test-sw" \
	'echo "postsw=$(stat -c %a /usr)-$(stat -c %a /var/tmp)"' \
	'echo "meta=$(stat -c %u /var/linktest):$(getfacl --numeric /etc/acltest | grep -c "user:977:r--"):$(getcap /usr/bin/newuidmap | grep -c setuid)"' \
	'echo "bashrc=$(stat -c %a:%u:%g /home/tuser/.bashrc)"' \
	'su - tuser -c "echo mark >> ~/.bashrc"; echo "uwrite=$?"' \
	'su - tuser -c "ls /nixstore" 2>&1 | grep -q "Permission denied" && echo STORE_"GATED"' \
	"nixgen-remove test-sw" \
	"nixgen-listid" \
	"nixgen-diffid test-up test-sw" \
	'ID=$(nixgen-listid | grep test-up | cut -c1-8); nixgen-diffid "$ID" test-sw' \
	'ok=1; for t in /usr/local/bin/nixgen-*; do nixgen-help | grep -q "$(basename "$t")" || { echo "undocumented: $t"; ok=0; }; done; [ $ok = 1 ] && echo HELP_"OK"' \
	'S=$(basename "$(ls -d /nixstoredev/nix/store/*-test-up)"); export-path /nixstoredev "$S" > /nixstoredev/ship.bundle && echo SHIP_"OK"' \
	"nixgen-remove test-up" \
	'[ ! -e "/nixstoredev/nix/store/$S" ] && echo SHIP_"GONE"' \
	'B=$(import-path /nixstoredev < /nixstoredev/ship.bundle) && rm /nixstoredev/ship.bundle && [ "$(basename "$B")" = "$S" ] && echo SHIP_"BACK"' \
	'nixgen-adopt "$S"' \
	'nixgen-adopt "$S" 2>&1 || echo DUP_"REFUSED"' \
	"echo /dev/vdb | nixgen-setup /dev/vdb inst-test --data 1G" \
	'echo root:secret | chpasswd && systemctl restart getty@tty1 && sleep 1 && ps -o args= -C agetty | grep tty1 | grep -qv autologin && echo PROMPT_"TTY1"' \
	> "$L2"
unset VM_CMD
rm -f build/tmp/iso-vmlinuz build/tmp/iso-initrd build/tmp/test-vmlinuz build/tmp/test-initrd

need "$L2" "nixgen=$NEWGEN"
need "$L2" "vartmp=1777"
need "$L2" "DB_CLEAN"
need "$L2" "shadow=600"
need "$L2" "storemode=700-700"
need "$L2" "tool=$TOOL"
need_n "$L2" "refusing to remove the running generation" 2
need_n "$L2" "GRUB entry pruned" 2
need "$L2" "committed: .*-test-rm"
need "$L2" "committed: .*-test-sw"
need "$L2" "committed: .*-inst-test"
need "$L2" "adopted: .*-test-up"
need "$L2" "STORE_PATH_GONE"
need "$L2" "AUTO_TTY1"
need "$L2" "AUTO_TTY2"
need "$L2" "-test-sw (soft)"
need "$L2" "postsw=755-1777"
need "$L2" "meta=977:1:1"
need "$L2" "bashrc=644:1100:1100"
need "$L2" "uwrite=0"
need "$L2" "STORE_GATED"
need "$L2" "test-sw (running)"
need_n "$L2" "Only in b/etc: diffmark" 2
need "$L2" "HELP_OK"
need "$L2" "SHIP_OK"
need "$L2" "SHIP_GONE"
need "$L2" "SHIP_BACK"
need "$L2" "DUP_REFUSED"
need "$L2" "installed: inst-test on /dev/vdb"
need "$L2" "PROMPT_TTY1"
echo "lifecycle assertions clean"

echo "--- boot 3: installed disk alone under OVMF (nixgen-setup output)"
OVMF_CODE= OVMF_VARS=
for pair in \
	/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd \
	/usr/share/edk2-ovmf/x64/OVMF_CODE.fd:/usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
	/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd; do
	c=${pair%%:*} v=${pair##*:}
	if [ -f "$c" ] && [ -f "$v" ]; then
		OVMF_CODE=$c OVMF_VARS=$v
		break
	fi
done
[ -n "$OVMF_CODE" ] || { echo "FAIL: no OVMF firmware (edk2-ovmf)"; exit 1; }
cp "$OVMF_VARS" build/tmp/test-ovmf-vars.fd
VM_CMD="qemu-system-x86_64 $ACCEL -machine q35 -m 2G \
 -drive if=pflash,format=raw,readonly=on,file=$OVMF_CODE \
 -drive if=pflash,format=raw,file=build/tmp/test-ovmf-vars.fd \
 -drive file=build/tmp/install-test.img,format=raw,if=virtio \
 -nic user,model=virtio-net-pci \
 -display none -no-reboot -serial mon:stdio"
export VM_CMD
python3 "$SERSH" custom \
	'grep -o "nixgen=[^ ]*" /proc/cmdline' \
	'grep -o "nixdata=[^ ]*" /proc/cmdline' \
	'echo "homefs=$(findmnt -no FSTYPE -T /home)"' \
	> "$L3"
unset VM_CMD
need "$L3" "-inst-test"
need "$L3" "nixdata=NIXDATA"
need "$L3" "homefs=ext4"
rm -f build/tmp/install-test.img build/tmp/test-ovmf-vars.fd
echo "installed disk booted under OVMF"

# the removed generation's entry is gone, the surviving one remains
ENTRIES=$(debugfs -R "cat /entries.cfg" build/vm/nixstore.img 2>/dev/null)
echo "$ENTRIES" | grep -q "test-rm" \
	&& { echo "FAIL: pruned GRUB entry still on disk"; exit 1; }
# test-up went over the wire: exported, removed, imported back,
# adopted. Its entry must be the adopted one, commit's is pruned.
echo "$ENTRIES" | grep -q "nixgen=$NEWGEN" \
	|| { echo "FAIL: shipped generation's GRUB entry lost"; exit 1; }
echo "$ENTRIES" | grep -q -- "-test-up (adopted)" \
	|| { echo "FAIL: adopted entry not on disk"; exit 1; }
# the switched-into generation was refused removal, its entry must live
echo "$ENTRIES" | grep -q -- "-test-sw" \
	|| { echo "FAIL: test-sw GRUB entry lost"; exit 1; }
echo "PASS: $NEWGEN booted from disk, kernel changed, perms restored," \
	"tree installed, remove + soft-switch + getty lifecycle clean," \
	"ship round trip adopted, in-box install boots under OVMF"
