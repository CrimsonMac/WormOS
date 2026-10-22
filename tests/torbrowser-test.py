#!/usr/bin/env python3
"""Set up Tor Browser on the desktop as a user would, and start it.

Needs internet access: the browser comes from torproject.org. The VM gets
the desktop test's virtual GPU, keyboard and tablet. After the first-boot
setup the user opens Tor Browser, whose window offers the download (its
libraries are in the image), then downloads and checks the browser
(tor-browser --download) and starts it, once normally and once in
anonymous mode, where it must use the system's Tor. The console output is written to build/torbrowser.log.

Usage: tests/torbrowser-test.py [--timeout SECONDS]
"""

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "torbrowser.log")


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", file))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")
desktop = load("desktop_test", "desktop-test.py")
USER, USER_PASSWORD, ROOT_PASSWORD = desktop.USER, desktop.USER_PASSWORD, desktop.ROOT_PASSWORD
BROWSER = f"/home/{USER}/.local/share/tor-browser"

RUNTIME_CHECKS = [
    ("libraries in the image", "wh list | grep -c -e '^tor-browser-runtime ' -e '^gtk3 '",
     r"^2$"),
    ("X11 stand-ins present", "ls /usr/lib/tor-browser/libx11-compat.so", r"compat\.so$"),
]

DOWNLOAD_CHECKS = [
    ("version recorded", f"cat {BROWSER}/version", r"^\d+\.\d+(\.\d+)*$"),
    ("browser belongs to the user", f"stat -c %U {BROWSER}/tor-browser/Browser/firefox.real",
     rf"^{USER}$"),
    ("no download left behind", f"ls {BROWSER}/download | wc -l", r"^0$"),
]

# When the browser does not start: run it as the launcher does, with its output.
DIAGNOSE = r"""cd ~/.local/share/tor-browser/tor-browser/Browser || exit 1
HOME=$PWD GSETTINGS_BACKEND=memory MOZ_ENABLE_WAYLAND=1 GDK_BACKEND=wayland \
LD_PRELOAD=/usr/lib/tor-browser/libx11-compat.so timeout 90 ./firefox > /tmp/tb-diag.log 2>&1
echo "exit status $?" >> /tmp/tb-diag.log
"""

