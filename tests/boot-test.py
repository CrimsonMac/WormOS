#!/usr/bin/env python3
"""Boot the WormOS image in QEMU and check the running system.

Logs in on the serial console and runs a set of checks. It then takes a
snapshot, changes the system, rewinds (a soft reboot that keeps the kernel
running) and checks the change is gone, then reboots and picks a snapshot
from the "Rewind to a snapshot" boot menu. Last, it exercises worm
unlock/lock and the package manager against out/repo, served over HTTP
from this machine: install, a trial removal that rolls back, a removal and
its undo. Firewall checks reach a web server in the VM from this machine
through a QEMU port forward. The full console output is written to
build/boot.log.

Usage: tests/boot-test.py [--timeout SECONDS]
"""

import argparse
import functools
import http.server
import os
import re
import select
import socket
import subprocess
import sys
import threading
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
    ("system bus runs unprivileged", "stat -c %U /proc/$(pidof dbus-daemon)",
     r"^messagebus$"),
    ("system bus answers",
     "dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus "
     "org.freedesktop.DBus.GetId", r'string "[0-9a-f]{32}"'),
    ("VMware tools installed, idle outside VMware",
     "ls /usr/bin/vmtoolsd && echo running=$(pidof vmtoolsd | wc -w)",
     r"^/usr/bin/vmtoolsd\nrunning=0$"),
    ("settings schemas compiled by the hook",
     "gsettings list-schemas | grep -c '^org.gtk.gtk4.Settings'", r"^[1-9]$"),
    ("file type database compiled by the hook",
     "stat -c %s /usr/share/mime/mime.cache; gio mime text/plain 2>&1 | head -1", r"^[1-9]\d+\n"),
    ("machine ID kept in /var", "readlink /etc/machine-id; cat /etc/machine-id",
     r"^\.\./var/lib/dbus/machine-id\n[0-9a-f]{32}$"),
    ("hostname", "hostname", r"^wormos$"),
    # The resolver keeps its trust anchor current in /var/lib/unbound.
    ("system files owned by root",
     "find / /var -xdev ! -user 0 ! -path '/var/lib/unbound*' | wc -l", r"^0$"),
    ("shadow is private", "stat -c '%a %u' /etc/shadow", r"^600 0$"),
]

# Kernel security features (kernel/wormos.config), settings and mounts.
KERNEL_CHECKS = [
    ("hardening on the command line", "cat /proc/cmdline", r"slab_nomerge debugfs=off .*iommu\.strict=1"),
    ("io_uring off", "cat /proc/sys/kernel/io_uring_disabled", r"^2$"),
    ("kernel pointers hidden", "cat /proc/sys/kernel/kptr_restrict", r"^2$"),
    ("protected links and files",
     "cat /proc/sys/fs/protected_symlinks /proc/sys/fs/protected_regular | tr '\\n' ' '",
     r"^1 2\s*$"),
    ("no ICMP redirects", "cat /proc/sys/net/ipv4/conf/all/accept_redirects", r"^0$"),
    ("IPv6 privacy addresses", "cat /proc/sys/net/ipv6/conf/eth0/use_tempaddr", r"^2$"),
    ("processes of others hidden", "grep ' /proc ' /proc/mounts", r"hidepid=invisible"),
    ("no programs from /dev, /run or /dev/shm",
     "grep -E ' /(dev|run|dev/shm) ' /proc/mounts | grep -c noexec", r"^3$"),
    ("random MAC address", "cat /sys/class/net/eth0/address", r"^[0-9a-f][26ae]:"),
    ("not QEMU's MAC address", "cat /sys/class/net/eth0/address", r"^(?!52:54:00:12:34:56)"),
    ("lockdown in integrity mode", "cat /sys/kernel/security/lockdown", r"\[integrity\]"),
    ("security modules active", "cat /sys/kernel/security/lsm", r"lockdown.*yama.*landlock"),
    ("landlock running", "dmesg | grep -c 'landlock: Up and running'", r"^1$"),
    ("seccomp filters available", "cat /proc/sys/kernel/seccomp/actions_avail", r"kill_process"),
    ("module signatures enforced", "cat /sys/module/module/parameters/sig_enforce", r"^Y$"),
    ("no raw kernel memory access", "ls /dev/mem /dev/port /proc/kcore 2>&1 | grep -c 'No such'", r"^3$"),
    ("sysrq off", "cat /proc/sys/kernel/sysrq", r"^0$"),
    ("nftables built in",
     "grep -cE '^CONFIG_(NF_TABLES_INET|NFT_CT|NFT_NAT|NFT_MASQ|NFT_REDIR|NFT_FIB_INET)=y' /boot/config-$(uname -r)",
     r"^6$"),
    ("no iptables", "grep -c '^CONFIG_IP_NF_IPTABLES=' /boot/config-$(uname -r)", r"^0$"),
    ("sandbox filesystems", "grep -cwE 'overlay|fuse' /proc/filesystems", r"^2$"),
    ("vm and tunnel devices", "ls /dev/vhost-net /dev/vhost-vsock /dev/vsock /dev/net/tun | wc -l", r"^4$"),
]

