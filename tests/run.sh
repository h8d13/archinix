#!/bin/sh -e
# Build + run regression tests against build/prefix libs. TAP output.
cd "$(dirname "$0")/.."
P=$PWD/build/prefix
export PKG_CONFIG_PATH="$P/lib/pkgconfig"

g++ -std=c++23 -O2 tests/parallel-optimise.cc -o build/parallel-optimise \
	$(pkg-config --cflags --libs nix-store nix-util)

ROOT=$(mktemp -d "$PWD/build/test-root.XXXXXX")
trap 'chmod -R u+w "$ROOT" && rm -rf "$ROOT"' EXIT
LD_LIBRARY_PATH=$P/lib build/parallel-optimise "$ROOT"
