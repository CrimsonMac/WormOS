# WormOS

A lightweight, security-focused Linux operating system built around instant, tamper-resistant snapshots.

> **Status:** 1.0, the first usable release: a read-only system with
> snapshots, packages, a firewall, sandboxes, Tor and a light Wayland
> desktop, running in a virtual machine (QEMU or VMware).

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

## Trying it

Release images run in a virtual machine with UEFI firmware. Download them
from the release page and check them against `SHA256SUMS`.

- **QEMU / virt-manager:** `wormos-1.0-x86_64.qcow2`. In virt-manager,
  import an existing disk image, choose a generic recent Linux, and set the
  firmware to UEFI (Customize configuration before install, Overview,
  Firmware) with Secure Boot off. From the command line:
  ```
  qemu-system-x86_64 -machine q35 -accel kvm -cpu host -m 2048 -smp 2 \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
      -drive file=wormos-1.0-x86_64.qcow2,if=virtio \
      -nic user,model=virtio-net-pci -device virtio-vga \
      -device virtio-keyboard-pci -device virtio-tablet-pci
  ```
- **QEMU on Windows:** install QEMU from https://qemu.weilnetz.de/w64/
  (it includes the UEFI firmware). For speed, turn on the Windows feature
  "Windows Hypervisor Platform" (Settings, Optional features, More Windows
  features) and restart. Then, in PowerShell, in the folder with the
  image:
  ```
  & "C:\Program Files\qemu\qemu-system-x86_64.exe" -machine q35 `
      -accel whpx,kernel-irqchip=off -m 2048 -smp 2 `
      -drive "if=pflash,format=raw,readonly=on,file=C:\Program Files\qemu\share\edk2-x86_64-code.fd" `
      -drive file=wormos-1.0-x86_64.qcow2,if=virtio `
      -nic user,model=virtio-net-pci -device virtio-vga -display sdl `
      -device virtio-keyboard-pci -device virtio-tablet-pci
  ```
  If QEMU says WHPX is not available, use `-accel tcg` instead: it works
  without the Windows feature, but much more slowly. Ctrl+Alt+G releases
  the mouse.
- **VMware Workstation / Player:** unzip `wormos-1.0-vmware.zip` and open
  `WormOS.vmx` (UEFI, NVMe disk, 2 GB of memory, 3D acceleration on).

On the first start, WormOS asks for a user name, a password for it and a
password for root, on the first console. Then log in with that user: the
desktop starts.

## The desktop

The desktop is [labwc](https://labwc.github.io), a light stacking Wayland
compositor, with the [foot](https://codeberg.org/dnkl/foot) terminal. It
idles at well under 100 MB of memory.

- Right click on the background (or Super+Space) opens the menu: terminals,
  applications installed from wormholes, snapshots, screenshots, log out.
- Super+Enter opens a terminal, Super+Q closes a window, Alt+Tab switches,
  Super+Arrow keys maximize or snap windows, Print saves a screenshot in
  `~/Pictures`.
- Ctrl+Alt+F2 switches to a second console (for logging in as root);
  Ctrl+Alt+F1 returns to the desktop.
- Administration is done as root: `su -` in a terminal, or the menu's root
  terminal.
- Settings: `/etc/xdg/labwc` (copy files to `~/.config/labwc` to change
  them; `environment` sets the keyboard layout) and `/etc/xdg/foot/foot.ini`.
- Screen resolution: `wlr-randr` lists outputs and their modes, and
  `wlr-randr --output NAME --custom-mode 1920x1080` changes one. To keep a
  setting, add the command to `~/.config/labwc/autostart`. (Without guest
  tools, VMware does not resize the screen with its window.)

Graphics: Intel GPUs (with the i915/xe firmware) and VMware's virtual GPU
use OpenGL ES through Mesa; other virtual GPUs (virtio, QXL, bochs) use a
fast software renderer, and so does any GPU on which OpenGL ES fails to
start.
Programs installed from Debian or Kali that support Wayland open on the
desktop as well (they see the system's fonts); X11-only programs do not
work yet.

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
isolated sandbox: a blackhole (see below).

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
system. Run untrusted software in a blackhole instead.

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

## Anonymous mode

```
worm anon on              # all traffic of the machine through Tor
worm anon status          # is it on, has Tor connected yet?
worm anon off
```

In anonymous mode only Tor itself reaches the network. Every other TCP
connection, from any program, user, wormhole or root, is sent through
Tor, DNS queries are answered by Tor, and everything Tor cannot carry
(UDP, ICMP, IPv6) is blocked. The rules load before the network comes
up and stay in place while Tor connects, so nothing leaks in between.
The setting is kept across restarts and is part of system snapshots.
Programs can also use Tor's SOCKS port directly (127.0.0.1:9050).

Anonymous mode hides where traffic comes from, not who you are: logging
in to an account still identifies you. For browsing, Tor Browser (planned)
protects against much more than routing alone.

## When something goes wrong

```
worm status               # the state of the system's protections
worm panic                # cut the network, stop all blackholes, snapshot
worm panic off            # bring the network back
```

`worm panic` drops all traffic and takes the network devices down, stops
every blackhole (keeping their files), and saves the system and homes as a
pinned snapshot for later analysis.

The boot menu has a **safe mode** entry: it rewinds to the newest
known-good snapshot and starts without network. Snapshots left behind by a
rewind and those saved by `worm panic` never count as known-good. Restart
normally to leave safe mode.

## Privacy and hardening

- DNS goes to a local resolver (unbound) that validates DNSSEC and sends
  queries encrypted (DNS over TLS) to Quad9. The DNS servers a network
  announces are not used. To use other resolvers, put a `forward-zone` in
  `/etc/unbound/unbound.conf.d/` (after `worm unlock`).
- Network devices get a random MAC address at every boot, and IPv6 uses
  temporary addresses for outgoing connections.
- The kernel is locked down (no unsigned modules, no access to kernel
  memory), io_uring is off, and kernel pointers and logs are hidden.
  Settings are in `/usr/lib/sysctl.d/50-wormos.conf`; override them in
  `/etc/sysctl.d/`.
- Users see only their own processes. Programs cannot run from `/dev`,
  `/run` or `/dev/shm`.

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

A Debian 13 (or similar) host with roughly 35 GB of free disk space:

```
sudo apt install build-essential bison flex gawk texinfo m4 help2man \
    libncurses-dev libssl-dev libelf-dev libgmp-dev libmpc-dev rsync curl xz-utils bzip2 \
    btrfs-progs dosfstools mtools fakeroot grub-efi-amd64-bin \
    qemu-system-x86 qemu-utils ovmf zip unzip \
    ninja-build gperf pipx python3-mako python3-yaml python3-ply python3-packaging python3-cairo \
    libexpat1-dev llvm-19-dev libclang-19-dev libclang-cpp19-dev \
    libllvmspirvlib-19-dev spirv-tools libclc-19-dev
