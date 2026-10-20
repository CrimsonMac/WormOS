# WormOS

A lightweight, security-focused Linux operating system built around instant, tamper-resistant snapshots.

> **Status:** early development. A minimal base system boots in QEMU with a
> read-only root and working snapshots.

## Philosophy

- **Testing:** a safe place to try software, write code and experiment. If something breaks, go back.
- **Secure:** hardened defaults, a firewall enabled out of the box, and privacy tools built in.
- **Reversible:** the base system is immutable, and every state can be snapshotted and restored in seconds.
- **Isolated:** risky work runs in disposable sandboxes that cannot reach the host or its snapshots.

## Planned features

- `snap`, a simple snapshot tool, plus a graphical timeline:
  ```
  snap now                       # snapshot the system now
  snap now -c "Installed nginx"  # snapshot with a comment
  snap list                      # list snapshots
  snap rewind                    # return to the last snapshot
  ```
- Separate system and home snapshots, built on Btrfs, with fast rewinds.
- Snapshots protected from the running system, verified at boot, and restorable from the boot menu.
- Blackholes: disposable sandboxes for untrusted software and malware analysis (`bh` / `blackhole`); a virtual machine mode for stronger isolation.
- An nftables firewall that denies inbound traffic by default (`worm fw`).
- Tor built in, Tor Browser with signature-verified installation, and an optional system-wide anonymous mode.
- Hardened defaults: full-disk encryption, MAC address randomization, encrypted DNS, a hardened kernel and no telemetry.
- Optional tool kits (forensics, blue team, network analysis), installed on demand into isolated containers (`worm kit`).
- An independent init system and package manager (`wh` / `wormhole`), with
  automatic snapshots before every change (`snap auto on|off`).
- A lightweight Wayland desktop.

## Snapshots

`snap` manages snapshots of the system (`/`) and of `/home`. The default
scope is the system, so rewinding never touches your files unless asked to.

```
snap now -c "Before trying nginx"   # take a snapshot with a comment
snap list                           # ID, date, scope, type, comment
snap diff 3                         # what changed since snapshot 3
snap restore 3 /etc/nginx           # bring back one file or folder
snap browse 3                       # read-only shell inside snapshot 3
snap rewind                         # go back to the latest snapshot
snap rewind 3 --home                # rewind /home only (--all for both)
snap verify                         # check snapshots for tampering
snap delete 3 / snap prune --keep 10 / snap pin 3 / snap comment 3 "..."
snap auto on|off                    # snapshots before package changes
```

How it works:

- The disk is a single btrfs filesystem with the subvolumes `@` (the system,
  mounted read-only), `@var` (logs and state), `@home` and `@snapshots`.
- Snapshots are read-only btrfs snapshots stored in `@snapshots`, which the
  running system never mounts. `snap` mounts it in a private mount namespace
  only for the duration of a command.
- Every snapshot has a manifest with the checksum, owner, mode and extended
  attributes of each file. Rewinds verify the target first and refuse a
  snapshot that was modified.
- A rewind is applied at a soft reboot: the init system stops userland, the
  initramfs init (`wormrd`) swaps the subvolume and the system starts again
  without restarting the kernel. The state being left is snapshotted first,
  so a rewind can itself be undone.
- The boot menu has a "Rewind to a snapshot" submenu with the latest system
  snapshots, which rewinds before anything from the installed system runs.

These protections raise the bar, but software running as root on the same
machine can always write to the disk directly. Untrusted code belongs in an
isolated sandbox, which is planned for a later phase.

## Packages

`wh` (short for wormhole) installs software from the signed WormOS
repository. Changes never touch the running system: they are made in a
staged copy, the current system is snapshotted, and a quick restart (the
kernel keeps running) switches over.

```
wh update                 # download the package index
wh search tcp             # find packages
wh install tcpdump        # install, with whatever it needs
wh install --try nmap     # trial: undone at the restart after next...
wh keep                   # ...unless you keep it
wh remove tcpdump
wh upgrade
wh history                # every change, with its snapshot
wh undo                   # go back to before the last change
```

The package index is signed with Ed25519 and checked on every use; packages
must match the SHA-256 the index lists, and an index older than the one
already downloaded is refused. Packages are unpacked by wh itself, which
never writes outside the system it installs into.

### Software from Debian and Kali

The core repository holds the base system and the tools that need direct
access to the machine. Everything else comes from existing distributions:
when a package is not in core, `wh install` looks in Debian stable, then in
Kali, and installs it into a container for that distribution, a
*wormhole*. Its commands appear on your `PATH` as if installed natively.

```
wh install htop               # not in core: found in Debian, installed there
wh install --from kali hashid # pick the source
wh remove htop
```

A wormhole is set up the first time it is needed, from the distribution's
own signed archive (checked with `gpgv` against its archive keyring, which
WormOS ships), and from then on it is managed by its own apt. These
installs take effect immediately, without a restart.

Wormholes share your home folder and the network: they make software
available, they are not a security boundary. They live on a separate
subvolume mounted `nosuid`, so nothing inside one can gain root on the
system. Run untrusted software in a blackhole instead (planned).

### Manual changes

To change the read-only system by hand:

```
worm unlock               # make / writable (only the root mount)
worm lock                 # read-only again, and snapshot the changes
```

## Firewall

