#!/usr/bin/env python3
"""Boot the WormOS image in QEMU and check the running system.

Logs in on the serial console and runs a set of checks. It then takes a
snapshot, changes the system, rewinds (a soft reboot that keeps the kernel
running) and checks the change is gone. Last,
it reboots and picks a snapshot from the "Rewind to a snapshot" boot menu.
The full console output is written to build/boot.log.

Usage: tests/boot-test.py [--timeout SECONDS]
"""

import argparse
import os
import re
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "boot.log")
ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")

# (description, shell command, regular expression expected in the output)
CHECKS = [
    ("kernel", "uname -r", r"-wormos"),
    ("os-release", ". /etc/os-release; echo $NAME", r"WormOS"),
    ("init is worminit", "readlink /proc/1/exe", r"/worminit$"),
    ("booted through initramfs", "dmesg | grep -c 'Freeing initrd memory'", r"^1$"),
    ("root on @ subvolume", "grep ' / ' /proc/mounts", r"subvol=/@\s"),
    ("root is read-only", "grep ' / ' /proc/mounts", r"\sro[,\s]"),
    ("root cannot be written", "touch /probe 2>&1", r"Read-only file system"),
    ("var on @var subvolume", "grep ' /var ' /proc/mounts", r"\srw,.*subvol=/@var"),
    ("root home is writable", "touch ~/probe && grep -c ' /root ' /proc/mounts", r"^1$"),
    ("snapshots not mounted", "grep -c snapshots /proc/mounts", r"^0$"),
    ("home on @home subvolume", "grep ' /home ' /proc/mounts", r"subvol=/@home"),
    ("network configured", "ip -4 addr show eth0", r"inet 10\.0\.2\.15"),
    ("syslog running", "pidof syslogd", r"^\d+"),
    ("hostname", "hostname", r"^wormos$"),
]

# Snapshot, change the system, and look at the change.
SNAPSHOT_CHECKS = [
    ("snap now", "snap now -c 'test base'", r"^Snapshot 1 created \(system\)$"),
    ("store stays hidden", "grep -c snapshots /proc/mounts", r"^0$"),
    ("change the system",
     "mount -o remount,bind,rw / && echo x > /etc/probe && mount -o remount,bind,ro / && echo ok",
     r"^ok$"),
    ("snap diff", "snap diff 1", r"^\+ /etc/probe$"),
    ("snap list", "snap list", r"^\s+1\s.*system\s+manual\s+test base$"),
    ("snap verify", "snap verify", r"^snapshot 1 \(system\): ok$"),
]

# After "snap rewind" and the soft reboot it causes.
REWIND_CHECKS = [
    ("services restarted", "pidof syslogd && grep -c ' /var ' /proc/mounts", r"^1$"),
    ("change is gone", "ls /etc/probe 2>&1", r"No such file"),
    ("previous state saved", "snap list", r"^\s+2\s.*auto\s+Before rewind to 1$"),
    ("saved state has the change", "snap diff 1 2", r"^\+ /etc/probe$"),
    ("root still read-only", "grep ' / ' /proc/mounts", r"\sro[,\s]"),
    ("change the system again",
     "mount -o remount,bind,rw / && echo y > /etc/probe2 && mount -o remount,bind,ro / && echo ok",
     r"^ok$"),
]

# After rewinding to snapshot 2 ("Before rewind to 1") from the boot menu.
MENU_CHECKS = [
    ("snapshot 2 state is back", "cat /etc/probe", r"^x$"),
    ("later change is gone", "ls /etc/probe2 2>&1", r"No such file"),
    ("menu rewind saved previous state", "snap list", r"^\s+3\s.*auto\s+Before rewind to 2$"),
]


class Console:
    def __init__(self, proc, log):
        self.proc = proc
        self.log = log
        self.buf = b""
        self.seq = 0

    def _read(self, timeout):
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not ready:
            return True
        chunk = os.read(self.proc.stdout.fileno(), 4096)
        if not chunk:
            return False
        self.buf += chunk
        self.log.write(chunk)
        self.log.flush()
        return True

    def expect(self, token, timeout):
        """Wait for token and return everything before it."""
        token = token.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            if b"end Kernel panic" in self.buf:
                raise RuntimeError("kernel panic")
            if token in self.buf:
                before, _, self.buf = self.buf.partition(token)
                return ANSI.sub(b"", before).decode(errors="replace")
            if not self._read(1):
                break
        raise TimeoutError(f"timed out waiting for {token!r}")

    def send(self, text):
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()

    def run(self, cmd, timeout=60):
        # The marker is assembled by printf, so it never appears in the
        # echoed command line and cannot be matched too early.
        self.seq += 1
        tag = f"__END {self.seq}"
        self.send(f"{cmd}; printf '%s_%s__\\n' {tag}\n")
        out = self.expect(f"__END_{self.seq}__", timeout).replace("\r", "")
        _, _, out = out.rpartition(tag)          # drop the echoed command
        return out.partition("\n")[2].strip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the login prompt")
    args = parser.parse_args()

    # Disk writes are discarded, so the test always starts from the built
    # image and leaves it untouched.
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    failures = 0
    passed = 0
    total = (len(CHECKS) + len(SNAPSHOT_CHECKS) + len(REWIND_CHECKS) +
             len(MENU_CHECKS) + 3)
    start = time.time()

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            print(f"      output: {out!r}")

    def run_checks(con, checks):
        for name, cmd, pattern in checks:
            out = con.run(cmd)
            check(name, re.search(pattern, out, re.MULTILINE) is not None, out)

    def login(con, timeout):
        con.expect("login:", timeout)
        con.send("root\n")
        con.expect("# ", 60)

    with open(LOG, "wb") as log:
        con = Console(proc, log)
        try:
            login(con, args.timeout)
            print(f"booted to login prompt in {time.time() - start:.0f}s")
            run_checks(con, CHECKS)
            run_checks(con, SNAPSHOT_CHECKS)

            boot_id = con.run("cat /proc/sys/kernel/random/boot_id")
            con.send("snap rewind -y\n")
            out = con.expect("login:", args.timeout)
            check("rewind applied by soft reboot",
                  "rewound system to snapshot 1" in out and "soft reboot" in out,
                  out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            out = con.run("cat /proc/sys/kernel/random/boot_id")
            check("kernel kept running", boot_id.splitlines()[-1] in out, out)
            run_checks(con, REWIND_CHECKS)

            # Reboot and pick the newest snapshot from the boot menu: down to
            # the submenu, enter it, take its first entry.
            con.send("reboot\n")
            con.expect("Rewind to a snapshot", args.timeout)
            con.send("\x1b[B")
            time.sleep(0.5)
            con.send("\r")
            con.expect("Snapshot 2", 60)
            time.sleep(0.5)
            con.send("\r")
            out = con.expect("login:", args.timeout)
            check("boot menu rewind applied", "rewound system to snapshot 2" in out,
                  out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            run_checks(con, MENU_CHECKS)

            con.send("poweroff\n")
            con.expect("Power down", 120)
            proc.wait(timeout=60)
            print("powered off cleanly")
        except (TimeoutError, RuntimeError, subprocess.TimeoutExpired) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()

    print(f"{passed}/{total} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
