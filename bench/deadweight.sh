#!/bin/sh -e
# What the linker throws away. Builds both libs static with one section
# per function/datum, links every consumer in the tree against them with
# --gc-sections, and records what --print-gc-sections discarded. A
# section removed from EVERY consumer link is reachable from nothing
# this fork ships: that is dead weight, on the linker's authority rather
# than a script's guess about which relocation belongs to whom.
#
# Consumers are the roots: arch/ tools (what a box runs), tests/, bench/.
# Anything only an out-of-tree user could call counts as dead here, which
# is the intent -- the libs exist for arch/.
#
# Output: build/tmp/deadweight.txt, symbols with sizes, grouped by the
# source file that defines them. bench/deadweight.py does the joining.
# usage: bench/deadweight.sh
cd "$(dirname "$0")/.."
REPO=$PWD
P=$REPO/build/lean-prefix
rm -rf build/lean-util build/lean-store "$P"
mkdir -p build/tmp
# stage two resolves stage one through pkg-config, at setup as well as
# at link time
export PKG_CONFIG_PATH="$P/lib/pkgconfig"


# -ffunction-sections/-fdata-sections: without them the linker's unit of
# removal is the whole .text of a translation unit, and nothing gets
# removed. prelink stays off: it merges every object into one, which
# loses the file each section came from.
for lib in util store; do
	meson setup "build/lean-$lib" "src/lib$lib" \
		--prefix "$P" --libdir lib -Dbuildtype=release \
		-Ddefault_library=static -Dprefer_static=true \
		-Dcpp_args='-ffunction-sections -fdata-sections' \
		> "build/tmp/lean-setup-$lib.log"
	ninja -C "build/lean-$lib" install \
		> "build/tmp/lean-build-$lib.log"
done

CFLAGS=$(pkg-config --cflags nix-store nix-util)
LIBS=$(pkg-config --static --libs nix-store nix-util)

: > build/tmp/deadweight-raw.txt
for src in arch/*.cc tests/*.cc bench/*.cc; do
	name=$(basename "$src" .cc)
	# -u forces the archive members in even when nothing references
	# them yet; gc-sections then decides per section instead of per
	# member, which is the resolution we want
	g++ -std=c++23 -O2 -ffunction-sections -fdata-sections "$src" \
		-o "build/tmp/lean-$name" $CFLAGS \
		-Wl,--gc-sections -Wl,--print-gc-sections \
		-Wl,--whole-archive "$P/lib/libnixstore.a" "$P/lib/libnixutil.a" \
		-Wl,--no-whole-archive $LIBS \
		2> "build/tmp/gc-$name.txt" || {
		# a consumer that no longer compiles is not a root, and
		# saying so matters: its references would otherwise look
		# like dead weight
		echo "SKIPPED (does not build): $src" >&2
		grep -v "removing unused section" "build/tmp/gc-$name.txt" \
			| head -3 >&2
		continue
	}
	sed -n "s/^.*removing unused section '\([^']*\)' in file '\([^']*\)'.*/\1 \2/p" \
		"build/tmp/gc-$name.txt" | sed "s|^|$name |" \
		>> build/tmp/deadweight-raw.txt
done

python3 bench/deadweight.py "$P" build/tmp/deadweight-raw.txt \
	| tee build/tmp/deadweight.txt
