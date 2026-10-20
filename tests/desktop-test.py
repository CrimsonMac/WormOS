#!/usr/bin/env python3
"""Boot the WormOS image with a virtual GPU and check the desktop.

The VM gets a virtio GPU, keyboard and tablet and a QEMU monitor socket.
The test logs in as root on the serial console for checks, and types on
the graphical console through the monitor (sendkey), as a user would:
it answers the first-boot setup on tty1, logs in there (which starts the
desktop), types into the terminal and opens a new one with a shortcut.
It also checks the screen itself (screendump) and measures memory use.
The full console output is written to build/desktop.log.

Usage: tests/desktop-test.py [--timeout SECONDS]
"""

import argparse
import importlib.util
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "desktop.log")

# The serial console helper is shared with the boot test.
_spec = importlib.util.spec_from_file_location(
    "boot_test", os.path.join(ROOT, "tests", "boot-test.py"))
boot_test = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(boot_test)

USER = "tester"
USER_PASSWORD = "Worm-hole 42"
ROOT_PASSWORD = "Root:pass 7"

# Budgets from the project goals.
IDLE_RAM_MB = 400
ROOTFS_MB = 600

DEVICE_CHECKS = [
    ("device manager running", "pidof udevd", r"^\d+"),
    ("modules come from kmod", "readlink /usr/sbin/modprobe", r"kmod$"),
    ("module loaded on demand", "modprobe uinput && lsmod | grep -c '^uinput '", r"^1$"),
    ("GPU device for the video group", "stat -c '%G %a' /dev/dri/card0", r"^video 660$"),
    ("render node for everyone", "stat -c '%G %a' /dev/dri/renderD128", r"^render 666$"),
    ("input devices for the input group",
     "stat -c '%G %a' /dev/input/event0", r"^input 660$"),
    ("devices have udev properties",
     "udevadm info /dev/input/event0 | grep -c ID_INPUT=1", r"^1$"),
    ("seat manager running", "pidof seatd", r"^\d+"),
    ("seat socket for the video group", "stat -c '%G %a' /run/seatd.sock", r"^video 770$"),
    ("Intel GPU firmware present", "ls /usr/lib/firmware/i915 | grep -c 'dmc.*\\.bin\\.zst'", r"^[1-9]"),
    ("su is setuid root", "stat -c '%U %a' /usr/bin/su", r"^root 4755$"),
    ("first-boot setup waiting on tty1", "cat /dev/vcs1", r"Username:"),
]

AFTER_SETUP_CHECKS = [
    ("user created", f"grep -c '^{USER}:x:1000:' /etc/passwd", r"^1$"),
    ("user in the desktop groups",
     f"id -Gn {USER} | tr ' ' '\\n' | grep -cxE 'wheel|video|input|render'", r"^4$"),
    ("user has a password", f"grep '^{USER}:' /etc/shadow | cut -d: -f2", r"^\$6\$"),
    ("root has a password", "grep '^root:' /etc/shadow | cut -d: -f2", r"^\$6\$"),
    ("root is read-only again", "touch /probe 2>&1", r"Read-only file system"),
    ("setup snapshot taken", "snap list", r"First boot: created " + USER),
    ("runtime directory", "stat -c '%U %a' /run/user/1000", rf"^{USER} 700$"),
    ("setup not offered again", "/usr/lib/worm/first-boot --needed; echo rc=$?", r"rc=1"),
]

SESSION_CHECKS = [
    ("compositor running as the user", "ps -o user,comm | grep -w labwc",
     rf"^{USER}\s+labwc"),
    ("wayland socket", "ls /run/user/1000/", r"wayland-0"),
    ("software renderer on a virtual GPU", "grep -c 'pixman' /run/user/1000/desktop.log",
     r"^[1-9]"),
    ("wallpaper running", "pidof swaybg", r"^\d+"),
    ("welcome terminal open", "pidof foot", r"^\d+$"),
]

TYPING_CHECKS = [
    ("typed into the terminal", "cat /tmp/typed", rf"^{USER} typed$"),
]

SHORTCUT_CHECKS = [
    ("Super+Enter opens a terminal", "pidof foot | wc -w", r"^2$"),
]

GRIM = (f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "
        "grim /tmp/shot.png' && od -An -c -N4 /tmp/shot.png")
BUDGET_CHECKS = [
    ("screenshot with grim", GRIM, r"P\s+N\s+G"),
]


class Monitor:
    """The QEMU monitor (HMP) on a UNIX socket."""

    # sendkey names for characters that are not plain letters or digits.
    KEYS = {
        " ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot",
        ",": "comma", "/": "slash", ";": "semicolon", "'": "apostrophe",
        "_": "shift-minus", ":": "shift-semicolon", ">": "shift-dot",
        "<": "shift-comma", '"': "shift-apostrophe", "!": "shift-1",
        "?": "shift-slash", "|": "shift-backslash", "$": "shift-4",
        "(": "shift-9", ")": "shift-0", "*": "shift-8", "&": "shift-7",
    }

    def __init__(self, path):
        deadline = time.time() + 30
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.read()

    def read(self):
        data = b""
        self.sock.settimeout(10)
        while not data.endswith(b"(qemu) "):
            data += self.sock.recv(4096)
        return data.decode(errors="replace")

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        return self.read()

    def key(self, name):
        self.cmd(f"sendkey {name}")
        time.sleep(0.08)

    def type(self, text):
        for ch in text:
            if ch in self.KEYS:
                self.key(self.KEYS[ch])
            elif ch.isupper():
                self.key(f"shift-{ch.lower()}")
            else:
                self.key(ch)


