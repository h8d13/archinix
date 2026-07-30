#!/bin/sh -e
# Compiler checks ./build.sh does not make: clang enforces
# -Werror=weak-vtables (GCC drops it), LTO gives -Wsuggest-final-* whole
# -program visibility, clang finds dead members, clang-tidy the rest.
# Log: build/tmp/lint.log
# usage: tests/lint.sh
cd "$(dirname "$0")/.."
REPO=$PWD
mkdir -p build/tmp
LOG=$REPO/build/tmp/lint.log
: > "$LOG"

log() { echo "\$ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1; }

build() { # build <tag> <cxx> <cxxflags> [meson args...]
	TAG=$1 CXX=$2 CXXFLAGS=$3
	shift 3
	P=$REPO/build/lint-$TAG/prefix
	rm -rf "build/lint-$TAG"
	export PKG_CONFIG_PATH=$P/lib/pkgconfig CXX CXXFLAGS
	for lib in libutil libstore; do
		log meson setup "build/lint-$TAG/$lib" "src/$lib" \
			--prefix "$P" --libdir lib -Dbuildtype=release "$@"
		log ninja -C "build/lint-$TAG/$lib" install
	done
}

echo "== clang, project flags"
build clang clang++ ""

echo "== gcc + LTO, devirtualisation"
build lto g++ \
	"-Wsuggest-final-types -Wsuggest-final-methods -Wsuggest-override" \
	-Db_lto=true

echo "== clang, dead members"
build dead clang++ \
	"-Wunused-member-function -Wunused-private-field -Wunused-template"

# StackAddressEscape off: fires inside boost's concurrent_table, not ours
echo "== clang-tidy"
for lib in libutil libstore; do
	log clang-tidy --quiet -p "build/lint-clang/$lib" \
		-checks=-*,performance-unnecessary-value-param,performance-move-const-arg,performance-unnecessary-copy-initialization,performance-for-range-copy,bugprone-use-after-move,clang-analyzer-*,-clang-analyzer-core.StackAddressEscape \
		$(ls src/$lib/*.cc)
done

# the clang pass fails the build on its own (-Werror); the rest report
# through warnings, so they are advisory
if grep -E "warning:" "$LOG"; then
	echo "advisory findings above (full log: $LOG)"
fi