# DNS: a local resolver with encrypted upstreams (needs no internet here).
DNS_CHECKS = [
    ("local resolver running", "pidof unbound", r"^\d+"),
    ("resolver runs unprivileged", "ps -o user,comm | grep -w unbound", r"^unbound\s+unbound"),
    ("resolver configuration valid", "unbound-checkconf 2>&1", r"no errors"),
    ("programs use the local resolver", "cat /etc/resolv.conf", r"^nameserver 127\.0\.0\.1$"),
    ("network's DNS servers only recorded",
     "cat /run/worm/dhcp-dns; grep -c 10.0.2.3 /etc/resolv.conf", r"^nameserver 10\.0\.2\.3\n0$"),
    ("resolver answers", "nslookup localhost 127.0.0.1 2>&1", r"Address:?\s+127\.0\.0\.1"),
    ("trust anchor kept up to date", "ls -l /var/lib/unbound/root.key", r"\sunbound\s+unbound\s"),
]

# The firewall. A web server in the VM on port 8080 is fetched from this
# machine through a QEMU port forward between these lists.
FIREWALL_CHECKS = [
    ("firewall active at boot", "worm fw status", r"^Firewall: active"),
    ("no ports open by default", "worm fw status", r"^Open ports: none$"),
    ("inbound and forwarding denied",
     "nft list table inet worm | grep -c 'policy drop'", r"^2$"),
    ("loaded before the network", "ls /etc/worm/services | grep -E 'firewall|network' | tr '\\n' ' '",
     r"^05-firewall 30-network\s*$"),
    ("web server started",
     "mkdir -p /tmp/www && echo hi > /tmp/www/index.html && httpd -p 8080 -h /tmp/www && echo ok",
     r"^ok$"),
]

FIREWALL_OPEN_CHECKS = [
    ("worm fw allow", "worm fw allow 8080", r"^Opened 8080/tcp\.$"),
]

FIREWALL_CLOSE_CHECKS = [
    ("port listed", "worm fw status", r"^\s+8080/tcp$"),
    ("port saved", "cat /etc/worm/firewall", r"^tcp 8080$"),
    ("root still read-only", "touch /probe 2>&1", r"Read-only file system"),
    ("allow is idempotent", "worm fw allow 8080/tcp", r"already open"),
    ("invalid port refused", "worm fw allow 70000 2>&1; echo rc=$?", r"invalid port(.|\n)*rc=1"),
    ("worm fw deny", "worm fw deny 8080", r"^Closed 8080/tcp\.$"),
]

FIREWALL_AFTER_CHECKS = [
    ("drops counted", "worm fw status", r"^Dropped since loaded: [1-9]"),
    ("bad port list loads closed",
     "mount -o remount,bind,rw / && echo 'tcp banana' >> /etc/worm/firewall && "
     "worm fw load 2>&1; echo rc=$?; worm fw status",
     r"invalid line(.|\n)*rc=1(.|\n)*Firewall: active(.|\n)*Open ports: none"),
    ("bad line removed",
     "sed -i '/banana/d' /etc/worm/firewall && mount -o remount,bind,ro / && worm fw load && echo ok",
     r"^ok$"),
    ("udp port kept for later", "worm fw allow 5353/udp", r"^Opened 5353/udp\.$"),
]

