#!/bin/sh -e
# Driver-heavy commit bench: the shape BASELINE's file-count sweeps
# cannot see. A generation carrying firmware, kernel modules and gpu
# blobs has two orders of magnitude fewer files than arch-base per
# gigabyte, so per-file costs stop mattering and per-byte ones take
# over. Three phases:
#   1. bench-large synthetic sweep: same total bytes, file size swept
#      1 -> 256 MiB. Flat MiB/s means size is not the variable.
#   2. bench-large --tree: the same libutil stages over a real tree,
#      which is the only thing with real content and real proportions.
#   3. trace-import: the stutter itself. import-dir's counter advances
#      per completed file, so timestamping its repaints measures the
#      pipeline stalling rather than just its total. Run under a
#      memory cap (MEMMAX), because on a host with gigabytes of spare
#      page cache the import never hits the dirty limit and the whole
#      effect is invisible -- see BASELINE.
# usage: bench/run-large.sh [tree] [total-mib]   env: COLD, MEMMAX
#        tree default: sole *-arch-base in build/archstore
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/prefix
export PKG_CONFIG_PATH="$P/lib/pkgconfig"
export LD_LIBRARY_PATH="$P/lib"

TREE=$1
[ -n "$TREE" ] || {
	for g in "$REPO"/build/archstore/nix/store/*-arch-base; do
		[ -d "$g" ] || continue
		[ -z "$TREE" ] || { echo "several arch-base trees, pass one explicitly" >&2; exit 1; }
		TREE=$g
	done
}
[ -d "$TREE" ] || { echo "no tree to bench (run arch/bootstrap.sh or pass one)" >&2; exit 1; }
TREE=$(realpath "$TREE")
TOTAL=${2:-1024}

mkdir -p build/tmp
WORK=$(mktemp -d "$REPO/build/tmp/benchlarge.XXXXXX")
trap 'find "$WORK" -type d -exec chmod u+w {} + ; rm -rf "$WORK"' EXIT

for b in bench-large bench-commit; do
	g++ -std=c++23 -O2 "bench/$b.cc" -o "build/tmp/$b" \
		$(pkg-config --cflags --libs nix-store nix-util)
done
[ -x build/tmp/import-dir ] || g++ -std=c++23 -O2 arch/import-dir.cc \
	-o build/tmp/import-dir $(pkg-config --cflags --libs nix-store nix-util)

echo "media: $(df --output=source,fstype "$REPO/build/tmp" | tail -1)"
echo "ram:   $(free -g | awk '/^Mem:/{print $2 " GiB"}')"
echo "tree:  $TREE ($(du -sh "$TREE" | cut -f1), $(find "$TREE" | wc -l) entries)"

echo
echo "=== libutil byte path, file size swept at constant total"
build/tmp/bench-large "$WORK/sweep" "$TOTAL"

echo
echo "=== libutil byte path, real tree"
build/tmp/bench-large "$WORK/real" --tree "$TREE"

echo
echo "=== commit phases, import and writeback separated (SYNC=1)"
rm -rf "$WORK/store"
SYNC=1 build/tmp/bench-commit "$WORK/store" gen "$TREE"

echo
echo "=== import stutter, unconstrained vs memory-capped"
for CAP in none ${MEMMAX:-2G}; do
	rm -rf "$WORK/store2"; sync
	echo "--- MEMMAX=$CAP"
	if [ "$CAP" = none ]; then
		python3 bench/trace-import.py "$WORK/store2" gen "$TREE"
	else
		MEMMAX=$CAP python3 bench/trace-import.py "$WORK/store2" gen "$TREE"
	fi
done
