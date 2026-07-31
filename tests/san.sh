#!/bin/sh -e
# Concurrent tests under ThreadSanitizer, then ASan+UBSan. Libs and
# tests both instrumented. Log: build/tmp/san.log
# usage: tests/san.sh
cd "$(dirname "$0")/.."
REPO=$PWD
mkdir -p build/tmp
LOG=$REPO/build/tmp/san.log
: > "$LOG"

log() { echo "\$ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1; }

for SAN in thread address,undefined; do
	TAG=$(echo "$SAN" | tr ',' '-')
	P=$REPO/build/san-$TAG/prefix
	rm -rf "build/san-$TAG"
	export PKG_CONFIG_PATH=$P/lib/pkgconfig
	echo "== $SAN"
	for lib in libutil libstore; do
		log meson setup "build/san-$TAG/$lib" "src/$lib" \
			--prefix "$P" --libdir lib -Dbuildtype=debugoptimized \
			-Db_sanitize="$SAN" -Db_lundef=false
		log ninja -C "build/san-$TAG/$lib" install
	done
	for t in parallel-optimise import-hashes; do
		log g++ -std=c++23 -O1 -g -fsanitize="$SAN" \
			-fno-omit-frame-pointer "tests/$t.cc" \
			-o "build/tmp/$t-$TAG" \
			$(pkg-config --cflags --libs nix-store nix-util)
		ROOT=$(mktemp -d "$REPO/build/tmp/san-root.XXXXXX")
		echo "# $t"
		log env LD_LIBRARY_PATH=$P/lib \
			"build/tmp/$t-$TAG" "$ROOT"
		chmod -R u+w "$ROOT" && rm -rf "$ROOT"
	done
done

if grep -E "SUMMARY: .*Sanitizer|runtime error:" "$LOG"; then
	echo "findings above (full log: $LOG)"
	exit 1
fi
echo "ok: no sanitizer findings"
