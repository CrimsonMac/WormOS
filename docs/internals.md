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
  `caps`, `groups`, `users` and `sysroot_files`.
- Autotools recipes must pass `--build="$(gcc -dumpmachine)"`; otherwise
  configure runs our cross binaries' host tools against the target libc.
- Nothing shipped names the build machine. wbuild compiles through small
  wrappers in `build/host/cc` that add `-ffile-prefix-map` (sources appear as
  `/usr/src/NAME`, sysroot headers as `/usr/include`); the flags stay out of
  CFLAGS because some packages, such as OpenSSL, record their CFLAGS. wbuild
  also rewrites sysroot paths in a package's text files
  (pkg-config files, `*-config` scripts) to plain target paths. wpack refuses
  a package in which any file still contains the repository path or the
  builder's home directory. Recipes whose configure scripts add run paths or
  record their command line (unbound) fix that themselves.
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
- `status.c` keeps a summary readable by everyone in `/run/snap-status`
  (`count`, `last`, `last_date`, `last_comment`, `auto`, `pending`),
  rewritten whenever the boot menu is and by `snap status`, which the
  service `11-snap-status` runs at boot. It is not written with `--top`.

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
- Hooks (`hooks.c`) rebuild caches that several packages contribute to.
  A package providing the tool ships `usr/share/wh/hooks/NAME.hook` with
  one or more `path =` lines and an `exec =` line. After a transaction
  that installed or removed anything beneath a path (or installed the hook
  itself), wh runs exec in the changed root, before committing it: words
  starting with `/` are taken inside the root, and the program runs through
  the root's own loader (`ld-linux-x86-64.so.2 --library-path ROOT/usr/lib`),
  so the same hook works for a staged system and for an image assembled on
  the build machine (without `LD_PRELOAD`, which the root's loader cannot
  satisfy; build-rootfs gives the files their owner afterwards). Hooks:
  `gsettings-schemas` (glib) and `mime-database` (shared-mime-info).
  Caches that belong to one package's own folder, such as the Adwaita icon
  cache, are built when the package is built instead.

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
- Besides commands, wh exports a package's desktop entries (to
  `/usr/local/share/applications`, unless the system has an entry of that
  name) and icons (`usr/share/icons` and `usr/share/pixmaps`, to the same
  places under `/usr/local/share`). An entry is only exported when its
  program is one of the commands just exported; it keeps only the
  `[Desktop Entry]` group, its `Exec` names the wrapper, and `Actions`,
  `TryExec` and `DBusActivatable` are dropped. Files are read inside the
  wormhole with `openat2` (`RESOLVE_IN_ROOT`), regular files of at most
  8 MiB only, and recorded with the commands, so removing the package
  removes them.
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

## Hardening and privacy

- Services in order: `01-sysctl` (`sysctl -p` on `/usr/lib/sysctl.d/*.conf`,
  then `/etc/sysctl.d/*.conf`), `05-firewall`, `10-filesystems`,
  `15-dns` (shipped by the unbound package), `16-anon` (tor), `20-loopback`,
  `25-net-privacy`, `30-network`.
- Kernel command line (`scripts/mkimage.sh`): `slab_nomerge debugfs=off
  iommu.passthrough=0 iommu.strict=1 efi=disable_early_pci_dma`.
- worminit mounts `/proc` with `hidepid=invisible` and `/dev`, `/run` and
  `/dev/shm` `noexec`. `/tmp` stays executable.
- `/usr/lib/worm/net-privacy` gives every device with a `device` link in
  `/sys/class/net` a random locally administered unicast MAC and sets its
  `use_tempaddr` to 2 (the sysctl defaults only reach devices created
  later).
- `/usr/lib/worm/dhcp-event` replaces busybox's udhcpc script. It records
  the announced DNS servers in `/run/worm/dhcp-dns` and, when unbound is
  installed, keeps `/run/resolv.conf` at 127.0.0.1 and ::1.
- unbound (`recipes/unbound/files/unbound.conf`) listens on loopback only,
  runs as the `unbound` user, forwards everything over TLS to Quad9 and
  validates DNSSEC with `/var/lib/unbound/root.key` (RFC 5011 updates),
  seeded from `/usr/share/unbound/root.key` (KSK-2017 and KSK-2024 DS
  records from IANA).
- Recipes can declare `users=(NAME)`: wh adds a system group and user with
  a locked password, no home and nologin (IDs from 999 down).

