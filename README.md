# nixstore

Nix store layer (libnixutil, libnixstore + C API) extracted from [NixOS/nix](https://github.com/NixOS/nix) 2.36.0 (`40f375fa`), buildable on any Linux without Nix: `./build.sh`.

Build depends on: `meson`, `ninja`, C++23 compiler:

Depends:

`boost`, `openssl`, `libsodium`, `libarchive`, `brotli`,
`zstd`, `blake3`, `nlohmann-json`, `sqlite`, `curl`, `libseccomp`

Arch package names (`boost` is headers only; the compiled
`context`/`coroutine`/`iostreams`/`url` libs live in `boost-libs`):

```
pacman -S --needed meson ninja gcc pkgconf boost boost-libs openssl libsodium \
	libarchive brotli zstd libblake3 nlohmann-json sqlite curl libseccomp
```

API reference:

C++ headers install to `include/nix/{util,store}/` ([internal API docs](https://hydra.nixos.org/job/nix/master/internal-api-docs/latest/download-by-type/doc/internal-api-docs))

C API in `nix_api_store.h` ([external API docs](https://hydra.nixos.org/job/nix/master/external-api-docs/latest/download-by-type/doc/external-api-docs)).

---

## arch/: Arch Linux generations ([arch/README.md](arch/README.md))

Immutable Arch generations on the store: updates build the next
generation offline, rollback is booting an older GRUB entry.

```
./build.sh                                       # store libs into build/prefix
arch/bootstrap.sh build/archstore                # base generation (prints <base>)
arch/iso/mkiso.sh build/archstore <base>         # bootable ISO
arch/iso/uefi-vm.sh iso                          # try it in QEMU (UEFI)
arch/iso/flashdisk.sh build/archstore /dev/sdX   # flash to hardware
```

In the box: `nixgen-{commit,update,switch,remove,listid,diffid,setup}`;
`nixgen-help` is the full reference.
