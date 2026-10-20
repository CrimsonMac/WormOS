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
  as the package `build/pkgs/linux`. The build warns about any option in the
  fragment that did not survive dependency resolution.
- Kernel security: nftables only (built in, no iptables or xtables), lockdown
  forced to integrity mode, LSMs `lockdown,yama,integrity,landlock` (AppArmor
  is built but only enabled with `lsm=`), and modules must be signed with the
  key generated in `build/kernel/certs`, which is never packaged. KVM, vhost
  and vsock serve later microVM blackholes; virtio-mmio and virtio-fs let the
  same kernel run inside one. `/dev/mem`, `/proc/kcore`, kexec and
  hibernation are compiled out. The stack-erase GCC plugin needs the host's
  GMP and MPC headers.
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
  plus a mount namespace; for root, a mount namespace only (helpers in
  `src/common/ns.c`). It shares `/dev`, `/proc`, `/sys`, `/tmp`, `/home`,
  `/run`, resolv.conf, hosts, passwd and group (user mode) and the host
  network.

## Blackholes

- `src/bh/`: `bh.c` (commands, state), `box.c` (namespaces, init),
  `sandbox.c` (Landlock, seccomp). `src/common/ns.c` holds the namespace
  helpers shared with `whc` (ID maps, binds that keep locked mount flags,
  pivot_root).
- State: `/var/lib/blackholes` is sticky and world-writable like `/tmp`;
  each user has `UID/` (0700, 0711 for root, refused unless owned by the
  caller) holding `NAME/` with `lock` (flocked by the running bh), `pid`,
  `info` (outside ID, creation time), `rootfs/` and `inbox/`.
- IDs: root inside maps to one ID outside: the caller's own for a user,
  `0x40000000 + n` (allocated per blackhole, under a lock on `UID/`) for
  root. Root's supplementary groups are dropped first.
- Start: `clone` with new user, mount, PID, network, IPC and UTS
  namespaces; the parent writes the ID maps and, for root, moves the child
  into `/sys/fs/cgroup/blackholes/NAME` (pids.max 1024, memory.max half of
  RAM), then releases it through a pipe. The child becomes root inside,
  re-arms `PR_SET_PDEATHSIG` (credential changes clear it), binds
  `rootfs` onto itself, binds the host's `/usr` and `/etc` read-only, mounts
  `/proc`, a tmpfs `/dev` with nodes bound from the host, a devpts instance
  and `/dev/shm`, binds the inbox, pivots, unshares a cgroup namespace,
  brings up `lo`, applies the sandbox, closes all descriptors above 2 and
  forks the program with a clean environment. It stays as init and exits
  with the program, which kills the rest of the PID namespace.
- Why no overlay of `/`: overlayfs clones its lower layer, and the kernel
  refuses to clone `/` from a user namespace because its submounts are
  locked. `/usr` and `/etc` have no submounts and can be bound alone. Host
  files are owned by unmapped IDs, so an overlay would not make them
  writable anyway.
- Sandbox: `PR_SET_NO_NEW_PRIVS`; a Landlock ruleset handling every file
  system right of the running ABI with one rule, everything beneath the new
  `/`; a seccomp filter that kills non-x86_64 ABIs (i386, x32), refuses
  mounts, namespace calls (and `clone` with `CLONE_NEW*`; `clone3` gets
  ENOSYS so libc falls back to `clone`), keyrings, BPF, perf, io_uring,
  modules, kexec, reboot, swap, syslog, clock setting, iopl/ioperm, file
  handles, and `TIOCSTI`/`TIOCLINUX` ioctls.
- `bh put` opens the sources first, then (as the blackhole's ID for root)
  creates each file in the inbox with `O_CREAT|O_EXCL|O_NOFOLLOW`: the
  blackhole may be running and controls what is in there.
- `bh close` sends SIGTERM to the bh holding the lock, which kills the
  blackhole's init; it then waits for the lock and deletes the tree with
  `*at` calls that never follow symlinks.

## worm

- `src/worm/`: `worm unlock` / `worm lock` bind-remount `/` (re-applying the
  statvfs flags), list processes with files open for writing (by mnt_id) on
  EBUSY, and run `snap now --auto` after locking.
- `fw.c` is the firewall. Open ports live in `/etc/worm/firewall` (`tcp 22`,
  `udp 6000-6010`); no package ships it, and a missing file means none.
  `worm fw allow|deny` applies the new rules first and only then rewrites
  the file, bind-remounting `/` writable for the moment if it was locked
  (refused while a package change is pending, as for `worm unlock`).
- The ruleset is generated into `/run/worm/firewall.nft` and loaded with
  `nft -f`: `destroy table inet worm` plus the new table, one transaction.
  Input policy drop with established/related, loopback, essential ICMP and
  ICMPv6 (neighbour discovery with hop limit 255), DHCPv6 replies and the
  sets `open_tcp`/`open_udp`; a named counter `dropped` counts the rest.
  Forward policy drop. Output is not filtered.
- The service `05-firewall` runs `worm fw load` before the network. A bad
  port list loads the firewall with no open ports and exits 1.
- nftables is built with mini-gmp and without the CLI, JSON or xtables.

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
