/*
 * The restrictions every process in a blackhole runs under, applied by its
 * init before any program inside starts and inherited by all of them.
 *
 * - No new privileges: setuid files and file capabilities do nothing.
 * - Landlock: file access only beneath the blackhole's own root, so a file
 *   descriptor or mount leaked from the host still opens nothing there.
 * - Landlock scopes: no signals to processes outside the blackhole and
 *   no connecting to abstract UNIX sockets created outside it.
 * - seccomp: a filter refusing system calls a sandbox has no business
 *   making (mounts, new namespaces, kernel keyrings, modules, BPF,
 *   io_uring, terminal input injection, line disciplines...), every
 *   non-x86_64 ABI, and every socket family but UNIX, IPv4, IPv6 and
 *   routing netlink (no vsock, which network namespaces do not isolate,
 *   no packet sockets, no netfilter).
 * - Capabilities: programs keep only those that act on the blackhole's
 *   own files and processes (sandbox_drop_caps), so root inside cannot
 *   configure its network namespace (tc, nf_tables), ptrace its init or
 *   use raw sockets: kernel code that has often been exploited.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/netlink.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "bh.h"

static int landlock(void)
{
	struct landlock_ruleset_attr attr = { 0 };
	struct landlock_path_beneath_attr beneath = { 0 };
	int abi, ruleset, ret;
	__u64 fs;

	abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 1) {
		warn("Landlock is not available: %s", strerror(errno));
		return -1;
	}
	/* Every file system right this kernel knows about. */
	fs = (LANDLOCK_ACCESS_FS_MAKE_SYM << 1) - 1;
	if (abi >= 2)
		fs |= LANDLOCK_ACCESS_FS_REFER;
	if (abi >= 3)
		fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
	if (abi >= 5)
		fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
	attr.handled_access_fs = fs;
	if (abi >= 6)
		attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;

	/* Older kernels reject the larger structure: pass only what they know. */
	ruleset = syscall(SYS_landlock_create_ruleset, &attr,
			  abi >= 6 ? sizeof(attr) : offsetof(struct landlock_ruleset_attr, scoped),
			  0);
	if (ruleset < 0) {
		warn("cannot create a Landlock ruleset: %s", strerror(errno));
		return -1;
	}
	beneath.allowed_access = fs;
	beneath.parent_fd = open("/", O_PATH | O_CLOEXEC);
	ret = beneath.parent_fd < 0 ? -1 :
		syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0);
	if (ret == 0)
		ret = syscall(SYS_landlock_restrict_self, ruleset, 0);
	if (ret < 0)
		warn("cannot apply Landlock: %s", strerror(errno));
	if (beneath.parent_fd >= 0)
		close(beneath.parent_fd);
	close(ruleset);
	return ret;
}

/* Refused with EPERM. */
static const int blocked[] = {
	SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot, SYS_move_mount,
	SYS_open_tree, SYS_fsopen, SYS_fsconfig, SYS_fsmount, SYS_fspick,
	SYS_mount_setattr,
#ifdef SYS_open_tree_attr
	SYS_open_tree_attr,
#endif
	SYS_unshare, SYS_setns,
	SYS_keyctl, SYS_add_key, SYS_request_key,
	SYS_bpf, SYS_perf_event_open, SYS_userfaultfd,
	SYS_io_uring_setup, SYS_io_uring_enter, SYS_io_uring_register,
	SYS_init_module, SYS_finit_module, SYS_delete_module,
	SYS_kexec_load, SYS_kexec_file_load, SYS_reboot,
	SYS_swapon, SYS_swapoff, SYS_syslog, SYS_acct, SYS_vhangup,
	SYS_settimeofday, SYS_clock_settime, SYS_clock_adjtime, SYS_adjtimex,
	SYS_iopl, SYS_ioperm, SYS_quotactl, SYS_quotactl_fd,
	SYS_open_by_handle_at, SYS_name_to_handle_at, SYS_lookup_dcookie, SYS_uselib,
	SYS_modify_ldt, SYS_fanotify_init, SYS_nfsservctl, SYS_pidfd_getfd,
	SYS_process_vm_writev,
};

#define CLONE_NEW_ANY (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | \
		       CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)
#define LOAD(field)    (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, \
					   offsetof(struct seccomp_data, field))
#define RET(action)    (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, (action))
#define DENY(err)      RET(SECCOMP_RET_ERRNO | (err))
#define JUMP(op, k, t, f) (struct sock_filter)BPF_JUMP(BPF_JMP | (op) | BPF_K, (k), (t), (f))

