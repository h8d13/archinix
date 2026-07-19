#!/bin/sh -e
# Stage order matters: each lib resolves the previous via pkg-config.
PREFIX=${PREFIX:-$PWD/build/prefix}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

for lib in libutil libstore; do
	meson setup "build/$lib" "src/$lib" --prefix "$PREFIX" \
		-Dbuildtype=release
	ninja -C "build/$lib" install
done
