#!/bin/sh -e
# Runs inside the box; sent over serial by bench/vm-perms.sh, which is
# where the reasoning lives. One metadata class per call: plant N
# deviating paths in the live root, commit it, then stage the committed
# generation the way boot stages it (overlay on the store disk, fresh
# tmpfs upper, metacopy=on) and time nixgen-restmeta against that view.
# The staged replay IS what nixgen-perms.service runs, so this needs no
# reboot and no GRUB menu (default=0 pins the first entry anyway).
# usage: perms-inbox.sh <class> <n>   (n=0 measures the base manifest)
CLASS=$1 N=${2:-0}
D=/srv/permbench
ERR=/tmp/pb-$CLASS-$N.err
# each class starts from the base tree: rows are then base + N, and the
# per-row slope is a subtraction instead of a guess
rm -rf "$D"

files() { find "$D" -type f -print0; }

if [ "$N" -gt 0 ]; then
	mkdir -p "$D"
	seq 1 "$N" | sed "s|^|$D/f|" | xargs -r touch
	case $CLASS in
	# 600: stricter than the canonical 444, so savemeta captures it
	mode) files | xargs -0 -r chmod 600 ;;
	own) files | xargs -0 -r chown 1000:1000 ;;
	caps) files | xargs -0 -r -n1 setcap cap_net_raw+ep ;;
	acls) files | xargs -0 -r setfacl -m u:1000:r-- ;;
	xattrs) files | xargs -0 -r setfattr -n user.pb -v 1 ;;
	# +A not +i: an immutable row would also block the cleanup rm
	attrs) files | xargs -0 -r chattr +A ;;
	*) echo "unknown class: $CLASS" >&2; exit 1 ;;
	esac
fi

S=$(date +%s%N)
G=$(nixgen-commit "pb-$CLASS-$N" | sed 's/^committed: //')
E=$(date +%s%N)
COMMIT=$(( (E - S) / 1000000 ))

C=$(mktemp -d /run/pb.XXXXXX)
mount -t tmpfs -o size=50%,mode=0755 pbcow "$C"
mkdir "$C/upper" "$C/work" "$C/root"
mount -t overlay overlay -o \
	"lowerdir=/nixstoredev/nix/store/$G,upperdir=$C/upper,workdir=$C/work,metacopy=on" \
	"$C/root"
M=$C/root/etc/nixgen
# the commit just wrote this tree, so the page cache holds all of it;
# a boot reads it cold off the store disk
sync
echo 3 > /proc/sys/vm/drop_caches
S=$(date +%s%N)
nixgen-restmeta "$C/root" 2> "$ERR"
E=$(date +%s%N)
REST=$(( (E - S) / 1000000 ))

echo "PERMSLINE class=$CLASS n=$N commit_ms=$COMMIT restmeta_ms=$REST" \
	"perms=$(wc -l < "$M/perms") caps=$(wc -l < "$M/caps")" \
	"acls=$(wc -l < "$M/acls") xattrs=$(wc -l < "$M/xattrs")" \
	"attrs=$(wc -l < "$M/attrs") skipped=$(wc -l < "$ERR")"
# fail-soft rows are the interesting failure: show them, capped, since
# one line per row over a serial console is its own kind of slow
head -5 "$ERR"

umount -R "$C"
rmdir "$C"