# Blackholes, mostly escape attempts from inside one. "bh open t1 -- CMD"
# runs one command in the blackhole t1; echo n=... gives a positive marker.
BLACKHOLE_CHECKS = [
    ("bh put", "echo sample > /root/sample && echo secret > /home/secret && bh put t1 /root/sample",
     r"^/root/sample -> t1:/inbox/sample$"),
    ("file in the inbox", "bh open t1 -- cat /inbox/sample", r"^sample$"),
    ("root inside", "bh open t1 -- id -u", r"^0$"),
    ("unprivileged outside", "bh open t1 -- cat /proc/self/uid_map", r"^\s*0\s+1073741825\s+1$"),
    ("own host name", "bh open t1 -- hostname", r"^t1$"),
    ("host home hidden", "bh open t1 -- sh -c 'echo n=$(ls -A /home | wc -l)'", r"^n=0$"),
    ("root's home hidden", "bh open t1 -- ls /root/probe 2>&1", r"No such file"),
    ("no disks", "bh open t1 -- sh -c 'echo n=$(ls /dev | grep -c vd)'", r"^n=0$"),
    ("cannot mount the snapshots",
     "bh open t1 -- mount -t btrfs -o subvol=@snapshots /dev/vda2 /tmp 2>&1",
     r"permission denied|not permitted"),
    ("only loopback", "bh open t1 -- sh -c 'echo n=$(ip -o link | wc -l)'", r"^n=1$"),
    ("no network", "bh open t1 -- wget -q -T 5 -O- http://10.0.2.2:1/ 2>&1", r"unreachable"),
    ("host processes hidden",
     "bh open t1 -- sh -c 'echo n=$(ls /proc | grep -c \"^[0-9]\")'", r"^n=[1-6]$"),
    ("cannot signal host processes", "bh open t1 -- kill -0 $(pidof syslogd) 2>&1",
     r"No such process"),
    ("host files read-only",
     "bh open t1 -- sh -c 'echo x >> /etc/passwd; touch /usr/bin/x' 2>&1",
     r"Permission denied(.|\n)*(Permission denied|Read-only file system)"),
    ("writes stay inside", "bh open t1 -- sh -c 'echo x > /escape && echo ok'; ls /escape 2>&1",
     r"^ok$(.|\n)*No such file"),
    ("no message bus", "bh open t1 -- ls /run/dbus 2>&1", r"No such file"),
    ("no sysrq", "bh open t1 -- sh -c 'echo b > /proc/sysrq-trigger' 2>&1", r"Permission denied"),
    ("no kernel log", "bh open t1 -- dmesg 2>&1", r"not permitted"),
    ("sandboxed", "bh open t1 -- grep -E 'NoNewPrivs|Seccomp:' /proc/self/status",
     r"NoNewPrivs:\s+1(.|\n)*Seccomp:\s+2"),
    ("no nested namespaces", "bh open t1 -- unshare -U true 2>&1", r"not permitted"),
    ("no inherited descriptors",
     "exec 7</home; bh open t1 -- sh -c 'echo fds=$(ls /proc/self/fd | tr \"\\n\" \" \")'; exec 7<&-",
     r"^fds=0 1 2 3\s*$"),
    ("files kept between sessions",
     "bh open t1 -- sh -c 'echo kept > /root/note'; bh open t1 -- cat /root/note", r"^kept$"),
    ("running with limits",
     "bh open t1 -- sleep 60 & sleep 5; cat /sys/fs/cgroup/blackholes/t1/pids.max; bh list",
     r"^1024$(.|\n)*^t1\s+running"),
    ("one session at a time", "bh open t1 -- true 2>&1", r"already open"),
    ("bh close", "bh close t1", r"^Closed blackhole t1\.$"),
    ("everything removed",
     "sleep 1; echo n=$(ls /var/lib/blackholes/0 /sys/fs/cgroup/blackholes | grep -c t1)",
     r"^n=0$"),
    ("temporary blackhole removed",
     "bh open -- touch /x && echo n=$(ls /var/lib/blackholes/0 | wc -l)", r"^n=0$"),
    ("bh list", "bh list", r"^No blackholes"),
    ("add a normal user",
     "mount -o remount,bind,rw / && adduser -D -H tester && mount -o remount,bind,ro / && echo ok",
     r"^ok$"),
    ("user's blackhole", "su tester -c 'bh open u1 -- cat /proc/self/uid_map'",
     r"^\s*0\s+[1-9]\d*\s+1$"),
    ("user's blackhole is private", "ls -ld /var/lib/blackholes/$(id -u tester)",
     r"^drwx------\s+\d+\s+tester\s"),
    ("user closes it", "su tester -c 'bh close u1'", r"^Closed blackhole u1\.$"),
]

