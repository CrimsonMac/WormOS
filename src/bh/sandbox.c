/*
 * The restrictions every process in a blackhole runs under, applied by its
 * init before any program inside starts and inherited by all of them.
 *
 * - No new privileges: setuid files and file capabilities do nothing.
 * - Landlock: file access only beneath the blackhole's own root, so a file
 *   descriptor or mount leaked from the host still opens nothing there.
 * - seccomp: a filter refusing system calls a sandbox has no business
 *   making (mounts, new namespaces, kernel keyrings, modules, BPF,
 *   io_uring, terminal input injection...) and every non-x86_64 ABI.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
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

	ruleset = syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
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
	struct sock_filter f[128];
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

	/* ioctl: no faking terminal input, which could reach the host shell. */
	f[n++] = JUMP(BPF_JEQ, SYS_ioctl, 0, 4);
	f[n++] = LOAD(args[1]);
	f[n++] = JUMP(BPF_JEQ, TIOCSTI, 1, 0);
	f[n++] = JUMP(BPF_JEQ, TIOCLINUX, 0, 1);
	f[n++] = DENY(EPERM);

	f[n++] = RET(SECCOMP_RET_ALLOW);

	prog.len = n;
	prog.filter = f;
	if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) < 0) {
		warn("cannot install the seccomp filter: %s", strerror(errno));
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
