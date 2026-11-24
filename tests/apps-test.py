#!/usr/bin/env python3
"""Check that programs from the tool kits, the core repository and the
Debian and Kali wormholes open and run on the desktop.

Boots a throwaway copy of the image with a virtual screen, goes through
the graphical first-boot setup (the desktop then runs as the new user,
with the session's real environment), installs the tool kits and a set of
graphical programs, then:

  - starts every menu entry the way the launcher does (gio launch, from
    the session's environment) and waits for its window: programs with a
    window open one, command line tools open a terminal;
  - runs the command of every terminal entry, and every exported command
    with --help, as the user, and looks for the signs of a broken program
    (a missing library, module, script or toolkit plugin);
  - checks the runtime pieces that make this work for every program:
    the companions of the runtime rules, the stand-ins (sudo, xdg-open,
    x-terminal-emulator) reaching the session's bridge, generated menu
    entries and Kali's helper scripts.

Needs internet access and a lot of downloads (the pentest kit alone is
over a gigabyte); takes an hour or more. Console output goes to
build/apps.log, a report of every program to build/apps-report.txt.

Usage: tests/apps-test.py [--kits all|NAME,...] [--no-gui-apps] [--image IMAGE]
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
LOG = os.path.join(ROOT, "build", "apps.log")
REPORT = os.path.join(ROOT, "build", "apps-report.txt")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tests", path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


boot_test = load("boot_test", "boot-test.py")
desktop = load("desktop_test", "desktop-test.py")

USER, USER_PASSWORD, ROOT_PASSWORD = desktop.USER, desktop.USER_PASSWORD, desktop.ROOT_PASSWORD
GCC = desktop.GCC
SYSROOT = os.path.join(ROOT, "build", "sysroot")

KITS = ["blueteam", "forensics", "network", "passwords", "reverse", "web", "windows", "pentest"]

# Graphical programs people install, across toolkits: GTK 3 and 4, Qt 5
# and 6, Java, Electron-like browsers, plain X11. (package, source)
GUI_APPS = [
    ("wireshark", "debian"), ("mousepad", "debian"), ("gnome-calculator", "debian"),
    ("keepassxc", "debian"), ("vlc", "debian"), ("gimp", "debian"),
    ("firefox-esr", "debian"), ("chromium", "debian"), ("filezilla", "debian"),
    ("thunar", "debian"), ("audacity", "debian"), ("qbittorrent", "debian"),
    ("okular", "debian"), ("sqlitebrowser", "debian"), ("meld", "debian"),
    ("xterm", "debian"), ("gparted", "debian"),
    ("ghidra", "kali"), ("burpsuite", "kali"), ("zaproxy", "kali"), ("rizin-cutter", "kali"),
]

# Signs of a program that cannot run at all (not of one that only
# dislikes --help as an argument).
BROKEN = re.compile(
    r"error while loading shared libraries|ModuleNotFoundError|ImportError|No module named|"
    r"can't open file|Could not find the Qt platform plugin|no Qt platform plugin|"
    r"Exec format error|^whc: |: not found$|command not found|cannot execute", re.M)

# Seconds to wait for a window: Java programs start slowly.
SLOW = re.compile(r"ghidra|burp|zap|armitage|maltego", re.I)

# Entries that do something other than open a window of their own.
NO_WINDOW = re.compile(r"-settings\.desktop$|bulk-rename|okularApplication_")

# The session's environment for commands run as the user (from worm-shell,
# the launcher's process).
IN_SESSION = r"""#!/bin/sh
pid=$(pidof worm-shell | cut -d' ' -f1)
tr '\0' '\n' < /proc/$pid/environ | grep -v '^_=' |
  sed "s/'/'\\\\''/g; s/^\([A-Za-z_][A-Za-z0-9_]*\)=\(.*\)/export \1='\2'/" > /tmp/session.env
chmod 644 /tmp/session.env
exec su tester -s /bin/sh -c ". /tmp/session.env; cd; $*"
"""

# Starts each entry given and prints what came of it: the windows that
# appeared and the end of its output.
PROBE = r"""#!/bin/sh
# probe WAIT ENTRY...: launch each entry as the launcher does.
wait=$1; shift
for f; do
  /tmp/in-session /tmp/toplevels | sort > /tmp/probe-before
  ps -o pid,user | awk '$2 == "tester" {print $1}' | sort > /tmp/probe-procs
  log=/tmp/probe-$(basename "$f").log
  /tmp/in-session "gio launch $f > $log 2>&1 &"
  i=0; new=
  while [ $i -lt "$wait" ] && [ -z "$new" ]; do
    sleep 2; i=$((i + 2))
    /tmp/in-session /tmp/toplevels | sort > /tmp/probe-after
    new=$(comm -13 /tmp/probe-before /tmp/probe-after)
  done
  echo "## $(basename "$f")"
  [ -n "$new" ] && echo "$new" | sed 's/^/   window: /'
  echo "   log: $(tail -c 400 "$log" | tr '\n' ' ')"
  # Close what it started.
  ps -o pid,user | awk '$2 == "tester" {print $1}' | sort > /tmp/probe-now
  for p in $(comm -13 /tmp/probe-procs /tmp/probe-now); do kill -9 "$p" 2>/dev/null; done
  sleep 1
