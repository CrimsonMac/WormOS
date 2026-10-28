#!/usr/bin/env python3
"""Boot WormOS in VMware Workstation and check it there.

Runs from WSL on a Windows PC with VMware Workstation. The test builds a VM
from the current image (or a release zip) in a Windows folder, starts it
with a window, and checks the VMware side of the system: tools, drivers,
network, clock, sound, the graphical first-boot setup, the desktop, the
pointer, the screen size following the window, copying text to the Windows
clipboard, the renderer (OpenGL ES with 3D acceleration, software without)
and a clean shutdown.

VMware cannot type into a VM from the command line, so the guest gets a
virtual keyboard and pointer (tests/input-tool.c over uinput). The serial
port connects to this script over TCP: VMware is the client of a port that
WSL forwards from the Windows side. Screens are captured from the VMware
window by a PowerShell script (vmshot.ps1).

Variants: --no-3d (3D acceleration off), --firmware bios, --disk sata|pvscsi|
lsilogic (instead of NVMe), --nic e1000e|e1000 (instead of vmxnet3). The
VM uses scripts/wormos.vmx.in, as the released VMware zip does. The full
console output goes to build/vmware.log. The VM is deleted afterwards
unless --keep is given.

Copying from Windows into the VM is not checked: VMware hands its text over
only when the real pointer enters the VM's window.

Usage: tests/vmware-test.py [--no-3d] [--firmware F] [--disk D] [--nic N] [--keep]
"""

import argparse
import importlib.util
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "build", "vmware.log")
VMRUN = os.environ.get("WORMOS_VMRUN", "/mnt/d/VMware Workstation Pro/vmrun.exe")
VMDIR = os.environ.get("WORMOS_VMWARE_DIR", "/mnt/d/wormos-tools/vms/test")
VMSHOT = os.environ.get("WORMOS_VMSHOT", "D:/wormos-tools/vmshot.ps1")


