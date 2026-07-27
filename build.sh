#!/bin/sh -e
# Stage order matters: each lib resolves the previous via pkg-config.
PREFIX=${PREFIX:-$PWD/build/prefix}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

for lib in libutil libstore; do
	# --libdir: meson guesses lib64 or a multiarch triplet off the
	# distro, and stage two looks in $PREFIX/lib/pkgconfig
	meson setup "build/$lib" "src/$lib" --prefix "$PREFIX" \
		--libdir lib -Dbuildtype=release
	ninja -C "build/$lib" install
done