ENVIRON = "tr '\\0' '\\n' < /proc/$(pidof -s firefox.real)/environ | grep -E '^(TOR_|LD_PRELOAD|HOME)'"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, default=600,
                        help="seconds to wait for the login prompt")
    args = parser.parse_args()

    tmp = tempfile.mkdtemp(prefix="wormos-torbrowser-")
    mon_path = os.path.join(tmp, "monitor")
    extra = (f"-vga none -device virtio-gpu-pci,addr=0x10 "
             f"-device virtio-keyboard-pci,addr=0x11 -device virtio-tablet-pci,addr=0x12 "
             f"-monitor unix:{mon_path},server,nowait")
    env = dict(os.environ, QEMU_EXTRA=extra, QEMU_MEM="3072")
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0
    total = len(RUNTIME_CHECKS) + len(DOWNLOAD_CHECKS) + 8

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
        deadline = time.time() + timeout
        while True:
            out = con.run(cmd)
            if re.search(pattern, out, re.MULTILINE) or time.time() > deadline:
                return out
            time.sleep(3)

    def screen(name):
        path = os.path.join(ROOT, "build", f"torbrowser-{name}.ppm")
        mon.cmd(f"screendump {path}")
        time.sleep(1)
        return desktop.read_ppm(path)

    def session(con, cmd, timeout=60):
        return con.run(f"su {USER} -c '/tmp/in-session {cmd}'", timeout)

    def root_login(con, timeout):
        con.expect("login:", timeout)
        con.send("root\n")
        con.expect("# ", 60)
        desktop.put_file(con, desktop.IN_SESSION.encode(), "/tmp/in-session")

    def browser_started(con, label):
        out = wait_for(con, "pidof firefox.real | wc -w", r"^[1-9]", 240)
        started = re.search(r"^[1-9]", out, re.M) is not None
        time.sleep(45)
        out = con.run("pidof firefox.real | wc -w")
        ok = started and re.search(r"^[1-9]", out, re.M) is not None
        if not ok:
            desktop.put_file(con, DIAGNOSE.encode(), "/tmp/tb-diag.sh")
            session(con, "sh /tmp/tb-diag.sh", 180)
            out = con.run("tail -n 25 /tmp/tb-diag.log")
        check(f"Tor Browser starts and keeps running ({label})", ok, out)

    start = time.time()
    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            root_login(con, args.timeout)
            print(f"booted to login prompt in {time.time() - start:.0f}s")
            mon = desktop.Monitor(mon_path)
            # A busy browser under emulation slows QEMU's answers down.
            mon.timeout = 120

            # The graphical first-boot setup, as in the desktop test.
            wait_for(con, desktop.GREETER_MODE, r"MODE=setup", 300)
            deadline = time.time() + 300
            while time.time() < deadline:
                w, h, px = screen("setup")
                if desktop.solid_block(px, w, h, desktop.ACCENT):
                    break
                time.sleep(5)
            mon.key("ret")
            time.sleep(2)
            mon.type("Tester\n")
            time.sleep(1)
            mon.type(USER + "\n")
            for secret in (USER_PASSWORD, USER_PASSWORD, ROOT_PASSWORD, ROOT_PASSWORD):
                time.sleep(2)
                mon.type(secret + "\n")
            out = wait_for(con, "pidof worm-shell", r"^\d+$", 300)
            check("desktop started", re.search(r"^\d+$", out, re.M) is not None, out)
            time.sleep(10)

            # The libraries are in the image: the first run offers the download.
            run_checks(con, RUNTIME_CHECKS)
            con.run(f"su {USER} -c '/tmp/in-session tor-browser > /tmp/tb.log 2>&1 &'")
            out = wait_for(con, "pidof tor-browser", r"^\d+$", 120)
            time.sleep(8)
            w, h, px = screen("setup")
            check("setup window offers the download",
                  re.search(r"^\d+$", out, re.M) is not None and
                  desktop.solid_block(px, w, h, desktop.ACCENT) is not None,
                  con.run("cat /tmp/tb.log"))
            con.run("kill $(pidof tor-browser)")

            # Download and check the browser.
            out = session(con, "tor-browser --download", 3600)
            check("downloaded, checked and unpacked", "is ready" in out, out[-500:])
            run_checks(con, DOWNLOAD_CHECKS)

            before = screen("before-browser")[2]
            session(con, "tor-browser")
            browser_started(con, "own Tor")
            # Its window takes a while to appear under emulation.
            deadline = time.time() + 240
            while True:
                w, h, after = screen("browser")
                changed = desktop.region_diff(before, after, w, (0, 40, w, h))
                if changed > w * h // 4 // 10 or time.time() > deadline:
                    break
                time.sleep(10)
            check("browser window on the screen", changed > w * h // 4 // 10,
                  f"{changed} pixels changed")
            out = con.run(ENVIRON)
            check("X11 stand-ins preloaded, private home",
                  re.search(r"^LD_PRELOAD=.*/usr/lib/tor-browser/libx11-compat\.so", out, re.M) and
                  f"HOME={BROWSER}/tor-browser/Browser" in out and "TOR_PROVIDER" not in out,
                  out)

            # Anonymous mode: the system's Tor carries the browser.
            con.run("kill $(pidof firefox.real)")
            time.sleep(5)
            con.run("worm anon on", 120)
            session(con, "tor-browser")
            browser_started(con, "anonymous mode")
            out = con.run(ENVIRON)
            check("uses the system Tor in anonymous mode",
                  "TOR_PROVIDER=none" in out and "TOR_SKIP_LAUNCH=1" in out and
                  "TOR_SOCKS_PORT=9050" in out, out)
            con.run("kill $(pidof firefox.real); worm anon off", 120)

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
