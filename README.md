# WormOS

A lightweight, security-focused Linux operating system built around instant, tamper-resistant snapshots.

> **Status:** early development. A minimal base system boots in QEMU.

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
- Blackholes: disposable sandboxes for untrusted software and malware analysis (`bh` / `blackhole`).
- An nftables firewall that denies inbound traffic by default (`worm fw`).
- Tor built in, Tor Browser with signature-verified installation, and an optional system-wide anonymous mode.
- Hardened defaults: full-disk encryption, MAC address randomization, encrypted DNS, a hardened kernel and no telemetry.
- Optional tool kits (forensics, blue team, network analysis), installed on demand into isolated containers (`worm kit`).
- An independent init system and package manager (`wh` / `wormhole`), with
  automatic snapshots before every change (`snap auto on|off`).
- A lightweight Wayland desktop.

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

### Requirements

A Debian 13 (or similar) host with roughly 25 GB of free disk space:

```
sudo apt install build-essential bison flex gawk texinfo m4 help2man \
    libncurses-dev libssl-dev libelf-dev rsync curl xz-utils bzip2 \
    btrfs-progs dosfstools mtools fakeroot grub-efi-amd64-bin \
    qemu-system-x86 ovmf
```

### Build and run

```
make            # toolchain, kernel, packages, root filesystem and disk image
make run        # boot the image in QEMU on the serial console (Ctrl-A X quits)
make run-gui    # boot the image in a QEMU window
make test       # boot the image and run automated checks
```

The first build compiles the toolchain and takes a while. Later builds reuse
it. Individual steps can be run with `make toolchain`, `make kernel`,
`make packages`, `make rootfs` and `make image`.

The development image has an empty root password.

### Layout

| Path | Contents |
| --- | --- |
| `scripts/` | Build scripts (toolchain, kernel, root filesystem, image, QEMU) |
| `tools/wbuild` | Package builder |
| `recipes/` | Package recipes |
| `src/` | WormOS components (e.g. `worminit`, the init system) |
| `kernel/` | Kernel configuration |
| `toolchain/` | Pinned toolchain sources and checksums |

### Package recipes

Each package is described by `recipes/NAME/recipe`, a short bash script:

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