# whc without network access: a tiny wormhole made of busybox and its
# libraries. make test-wormholes covers real Debian and Kali ones.
WHC_CHECKS = [
    ("small wormhole",
     "W=/var/lib/wormholes/mini && mkdir -p $W/usr/bin $W/usr/lib $W/etc $W/tmp && "
     "cp /usr/bin/busybox $W/usr/bin/ && "
     "cp /usr/lib/ld-linux-x86-64.so.2 /usr/lib/libc.so.6 /usr/lib/libm.so.6 "
     "/usr/lib/libresolv.so.2 $W/usr/lib/ && "
     "ln -s usr/bin $W/bin && ln -s usr/lib $W/lib && ln -s usr/lib $W/lib64 && echo ok",
     r"^ok$"),
    ("whc as root", "touch /var/lib/wormholes/mini/inside && whc mini busybox ls /inside",
     r"^/inside$"),
    ("whc as a user keeps their ID",
     "su tester -c 'whc mini busybox cat /proc/self/uid_map'",
     r"^\s*([1-9]\d*)\s+\1\s+1$"),
    ("whc user cannot change the wormhole",
     "su tester -c 'whc mini busybox touch /usr/x' 2>&1", r"Permission denied|Read-only"),
    ("whc shares the host's /tmp",
     "echo shared > /tmp/whc-probe && su tester -c 'whc mini busybox cat /tmp/whc-probe'",
     r"^shared$"),
    ("wormhole removed", "rm -rf /var/lib/wormholes/mini && echo ok", r"^ok$"),
]

STATUS_CHECKS = [
    ("worm status", "worm status",
     r"System\s+read-only(.|\n)*Mode\s+normal(.|\n)*Packages\s+no change waiting(.|\n)*"
     r"Firewall\s+active, 1 open port(.|\n)*DNS\s+encrypted(.|\n)*Blackholes\s+0 running"
     r"(.|\n)*Kernel\s+lockdown integrity(.|\n)*io_uring off"),
]

# Anonymous mode (worm anon). Tor cannot connect here; what matters is
# that nothing leaves the machine directly. {repo} is the repository URL.
ANON_CHECKS = [
    ("tor installed", "tor --version | head -1", r"^Tor version 0\.4\.9"),
    ("worm anon on", "worm anon on", r"^Anonymous mode is on"),
    ("tor runs unprivileged", "sleep 3; ps -o user,comm | grep -w tor", r"^tor\s+tor"),
    ("connections sent to Tor", "nft list chain inet worm anon_nat",
     r"udp dport 53 redirect to :5353(.|\n)*l4proto (tcp|6) redirect to :9040"),
    ("everything else blocked", "ping -c 1 -W 3 10.0.2.2 2>&1", r"not permitted|100% packet loss"),
    ("direct request attempted", "wget -q -T 10 -O- {repo}anon-leak-check 2>&1; echo done",
     r"done$"),
    ("connections reach Tor", "wget -q -T 10 -O- http://198.51.100.1/ 2>&1; echo rc=$?",
     r"\A(?![\s\S]*refused)[\s\S]*rc="),
    ("status shows it", "worm status", r"DNS\s+through Tor(.|\n)*Anonymity\s+on"),
    ("survives a firewall reload", "worm fw load && nft list chain inet worm anon_nat | grep -c 9040",
     r"^1$"),
    ("worm anon off", "worm anon off", r"^Anonymous mode is off"),
    ("tor stopped", "sleep 2; pidof tor || echo none", r"^none$"),
    ("direct connections again", "wget -q -T 10 -O- {repo}anon-off-check 2>&1; echo done",
     r"done$"),
]

