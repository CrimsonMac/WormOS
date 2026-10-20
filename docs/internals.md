# WormOS internals

How the build, the boot and the system tools fit together. Build commands are
in the README.

## Toolchain and packages

- `scripts/build-toolchain.sh` builds the cross toolchain for
  `x86_64-wormos-linux-gnu` into `build/tools`, with target headers and
  libraries in `build/sysroot` (binutils 2.47, gcc 15.3.0, glibc 2.44, Linux
  6.18.57 headers). Finished steps are stamped in `build/stamps/`.
- `scripts/build-kernel.sh` builds Linux 6.18.57 LTS: x86_64_defconfig with
  `kernel/wormos.config` merged on top, out of tree in `build/kernel`, staged
  as the package `build/pkgs/linux`.
- `tools/wbuild NAME` builds `recipes/NAME/recipe` (PKGBUILD-like) into
  `build/pkgs/NAME`, with metadata in `build/pkginfo/NAME.info`. It copies each
  package's `usr/include` and `usr/lib` into the sysroot, so `PACKAGES` in the
  Makefile is in build order. Recipe fields include `release`, `depends`,
  `caps`, `groups` and `sysroot_files`.
- Autotools recipes must pass `--build="$(gcc -dumpmachine)"`; otherwise
  configure runs our cross binaries' host tools against the target libc.
- `tools/wpack` writes `out/repo/NAME-VER-REL.wpkg`: a zstd pax tar with
  `.PKGINFO` first, stripped, root-owned, `go-w`.
- `tools/wrepo` packs everything, writes `out/repo/index` (with a `date`) and
  signs it with `build/host/wsign` (Ed25519, Monocypher). The secret key lives
  outside the repository in `~/.config/wormos/core.key`; the public key is
  `keys/core.pub`.
- All upstream sources were GPG-verified once; their SHA-256 sums are pinned in
  `toolchain/sources` and in the recipes.

## Image

- `scripts/build-rootfs.sh` assembles `build/fsroot/@` by installing the base
  `PACKAGES` with the host build of wh (`build/host/wh --root`) from
  `file://out/repo`, so the image has a complete `/usr/lib/wh/db`. It runs
  under fakeroot with state in `build/fakeroot.state`.
- `scripts/build-initramfs.sh` builds the initramfs with the kernel's
  `gen_init_cpio` (no root needed): wormrd, snap, busybox and the libraries
  they need (found via readelf NEEDED).
- `scripts/mkimage.sh` writes `out/wormos.img` without loop devices: GPT with
  an ESP (GRUB) and a btrfs partition holding `@`, `@var`, `@home`,
  `@snapshots` and `@wormholes`.
  - It needs sudo for one step. mkfs.btrfs walks the tree with nftw, whose
    stat calls fakeroot cannot intercept. So mkimage dumps the owners fakeroot
    recorded (`find -printf` under fakeroot), copies the tree with sudo,
    replays owners and then modes with `scripts/apply-ownership.py` (which
    fails on any path without a record) and runs `sudo mkfs.btrfs` on the copy.

## Boot

- GRUB loads `/@/boot/vmlinuz` and `/@/boot/initramfs.img` with
  `root=UUID=<fs uuid> rootflags=subvol=@,compress=zstd ro`, kept in the
  exported GRUB variable `wormos_cmdline`.
- `src/wormrd/wormrd.c` is the initramfs `/init`. It finds the btrfs by UUID or
  label (reading the superblock itself), mounts the top level at `/top`, runs
  `snap --top /top boot [--rewind ID]`, mounts `@` read-only and switches root
  to worminit. A busybox rescue shell is included.
- `src/worminit/worminit.c` is PID 1. Services are key=value files in
  `/etc/worm/services/` with `type` oneshot, daemon or respawn.
- Subvolumes: `@` (root, read-only), `@var` (`/var`, not in system snapshots),
  `@home`, `@snapshots` (never mounted), `@wormholes`. `/root` is bind-mounted
  from `/var/roothome`. `/var`, `/home` and `/root` come from fstab.

## snap