done
"""

# Runs every exported command with --help as the user and prints those
# showing signs of a broken program.
COMMANDS = r"""#!/bin/sh
cd /tmp
for w in /usr/local/bin/*; do
  out=$(su tester -s /bin/sh -c ". /tmp/session.env; cd; timeout -s KILL 20 $w --help </dev/null 2>&1")
  echo "## $(basename $w) $?"
  printf '%s\n' "$out" | head -c 600
  echo
done
echo "## end"
"""


def build_toplevels():
    """Compile tests/toplevels.c with the WormOS toolchain (None without one)."""
    if not os.access(GCC, os.X_OK):
        return None
    work = tempfile.mkdtemp(prefix="wormos-toplevels-")
    xml = os.path.join(ROOT, "src", "shell", "protocol",
                       "wlr-foreign-toplevel-management-unstable-v1.xml")
    header = os.path.join(work, "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h")
    code = os.path.join(work, "protocol.c")
    subprocess.run(["wayland-scanner", "client-header", xml, header], check=True)
    subprocess.run(["wayland-scanner", "private-code", xml, code], check=True)
    out = os.path.join(work, "toplevels")
    subprocess.run([GCC, "-O2", "-s", f"-I{work}", f"-I{SYSROOT}/usr/include", "-o", out,
                    os.path.join(ROOT, "tests", "toplevels.c"), code,
                    f"-L{SYSROOT}/usr/lib", "-lwayland-client"], check=True)
    with open(out, "rb") as f:
        return f.read()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kits", default="all", help="kits to install (all, or NAME,NAME)")
    parser.add_argument("--no-gui-apps", action="store_true",
                        help="leave out the graphical programs from Debian and Kali")
    parser.add_argument("--image", help="test this image instead of out/wormos.img")
    parser.add_argument("--timeout", type=int, default=600, help="seconds to boot")
    args = parser.parse_args()
    kits = KITS if args.kits == "all" else [k for k in args.kits.split(",") if k]

    toplevels = build_toplevels()
    if toplevels is None:
        print("SKIP  no WormOS toolchain to build the window lister (tests/toplevels.c)")
        return 0

    tmp = tempfile.mkdtemp(prefix="wormos-apps-")
    mon_path = os.path.join(tmp, "monitor")
    qmp_path = os.path.join(tmp, "qmp")
    extra = (f"-vga none -device virtio-gpu-pci,addr=0x10,id=video0 "
             f"-device virtio-keyboard-pci,addr=0x11 -device virtio-tablet-pci,addr=0x12 "
             f"-monitor unix:{mon_path},server,nowait -qmp unix:{qmp_path},server,nowait")
    env = dict(os.environ, QEMU_EXTRA=extra, QEMU_MEM="6144", QEMU_CPUS="6",
               QEMU_VDAGENT_MOUSE="on")
    if args.image:
        env["IMAGE"] = os.path.abspath(args.image)
    # The image's 8 GiB do not hold every kit: a copy with a bigger disk.
    image = os.path.join(tmp, "apps.qcow2")
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F",
                    "qcow2" if env.get("IMAGE", "").endswith(".qcow2") else "raw",
                    "-b", env.get("IMAGE", os.path.join(ROOT, "out", "wormos.img")),
                    image, "60G"], check=True)
    env["IMAGE"] = image
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    passed = failures = 0
    report = open(REPORT, "w")

    def check(name, ok, out=""):
        nonlocal passed, failures
        passed += bool(ok)
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}", flush=True)
        if not ok:
            print(f"      output: {out[-700:]!r}", flush=True)

    with open(LOG, "wb") as log:
        con = boot_test.Console(proc, log)
        try:
            con.expect("login:", args.timeout)
            mon = desktop.Monitor(mon_path)
            con.send("root\n")
            con.expect("# ", 60)
            con.send("exec /bin/sh\n")  # root logs in to zsh; tests drive plain sh
            con.run("dmesg -n 1")
            # The disk's file system takes the whole (bigger) disk.
            con.run("worm unlock >/dev/null; python3 -c \"import fcntl,os,struct; "
                    "fcntl.ioctl(os.open('/', os.O_RDONLY), 0x50009403, "
                    "bytearray(struct.pack('q4088s', 0, b'max')))\"; worm lock >/dev/null")
            out = con.run("df -m / | tail -1")
            check("file system grown to the disk", re.search(r"\s(\d{5,})\s", out), out)

            # The graphical first-boot setup, as a person would answer it.
            deadline = time.time() + 300
            while "MODE=setup" not in con.run(desktop.GREETER_MODE) and time.time() < deadline:
                time.sleep(3)
            time.sleep(5)
            mon.key("ret")
            time.sleep(2)
            mon.type("Tester\n")
            time.sleep(1)
            mon.type(USER + "\n")
            time.sleep(1)
            mon.type(USER_PASSWORD + "\n")
            time.sleep(1)
            mon.type(USER_PASSWORD + "\n")
            time.sleep(2)
            for secret in (ROOT_PASSWORD, ROOT_PASSWORD):
                time.sleep(1)
                mon.type(secret + "\n")
            deadline = time.time() + 300
            while not re.search(r"^\d+", con.run("pidof worm-shell"), re.M):
                if time.time() > deadline:
                    raise TimeoutError("the desktop did not start after the setup")
                time.sleep(3)
            time.sleep(10)
            check("desktop session running", True)

            desktop.put_file(con, toplevels, "/tmp/toplevels")
            desktop.put_file(con, IN_SESSION.encode(), "/tmp/in-session")
            desktop.put_file(con, PROBE.encode(), "/tmp/probe")
            desktop.put_file(con, COMMANDS.encode(), "/tmp/commands")
            out = con.run("/tmp/in-session /tmp/toplevels")
            check("windows listed (the welcome terminal)", "foot" in out, out)
            out = con.run("ls -l $(/tmp/in-session 'echo $XDG_RUNTIME_DIR')/wormhole-bridge")
            check("the session's wormhole bridge is listening", out.startswith("p"), out)

            # The kits, then the graphical programs.
            for kit in kits:
                begin = time.time()
                out = con.run(f"worm kit install {kit} 2>&1 | tail -3", 7200)
                check(f"kit {kit} installed ({time.time() - begin:.0f}s)",
                      re.search(r"(\d+) of \1 tools installed", out), out)
            if not args.no_gui_apps:
                for pkg, source in GUI_APPS:
                    out = con.run(f"wh -y install --from {source} {pkg} 2>&1 | tail -4", 3600)
                    check(f"{pkg} installed from {source}",
                          "->" in out or "already" in out, out)

            # The runtime: companions and stand-ins.
            out = con.run("whc debian dpkg-query -W -f '${Package} ${db:Status-Abbrev}\\n' "
                          "librsvg2-common qt6-wayland 2>&1")
            check("runtime rules added the SVG loader and Qt's Wayland plugin",
                  re.search(r"^librsvg2-common ii", out, re.M) and
                  re.search(r"^qt6-wayland ii", out, re.M), out)
            out = con.run("for n in sudo pkexec xdg-open x-terminal-emulator; do "
                          "readlink /var/lib/wormholes/debian/usr/local/bin/$n; done")
            check("stand-ins in the wormhole", out.count("../lib/wormhole/bridge") == 4, out)
            out = con.run("ls /var/lib/wormholes/debian/usr/share/doc/ | wc -l")
            check("documentation kept in wormholes", re.search(r"^\d{2,}", out), out)

            # Every menu entry, as the launcher starts it.
            entries = con.run("ls /usr/local/share/applications/*.desktop "
                              "/usr/share/applications/*.desktop").split()
            report.write("# Menu entries\n")
            for entry in entries:
                name = os.path.basename(entry)
                text = con.run(f"grep -E '^(Exec|Terminal|NoDisplay)=' {entry}")
                if "NoDisplay=true" in text:
                    continue
                terminal = "Terminal=true" in text
                wait = 150 if SLOW.search(name + text) else 40
                out = con.run(f"/tmp/probe {wait} {entry}", wait + 120)
                report.write(out + "\n")
                windows = re.findall(r"^   window: (.+)$", out, re.M)
                if NO_WINDOW.search(name):
                    continue
                ok = any("foot" in w for w in windows) if terminal else bool(windows)
                ok = ok and not BROKEN.search(out)
                check(f"{name} opens {'a terminal' if terminal else 'a window'}", ok, out)
                # A terminal entry's command, run here to read its output.
                m = re.search(r'^Exec=/usr/lib/worm/exec-in-shell "(.*)"$', text, re.M)
                if m and terminal:
                    cmd = m.group(1).replace("\\$", "$").replace('\\"', '"')
                    out = con.run(f"/tmp/in-session 'timeout -s KILL 30 sh -c \"{cmd}\" "
                                  f"</dev/null 2>&1 | tail -c 500'", 90)
                    report.write(f"   command: {out!r}\n")
                    check(f"{name}: {cmd} runs", not BROKEN.search(out), out)

            # Every exported command.
            out = con.run("/tmp/commands", 7200)
            report.write("# Commands\n" + out + "\n")
            for block in out.split("## ")[1:]:
                head, _, body = block.partition("\n")
                if head.startswith("end"):
                    break
                cmd, _, status = head.partition(" ")
                check(f"command {cmd} runs", status not in ("126", "127") and
                      not BROKEN.search(body), body)

            # Kali's impacket keeps its scripts with its documentation.
            out = con.run("su tester -c 'impacket-GetADUsers -h 2>&1 | head -3'")
            check("impacket scripts run", "Impacket" in out, out)
            # Commands behind Debian's alternatives (nc is a link to
            # nc.openbsd) and full versions of BusyBox's commands.
            out = con.run("head -3 /usr/local/bin/nc /usr/local/bin/ipcalc 2>&1")
            check("alternatives and BusyBox's commands exported",
                  out.count("Exported by wh") == 2, out)
            # Packages split in parts (vlc, keepassxc) export their commands.
            out = con.run("ls /usr/local/bin/vlc /usr/local/bin/keepassxc 2>&1")
            check("split packages export their commands", "No such" not in out, out)
            # A package without menu entries gets one listing its commands.
            out = con.run("grep -h '^Exec=' /usr/local/share/applications/wormhole-hexyl.desktop")
            check("generated menu entry", "exec-in-shell --about hexyl" in out, out)
            out = con.run("/tmp/in-session '/usr/lib/worm/exec-in-shell --run --about hexyl'")
            check("generated entry shows the package's commands",
                  re.search(r"^\s+hexyl$", out, re.M), out)
            # Kali's helper scripts run inside the wormhole (john lists its
            # helpers there).
            out = con.run("grep -h '^Exec=' /usr/local/share/applications/kali-john.desktop")
            check("helper script entry", "--in kali --script john.sh" in out, out)
            out = con.run("/tmp/in-session '/usr/lib/worm/exec-in-shell --run --in kali "
                          "--script john.sh' 2>&1 | tail -5")
            check("helper script runs inside the wormhole", "2john" in out, out)

            # The stand-ins reach the bridge: a terminal and the root
            # terminal open on the desktop.
            before = con.run("/tmp/in-session /tmp/toplevels | grep -c foot")
            con.run("/tmp/in-session 'whc debian x-terminal-emulator -e true'")
            time.sleep(4)
            after = con.run("/tmp/in-session /tmp/toplevels | grep -c foot")
            check("x-terminal-emulator in a wormhole opens a terminal",
                  int(after or 0) > int(before or 0), before + " " + after)
            con.run("/tmp/in-session 'whc debian sudo id' 2>&1")
            time.sleep(4)
            out = con.run("/tmp/in-session /tmp/toplevels")
            check("sudo in a wormhole opens the root terminal", "Root terminal" in out, out)
            con.run("pkill -f 'Root terminal' ; true")
            con.run("/tmp/in-session 'whc debian xdg-open https://example.org/wormos-test'")
            time.sleep(2)
            out = con.run("cat /run/user/1000/bridge.log")
            check("xdg-open in a wormhole reaches the bridge",
                  "open debian" in out and "example.org/wormos-test" in out, out)
            # Root's files are shared with root's wormhole programs.
            out = con.run("cd /root && echo here > rootfile && "
                          "whc debian cat /root/rootfile; rm -f /root/rootfile")
            check("root's home is shared with wormhole programs", out == "here", out)
            # sudo on the system (the root terminal's password prompt, here
            # answered on the serial console as root, who needs none).
            out = con.run("sudo id -u")
            check("sudo as root runs the command", out == "0", out)

            con.send("poweroff\n")
            con.expect("Power down", 300)
        except (TimeoutError, RuntimeError) as e:
            print(f"FAIL  {e} (see {LOG})")
            failures += 1
        finally:
            if proc.poll() is None:
                proc.kill()
    report.close()
    print(f"{passed}/{passed + failures} checks passed (report: {REPORT})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