pipx install meson==1.12.1
```

Meson comes from pipx because some packages need a newer one than Debian
13 ships (the build puts `~/.local/bin` first in `PATH`). LLVM is only used
on the build machine, to build Mesa's OpenCL compiler (`mesa_clc`) for its
Intel drivers; nothing from LLVM goes into WormOS.

### Build and run

```
make            # toolchain, kernel, packages, repository, root filesystem and disk image
make run        # boot the image in QEMU on the serial console (Ctrl-A X quits)
make run-gui    # boot the image in a QEMU window
make test       # boot the image and run automated checks
```

`make test-wormholes` checks the Debian and Kali wormholes against the real
archives; it needs internet access and takes several minutes.

`make test` also runs `tests/desktop-test.py`: the VM gets a virtio GPU,
keyboard and tablet, and the test types on the graphical console through
the QEMU monitor like a user would. It answers the first-boot setup, logs
in (which starts the desktop), types into the terminal, opens another with
Super+Enter, checks a screendump of the real screen, and compares memory
use and system size with their budgets. `tests/desktop-test.py --release
out/release/wormos-1.0-x86_64.qcow2` runs the same checks on a release
image, where root can only log in once the first-boot setup is done.

`make test` boots a throwaway copy of the image, checks the running system
and exercises snapshots end to end (take, diff, restore, browse, rewind of
the system and of `/home`, and a rewind from the boot menu), then
`worm unlock`/`lock` and the package manager against `out/repo`, served to
the VM over HTTP (install, a trial that rolls back, remove and undo). It
also opens and closes a firewall port and checks it from the host through a
QEMU port forward (`QEMU_HOSTFWD`), tries to escape from a blackhole (as
root and as a normal user), switches anonymous mode on and off (checking
that nothing reaches the host directly), runs `worm panic` and boots safe
mode from the menu. `make test-wormholes` also checks that Tor connects and
traffic really goes through it.

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

The development image has an empty root password (the tests log in with
it). `make release VERSION=1.0` builds the release images instead
(`scripts/release.sh`): root stays locked until the first-boot setup, the
disk is 20 GiB, and the output is a qcow2 image and a VMware bundle in
`out/release`.

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
