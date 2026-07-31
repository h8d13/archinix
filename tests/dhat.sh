#!/bin/sh -e
# Heap profile of the import+optimise path under DHAT: where the bytes
# come from, how long they live, how much of each block is ever read.
# Own lib build because the release one carries no line info, and
# valgrind is ~20x slower, so the tree is small by default.
# Log: build/tmp/dhat.log, full profile: build/tmp/dhat.json
# usage: tests/dhat.sh [nfiles] [file-kb] [content-pool]
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/dhat/prefix
export PKG_CONFIG_PATH="$P/lib/pkgconfig"
mkdir -p build/tmp
LOG=$REPO/build/tmp/dhat.log
: > "$LOG"

log() { echo "\$ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1; }

rm -rf build/dhat
for lib in libutil libstore; do
	log meson setup "build/dhat/$lib" "src/$lib" --prefix "$P" \
		--libdir lib -Dbuildtype=release -Ddebug=true
	log ninja -C "build/dhat/$lib" install
done

log g++ -std=c++23 -O2 -g -fno-omit-frame-pointer \
	bench/bench-import.cc -o build/tmp/bench-import-g \
	$(pkg-config --cflags --libs nix-store nix-util)

ROOT=$(mktemp -d "$REPO/build/tmp/dhat-root.XXXXXX")
trap 'chmod -R u+w "$ROOT" && rm -rf "$ROOT"' EXIT

log env LD_LIBRARY_PATH=$P/lib \
	valgrind --tool=dhat --dhat-out-file="$REPO/build/tmp/dhat.json" \
	build/tmp/bench-import-g "$ROOT" "${1:-400}" "${2:-8}" "${3:-200}"

grep -E "^==[0-9]+== (Total|At t-gmax|At t-end|Reads|Writes)" "$LOG"
echo "full log: $LOG"
echo "per-site: open /usr/lib/valgrind/dh_view.html on build/tmp/dhat.json"
