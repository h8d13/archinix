# The store libs alone, on stock Debian: no Arch, no Nix, no arch/.
#
#   docker build -t nixstore .
#   docker run --rm -it -v "$PWD/work:/work" nixstore
#
# Inside, headers are under /usr/local/include/nix/{util,store} and both
# libs are on the linker path, so a consumer is one line:
#
#   g++ -std=c++23 -O2 mytool.cc -o mytool \
#       $(pkg-config --cflags --libs nix-store nix-util)
#
# See src/README.md for the API. A store is just a directory you own:
# point openStore() at /work and it creates the skeleton on first use.
FROM debian:forky-slim

# exactly what src/*/meson.build asks for: blake3 >= 1.8.2, boost
# >= 1.87 (headers plus the compiled context lib), openssl, sqlite
# >= 3.6.19, and a C++23 compiler
RUN apt-get update && apt-get install -y --no-install-recommends \
	g++ meson ninja-build pkg-config \
	libblake3-dev libboost-dev libboost-context-dev \
	libssl-dev libsqlite3-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /nixstore
COPY .version build.sh ./
COPY nix-meson-build-support ./nix-meson-build-support
COPY src ./src

# meson defaults libdir to the multiarch triplet under /usr, so install
# to /usr/local where it stays plain lib/ and ldconfig finds it
RUN PREFIX=/usr/local ./build.sh \
	&& echo /usr/local/lib > /etc/ld.so.conf.d/nixstore.conf \
	&& ldconfig

WORKDIR /work
CMD ["/bin/bash"]