Inbound traffic is dropped unless it answers a connection this machine
started (outbound is not filtered). The rules load before any network
interface comes up.

```
worm fw status            # active? which ports are open?
worm fw allow 22          # open a TCP port
worm fw allow 51820/udp   # UDP, or a range such as 6000-6010
worm fw deny 22           # close it again
```

Open ports are stored in `/etc/worm/firewall`, so they are part of system
snapshots: rewinding brings back the firewall of that time.

## Blackholes

A blackhole is a disposable sandbox for software you do not trust. Things
go in, nothing escapes.

```
bh open                   # a temporary blackhole, deleted when you leave
bh put lab sample.bin     # copy a file into the blackhole "lab" (/inbox)
bh open lab               # a shell inside it; its files stay between sessions
bh open lab -- ./x        # run one command inside
bh list                   # your blackholes
bh close lab              # stop it and delete everything in it
```

Inside, programs run as root of the blackhole only: outside, they are an
unprivileged user. They see the system's programs read-only, but not
`/home`, `/root`, `/var`, the disks or the snapshots, nor the host's
processes. There is no network except loopback. A seccomp filter refuses
mounts, new namespaces, kernel keyrings, BPF, io_uring and similar calls,
and Landlock keeps file access inside the blackhole. Blackholes opened by
root are limited to 1024 processes and half the memory.

Limits: a blackhole shares the kernel with the host, so a kernel exploit
can still escape it (a virtual machine mode is planned). Blackholes have
no network access yet, and only one session per blackhole runs at a time.

## Roadmap

1. Minimal bootable base system (kernel, libc, init)
2. Snapshot system and package manager
3. Security baseline (firewall, sandboxing, hardening)
4. Graphics stack and Wayland compositor
5. Desktop shell and snapshot timeline
6. Tool kits
7. Bootable ISO and installer

## Building

WormOS is built entirely from source with its own cross toolchain
(`x86_64-wormos-linux-gnu`). Development targets x86_64 and is tested in
QEMU with UEFI firmware.

How the pieces fit together (image, boot, snapshots, packages, wormholes)
is described in [docs/internals.md](docs/internals.md).

### Requirements

A Debian 13 (or similar) host with roughly 25 GB of free disk space:

```
sudo apt install build-essential bison flex gawk texinfo m4 help2man \
    libncurses-dev libssl-dev libelf-dev libgmp-dev libmpc-dev rsync curl xz-utils bzip2 \
    btrfs-progs dosfstools mtools fakeroot grub-efi-amd64-bin \
    qemu-system-x86 ovmf
```

### Build and run

```
make            # toolchain, kernel, packages, repository, root filesystem and disk image
make run        # boot the image in QEMU on the serial console (Ctrl-A X quits)
make run-gui    # boot the image in a QEMU window
make test       # boot the image and run automated checks
```

`make test-wormholes` checks the Debian and Kali wormholes against the real
archives; it needs internet access and takes several minutes.

`make test` boots a throwaway copy of the image, checks the running system
and exercises snapshots end to end (take, diff, restore, browse, rewind of
the system and of `/home`, and a rewind from the boot menu), then
`worm unlock`/`lock` and the package manager against `out/repo`, served to
the VM over HTTP (install, a trial that rolls back, remove and undo). It
also opens and closes a firewall port and checks it from the host through a
QEMU port forward (`QEMU_HOSTFWD`), and tries to escape from a blackhole
(as root and as a normal user).

Building the disk image uses `sudo` for one step (applying file ownership
before `mkfs.btrfs`); everything else runs as a normal user.

The first build compiles the toolchain and takes a while. Later builds reuse
it. Individual steps can be run with `make toolchain`, `make kernel`,
`make packages`, `make repo`, `make rootfs` and `make image`.

`make repo` packs every package into `out/repo` and signs the index. The
first run creates a signing key in `~/.config/wormos/core.key` (keep it
safe and backed up); its public half is `keys/core.pub`, which is built
into the system. The root filesystem is assembled by installing the base
packages from that repository with `wh --root`.

The development image has an empty root password.

The root filesystem is read-only; change it with `worm unlock` and
`worm lock`. (A plain `mount -o remount,ro /` would make the whole btrfs
filesystem read-only, including `/var` and `/home`.)

### Layout

| Path | Contents |
| --- | --- |
| `scripts/` | Build scripts (toolchain, kernel, root filesystem, image, QEMU) |
| `tools/wbuild` | Package builder |
| `tools/wpack`, `tools/wrepo` | Package files and the signed repository |
| `keys/` | Public key of the package repository |
| `recipes/` | Package recipes |
| `src/` | WormOS components: `worminit` (init), `wormrd` (initramfs init), `snap`, `wh`, `worm` |
| `kernel/` | Kernel configuration |
| `toolchain/` | Pinned toolchain sources and checksums |

### Package recipes

Each package is described by `recipes/NAME/recipe`, a short bash script
(see the comment at the top of `tools/wbuild` for every field, including
`depends`, `caps` for file capabilities and `groups`):

```bash
name=example
version=1.0
description="An example package"
source=(https://example.org/example-$version.tar.xz)
sha256=(...)

build() {
	./configure --prefix=/usr --host="$TARGET"
	make -j"$JOBS"
}

package() {
	make DESTDIR="$pkgdir" install
}
```

Build it with `tools/wbuild example`. Every source is verified against its
pinned SHA-256 checksum before use.