# worm panic, at the end: its snapshot would change the IDs checked above.
# {repo} is the repository URL.
PANIC_CHECKS = [
    ("a blackhole running", "bh open p1 -- sleep 600 & sleep 5; bh list", r"^p1\s+running"),
    ("worm panic", "worm panic",
     r"Network cut(.|\n)*Stopped 1 blackhole(.|\n)*snapshot \d+ \(pinned"),
    ("blackhole stopped, files kept", "sleep 1; bh list", r"^p1\s+idle"),
    ("devices down", "ip -o link show eth0", r"^\d+: eth0: <(?![^>]*\bUP\b)"),
    ("nothing gets out", "wget -q -T 5 -O- {repo}index 2>&1 | head -1", r"\S"),
    ("panic snapshot pinned", "snap list", r"^\s*\d+\*.*all\s+manual\s+Panic at"),
    ("firewall refuses changes", "worm fw allow 22 2>&1", r"network is cut"),
    ("status shows it", "worm status", r"Firewall\s+network cut"),
    ("worm panic off", "worm panic off", r"network is back"),
    ("network works again", "sleep 5; wh --repo {repo} update",
     r"^Package index updated: \d+ packages$"),
]

# After booting the menu's safe mode entry.
SAFE_CHECKS = [
    ("safe mode", "worm status", r"Mode\s+SAFE MODE"),
    ("network not started", "pidof udhcpc || echo none", r"^none$"),
    ("network cut", "worm fw status", r"network is cut"),
    ("stays cut until a normal restart", "worm panic off 2>&1", r"restart normally"),
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
    ("store stays hidden after use", "grep -c snapshots /proc/mounts", r"^0$"),
]

# Inside "snap browse 1", a read-only shell in the snapshot.
BROWSE_CHECKS = [
    ("browse shows the snapshot", "pwd; ls /run/snap/view/etc/probe 2>&1", r"No such file"),
    ("browse is read-only", "touch /run/snap/view/x 2>&1", r"Read-only file system"),
    ("browse hides the store", "ls /run/snap/top 2>&1 | wc -l", r"^0$"),
]

# After "snap rewind" and the soft reboot it causes.
REWIND_CHECKS = [
    ("services restarted", "pidof syslogd && grep -c ' /var ' /proc/mounts", r"^1$"),
    ("change is gone", "ls /etc/probe 2>&1", r"No such file"),
    ("firewall reloaded", "worm fw status", r"^Firewall: active(.|\n)*^\s+5353/udp$"),
    ("previous state saved", "snap list", r"^\s+2\s.*auto\s+Before rewind to 1$"),
    ("saved state has the change", "snap diff 1 2", r"^\+ /etc/probe$"),
    ("root still read-only", "grep ' / ' /proc/mounts", r"\sro[,\s]"),
    ("change the system again",
     "mount -o remount,bind,rw / && echo y > /etc/probe2 && mount -o remount,bind,ro / && echo ok",
     r"^ok$"),
    ("restore one file", "snap restore 2 /etc/probe && cat /etc/probe",
     r"^Restored /etc/probe from snapshot 2 .*\n(.*\n)?x$"),
    ("home snapshot",
     "mkdir -p /home/u && echo keep > /home/u/a && snap now --home -c home",
     r"^Snapshot \d+ created \(home\)$"),
    ("change home", "echo junk > /home/u/b && rm /home/u/a && echo ok", r"^ok$"),
]

# After "snap rewind --home".
HOME_CHECKS = [
    ("home file is back", "cat /home/u/a", r"^keep$"),
    ("later home file is gone", "ls /home/u/b 2>&1", r"No such file"),
    ("system was not rewound", "cat /etc/probe2", r"^y$"),
]

# After rewinding from the boot menu to the newest system snapshot, 3,
# taken just before /etc/probe was restored.
MENU_CHECKS = [
    ("snapshot 3 state is back", "cat /etc/probe2", r"^y$"),
    ("later restore is undone", "ls /etc/probe 2>&1", r"No such file"),
    ("menu rewind saved previous state", "snap list", r"^\s+\d+\s.*system\s+auto\s+Before rewind to 3$"),
    ("firewall after reboot", "worm fw status", r"^Firewall: active(.|\n)*^\s+5353/udp$"),
]


