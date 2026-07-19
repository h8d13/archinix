#!/bin/sh -e
# Ship-a-generation round trip: import-dir -> export-path -> bundle ->
# import-path into a second store. Pins: content identity across
# stores (same CA basename, byte-identical tree, exec bit), re-import
# idempotence, dedup on the receiving side, and rejection of a
# content-corrupted bundle (CA recompute) and of a truncated one. TAP.
# usage: tests/export-roundtrip.sh <tmpdir>
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/prefix

[ -n "$1" ] || { echo "usage: $0 <tmpdir>" >&2; exit 1; }
mkdir -p "$1"
ROOT=$(realpath "$1")

for t in import-dir export-path import-path; do
	[ -x "build/$t" ] || g++ -std=c++23 -O2 "arch/$t.cc" -o "build/$t" \
		$(PKG_CONFIG_PATH=$P/lib/pkgconfig pkg-config --cflags --libs nix-store nix-util)
done

N=0 FAIL=0
ok() {	# ok <cond-exit-status> <desc>
	N=$((N + 1))
	if [ "$1" = 0 ]; then echo "ok $N - $2"
	else echo "not ok $N - $2"; FAIL=1; fi
}

# tree with a subdir, an executable, and duplicate content. Payload is
# uppercase: NAR framing tags and nix32 hashes are lowercase, so a
# targeted uppercase byte-flip below corrupts content only
mkdir -p "$ROOT/src/sub"
echo "PAYLOAD one" > "$ROOT/src/f1"
echo "PAYLOAD one" > "$ROOT/src/sub/f1-dup"
printf '#!/bin/sh\necho hi\n' > "$ROOT/src/tool"
chmod +x "$ROOT/src/tool"

RUN="env LD_LIBRARY_PATH=$P/lib"
P1=$($RUN build/import-dir "$ROOT/a" gen "$ROOT/src" 2> "$ROOT/log-import")

$RUN build/export-path "$ROOT/a" "$(basename "$P1")" \
	> "$ROOT/bundle" 2> "$ROOT/log-export"
[ -s "$ROOT/bundle" ]; ok $? "export produced a bundle"

P2=$($RUN build/import-path "$ROOT/b" < "$ROOT/bundle" 2> "$ROOT/log-b")
[ -d "$P2" ]; ok $? "import into second store"
[ "$(basename "$P1")" = "$(basename "$P2")" ]; ok $? "same CA basename on both stores"
diff -r "$P1" "$P2"; ok $? "trees byte-identical"
[ -x "$P2/tool" ]; ok $? "exec bit survived the wire"

# receiving side dedups: duplicate content on one inode
[ "$(stat -c %i "$P2/f1")" = "$(stat -c %i "$P2/sub/f1-dup")" ]
ok $? "receiver deduplicated duplicate content"

# idempotent: same bundle again lands on the same path
P3=$($RUN build/import-path "$ROOT/b" < "$ROOT/bundle" 2> "$ROOT/log-b2")
[ "$P3" = "$P2" ]; ok $? "re-import is idempotent"

# content corruption: flip payload bytes only (uppercase, see above);
# the NAR still parses, the CA recompute must refuse before
# registration
tr 'P' 'Q' < "$ROOT/bundle" > "$ROOT/bundle-corrupt"
if $RUN build/import-path "$ROOT/c" < "$ROOT/bundle-corrupt" \
		> "$ROOT/log-c" 2>&1; then s=1; else s=0; fi
ok $s "corrupted bundle rejected"
grep -q "integrity check failed" "$ROOT/log-c"
ok $? "rejection names the integrity check"
[ ! -d "$ROOT/c/nix/store/$(basename "$P1")" ]
ok $? "nothing registered from the corrupt bundle"

# truncation: cut the stream mid-NAR
head -c 512 "$ROOT/bundle" > "$ROOT/bundle-trunc"
if $RUN build/import-path "$ROOT/d" < "$ROOT/bundle-trunc" \
		> "$ROOT/log-d" 2>&1; then s=1; else s=0; fi
ok $s "truncated bundle rejected"

# ship-lane boot glue: an imported generation is store-registered but
# GRUB-invisible until nixgen-adopt appends its menuentry. Pin the
# refusals (no kernel, duplicate) and the stanza itself; the shape must
# stay prunable by nixgen-remove's awk (menuentry line through '}')
ADOPT="sh arch/nixgen/nixgen-adopt"
if $ADOPT "$(basename "$P2")" "$ROOT/b" > "$ROOT/log-adopt" 2>&1; then
	s=1; else s=0; fi
ok $s "adopt refuses generation without kernel"
[ ! -f "$ROOT/b/entries.cfg" ]; ok $? "no entry written on refusal"

mkdir -p "$ROOT/bootsrc/boot"
echo k > "$ROOT/bootsrc/boot/vmlinuz-linux"
echo i > "$ROOT/bootsrc/boot/initramfs-linux.img"
PB=$($RUN build/import-dir "$ROOT/b" bootgen "$ROOT/bootsrc" \
	2> "$ROOT/log-bootgen")
$ADOPT "$(basename "$PB")" "$ROOT/b" > "$ROOT/log-adopt2" 2>&1
ok $? "adopt enters a bootable generation"
grep -q "nixgen=$(basename "$PB") " "$ROOT/b/entries.cfg"
ok $? "entries.cfg carries the menuentry"
if $ADOPT "$(basename "$PB")" "$ROOT/b" > "$ROOT/log-adopt3" 2>&1; then
	s=1; else s=0; fi
ok $s "duplicate adopt refused"

echo "1..$N"
exit $FAIL
