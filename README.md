# newos

An independent Linux-based operating system built from source, designed to be lightweight and fully reversible.

> **Status:** early development. Nothing is bootable yet.

## Goals

- **Independent:** built on the Linux kernel, with its own init system, package manager and desktop shell.
- **Immutable with full history:** every system change creates a new generation. Generations can be browsed, compared, rolled back to, or booted directly from the boot menu.
- **Lightweight:** a modern Wayland desktop with a small footprint (target ISO under 600 MB, idle memory under 400 MB).
- **Simple:** a small, readable codebase where every component is understood.

## Roadmap

1. Minimal bootable base system (kernel, libc, init)
2. Package manager and generation system
3. Graphics stack and Wayland compositor
4. Desktop shell and timeline application
5. Bootable ISO and installer

## Building

Build instructions will be added once the base system is in place.
Development currently targets x86_64 and is tested in QEMU.
