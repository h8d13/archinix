# The store libs alone, on stock Debian.
#
#   docker build -t nixstore .
#   docker run --rm -it -v "$PWD/work:/work" nixstore
#
# Inside, headers are under /usr/local/include/nix/{util,store} and both
# a consumer is one line:
#
#   g++ -std=c++23 -O2 mytool.cc -o mytool \
#       $(pkg-config --cflags --libs nix-store nix-util)
#
# See src/README.md for example API. A store is just a directory you own:
# point openStore() at /work and it creates the skeleton on first use.

FROM debian:forky-slim

# exactly what src/*/meson.build asks for.

RUN apt-get update && apt-get install -y --no-install-recommends \
	g++ meson ninja-build pkg-config \
	libboost-dev \
	libssl-dev libsqlite3-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /nixstore
COPY .version build.sh ./
COPY nix-meson-build-support ./nix-meson-build-support
COPY src ./src

RUN PREFIX=/usr/local ./build.sh && ldconfig

WORKDIR /work
CMD ["/bin/bash"]
# do a backflip !