# Package manager and worm checks. {repo} is the repository URL.
WORM_CHECKS = [
    ("installed packages recorded", "wh list", r"^wh\s+0\.1\.0-\d+"),
    ("wh update", "wh --repo {repo} update", r"^Package index updated: \d+ packages$"),
    ("wh search", "wh search tcp", r"^tcpdump 4\.99\.7-1$"),
    ("worm unlock", "worm unlock", r"^The system is unlocked"),
    ("wh refuses while unlocked", "wh --repo {repo} -y install tcpdump 2>&1",
     r"the system is unlocked"),
    ("lock refused while a file is open",
     "exec 3>/etc/held; worm lock 2>&1", r"^Cannot lock yet(.|\n)*/etc/held"),
    ("worm lock", "exec 3>&-; worm lock",
     r"^The system is locked\.\n(.*\n)?Snapshot \d+ created"),
    ("manual change kept", "ls /etc/held && touch /etc/x 2>&1", r"Read-only file system"),
]

AFTER_INSTALL_CHECKS = [
    ("tcpdump runs", "tcpdump --version 2>&1", r"^tcpdump version 4\.99\.7"),
    ("install in history", "wh history", r"\sinstall\s+\d+\s+libpcap tcpdump$"),
    ("install snapshot", "snap list", r"auto\s+wh install libpcap tcpdump$"),
    ("still read-only after install", "touch /usr/x 2>&1", r"Read-only file system"),
]

AFTER_TRIAL_CHECKS = [
    ("trial removed tcpdump", "ls /usr/bin/tcpdump 2>&1", r"No such file"),
    ("trial is reported", "snap list", r"A trial is running"),
]

AFTER_TRIAL_END_CHECKS = [
    ("trial rolled back", "tcpdump --version 2>&1", r"^tcpdump version"),
]

AFTER_REMOVE_CHECKS = [
    ("tcpdump removed", "ls /usr/bin/tcpdump 2>&1", r"No such file"),
    ("library kept", "wh list", r"^libpcap\s"),
]