- `src/snap/` mounts the top level in a private mount namespace. The store
  layout is documented at the top of `store.c`:
  `@snapshots/<id>/{info,system,home,system.manifest,home.manifest}` plus
  `pending`, `activate`, `trial`, `config`, `last-id`, `grub.cfg` and `.lock`.
- Manifests (`manifest.c`) record a SHA-256 and metadata per file; the manifest
  hash goes into `info`. Hashes are reused from the previous snapshot for
  unchanged inodes.
- `menu.c` writes `@snapshots/grub.cfg`, which the ESP grub.cfg sources (the
  "Rewind to a snapshot" submenu, `wormos.rewind=ID`).
- Soft reboot: a rewind writes `pending` and sends SIGHUP to PID 1. worminit
  kills everything, unmounts all but `/proc`, `/sys` and `/dev`, and execs
  `wormrd --soft-reboot`, which runs `snap boot` and `pivot_root`s into the new
  `@`. The kernel keeps running.
- Staging (`stage.c`): `snap stage begin|commit [--try]|abort|keep|status`
  works on `@.next`.

## wh

- `src/wh/`: `wh.c` (commands, transactions), `repo.c` (index, signatures,
  downloads via curl), `db.c` (installed database), `pkg.c` (tar reader using
  openat2 `RESOLVE_IN_ROOT`, capabilities, groups), `sig.c` and `wsign.c`
  (Ed25519). SHA-256 is shared with `src/snap`.
- Transactions: `snap stage begin` → wh mounts `@.next` privately at
  `/run/wh/next` → install → `snap stage commit [--try]` (prints the
  before-snapshot ID) → SIGHUP to PID 1 → `snap boot` renames `@.next` to `@`.
  Downloads and history use the live `/var`. With `--try`, the next boot rolls
  back unless `snap stage keep` was run.

## Wormholes

- `src/wh/foreign.c` and `src/wh/whc.c`. Sources are listed in
  `/etc/wh/sources` (shipped by the wh recipe): Debian trixie (main, updates,
  security) and kali-rolling.
- Setup is a small debootstrap of our own:
  1. Check InRelease with `gpgv --output X.part` and rename after success
     (GnuPG 2.5's gpgv never renames its own `.part` output).
  2. Fetch Packages (xz, then gz, then plain; Kali has no xz) and check it.
  3. Select `Priority: required`, `Essential: yes` and apt, with dependencies.
  4. Check each `.deb`, unpack it with `tar_unpack` after creating the
     merged-/usr links.
  5. Run the dpkg second stage inside via `whc --root --dir --bind`.
- apt inside is pinned to our keyring copy (`signed-by`), recommends are off,
  docs and locales are excluded and `policy-rc.d` exits 101.
- Containers live in subvolumes under `/var/lib/wormholes` (`@wormholes`,
  mounted `nosuid,nodev`, mode 0755). Exported commands are wrappers in
  `/usr/local/bin` (a link to `/var/usrlocal`) and never shadow `/usr/bin` or
  `/usr/sbin`. Records are in `/var/lib/wh/foreign/PKG`. Foreign installs are
  live, with no restart.
- `whc NAME CMD`: for a normal user, a user namespace mapping only their uid
  plus a mount namespace; for root, a mount namespace only. It shares `/dev`,
  `/proc`, `/sys`, `/tmp`, `/home`, `/run`, resolv.conf, hosts, passwd and
  group (user mode) and the host network.

## worm

- `src/worm/`: `worm unlock` / `worm lock` bind-remount `/` (re-applying the
  statvfs flags), list processes with files open for writing (by mnt_id) on
  EBUSY, and run `snap now --auto` after locking.

## Tests

- `make test` (`tests/boot-test.py`) boots a throwaway copy of the image
  (`run-qemu.sh --snapshot`), logs in over serial, runs the checks and powers
  off. It serves `out/repo` over HTTP to the VM at 10.0.2.2. About a minute
  under TCG; a hang means a check is waiting for its 600 s timeout.
- `make test-wormholes` (`tests/wormhole-test.py`) needs internet and takes
  several minutes: it sets up the Debian and Kali wormholes from the real
  archives.
- Faster loop for snap: build natively (`make -C src/snap CC=gcc`) and use
  `--top` on a loopback btrfs image mounted with sudo.