def read_ppm(path):
    """Return (width, height, pixels) of a binary PPM (P6) file."""
    with open(path, "rb") as f:
        data = f.read()
    fields = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    width, height = int(fields.group(1)), int(fields.group(2))
    return width, height, data[fields.end():]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the login prompt")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="wormos-desktop-")
    mon_path = os.path.join(tmp, "monitor")
    extra = (f"-vga none -device virtio-gpu-pci,addr=0x10 "
             f"-device virtio-keyboard-pci,addr=0x11 -device virtio-tablet-pci,addr=0x12 "
             f"-monitor unix:{mon_path},server,nowait")
    env = dict(os.environ, QEMU_EXTRA=extra, QEMU_MEM="1024")
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0
    total = (len(DEVICE_CHECKS) + len(AFTER_SETUP_CHECKS) + len(SESSION_CHECKS) +
             len(TYPING_CHECKS) + len(SHORTCUT_CHECKS) + len(BUDGET_CHECKS) + 5)

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

    def wait_for(con, cmd, pattern, timeout):
        """Run cmd until its output matches; return the last output."""
        deadline = time.time() + timeout
        while True:
            out = con.run(cmd)
            if re.search(pattern, out, re.MULTILINE) or time.time() > deadline:
                return out
            time.sleep(2)

    start = time.time()
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            con.send("root\n")
            con.expect("# ", 60)
            print(f"booted to login prompt in {time.time() - start:.0f}s")
            mon = Monitor(mon_path)
            run_checks(con, DEVICE_CHECKS)

            # First-boot setup on tty1, typed on the keyboard.
            mon.type(USER + "\n")
            for secret in (USER_PASSWORD, USER_PASSWORD, ROOT_PASSWORD, ROOT_PASSWORD):
                time.sleep(1)
                mon.type(secret + "\n")
            out = wait_for(con, "cat /dev/vcs1", r"Done\. Log in as", 300)
            check("first-boot setup finished", "Done. Log in as" in out, out[-300:])
            run_checks(con, AFTER_SETUP_CHECKS)

            # Log in on tty1: the desktop starts.
            wait_for(con, "cat /dev/vcs1", r"login:", 60)
            mon.type(USER + "\n")
            time.sleep(2)
            mon.type(USER_PASSWORD + "\n")
            out = wait_for(con, "pidof foot", r"^\d+", 300)
            check("desktop started after login", re.search(r"^\d+", out, re.M) is not None,
                  con.run("tail -n 20 /run/user/1000/desktop.log"))
            time.sleep(5)
            run_checks(con, SESSION_CHECKS)

            # The welcome terminal has the focus.
            mon.type("echo $(id -un) typed > /tmp/typed\n")
            wait_for(con, "cat /tmp/typed", r"typed", 60)
            run_checks(con, TYPING_CHECKS)

            mon.key("meta_l-ret")
            wait_for(con, "pidof foot | wc -w", r"^2$", 60)
            run_checks(con, SHORTCUT_CHECKS)

            # What is really on the screen: the wallpaper's purple and the
            # dark terminal, not a blank or text console.
            time.sleep(3)
            shot = os.path.join(tmp, "screen.ppm")
            mon.cmd(f"screendump {shot}")
            time.sleep(1)
            width, height, pixels = read_ppm(shot)
            colors = {pixels[i:i + 3] for i in range(0, len(pixels), 3 * 997)}
            purple = sum(1 for c in colors if c[2] > c[1] + 40 and c[0] > c[1] + 20)
            check("screen shows the desktop",
                  width >= 1024 and len(colors) > 50 and purple > 5,
                  f"{width}x{height}, {len(colors)} colours, {purple} purple")

            run_checks(con, BUDGET_CHECKS)
            used = con.run("free -m | awk '/^Mem:/ { print $3 }'")
            mb = int(used.split()[-1]) if used.split() else 0
            check(f"idle memory with the desktop: {mb} MB (budget {IDLE_RAM_MB})",
                  0 < mb <= IDLE_RAM_MB, used)
            size = con.run("du -smx / | cut -f1")
            mb = int(size.split()[-1]) if size.split() else 0
            check(f"system size: {mb} MB (budget {ROOTFS_MB})", 0 < mb <= ROOTFS_MB, size)

            con.send("poweroff\n")
            con.expect("Power down", 120)
            proc.wait(timeout=60)
            print("powered off cleanly")
        except (TimeoutError, RuntimeError, OSError, subprocess.TimeoutExpired) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()

    print(f"{passed}/{total} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
