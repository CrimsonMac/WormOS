#!/usr/bin/env python3
"""Boot the WormOS image in QEMU and check the running system.

Logs in on the serial console, runs a set of checks and powers the machine
off. The full console output is written to build/boot.log.

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

    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    failures = 0
    passed = 0
    start = time.time()

    with open(LOG, "wb") as log:
        con = Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            print(f"booted to login prompt in {time.time() - start:.0f}s")
            con.send("root\n")
            con.expect("# ", 60)

            for name, cmd, pattern in CHECKS:
                out = con.run(cmd)
                ok = re.search(pattern, out, re.MULTILINE) is not None
                passed += ok
                failures += not ok
                print(f"{'PASS' if ok else 'FAIL'}  {name}")
                if not ok:
                    print(f"      output: {out!r}")

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

    print(f"{passed}/{len(CHECKS)} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
