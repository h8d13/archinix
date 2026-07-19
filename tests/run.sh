#!/bin/sh -e
# Build + run regression tests against build/prefix libs. TAP output.
cd "$(dirname "$0")/.."
P=$PWD/build/prefix
export PKG_CONFIG_PATH="$P/lib/pkgconfig"

for t in parallel-optimise import-hashes nar-parse; do
	g++ -std=c++23 -O2 "tests/$t.cc" -o "build/$t" \
		$(pkg-config --cflags --libs nix-store nix-util)
done
for t in import-dir rm-path export-path import-path; do
	g++ -std=c++23 -O2 "arch/$t.cc" -o "build/$t" \
		$(pkg-config --cflags --libs nix-store nix-util)
done

FAIL=0
run() {	# run <name> <command...>: fresh root, restore perms, cleanup
	echo "# $1"; shift
	ROOT=$(mktemp -d "$PWD/build/test-root.XXXXXX")
	# store paths are made read-only, restore write perm before cleanup
	LD_LIBRARY_PATH=$P/lib "$@" "$ROOT" || FAIL=1
	chmod -R u+w "$ROOT" && rm -rf "$ROOT"
}

run needed-drift sh -e tests/needed-drift.sh
run parallel-optimise build/parallel-optimise
run import-hashes build/import-hashes
run nar-parse build/nar-parse
run import-select sh -e tests/import-select.sh
run gc-links sh -e tests/gc-links.sh
run export-roundtrip sh -e tests/export-roundtrip.sh

exit $FAIL
