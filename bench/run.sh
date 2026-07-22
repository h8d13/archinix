#!/bin/sh -e
# Build + run benchmarks against build/prefix libs.
# bench-optimise: whole-store dedup walk (cold link-creation, warm scan)
# bench-import: the arch/ hot path, capture on vs off (see BASELINE)
# Synthetic + small: quick smoke only. Real-world numbers (full rootfs,
# target media) come from bench/run-real.sh.
# usage: bench/run.sh [npaths] [files-per-path] [content-pool] [warm-loops]
cd "$(dirname "$0")/.."
P=$PWD/build/prefix
export PKG_CONFIG_PATH="$P/lib/pkgconfig"
mkdir -p build/tmp

for b in bench-optimise bench-import; do
	g++ -std=c++23 -O2 "bench/$b.cc" -o "build/tmp/$b" \
		$(pkg-config --cflags --libs nix-store nix-util)
done

# disk-backed, not /tmp: 1M-file runs overflow tmpfs
ROOT=$(mktemp -d "$PWD/build/tmp/bench-root.XXXXXX")
# store paths are made read-only, restore write perm before cleanup
trap 'chmod -R u+w "$ROOT" && rm -rf "$ROOT"' EXIT
# PERF="perf record -g -o file.data" to profile
LD_LIBRARY_PATH=$P/lib $PERF build/tmp/bench-optimise "$ROOT" "${1:-200}" "${2:-500}" "${3:-5000}" "${4:-1}"
echo
LD_LIBRARY_PATH=$P/lib build/tmp/bench-import "$ROOT/import"
