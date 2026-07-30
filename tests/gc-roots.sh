#!/bin/sh -e
# GC roots are what keeps a live generation alive, so the collector's
# whole liveness answer is "is this path rooted". Pinned here because
# that is now the only question it asks: references cannot exist, so
# there is no closure to fall back on if the root check is wrong.
# import-dir/import-path root what they land, rm-path drops its own
# root and deletes, and a root it does not own refuses the deletion
# instead of taking it. TAP.
# usage: tests/gc-roots.sh <tmpdir>
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/prefix

[ -n "$1" ] || { echo "usage: $0 <tmpdir>" >&2; exit 1; }
mkdir -p "$1"
ROOT=$(realpath "$1")

for t in import-dir rm-path store-paths; do
	[ -x "build/$t" ] || g++ -std=c++23 -O2 "arch/$t.cc" -o "build/$t" \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
done

N=0 FAIL=0
ok() {	# ok <cond-exit-status> <desc>
	N=$((N + 1))
	if [ "$1" = 0 ]; then echo "ok $N - $2"
	else echo "not ok $N - $2"; FAIL=1; fi
}

mkdir -p "$ROOT/tree"
echo content > "$ROOT/tree/file"

RUN="env LD_LIBRARY_PATH=$P/lib"
STORE=$ROOT/store
GCROOTS=$STORE/nix/var/nix/gcroots

PA=$($RUN build/import-dir "$STORE" gen "$ROOT/tree" 2> "$ROOT/log-import")
BA=$(basename "$PA")

[ -L "$GCROOTS/$BA" ]; ok $? "import-dir roots the path it imported"
[ "$(readlink "$GCROOTS/$BA")" = "/nix/store/$BA" ]
ok $? "the root points at the logical store path"

# a root rm-path does not own: same path, another name. rm-path drops
# only the link named after the path, so this one still holds it and
# the collector has to refuse rather than delete out from under it
ln -s "/nix/store/$BA" "$GCROOTS/pinned-elsewhere"
if $RUN build/rm-path "$STORE" "$BA" > "$ROOT/log-rm1" 2>&1; then
	s=1; else s=0; fi
ok $s "rm-path refuses a path held by a root it does not own"
grep -q "garbage collector root" "$ROOT/log-rm1"
ok $? "the refusal says the path is still rooted"
[ -d "$PA" ]; ok $? "refused path is still on disk"
$RUN build/store-paths "$STORE" | grep -q "^$BA"
ok $? "refused path is still registered"

# rm-path drops its own root on the way in, so the earlier refusal
# must not have left the store unroot-able: put the store back the way
# import-dir left it and the same command now succeeds
ln -sf "/nix/store/$BA" "$GCROOTS/$BA"
rm "$GCROOTS/pinned-elsewhere"
$RUN build/rm-path "$STORE" "$BA" > "$ROOT/log-rm2" 2>&1
ok $? "deletes once the foreign root is gone"
[ ! -e "$PA" ]; ok $? "path gone from disk"
[ ! -e "$GCROOTS/$BA" ]; ok $? "rm-path dropped its own root"
[ -z "$($RUN build/store-paths "$STORE")" ]
ok $? "nothing left registered"

echo "1..$N"
exit $FAIL
