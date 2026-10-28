/*
 * powerkey - power off cleanly when the power button is pressed.
 *
 * Watches every input device that has a power key: the ACPI power button,
 * which is what QEMU's system_powerdown, virt-manager's and VMware's
 * "Shut Down" (without the tools) and a PC's power button press, and
 * keyboards with a power key. Devices that appear later are picked up
 * through inotify on /dev/input. A press asks worminit to power off
 * (SIGUSR2, as busybox poweroff does). The devices are not grabbed, so
 * the desktop still sees the key.
 *
 * Runs as root from the service 03-powerkey.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#define MAX_DEVICES 64
#define BITS_PER_LONG (8 * sizeof(long))

static struct pollfd fds[MAX_DEVICES + 1];
static int nfds = 1;    /* fds[0] is the inotify descriptor */

static int has_power_key(int fd)
{
	unsigned long keys[KEY_MAX / BITS_PER_LONG + 1] = {0};

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
		return 0;
	return (keys[KEY_POWER / BITS_PER_LONG] >> (KEY_POWER % BITS_PER_LONG)) & 1;
}

static void add_device(const char *name)
{
	char path[64], label[128] = "?";
	int fd;

	if (strncmp(name, "event", 5) || nfds > MAX_DEVICES)
		return;
	snprintf(path, sizeof path, "/dev/input/%s", name);
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	if (!has_power_key(fd)) {
		close(fd);
		return;
	}
	ioctl(fd, EVIOCGNAME(sizeof label), label);
	syslog(LOG_INFO, "watching %s (%s)", path, label);
	fds[nfds].fd = fd;
	fds[nfds].events = POLLIN;
	nfds++;
}

static int watched(const char *name)
{
	char path[64];
	struct stat node, open_dev;

	snprintf(path, sizeof path, "/dev/input/%s", name);
	if (stat(path, &node) < 0)
		return 1;
	for (int i = 1; i < nfds; i++)
		if (fstat(fds[i].fd, &open_dev) == 0 && open_dev.st_rdev == node.st_rdev)
			return 1;
	return 0;
}

static void remove_device(int i)
{
	close(fds[i].fd);
	fds[i] = fds[--nfds];
}

static int power_pressed(int fd)
{
	struct input_event ev[16];
	ssize_t n;

	while ((n = read(fd, ev, sizeof ev)) > 0)
		for (size_t i = 0; i < n / sizeof ev[0]; i++)
			if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1)
				return 1;
	return n < 0 && errno != EAGAIN ? -1 : 0;
}

int main(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	DIR *dir;
	struct dirent *d;

	openlog("powerkey", 0, LOG_DAEMON);
	fds[0].fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	fds[0].events = POLLIN;
	if (fds[0].fd < 0 || inotify_add_watch(fds[0].fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
		perror("powerkey: /dev/input");
		return 1;
	}
	if ((dir = opendir("/dev/input"))) {
		while ((d = readdir(dir)))
			add_device(d->d_name);
		closedir(dir);
	}

	for (;;) {
		if (poll(fds, nfds, -1) < 0) {
			if (errno == EINTR)
				continue;
			perror("powerkey: poll");
			return 1;
		}
		if (fds[0].revents & POLLIN) {
			ssize_t n = read(fds[0].fd, buf, sizeof buf);
			for (char *p = buf; n > 0 && p < buf + n; ) {
				struct inotify_event *ev = (struct inotify_event *)p;
				/* udev changes the node after creating it: IN_ATTRIB
				 * comes again for devices already watched. */
				if (ev->len && !watched(ev->name))
					add_device(ev->name);
				p += sizeof *ev + ev->len;
			}
		}
		for (int i = nfds - 1; i >= 1; i--) {
			int r;
			if (!fds[i].revents)
				continue;
			if (fds[i].revents & (POLLHUP | POLLERR) || (r = power_pressed(fds[i].fd)) < 0) {
				remove_device(i);
				continue;
			}
			if (r) {
				syslog(LOG_NOTICE, "power button pressed: powering off");
				sync();
				kill(1, SIGUSR2);
			}
		}
	}
}