static int seccomp(void)
{
	struct sock_filter f[256];
	struct sock_fprog prog;
	unsigned n = 0;

	/* Only the native ABI: 32-bit and x32 calls would bypass the numbers below. */
	f[n++] = LOAD(arch);
	f[n++] = JUMP(BPF_JEQ, AUDIT_ARCH_X86_64, 1, 0);
	f[n++] = RET(SECCOMP_RET_KILL_PROCESS);
	f[n++] = LOAD(nr);
	f[n++] = JUMP(BPF_JGE, 0x40000000, 0, 1);
	f[n++] = RET(SECCOMP_RET_KILL_PROCESS);

	for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
		f[n++] = JUMP(BPF_JEQ, blocked[i], 0, 1);
		f[n++] = DENY(EPERM);
	}

	/* clone3 hides its flags in memory; libc falls back to clone. */
	f[n++] = JUMP(BPF_JEQ, SYS_clone3, 0, 1);
	f[n++] = DENY(ENOSYS);

	/* clone: no new namespaces. */
	f[n++] = JUMP(BPF_JEQ, SYS_clone, 0, 4);
	f[n++] = LOAD(args[0]);
	f[n++] = JUMP(BPF_JSET, CLONE_NEW_ANY, 0, 1);
	f[n++] = DENY(EPERM);
	f[n++] = RET(SECCOMP_RET_ALLOW);

	/*
	 * ioctl: no faking terminal input, no console tricks, no line
	 * disciplines (n_gsm and friends have been exploited for root).
	 */
	f[n++] = JUMP(BPF_JEQ, SYS_ioctl, 0, 7);
	f[n++] = LOAD(args[1]);
	f[n++] = JUMP(BPF_JEQ, TIOCSTI, 3, 0);
	f[n++] = JUMP(BPF_JEQ, TIOCLINUX, 2, 0);
	f[n++] = JUMP(BPF_JEQ, TIOCSETD, 1, 0);
	f[n++] = JUMP(BPF_JEQ, TIOCCONS, 0, 1);
	f[n++] = DENY(EPERM);
	f[n++] = RET(SECCOMP_RET_ALLOW);

	/* socket: UNIX, IPv4, IPv6, and netlink for routing and socket lists only. */
	f[n++] = JUMP(BPF_JEQ, SYS_socket, 0, 11);
	f[n++] = LOAD(args[0]);
	f[n++] = JUMP(BPF_JEQ, AF_UNIX, 8, 0);
	f[n++] = JUMP(BPF_JEQ, AF_INET, 7, 0);
	f[n++] = JUMP(BPF_JEQ, AF_INET6, 6, 0);
	f[n++] = JUMP(BPF_JEQ, AF_NETLINK, 1, 0);
	f[n++] = DENY(EAFNOSUPPORT);
	f[n++] = LOAD(args[2]);
	f[n++] = JUMP(BPF_JEQ, NETLINK_ROUTE, 2, 0);
	f[n++] = JUMP(BPF_JEQ, NETLINK_SOCK_DIAG, 1, 0);
	f[n++] = DENY(EPROTONOSUPPORT);
	f[n++] = RET(SECCOMP_RET_ALLOW);

	/* socketpair only makes sense for UNIX sockets. */
	f[n++] = JUMP(BPF_JEQ, SYS_socketpair, 0, 4);
	f[n++] = LOAD(args[0]);
	f[n++] = JUMP(BPF_JEQ, AF_UNIX, 1, 0);
	f[n++] = DENY(EAFNOSUPPORT);
	f[n++] = RET(SECCOMP_RET_ALLOW);

	f[n++] = RET(SECCOMP_RET_ALLOW);

	prog.len = n;
	prog.filter = f;
	if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) < 0) {
		warn("cannot install the seccomp filter: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Called in each program's process before exec: keep only the capabilities
 * that act on the blackhole's own files and processes. NO_NEW_PRIVS (set by
 * sandbox_apply) means exec cannot give any back.
 */
int sandbox_drop_caps(void)
{
	static const int keep[] = {
		CAP_CHOWN, CAP_DAC_OVERRIDE, CAP_FOWNER, CAP_FSETID, CAP_KILL,
		CAP_SETGID, CAP_SETUID, CAP_NET_BIND_SERVICE,
	};
	struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
	struct __user_cap_data_struct data[2] = { 0 };

	for (int cap = 0; cap <= 63; cap++) {
		int kept = 0;

		for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
			kept |= keep[i] == cap;
		if (!kept && prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL) {
			warn("cannot drop capability %d: %s", cap, strerror(errno));
			return -1;
		}
	}
	for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
		data[keep[i] / 32].effective |= 1u << (keep[i] % 32);
		data[keep[i] / 32].permitted |= 1u << (keep[i] % 32);
	}
	if (syscall(SYS_capset, &hdr, data) < 0 ||
	    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) < 0) {
		warn("cannot drop capabilities: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int sandbox_apply(void)
{
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		warn("cannot set no_new_privs: %s", strerror(errno));
		return -1;
	}
	if (landlock() < 0)
		return -1;
	return seccomp();
}