def load(name):
    spec = importlib.util.spec_from_file_location(
        name.replace("-", "_"), os.path.join(ROOT, "tests", f"{name}.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot-test")
desktop = load("desktop-test")
USER, USER_PASSWORD, ROOT_PASSWORD = desktop.USER, desktop.USER_PASSWORD, desktop.ROOT_PASSWORD

DISKS = {
    "nvme": ("nvme0", None, "nvme0n1"),
    "sata": ("sata0", None, "sda"),
    "pvscsi": ("scsi0", "pvscsi", "sda"),
    "lsilogic": ("scsi0", "lsilogic", "sda"),
}
NIC_DRIVERS = {"vmxnet3": "vmxnet3", "e1000e": "e1000e", "e1000": "e1000"}

# Runs a command as on the desktop, with the session's environment.
IN_SESSION = desktop.IN_SESSION


def winpath(path):
    return subprocess.run(["wslpath", "-w", path], check=True, capture_output=True,
                          text=True).stdout.strip()


def vmrun(*args, timeout=120):
    """Run vmrun.exe (from a Windows folder, so cmd.exe does not complain)."""
    out = subprocess.run([VMRUN, "-T", "ws", *args], cwd=VMDIR, capture_output=True,
                         text=True, timeout=timeout)
    return (out.stdout + out.stderr).replace("\r", "").strip()


def running(vmx):
    return winpath(vmx).lower() in vmrun("list").lower()


def read_vmx(path):
    entries = {}
    with open(path) as f:
        for line in f:
            key, sep, value = line.partition("=")
            if sep:
                entries[key.strip()] = value.strip().strip('"')
    return entries


def write_vmx(path, entries):
    with open(path, "w", newline="\n") as f:
        for key, value in entries.items():
            f.write(f'{key} = "{value}"\n')


def make_vm(args, port):
    """Create the VM folder; return the .vmx path."""
    vmx = os.path.join(VMDIR, "WormOS.vmx")
    if os.path.exists(vmx) and running(vmx):
        vmrun("stop", winpath(vmx), "hard")
    shutil.rmtree(VMDIR, ignore_errors=True)
    os.makedirs(VMDIR)
    disk = os.path.join(VMDIR, "wormos.vmdk")
    subprocess.run(["qemu-img", "convert", "-O", "vmdk", "-o", "subformat=monolithicSparse",
                    os.path.join(ROOT, "out", "wormos.img"), disk], check=True)
    template = os.path.join(ROOT, "scripts", "wormos.vmx.in")
    with open(template) as f:
        text = f.read().replace("@VERSION@", "test").replace("@DISK@", "wormos.vmdk")
    with open(vmx, "w") as f:
        f.write(text)
    entries = read_vmx(vmx)
    if args.disk != "nvme":
        disk_file = entries.pop("nvme0:0.fileName")
        for key in ("nvme0.present", "nvme0:0.present"):
            entries.pop(key)
        bus, dev, _ = DISKS[args.disk]
        entries[f"{bus}.present"] = "TRUE"
        if dev:
            entries[f"{bus}.virtualDev"] = dev
        entries[f"{bus}:0.present"] = "TRUE"
        entries[f"{bus}:0.fileName"] = disk_file
    entries["ethernet0.virtualDev"] = args.nic
    entries["firmware"] = args.firmware
    if args.firmware == "bios":
        entries.pop("uefi.secureBoot.enabled", None)
    entries["mks.enable3d"] = "TRUE" if args.three_d else "FALSE"
    entries["displayName"] = "WormOS test"
    # The serial console: VMware connects to this script.
    entries.update({
        "serial0.present": "TRUE",
        "serial0.fileType": "network",
        "serial0.fileName": f"tcp://127.0.0.1:{port}",
        "serial0.network.endPoint": "client",
        "serial0.yieldOnMsrRead": "TRUE",
    })
    write_vmx(vmx, entries)
    return vmx


class SocketProc:
    """What boot_test.Console needs from a process, over a socket."""

    class _In:
        def __init__(self, sock):
            self.sock = sock

        def write(self, data):
            self.sock.sendall(data)

        def flush(self):
            pass

    def __init__(self, sock):
        self.stdout = sock
        self.stdin = self._In(sock)


def read_png(path):
    """Return (width, height, RGB pixels) of an 8-bit RGB or RGBA PNG."""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, width = 8, b"", 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and color in (2, 6), (depth, color)
            bpp = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * bpp
    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if bpp == 4:
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = out[0::4], out[1::4], out[2::4]
        out = rgb
    return width, height, bytes(out)


def capture(name, keep=False):
    """Capture the VMware window; return (width, height, pixels). With
    keep, the PNG stays in build/ for a look."""
    path = os.path.join(VMDIR, f"{name}.png")
    subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    VMSHOT, winpath(path)], capture_output=True, timeout=60)
    shot = read_png(path)
    if keep:
        shutil.move(path, os.path.join(ROOT, "build", f"vmware-{name}.png"))
    else:
        os.remove(path)
    return shot


def changed_box(a, b, width, height):
    """Pixels that differ between two captures, and their bounding box."""
    n, xs, ys = 0, [], []
    for y in range(0, height, 2):
        row = 3 * y * width
        for x in range(0, width, 2):
            i = row + 3 * x
            if a[i:i + 3] != b[i:i + 3]:
                n += 1
                xs.append(x)
                ys.append(y)
    if not n:
        return 0, None
    return n, (min(xs), min(ys), max(xs), max(ys))


