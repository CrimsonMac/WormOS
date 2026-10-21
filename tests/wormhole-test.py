#!/usr/bin/env python3
"""Check wormholes: software from Debian and Kali in containers.

Boots a throwaway copy of the image (like boot-test.py) and needs internet
access: it sets up the Debian and Kali wormholes from their real archives,
installs a package from each, runs the exported commands as root and as a
normal user, checks what the containers can and cannot touch, and removes
the packages again. Takes several minutes. Console output goes to
build/wormhole.log.

With --dns ADDR the VM resolves names through ADDR instead of the DNS
relay of QEMU, which forwards to the first nameserver of this machine.

Usage: tests/wormhole-test.py [--timeout SECONDS] [--dns ADDR]
"""

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "wormhole.log")

spec = importlib.util.spec_from_file_location(
    "boot_test", os.path.join(ROOT, "tests", "boot-test.py"))
boot_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot_test)

CHECKS = [
    # The first answer needs a TLS handshake and DNSSEC priming: slow under TCG.
    ("encrypted DNS resolves",
     "i=0; until nslookup deb.debian.org >/dev/null 2>&1 || [ $i -ge 30 ]; do sleep 2; i=$((i+1)); done; "
     "nslookup deb.debian.org 2>&1 | tail -3", r"Address:?\s+[0-9a-f.:]+"),
    ("debian install", "wh -y install --from debian htop 2>&1 | tail -3",
     r"htop -> /usr/bin/htop \(debian\)"),
    ("debian wormhole ready", "cat /var/lib/wormholes/debian/etc/os-release",
     r"Debian GNU/Linux 13"),
    ("exported command", "head -2 /usr/local/bin/htop", r"Exported by wh from the debian wormhole"),
    ("runs as root", "htop --version", r"^htop 3\."),
    ("listed", "wh list | tail -2", r"^htop\s+\S+\s+\[debian\]$"),
    ("found by search", "wh search htop", r"^htop .*\[debian\]$"),
    ("no setuid escape", "mount | grep ' /var/lib/wormholes '", r"nosuid,nodev"),
    ("add a user", "worm unlock >/dev/null && adduser -D tester && worm lock >/dev/null && echo ok",
     r"^ok$"),
    ("runs as a user", "su tester -c 'htop --version'", r"^htop 3\."),
    ("user keeps their identity", "su tester -c 'whc debian id -un'", r"^tester$"),
    ("user cannot change the container",
     "su tester -c 'whc debian touch /usr/x' 2>&1", r"(Permission denied|Read-only)"),
    ("user sees their home", "su tester -c 'cd && touch here && whc debian ls ~/here'",
     r"/home/tester/here$"),
    ("host network shared", "whc debian cat /etc/resolv.conf | grep -c nameserver", r"^[1-9]"),
    # Wayland programs from wormholes appear on the desktop: here a
    # headless compositor, run by the user, and Debian's event viewer.
    ("wayland program from debian", "wh -y install --from debian wev 2>&1 | tail -3",
     r"wev -> /usr/bin/wev \(debian\)"),
    ("system fonts shared", "su tester -c 'whc debian ls /usr/local/share/fonts/wormos'",
     r"\bhack\b"),
    ("wayland program shows a window",
     "/usr/lib/worm/runtime-dirs; su tester -c 'export XDG_RUNTIME_DIR=/run/user/1000 "
     "WLR_BACKENDS=headless WLR_RENDERER=pixman; labwc > /tmp/labwc.log 2>&1 & "
     # script gives wev a terminal, so its output is not lost in a buffer.
     "sleep 5; WAYLAND_DISPLAY=wayland-0 timeout 10 whc debian script -qfc wev /tmp/wev.log "
     "> /dev/null 2>&1; "
     "grep -c xdg_toplevel /tmp/wev.log'; kill $(pidof labwc)", r"^[1-9]"),
    ("core command not shadowed", "ls /usr/local/bin/ls 2>&1", r"No such file"),
    ("debian remove", "wh -y remove htop && ls /usr/local/bin/htop 2>&1",
     r"No such file"),
    ("kali fallback", "wh -y install --from kali hashid 2>&1 | tail -3",
     r"hashid -> .* \(kali\)"),
    ("kali tool runs", "hashid --version 2>&1", r"^(?i:hashid) v\d"),
    ("kali wormhole", "cat /var/lib/wormholes/kali/etc/os-release", r"Kali GNU/Linux"),
    ("system still read-only", "touch /usr/x 2>&1", r"Read-only file system"),
    ("anonymous mode on", "worm anon on", r"^Anonymous mode is on"),
    ("tor connects",
     "i=0; until worm anon status | grep -q connected || [ $i -ge 300 ]; do sleep 2; i=$((i+1)); done; "
     "worm anon status", r"connected"),
    ("traffic goes through Tor",
     "curl -s -m 60 https://check.torproject.org/api/ip", r'"IsTor":\s*true'),
    ("names resolve through Tor", "nslookup deb.debian.org 2>&1 | tail -2", r"Address"),
    ("anonymous mode off", "worm anon off", r"^Anonymous mode is off"),
    ("direct again", "curl -s -m 60 https://check.torproject.org/api/ip", r'"IsTor":\s*false'),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=3600,
                        help="seconds to wait for each step")
    parser.add_argument("--dns", metavar="ADDR",
                        help="name server for the VM, instead of QEMU's relay")
    args = parser.parse_args()

    env = dict(os.environ, QEMU_MEM=os.environ.get("QEMU_MEM", "2048"))
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", 600)
            con.send("root\n")
            con.expect("# ", 60)
            con.run("dmesg -n 1")
            if args.dns:
                # After DHCP has written its own (the lease outlasts the test).
                con.run("while [ ! -s /run/resolv.conf ]; do sleep 1; done; "
                        f"echo 'nameserver {args.dns}' > /run/resolv.conf")
            for name, cmd, pattern in CHECKS:
                start = time.time()
                out = con.run(cmd, args.timeout)
                ok = re.search(pattern, out, re.MULTILINE) is not None
                passed += ok
                failures += not ok
                print(f"{'PASS' if ok else 'FAIL'}  {name} ({time.time() - start:.0f}s)")
                if not ok:
                    print(f"      output: {out[-400:]!r}")
            con.send("poweroff\n")
            con.expect("Power down", 120)
        except (TimeoutError, RuntimeError) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()
    print(f"{passed}/{len(CHECKS)} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