## Anonymous mode

- `worm anon` (`src/worm/anon.c`). The switch is `/etc/worm/anon` (part of
  system snapshots). `on` writes it (bind-remounting `/` if locked),
  reloads the firewall, then starts Tor; `off` removes it, reloads the
  firewall and stops Tor. At boot the firewall service loads the anonymous
  rules before the network, and `16-anon` (`worm anon start`, from the tor
  package) starts Tor once `/var` is mounted.
- Tor (`recipes/tor`) is built as a client only (`--disable-module-relay`),
  without GeoIP data. `/etc/tor/torrc`: user `tor`, data in `/var/lib/tor`,
  pid and notices log in `/run/tor`, SocksPort 9050, TransPort 9040,
  DNSPort 5353 (IPv4 and IPv6 loopback), .onion names automapped into
  10.192.0.0/10. `worm anon status` reads the last "Bootstrapped N%" line
  of the log.
- Rules (`write_anon()` in `fw.c`), added to the `inet worm` table: an
  output filter (policy drop) accepting loopback, Tor's UID and DHCP
  renewals, rejecting the rest (TCP with a reset); and a NAT output chain
  returning Tor's packets, redirecting UDP port 53 to 5353 and every other
  IPv4 TCP connection outside 127.0.0.0/8 to 9040. The resolver's own
  upstream connections are redirected like any other. Without a `tor`
  user nothing is accepted or excepted, so the machine fails closed. A cut
  network (panic, safe mode) replaces the whole table and wins.

## Panic and safe mode

- `worm panic` (`src/worm/panic.c`): `fw_cut()` writes the marker
  `/run/worm/network-cut` and loads a table dropping everything but
  loopback in input, output and forward; hardware devices go down (the
  DHCP script leaves them down while the marker exists). Then every
  running blackhole of every user is stopped: the bh holding its lock gets
  SIGKILL and its init follows through `PR_SET_PDEATHSIG`. The PID comes
  from the user's `pid` file, so it is only signalled when that process
  belongs to the owner of the user's directory and is not PID 1; waiting
  for the lock is bounded. Root's blackhole cgroups get `cgroup.kill` and
  are removed. Last, `snap now --all --suspect` and `snap pin`.
- `worm panic off` removes the marker, reloads the normal rules, brings
  the devices up and sends SIGUSR1 to udhcpc (taking a device down dropped
  its routes).
- Snapshots marked `suspect = yes` in `info`: the "Before rewind" ones
  (`snap boot`) and `snap now --suspect` (panic).
- `menu.c` writes a safe mode entry before the "Rewind to a snapshot"
  submenu: the newest system snapshot not marked suspect, booted with
  `wormos.rewind=ID wormos.safe` (the current system before the first
  snapshot; the ESP grub.cfg has the same fallback when the store has no
  grub.cfg yet).
- With `wormos.safe`, worminit skips services marked `safe = no`
  (`25-net-privacy`, `30-network`) and `worm fw load` cuts the network. A
  soft reboot keeps the kernel command line, so safe mode lasts until a
  full restart.
- `worm status` (`status.c`) summarises the root mount, mode, snapshots
  (`snap list`, `snap auto`), staged package changes, firewall, the
  resolver, blackholes, lockdown and io_uring.

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

## Devices and modules

- The kernel builds Intel's i915 and xe, VMware's vmwgfx, QXL, uinput and the
  VMware guest drivers as modules (zstd-compressed, signed); everything a
  VM needs to boot stays built in. `linux-firmware` ships only the Intel GPU
  firmware (`i915/`, `xe/`), zstd-compressed; the kernel decompresses it.
- kmod provides `modprobe`/`depmod` (busybox's module applets and mdev are
  left out). eudev is the device manager: the service `02-udev` starts
  `udevd`, replays the boot devices (`udevadm trigger`, which loads modules
  and firmware) and waits for it to settle. Network devices keep their
  kernel names (the `80-net-name-slot` rule is removed).
- The hardware database is compiled when the package is built: the eudev
  recipe runs its own `udevadm hwdb --update --usr` through the sysroot's
  loader (`/etc` is read-only on the target). The OUI (MAC vendor) table is
  left out.
- Device groups: `video` (`/dev/dri/card*`), `render` (render nodes, open
  to everyone), `input`, `kvm` and the usual others in base-files.