def build_input_tool():
    """Compile tests/input-tool.c with the WormOS toolchain; return its bytes."""
    out = os.path.join(ROOT, "build", "input-tool")
    subprocess.run([os.path.join(ROOT, "build", "tools", "bin", "x86_64-wormos-linux-gnu-gcc"),
                    "-O2", "-s", "-o", out, os.path.join(ROOT, "tests", "input-tool.c")],
                   check=True)
    with open(out, "rb") as f:
        return f.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-3d", dest="three_d", action="store_false",
                        help="turn off VMware's 3D acceleration (on in the shipped .vmx)")
    parser.add_argument("--firmware", choices=("efi", "bios"), default="efi")
    parser.add_argument("--disk", choices=sorted(DISKS), default="nvme")
    parser.add_argument("--nic", choices=sorted(NIC_DRIVERS), default="vmxnet3")
    parser.add_argument("--keep", action="store_true", help="keep the VM afterwards")
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    vmx = make_vm(args, port)
    win_vmx = winpath(vmx)

    passed = failures = 0

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += ok
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}", flush=True)
        if not ok:
            print(f"      output: {out!r}", flush=True)

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
            time.sleep(2)

    start = time.time()
    starter = subprocess.Popen([VMRUN, "-T", "ws", "start", win_vmx, "gui"], cwd=VMDIR,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    listener.settimeout(120)
    sock, _ = listener.accept()
    listener.close()
    _, disk_bus_dev, disk_name = DISKS[args.disk]

    with open(LOG, "wb") as log:
        con = boot_test.Console(SocketProc(sock), log)
        try:
            con.expect("login:", args.timeout)
            print(f"booted to login prompt in {time.time() - start:.0f}s", flush=True)
            con.send("root\n")
            con.expect("# ", 60)

            def input(cmd):
                con.run(f"printf '%s\\n' '{cmd}' > /tmp/input")
                # Traffic on VMware's network serial port while keys are
                # typed loses keys in the guest: wait until the input tool
                # is done (about 40 ms a key, more with Shift).
                if cmd.startswith("type "):
                    time.sleep(0.6 + 0.07 * len(cmd))

            def first_boot():
                input("key ret")                     # Get started
                time.sleep(2)
                input("type Tester\\n")
                time.sleep(1)
                input(f"type {USER}\\n")
                for secret in (USER_PASSWORD, USER_PASSWORD):
                    time.sleep(1)
                    input(f"type {secret}\\n")
                time.sleep(2)
                for secret in (ROOT_PASSWORD, ROOT_PASSWORD):
                    time.sleep(1)
                    input(f"type {secret}\\n")

            nic = NIC_DRIVERS[args.nic]
            run_checks(con, [
                ("VMware detected", "vmware-checkvm", r"\(good\)"),
                ("VMware tools running", "pidof vmtoolsd", r"^\d+$"),
                ("VMware GPU driver", "basename $(readlink /sys/class/drm/card0/device/driver)",
                 r"^vmwgfx$"),
                ("VMware support modules", "lsmod | cut -d' ' -f1 | grep -cxE 'vmw_vmci|vmw_balloon'",
                 r"^2$"),
                ("network card driver",
                 "for n in /sys/class/net/*/device; do basename $(readlink $n/driver); done",
                 rf"^{nic}$"),
                ("disk found", "ls /sys/block", rf"\b{disk_name}\b"),
                ("sound card found", "cat /proc/asound/cards", r"HDA-Intel"),
                ("firmware type", "test -d /sys/firmware/efi && echo efi || echo bios",
                 rf"^{args.firmware}$"),
                ("VMware's socket transport for the tools",
                 "lsmod | grep -c '^vmw_vsock_vmci_transport '", r"^1$"),
                ("power button watched",
                 "ls -l /proc/$(pidof powerkey)/fd | grep -c /dev/input/event", r"^[1-9]"),
            ])
            out = wait_for(con, "ip -4 -o addr show scope global", r"inet \d", 60)
            check("address from VMware's network", re.search(r"inet \d", out) is not None, out)
            guest_ip = re.search(r"inet ([\d.]+)", out)
            guest_ip = guest_ip.group(1) if guest_ip else "?"
            out = wait_for(con, "curl -sS -o /dev/null -w '%{http_code}' https://deb.debian.org/",
                           r"^(200|301|302)$", 120)
            check("internet through encrypted DNS", re.search(r"^(200|301|302)$", out) is not None,
                  out)
            # The system clock and the hardware clock both run in UTC.
            guest_time = int(con.run("date +%s") or 0)
            check("clock matches the host", abs(guest_time - time.time()) < 10,
                  f"guest {guest_time}, host {time.time():.0f}")
            out = con.run("hwclock -r -u | cut -c1-24")
            try:
                rtc = time.mktime(time.strptime(out.strip(), "%a %b %d %H:%M:%S %Y"))
                rtc_ok = abs(rtc - time.mktime(time.gmtime())) < 120
            except ValueError:
                rtc_ok = False
            check("hardware clock in UTC", rtc_ok, f"{out} (UTC now {time.asctime(time.gmtime())})")
            check("VMware sees the tools", vmrun("checkToolsState", win_vmx) == "running",
                  vmrun("checkToolsState", win_vmx))
            deadline = time.time() + 120
            while (out := vmrun("readVariable", win_vmx, "guestVar", "ip")) != guest_ip:
                if time.time() > deadline:
                    break
                time.sleep(5)
            check("VMware sees the guest's address", out == guest_ip, f"{out} vs {guest_ip}")

            check("input tool copied in",
                  desktop.put_file(con, build_input_tool(), "/tmp/input-tool"))
            con.run("modprobe uinput; (/tmp/input-tool /tmp/input > /tmp/input.log 2>&1 &)")
            out = wait_for(con, desktop.GREETER_MODE, r"MODE=setup", 300)
            check("graphical first-boot setup shown", "MODE=setup" in out,
                  con.run("cat /run/wormlogin/greeter.log; tail /run/wormlogin/greeter/labwc.log"))
            # It stays (with 3D acceleration it used to start over and over).
            greeter = con.run("pidof wormos-greeter")
            time.sleep(20)
            out = con.run("pidof wormos-greeter")
            check("setup screen stays up", out == greeter and out != "",
                  f"{greeter} then {out}; " + con.run("tail -n 5 /run/wormlogin/greeter.log"))
            w, h, px = capture("setup")
            check("setup screen visible in the VMware window",
                  desktop.solid_block(px, w, h, desktop.ACCENT) is not None, f"{w}x{h}")
            first_boot()
            out = wait_for(con, "snap list", r"First boot: created", 300)
            check("first-boot setup finished", "First boot: created" in out, out[-300:])

            out = wait_for(con, "pidof foot", r"^\d+", 300)
            check("desktop started after the setup", re.search(r"^\d+", out, re.M) is not None,
                  con.run("tail -n 20 /run/user/1000/desktop.log"))
            time.sleep(5)
            # The renderer: OpenGL ES on VMware's 3D acceleration, software
            # without it (OpenGL ES fails and the session starts again).
            run_checks(con, [c for c in desktop.SESSION_CHECKS
                             if c[0] != "software renderer on a virtual GPU"])
            env = con.run("tr '\\0' '\\n' < /proc/$(pidof -s labwc)/environ | grep -E '^(WLR|GSK)_RENDERER'")
            if args.three_d:
                check("OpenGL ES with 3D acceleration", env == "" and
                      con.run("ls /run/user/1000/desktop.log.gles 2>&1 | grep -c 'No such'") == "1",
                      env + con.run("grep -v xkbcommon /run/user/1000/desktop.log.gles | tail -5"))
                check("GTK draws with OpenGL", con.run(
                    "grep -c libgallium /proc/$(pidof worm-shell)/maps") not in ("", "0"))
            else:
                check("software renderer without 3D acceleration",
                      "WLR_RENDERER=pixman" in env and "GSK_RENDERER=cairo" in env, env)
            check("cursor drawn by the compositor (VMware hides its own)",
                  "WLR_NO_HARDWARE_CURSORS=1" in con.run(
                      "tr '\\0' '\\n' < /proc/$(pidof labwc)/environ | grep CURSORS"))

            input("type echo $(id -un) typed > /tmp/typed\\n")
            out = wait_for(con, "cat /tmp/typed", r"typed", 60)
            if out != f"{USER} typed":
                capture("typing-failed", keep=True)
            check("typed into the terminal", out == f"{USER} typed", out)
            input("type echo $(id -un) one two")
            input("key ctrl+backspace")
            input("type > /tmp/word\\n")
            out = wait_for(con, "cat /tmp/word", r"\S", 60)
            check("Ctrl+Backspace deletes the last word", out == f"{USER} one", out)
            input("key super+ret")
            out = wait_for(con, "pidof foot | wc -w", r"^2$", 60)
            check("Super+Enter opens a terminal", out == "2", out)

            time.sleep(3)
            w, h, px = capture("desktop")
            colors = {px[i:i + 3] for i in range(0, len(px), 3 * 997)}
            purple = sum(1 for c in colors if c[2] > c[1] + 40 and c[0] > c[1] + 20)
            check("VMware window shows the desktop", len(colors) > 50 and purple > 5,
                  f"{w}x{h}, {len(colors)} colours, {purple} purple")

            # The pointer: moving it over the wallpaper changes only a
            # small area of the window (the cursor at its old and new place).
            input("move 0.85 0.45")
            time.sleep(2)
            a = capture("pointer-a")[2]
            input("move 0.87 0.47")
            time.sleep(2)
            b = capture("pointer-b")[2]
            # Only the right part of the window: the terminal in the middle
            # may change too (its cursor).
            right = w * 7 // 10
            a_right = b"".join(a[3 * (y * w + right):3 * (y + 1) * w] for y in range(h))
            b_right = b"".join(b[3 * (y * w + right):3 * (y + 1) * w] for y in range(h))
            n, box = changed_box(a_right, b_right, w - right, h)
            small = box is not None and box[2] - box[0] < 200 and box[3] - box[1] < 200
            check("pointer visible and moving", 4 <= n < 3000 and small, f"{n} pixels in {box}")
            input("click right")
            time.sleep(2)
            n, box = changed_box(b, capture("menu")[2], w, h)
            check("right click on the desktop opens the menu", n > 500, f"{n} pixels")
            input("key esc")

            # The screen size VMware asked for (the window's) is in use.
            mode = con.run(f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "
                           "wlr-randr' | grep current")
            with open(os.path.join(VMDIR, "vmware.log"), errors="replace") as f:
                sizes = re.findall(r"SWBScreen: Screen \d+ Defined: xywh\(0, 0, (\d+), (\d+)\)",
                                   f.read())
            want = "x".join(sizes[-1]) if sizes else "?"
            check("screen follows the VMware window", want in mode and want != "1024x768",
                  f"window {want}, guest {mode}")

            run_checks(con, desktop.SOUND_CHECKS)
            out = con.run(f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 sh -c \"{desktop.PLAY.strip()}\"'",
                          timeout=90)
            check("sound plays through PipeWire", "played" in out, out)

            # Copying in the VM puts the text on the Windows clipboard
            # (VMware turns \n into \r\n). Only an equality test is
            # printed, never the Windows clipboard itself.
            word = f"wormos-vmware-{os.getpid()}"
            con.run(f"su {USER} -c 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "
                    f"clip copy {word}'")
            time.sleep(3)
            out = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                                  f"(Get-Clipboard -Raw) -eq '{word}'"],
                                 capture_output=True, text=True).stdout.strip()
            check("copy in the VM reaches the Windows clipboard", out == "True", out)

            # A clean shutdown from inside: VMware sees the VM power off.
            con.send("poweroff\n")
            try:
                con.expect("Power down", 120)
                clean = True
            except (TimeoutError, OSError):
                clean = False
            deadline = time.time() + 60
            while running(vmx) and time.time() < deadline:
                time.sleep(3)
            check("clean shutdown, VMware powers off", clean and not running(vmx),
                  boot_test.ANSI.sub(b"", con.buf[-400:]).decode(errors="replace"))
        except (TimeoutError, RuntimeError, OSError) as e:
            check(f"test aborted: {e}", False,
                  boot_test.ANSI.sub(b"", con.buf[-800:]).decode(errors="replace"))
        finally:
            if running(vmx):
                vmrun("stop", win_vmx, "hard")
            starter.wait(timeout=60)
            sock.close()
            if not args.keep:
                time.sleep(2)
                shutil.rmtree(VMDIR, ignore_errors=True)

    print(f"{passed}/{passed + failures} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
