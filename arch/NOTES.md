## Gotchas / Notes

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
- **A store root is two directories.** `<root>/nix/store` holds the
  paths and `<root>/nix/var/nix/db` holds the registration db, which is
  the source of truth for what is valid: a directory listing also shows
  the `.links` dedup farm and half-written `tmp-*` imports, so ask
  `store-paths` rather than globbing. `rootDir` moves both (`/` on the
  host build, `/nixstoredev` in the box), but only the physical half:
  GRUB entries and the initramfs spell out the logical `/nix/store`, so
  that one cannot move.
- **Import canonicalises permissions** (dirs 0755, files 0444/0555,
  root-owned, no xattrs: NAR keeps only the executable bit; 0755 dirs
  keep the manifest to true deviations instead of every dir in the
  tree, one boot-time copy-up per row). `nixgen-savemeta` captures
  what that strips (modes, ownership incl. symlinks, capabilities,
  POSIX ACLs, user/trusted xattrs, chattr flags) into
  `etc/nixgen/{perms,caps,acls,xattrs,attrs}`; `nixgen-restmeta`
  replays it at boot (nixgen-perms.service) and inside every build
  sandbox. A base imported without the manifest breaks the chain:
  special bits and ownership are gone and sub-444 secrets (shadow)
  turn world-readable on the booted view. generation.sh warns when
  the base lacks it;
  re-bootstrap to fix. Plain 644-vs-444 file modes stay canonical on
  purpose (root bypasses them; restoring would copy-up every file),
  except /etc (captured whole, minus etc/nixgen: it is where humans
  `cp` defaults from, and a canonical-444 source mints readonly
  copies; subsumes the old /etc/skel rule useradd depends on) and
  /root (the operator's own files must not come back readonly).
  tests/meta-test.sh pins both. The boot/switch/update overlays mount
  `metacopy=on` so the /etc replay copies up metadata only; userns
  builds (generation.sh, enter.sh) keep full copy-up, the kernel
  rejects metacopy with userxattr. Bases predating the /etc rule
  replay 444 there; re-bootstrap to fix (meta-test refuses them).
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
- **space saving** out of the arch tarball (~500mb) the system
  already saves about ~24mb. Mostly duplicated `.mo` files (fixed
  upstream) and license files or similar. This was found on first day
  of making this project at first store import.
- **what happens if AI is tasked with un-slopping** instead of
  creating slop? Idea was that atomic FS are fascinating and the
  "larger" part of the `Eelco Dolstra`
  [thesis](https://edolstra.github.io/pubs/phd-thesis.pdf), yet much
  code got intertwined to bolt into FS semantics, without ever asking
  if it can still be standalone... SoC, and OoO? The idea was that any
  surface covered is potential failures. And small glue instead,
  enables understanding + optimizing complex parts to liking.
- **Flow:** one agent drives VMs over `serial QEMU` and the other **is
  cutting down cb**. Instead of being adversarial for everything (and
  hallucinating "problems/solutions"), catches issues real-time as it
  adapts cb, this creates a self-virtuous loop. + smaller end-result.
  In 7 days this resulted in +4k lines (2k of which docs/comms) and -60k
  lines from original. I also tried to make "code as docs/comments".
- **cutting into `libstore` and `libutil`** What served only those two
  goes with them: path signing (content is re-hashed against the db
  instead), JSON output, the terminal width tables, subprocess
  spawning, and the garbage collector's daemon half. GC no longer
  guesses which paths are live by walking `/proc` and shelling out to
  `lsof`, and has no roots socket, temp-root protocol or auto-GC:
  roots are explicit, `import-dir`/`import-path` register one per path
  and `rm-path` drops it before deleting.
- **the store takes a path and nothing else.** The `Setting<T>` /
  `Config` framework went with nix.conf: no config file, no `NIX_*`
  discovery, no URI query parameters, no settings singleton. What
  survived became plain fields with defaults, held per-store rather
  than globally, so changing one is an edit and a recompile. arch/ is
  the operator surface; a knob nothing can reach is not a knob.
- **references are refused at the door.** A generation is an
  independent, self-contained tree: `import-dir` creates them with
  none, and sharing between them is the `.links` hardlink farm, which
  works on content below the store-path level. `import-path` now
  rejects a stream that claims references, so the invariant is
  enforced rather than assumed, and the closure walk that existed for
  a case that never occurs is gone. The `Refs` table and its `on
  delete restrict` FK stay as backstop: bypass the door and a delete
  fails loudly instead of losing data.
- **store layout stays in the library.** `store-paths` (list) and
  `store-resolve` (name / hash-prefix / full basename -> basename) ask
  the db, which is the only thing that knows what is registered;
  `nixgen-remove`, `-switch` and `-diffid` used to glob the store dir
  and would match the `.links` farm or a half-written `tmp-*` import
  as readily as a generation. `nixgen-verify` closes the other half:
  content addressing is the premise and the store is the root
  filesystem, so `--content` re-hashes every path and link against the
  db before a boot finds the corruption for you.
