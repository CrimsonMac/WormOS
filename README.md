# WormOS

A lightweight, security-focused Linux operating system built around instant, tamper-resistant snapshots.

> **Status:** early development. Nothing is bootable yet.

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
- Snapshots protected from the running system and verified at boot.
- A firewall enabled by default.
- Tor and Tor Browser integration.
- Optional tool kits (forensics, blue team, network analysis), installed on demand.
- An independent init system and package manager.
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

Build instructions will be added once the base system is in place.
Development currently targets x86_64 and is tested in QEMU.