AFTER_UNDO_CHECKS = [
    ("undo brought tcpdump back", "tcpdump --version 2>&1", r"^tcpdump version"),
    ("undo in history", "wh history", r"\sundo\s+\d+\s+#\d+$"),
]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def fetch(port, timeout=10):
    """Fetch / from the VM's web server through the forwarded port."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
            s.sendall(b"GET / HTTP/1.0\r\n\r\n")
            return s.recv(64).startswith(b"HTTP/")
    except OSError:
        return False


# Paths the VM requested from the repository server, to detect leaks.
REQUESTS = []


def serve_repo():
    """Serve out/repo over HTTP; the VM reaches it at 10.0.2.2."""
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            REQUESTS.append(self.path)
            super().do_GET()

    handler = functools.partial(Quiet, directory=os.path.join(ROOT, "out", "repo"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f"http://10.0.2.2:{server.server_address[1]}/"


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
    web_port = free_port()
    env = dict(os.environ, QEMU_HOSTFWD=f"tcp:127.0.0.1:{web_port}-:8080")
    proc = subprocess.Popen([os.path.join(ROOT, "scripts", "run-qemu.sh"), "--snapshot"],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)
    failures = 0
    passed = 0
    repo = serve_repo()
    worm_checks = [(n, c.format(repo=repo), p) for n, c, p in WORM_CHECKS]
    panic_checks = [(n, c.format(repo=repo), p) for n, c, p in PANIC_CHECKS]
    anon_checks = [(n, c.format(repo=repo), p) for n, c, p in ANON_CHECKS]
    total = (len(CHECKS) + len(KERNEL_CHECKS) + len(DNS_CHECKS) + len(FIREWALL_CHECKS) +
             len(STATUS_CHECKS) + len(panic_checks) + len(SAFE_CHECKS) + 2 +
             len(anon_checks) + 2 +
             len(FIREWALL_OPEN_CHECKS) + len(FIREWALL_CLOSE_CHECKS) +
             len(FIREWALL_AFTER_CHECKS) + 3 + len(BLACKHOLE_CHECKS) + len(WHC_CHECKS) +
             len(SNAPSHOT_CHECKS) +
             len(BROWSE_CHECKS) + len(REWIND_CHECKS) + len(HOME_CHECKS) + len(MENU_CHECKS) +
             len(worm_checks) + len(AFTER_INSTALL_CHECKS) +
             len(AFTER_TRIAL_CHECKS) + len(AFTER_TRIAL_END_CHECKS) +
             len(AFTER_REMOVE_CHECKS) + len(AFTER_UNDO_CHECKS) + 9)
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

    def restart_with(con, name, cmd, expect):
        """Run a command that restarts the system; check the boot output."""
        con.send(cmd + "\n")
        out = con.expect("login:", args.timeout)
        check(name, expect in out, out[-500:])
        con.send("root\n")
        con.expect("# ", 60)

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
            run_checks(con, KERNEL_CHECKS)
            run_checks(con, DNS_CHECKS)
            run_checks(con, FIREWALL_CHECKS)
            check("closed port unreachable from outside", not fetch(web_port))
            run_checks(con, FIREWALL_OPEN_CHECKS)
            check("open port reachable from outside", fetch(web_port))
            run_checks(con, FIREWALL_CLOSE_CHECKS)
            check("port unreachable again after deny", not fetch(web_port))
            run_checks(con, FIREWALL_AFTER_CHECKS)
            run_checks(con, BLACKHOLE_CHECKS)
            run_checks(con, WHC_CHECKS)
            run_checks(con, STATUS_CHECKS)
            run_checks(con, anon_checks)
            check("nothing reached the network directly in anonymous mode",
                  "/anon-leak-check" not in REQUESTS, REQUESTS[-5:])
            check("direct requests arrive again after anon off",
                  "/anon-off-check" in REQUESTS, REQUESTS[-5:])
            run_checks(con, SNAPSHOT_CHECKS)
            con.send("snap browse 1\n")
            con.expect("Exit the shell to leave.", 60)
            run_checks(con, BROWSE_CHECKS)
            con.send("exit\n")
            time.sleep(1)

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

            con.send("snap rewind --home -y\n")
            out = con.expect("login:", args.timeout)
            check("home rewind applied", "rewound home to snapshot" in out, out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            run_checks(con, HOME_CHECKS)

            # Reboot and pick the newest snapshot from the boot menu: down to
            # the submenu (after safe mode), enter it, take its first entry.
            con.send("reboot\n")
            con.expect("Rewind to a snapshot", args.timeout)
            con.send("\x1b[B")
            time.sleep(0.5)
            con.send("\x1b[B")
            time.sleep(0.5)
            con.send("\r")
            con.expect("Snapshot 3", 60)
            time.sleep(0.5)
            con.send("\r")
            out = con.expect("login:", args.timeout)
            check("boot menu rewind applied", "rewound system to snapshot 3" in out,
                  out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            run_checks(con, MENU_CHECKS)

            run_checks(con, worm_checks)
            restart_with(con, "install applied", f"wh --repo {repo} -y install tcpdump",
                         "applied the package change")
            run_checks(con, AFTER_INSTALL_CHECKS)
            restart_with(con, "trial applied", "wh --try -y remove tcpdump",
                         "applied the package change (trial)")
            run_checks(con, AFTER_TRIAL_CHECKS)
            restart_with(con, "trial ended", "kill -HUP 1", "the trial was not kept")
            run_checks(con, AFTER_TRIAL_END_CHECKS)
            restart_with(con, "remove applied", "wh -y remove tcpdump",
                         "applied the package change")
            run_checks(con, AFTER_REMOVE_CHECKS)
            restart_with(con, "undo applied", "wh undo", "rewound system to snapshot")
            run_checks(con, AFTER_UNDO_CHECKS)

            run_checks(con, panic_checks)
            panic_id = re.search(r"^\s*(\d+)\*.*Panic at", con.run("snap list"), re.MULTILINE)

            # Safe mode, the second entry: a known-good snapshot, never the
            # one saved by the panic.
            con.send("reboot\n")
            out = con.expect("Rewind to a snapshot", args.timeout)
            con.send("\x1b[B")
            time.sleep(0.5)
            con.send("\r")
            out = con.expect("login:", args.timeout)
            rewound = re.search(r"rewound system to snapshot (\d+)", out)
            check("safe mode rewinds to a known-good snapshot",
                  rewound is not None and panic_id is not None and
                  rewound.group(1) != panic_id.group(1), out[-500:])
            check("safe mode leaves out the network",
                  "safe mode: not starting 30-network" in out, out[-500:])
            con.send("root\n")
            con.expect("# ", 60)
            run_checks(con, SAFE_CHECKS)

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
