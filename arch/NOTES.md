## Gotchas

- **Reruns of mkiso.sh reuse the nixarch generations** and only
  reassemble the ISO. `REBUILD=1` discards them first; required after
  changing setup-boot.sh or any `arch/iso/initcpio-*` file. After any ISO
  rebuild, restart QEMU: a live VM's GRUB menu points at pre-rebuild
  hashes.
- **Boot entries carry `rd.systemd.gpt_auto=0`.** Root comes from
  `nixgen=`, so systemd-gpt-auto-generator must not go hunting for a
  root partition and race the generated `sysroot.mount`. Entries written
  by commit/update/adopt inherit it from `/proc/cmdline`; hand-written
  ones need it too.
- **Uncommitted writes live in RAM and vanish**: overlay upper is a
  tmpfs (75% of RAM), no swap. A big enough pacman transaction (~1 GiB
  of downloads+extract) dies with `Write failed` in a 2G box. Big
  installs: `nixgen-update` (upper on the store disk), more `-m`, or
  commit + reboot between chunks (the upper resets).
- **State is three categories, not two.** Committed content rolls back
  with the generation; the RAM upper vanishes; paths listed in
  `/etc/nixgen/state` (default `/home`, `/var/log`) can ride a data
  partition (`nixgen-data`, then `nixdata=NIXDATA` on the entry) and
  flow *forward* across generations. Bulk mutable data (a Steam
  library, a database) belongs there: through the upper it eats RAM,
  through commit it branches with the config tree. `/var/lib/pacman`
  is deliberately not listed: the package db describes the static tree
  and must roll back with it.
- **Import canonicalises permissions** (dirs 0755, files 0444/0555,
  root-owned, no xattrs: NAR keeps only the executable bit; 0755 dirs
  keep the manifest to true deviations instead of every dir in the
  tree, one boot-time copy-up per row). `nixgen-savemeta` captures
  what that strips (modes, ownership incl. symlinks, capabilities,
  POSIX ACLs) into `etc/nixgen/{perms,caps,acls}`; `nixgen-restmeta`
  replays it at boot (nixgen-perms.service) and inside every build
  sandbox. A base imported without the manifest breaks the chain:
  special bits and ownership are gone and sub-444 secrets (shadow)
  turn world-readable on the booted view. generation.sh warns when
  the base lacks it;
  re-bootstrap to fix. Plain 644-vs-444 file modes stay canonical on
  purpose (root bypasses them; restoring would copy-up every file),
  except /etc/skel (useradd copies its modes to new users) and /root
  (the operator's own files must not come back readonly), both
  captured whole (tests/meta-test.sh pins this).
- **Diskless BIOS boots pay ~10s** of GRUB probing for the absent
  NIXSTORE label. Known cost, attached-disk boots don't pay it.
- **USB store disks enumerate late.** `udevadm settle` doesn't wait
  for undiscovered hardware, so disk-only boots on usb lost a ~5s
  race and fell into the ISO hunt (`wrong fs type` spam, no
  recovery). Disk-boot GRUB entries carry `nixsource=disk`, which makes
  `nixgen-store.service` `udevadm wait` (up to 30s) for the store disk
  instead: it returns the moment udev has the device, and covers
  hardware that has not been discovered yet.
  Virtio enumerates instantly: a VM PASS does not cover this path.
- **update-test.sh pins a dated Arch Archive snapshot** to prove a real
  kernel version change; archive use lives in the test only, stock
  generations track live mirrors.