- seatd (`45-seatd`, `seatd -g video`) hands the GPU and input devices to
  the compositor, so it runs as the user.

## Graphics stack

- Build support for meson projects: `meson_build`/`meson_package` in wbuild
  with a generated cross file, and `host_env` plus `$NATIVE`
  (`build/host/native`) for tools the build runs on this machine
  (wayland-scanner, Mesa's `mesa_clc`). A wrapper keeps the target's
  pkg-config settings away from lookups for this machine.
- Mesa: gallium drivers iris and crocus (Intel), virgl and svga (virtual
  GPUs) and softpipe; EGL, GBM and OpenGL ES; no LLVM, no GLX, no Vulkan.
  The Intel drivers compile OpenCL kernels at build time with `mesa_clc`,
  which the Mesa recipe first builds for this machine from Debian's LLVM.
- Text: freetype, harfbuzz, fribidi, fontconfig (cache in
  `/var/cache/fontconfig`, created on first use), cairo, pango, glib.
  Fonts: Inter (sans-serif) and Hack (monospace), made the defaults by
  `/etc/fonts/conf.d/60-*.conf`.
- glibc ships the `C.UTF-8` locale (compiled by the target's localedef
  through its own loader); `/etc/profile` sets `LANG=C.UTF-8`.

## Message bus

- `dbus` is the reference D-Bus daemon (expat only; no systemd, X11
  autolaunch, SELinux or AppArmor). The service `40-dbus` runs the system
  bus as the `messagebus` user on `/run/dbus/system_bus_socket`. There is
  no setuid activation helper: no system service is started on demand.
- `/etc/machine-id` links to `/var/lib/dbus/machine-id`, created by
  `dbus-uuidgen --ensure` at the first boot, so each installation has its
  own ID while `/etc` stays read-only.
- Each desktop login gets a session bus, started by the session script
  before labwc and stopped after it, listening on `$XDG_RUNTIME_DIR/bus`
  (`unix:runtime=yes`, also the default address clients try). Wormholes
  share `/run`, so their programs find both buses; blackholes see neither.
  labwc's `autostart` passes `WAYLAND_DISPLAY` and `XDG_CURRENT_DESKTOP` to
  the bus (`dbus-update-activation-environment`) for programs it starts on
  demand.

## GTK

- GTK 4.24 with the Wayland backend only: no X11, Vulkan, GStreamer,
  printing or desktop search. Its dependencies: gdk-pixbuf (PNG, JPEG and
  TIFF loaders built into the library, so no loader cache; no glycin),
  libjpeg-turbo (CMake, SIMD assembled with the build machine's nasm),
  libtiff, libepoxy (EGL only), graphene, shared-mime-info, cairo with
  cairo-gobject and harfbuzz with its subset library and GLib integration.
  SVG icons are drawn by GTK's own SVG renderer; there is no librsvg.
- `wormos-gtk-theme` compiles GTK's own theme sources (from the GTK
  release) with sassc, with `_colors.scss` replaced by the WormOS palette
  and rounder corners, into `/usr/share/themes/WormOS/gtk-4.0` (dark only;
  `gtk-dark.css` is the same file).
- `/etc/gtk-4.0/settings.ini` selects the theme, the Adwaita icons and
  cursors, Inter and the dark colour scheme. labwc uses the same cursors
  (`XCURSOR_THEME`). With wlroots' pixman renderer the session also sets
  `GSK_RENDERER=cairo`: GTK's OpenGL renderer would run on softpipe.
- dconf keeps GSettings values (its service is started on demand by the
  session bus). gtk4-layer-shell is there for the shell's panel.

## Desktop shell

- `src/shell/` is `worm-shell`, one GTK program for the panel, the
  application launcher and notifications, started by labwc's `autostart`.
  It is a single-instance GtkApplication (`org.wormos.Shell`): `worm-shell
  --launcher` (the Super key on its own, or Super+Space) toggles the
  launcher in the running shell; `--quit` stops it.
- `wayland.c` binds two wlroots protocols on GTK's own connection:
  wlr-foreign-toplevel-management (`toplevels.c`, the task list: click to
  focus or minimise, middle click to close) and wlr-output-management
  (`outputs.c`). The protocol XML is copied from wlroots and compiled with
  wayland-scanner.
- Panel (`panel.c`): a layer-shell surface along the top of each monitor
  with an exclusive zone. It takes no keyboard focus, except while one of
  its menus is open. Left: the launcher button and the windows; middle: the
  clock (a calendar on click); right: snapshots (from `/run/snap-status`),
  protection (firewall, anonymous mode, a cut network), network (a device
  with a carrier and its address) and the power menu (display scale, log
  out, restart, shut down; restart and shut down need a second click).
  Status is refreshed every five seconds (`status.c`).
- Display scale: `~/.config/wormos/display` holds `scale = auto` or a
  factor. Automatic is 2 for outputs at least 2000 pixels high and 1
  otherwise. The shell applies it when the setting changes and when an
  output appears or changes mode, so a scale set with wlr-randr lasts until
  then.
- Restart and shut down use `/usr/lib/worm/power` (from the worm package),
  setuid root: it takes only `off` or `reboot`, clears its environment,
  requires the caller (real user and groups) to be root or in `wheel`, and
  signals PID 1 as busybox `poweroff` and `reboot` do. Logging out runs
  `labwc --exit`.
- Launcher (`launcher.c`): an overlay layer surface with exclusive keyboard
  focus: a search field over a grid of the desktop entries GIO lists
  (`g_app_info_should_show`), filtered by name, description, generic name,
  program and keywords. Enter starts the first match, Escape or a click
  outside closes it.
- Notifications (`notify.c`): `org.freedesktop.Notifications` on the session
  bus (Notify, CloseNotification, GetCapabilities, GetServerInformation and
  the two signals), shown as cards in an overlay surface at the top right.
  Default timeout six seconds, none for critical ones; body markup is used
  when Pango accepts it.
- The look is `/usr/share/wormos/shell/shell.css`, on top of the GTK theme.

## Desktop session

- `wormos-desktop` holds the session: `/etc/profile.d/desktop.sh` runs
  `/usr/lib/worm/session` after a non-root login on tty1, and logging out
  of the desktop ends the login. At the first login it creates the
  standard folders (Desktop, Documents, Downloads, Music, Pictures, Videos,
  Public, Templates) and records them in `~/.config/user-dirs.dirs`. The
  session script checks
  `XDG_RUNTIME_DIR` (`/run/user/UID`, created by the `12-runtime-dirs`
  service and by the first-boot setup) and membership of `video`, sets the
  Wayland environment and `LIBSEAT_BACKEND=seatd`, and runs labwc with its
  output in `$XDG_RUNTIME_DIR/desktop.log`.
- Renderer: OpenGL ES when a card is driven by i915, xe or vmwgfx;
  otherwise wlroots' pixman renderer (`WLR_RENDERER=pixman`), which beats
  OpenGL emulated on the CPU. wlroots does not fall back to pixman by itself
  when a render node exists, so if labwc fails within 15 seconds with
  OpenGL ES, the session starts it again with pixman (the first log is kept
  as `desktop.log.gles`). The session starts in the user's home folder.
  On vmwgfx it sets `WLR_NO_HARDWARE_CURSORS=1`: VMware's cursor plane
  stays invisible with wlroots.
- Screen size: the virtual GPUs (virtio-gpu, QXL, vmwgfx) change their
  preferred mode when the viewer's window is resized, but wlroots only reads
  an output's modes when it connects. `recipes/wlroots/files/
  follow-preferred-mode.patch` makes the DRM backend look at the preferred
  mode on every hotplug event for outputs with a virtual connector or the
  `hotplug_mode_update` property; when it changes, the mode is added to the
  output and requested through `request_state`, which labwc applies. A mode
  set by hand (wlr-randr) stays until the window size changes again.
- VMware passes the window size to the guest only through its tools:
  `open-vm-tools` builds just the root service (vmtoolsd) with the
  resolutionKMS plugin (window size to vmwgfx), timeSync, powerOps
  (shutdown and reboot call busybox `poweroff`/`reboot`, no scripts) and
  guestInfo. The plugins that let the host reach into the guest (vix:
  running programs and copying files; shared folders; guestStore, gdp and
  componentMgr; appInfo, which lists running programs; vmbackup) are
  removed. The service `48-vmtools` starts it only when `vmware-checkvm`
  finds VMware. open-vm-tools needs libtirpc and libxcrypt (glibc no longer
  ships Sun RPC or crypt); libxcrypt is built with the strong hashes only.
- `xcompose` ships libX11's UTF-8 Compose table (`/usr/share/X11/locale`,
  mapped to every UTF-8 locale) so dead keys and the Compose key work.
- labwc's configuration is in `/etc/xdg/labwc` (`rc.xml`, `menu.xml`,
  `autostart`, `environment`), the theme in `/usr/share/themes/WormOS`,
  foot's in `/etc/xdg/foot/foot.ini`. `rc.xml` clears labwc's default
  left and middle button bindings on the background, so only the right
  button opens the menu. `autostart` starts swaybg with the wallpaper
  (2560x1440, drawn at build time with pycairo by `wallpaper.py`) and, once
  per user, a welcome terminal showing the full guide (`welcome --full`).
- foot starts login shells, so every new terminal reads `/etc/profile`;
  `/etc/profile.d/motd.sh` runs `welcome` (system details, main commands
  and a random line from `/usr/share/wormos/tips`) in interactive shells
  with `WAYLAND_DISPLAY` set, unless `~/.hushlogin` exists. It exports
  `WORMOS_GREETED` so shells started inside the terminal stay quiet; `su -`
  starts with a clean environment without `WAYLAND_DISPLAY`, so it stays
  quiet too. The menu's "Applications" entry is a pipe menu
  (`/usr/lib/worm/apps-menu`) listing desktop entries in wormholes whose
  program wh exported.
- Wormholes: whc shares `/run` (so the Wayland socket) and `/dev/dri`, and
  binds `/usr/share/fonts` read-only on `/usr/local/share/fonts/wormos`
  (the mount point is created when a wormhole is set up).
- Consoles: `/usr/lib/worm/console TTY` runs getty on tty1 and tty2. On
  tty1, while `/etc/passwd` has no user with a UID of 1000 or more, it first
  runs `/usr/lib/worm/first-boot` there (with the tty as controlling
  terminal): user name, user and root passwords, then `worm unlock`,
  `adduser`, the groups `wheel video input audio render kvm`, `chpasswd -c
  sha512` and `worm lock -c "First boot: created NAME"`, which also takes
  a snapshot.
- `su` is a separate, minimal busybox build holding only `su`, installed
  setuid root; the main busybox binary has no setuid applets.

## Releases

- `scripts/release.sh VERSION` (`make release VERSION=X`) builds the root
  filesystem with `WORMOS_RELEASE=1` (root's password field becomes `!`
  until the first-boot setup), a 20 GiB image (`IMAGE` and `DISK_MB` are
  parameters of `mkimage.sh`), and converts it with qemu-img: a compressed
  qcow2, and a monolithic sparse VMDK zipped with `WormOS.vmx` (from
  `scripts/wormos.vmx.in`: UEFI, NVMe, vmxnet3, 3D on). `SHA256SUMS` lists
  both.
- `mkimage.sh` copies only the data extents of the partition images into
  the disk image (`scripts/sparse-copy.py`, SEEK_DATA/SEEK_HOLE), so a
  20 GiB disk takes seconds.
- GRUB is installed at the removable path `\EFI\BOOT\BOOTX64.EFI`, so the
  images boot with fresh firmware settings. `run-qemu.sh` gives the disk
  `bootindex=0` so that added devices never shift the boot order.

## Tests

- `make test` (`tests/boot-test.py`) boots a throwaway copy of the image
  (`run-qemu.sh --snapshot`), logs in over serial, runs the checks and powers
  off. It serves `out/repo` over HTTP to the VM at 10.0.2.2. About a minute
  under TCG; a hang means a check is waiting for its 600 s timeout.
- `make test-wormholes` (`tests/wormhole-test.py`) needs internet and takes
  several minutes: it sets up the Debian and Kali wormholes from the real
  archives.
- `tests/desktop-test.py` (also run by `make test`) adds a virtio GPU,
  keyboard and tablet and a QEMU monitor socket, types on tty1 with
  `sendkey`, reads the console text from `/dev/vcs1` over the serial
  console, and checks `screendump` output for the wallpaper's colours. It
  clicks on the background through QMP (`input-send-event` on the tablet)
  and compares screen regions, and resizes the screen through QEMU's VNC
  server (a minimal client sending ExtendedDesktopSize `SetDesktopSize`,
  which QEMU forwards to virtio-gpu like a window resize). With
  `--release IMAGE` it checks a release image: it types the first-boot
  setup blind, then logs in as root on the serial console with the
  password it set.
- Faster loop for snap: build natively (`make -C src/snap CC=gcc`) and use
  `--top` on a loopback btrfs image mounted with sudo.
