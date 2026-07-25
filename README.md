# archinix

## [Nixstore](src/)

Local-store-only extraction of the Nix store layer (libnixutil,
libnixstore) from [NixOS/nix](https://github.com/NixOS/nix)
2.36.0 (`40f375fa`), buildable on any Linux without Nix: `./build.sh`.

> [!NOTE]
> Remote stores (s3/http/ssh/daemon), the `.drv` realisation machinery (arguably, what an overlay already is; in kernel)
> are cut: stores hold imported trees only; and the shell glue for retrieval/integration with userland.
> Also removes any support for other platforms than `unix` and `linux`, from build targets.

Build depends on: `meson`, `ninja`, C++23 compiler:

Arch package names (`boost` is headers only; the compiled `context`
and `iostreams` libs live in `boost-libs`.)

```
pacman -S --needed meson ninja gcc pkgconf boost boost-libs openssl \
	libblake3 nlohmann-json sqlite
```

API reference:

C++ headers install to `include/nix/{util,store}/` ([internal API docs](https://hydra.nixos.org/job/nix/master/internal-api-docs/latest/download-by-type/doc/internal-api-docs)).

---

## [Arch Linux generations](arch/)

Immutable `x86-efi` generations on the Nixstore.

### From releases: [ISO](https://github.com/h8d13/archinix/releases)

**GRUB only**. Store filesystem is `ext4` by default, with `btrfs`, `xfs` and `f2fs`

> Sourced here: ([`arch/nixgen/nixgen-fs`](arch/nixgen/nixgen-fs))

`nixgen-setup /dev/disk --fs xfs` installs current running generation to a hard disk.

Optional: pass `--data 100G` for a separate `/home` persistent part. 

And `--user myuser` to create a user's home directly.

> [!IMPORTANT]
> The ISO is read-only, and so is a running system: writes land in a
> `tmpfs` overlay upper (capped at 75% of RAM, allocated only as
> written) and vanish on reboot unless you `nixgen-commit mychange`.

On a box with limited RAM; for large downloads you'll need to use:

`nixgen-update kde "pacman -S plasma"` if no cmd is provided `pacman -Syu --noconfirm` is the default.
This builds it with the overlay upper on the store disk. You can also setup swap on [zram](https://wiki.archlinux.org/title/Zram#Using_zram-generator) for this purpose.

Then, in the box: `nixgen-{commit,update,switch,remove,listid,diffid,setup}`;

`nixgen-help` is the full [reference](https://github.com/h8d13/archinix/blob/main/arch/nixgen/nixgen-help).

---

## [Example scripts](https://github.com/h8d13/nixarch.cfg/)

This aims to document/illustrate examples of configurations that can shape a system.
