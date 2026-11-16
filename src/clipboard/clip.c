/*
 * clip: the clipboard from the command line, and the agent that shares it
 * with the machine the VM runs on.
 *
 *   clip copy [TEXT...]   copy TEXT, or standard input
 *   clip paste            print the clipboard
 *   clip agent [PORT]     share the clipboard over the SPICE agent port
 *
 * It uses the compositor's data control protocol (ext-data-control-v1),
 * which works without a window or keyboard focus. Like any source on
 * Wayland, a copy only lasts while its program runs, so `clip copy` stays
 * in the background until something else is copied.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clip.h"

struct clip clip;

const char *const text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT", NULL,
};

void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("clip: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

void warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("clip: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* Offers */

static void offer_destroy(struct offer *offer)
{
	if (!offer)
		return;
	ext_data_control_offer_v1_destroy(offer->obj);
	for (size_t i = 0; i < offer->nmimes; i++)
		free(offer->mimes[i]);
	free(offer->mimes);
	free(offer);
}

static void offer_mime(void *data, struct ext_data_control_offer_v1 *obj, const char *mime)
{
	struct offer *offer = data;
	char **mimes;

	(void)obj;
	if (offer->nmimes >= 64)
		return;
	mimes = realloc(offer->mimes, (offer->nmimes + 1) * sizeof(*mimes));
	if (!mimes || !(mimes[offer->nmimes] = strdup(mime)))
		die("out of memory");
	offer->mimes = mimes;
	offer->nmimes++;
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
	.offer = offer_mime,
};

static bool offer_has(const struct offer *offer, const char *mime)
{
	for (size_t i = 0; i < offer->nmimes; i++)
		if (strcmp(offer->mimes[i], mime) == 0)
			return true;
	return false;
}

const char *offer_text_mime(const struct offer *offer)
{
	if (!offer)
		return NULL;
	for (size_t i = 0; text_mimes[i]; i++)
		if (offer_has(offer, text_mimes[i]))
			return text_mimes[i];
	return NULL;
}

bool offer_is_ours(const struct offer *offer)
{
	return offer && offer_has(offer, MARKER_MIME);
}

/* Asks the offer's owner to write it into a pipe; returns the reading end. */
int offer_receive(struct offer *offer, const char *mime)
{
	int p[2];

	if (pipe2(p, O_CLOEXEC) < 0)
		return -1;
	ext_data_control_offer_v1_receive(offer->obj, mime, p[1]);
	close(p[1]);
	wl_display_flush(clip.display);
	return p[0];
}

/* The data device */

static void device_data_offer(void *data, struct ext_data_control_device_v1 *device,
			      struct ext_data_control_offer_v1 *obj)
{
	struct offer *offer = calloc(1, sizeof(*offer));

	(void)data;
	(void)device;
	if (!offer)
		die("out of memory");
	offer->obj = obj;
	ext_data_control_offer_v1_add_listener(obj, &offer_listener, offer);
}

static void device_selection(void *data, struct ext_data_control_device_v1 *device,
			     struct ext_data_control_offer_v1 *obj)
{
	(void)data;
	(void)device;
	offer_destroy(clip.selection);
	clip.selection = obj ? ext_data_control_offer_v1_get_user_data(obj) : NULL;
	if (clip.on_selection)
		clip.on_selection(clip.selection);
}

static void device_primary(void *data, struct ext_data_control_device_v1 *device,
			   struct ext_data_control_offer_v1 *obj)
{
	(void)data;
	(void)device;
	offer_destroy(clip.primary);
	clip.primary = obj ? ext_data_control_offer_v1_get_user_data(obj) : NULL;
}

static void device_finished(void *data, struct ext_data_control_device_v1 *device)
{
	(void)data;
	(void)device;
	clip.finished = true;
}

static const struct ext_data_control_device_v1_listener device_listener = {
	.data_offer = device_data_offer,
	.selection = device_selection,
	.finished = device_finished,
	.primary_selection = device_primary,
};

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
			    int32_t pw, int32_t ph, int32_t subpixel, const char *make,
			    const char *model, int32_t transform)
{
	(void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph;
	(void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t width,
			int32_t height, int32_t refresh)
{
	struct output *out = data;

	(void)o;
	(void)refresh;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		out->width = width;
		out->height = height;
	}
}

static void output_done(void *data, struct wl_output *o)
{
	(void)data;
	(void)o;
}

static void output_scale(void *data, struct wl_output *o, int32_t factor)
{
	(void)data;
	(void)o;
	(void)factor;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	(void)data;
	if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 &&
	    !clip.pointer_manager) {
		clip.pointer_manager_version = version < 2 ? version : 2;
		clip.pointer_manager = wl_registry_bind(registry, name,
							&zwlr_virtual_pointer_manager_v1_interface,
							clip.pointer_manager_version);
	} else if (strcmp(interface, wl_output_interface.name) == 0 &&
		   clip.noutputs < MAX_OUTPUTS && version >= 2) {
		struct output *out = &clip.outputs[clip.noutputs++];

		out->obj = wl_registry_bind(registry, name, &wl_output_interface, 2);
		wl_output_add_listener(out->obj, &output_listener, out);
	} else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 &&
		   !clip.manager) {
		clip.manager = wl_registry_bind(registry, name,
						&ext_data_control_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !clip.seat)
		clip.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* Connects and waits for the current clipboard. Returns -1 without a display. */
int clip_connect(void)
{
	struct wl_registry *registry;

	clip.display = wl_display_connect(NULL);
	if (!clip.display)
		return -1;
	registry = wl_display_get_registry(clip.display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(clip.display);
	if (!clip.manager || !clip.seat)
		die("the compositor offers no clipboard access (ext-data-control-v1)");
	clip.device = ext_data_control_manager_v1_get_data_device(clip.manager, clip.seat);
	ext_data_control_device_v1_add_listener(clip.device, &device_listener, NULL);
	wl_display_roundtrip(clip.display);
	return 0;
}

/* A source offering text; marker adds MARKER_MIME (the agent's copies). */
struct ext_data_control_source_v1 *source_new(const struct ext_data_control_source_v1_listener *l,
					      bool marker)
{
	struct ext_data_control_source_v1 *source =
		ext_data_control_manager_v1_create_data_source(clip.manager);

	ext_data_control_source_v1_add_listener(source, l, NULL);
	for (size_t i = 0; text_mimes[i]; i++)
		ext_data_control_source_v1_offer(source, text_mimes[i]);
	if (marker)
		ext_data_control_source_v1_offer(source, MARKER_MIME);
	return source;
}

/* The poll loop */

struct watch {
	int fd;
	short events;
	watch_fn fn;
	void *data;
};

static struct watch *watches;
static size_t nwatches;

int watch_add(int fd, short events, watch_fn fn, void *data)
{
	struct watch *w = realloc(watches, (nwatches + 1) * sizeof(*w));

	if (!w)
		die("out of memory");
	watches = w;
	watches[nwatches++] = (struct watch){ fd, events, fn, data };
	return 0;
}

void watch_events(int fd, short events)
{
	for (size_t i = 0; i < nwatches; i++)
		if (watches[i].fd == fd)
			watches[i].events = events;
}

/* Marks the watch removed; the loop drops it after the current round. */
void watch_remove(int fd)
{
	for (size_t i = 0; i < nwatches; i++)
		if (watches[i].fd == fd)
			watches[i].fd = -1;
}

static void watches_compact(void)
{
	size_t j = 0;

	for (size_t i = 0; i < nwatches; i++)
		if (watches[i].fd >= 0)
			watches[j++] = watches[i];
	nwatches = j;
}

void loop_run(bool (*done)(void), void (*tick)(void))
{
	int wfd = wl_display_get_fd(clip.display);

	while (!clip.finished && !(done && done())) {
		size_t n = nwatches;
		struct pollfd *pfd = calloc(n + 1, sizeof(*pfd));

		if (!pfd)
			die("out of memory");
		while (wl_display_prepare_read(clip.display) != 0)
			wl_display_dispatch_pending(clip.display);
		if (wl_display_flush(clip.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(clip.display);
			die("lost the connection to the compositor");
		}
		pfd[0] = (struct pollfd){ .fd = wfd, .events = POLLIN };
		for (size_t i = 0; i < n; i++)
			pfd[i + 1] = (struct pollfd){ .fd = watches[i].fd, .events = watches[i].events };
		if (poll(pfd, n + 1, tick ? 500 : -1) < 0) {
			wl_display_cancel_read(clip.display);
			free(pfd);
			if (errno == EINTR)
				continue;
			die("poll: %s", strerror(errno));
		}
		if (pfd[0].revents & POLLIN) {
			if (wl_display_read_events(clip.display) < 0)
				die("lost the connection to the compositor");
		} else {
			wl_display_cancel_read(clip.display);
		}
		if (pfd[0].revents & (POLLERR | POLLHUP))
			die("lost the connection to the compositor");
		if (wl_display_dispatch_pending(clip.display) < 0)
			die("lost the connection to the compositor");
		/* Callbacks may add watches (beyond n) or remove them (fd -1). */
		for (size_t i = 0; i < n; i++)
			if (pfd[i + 1].revents && watches[i].fd == pfd[i + 1].fd)
				watches[i].fn(watches[i].fd, pfd[i + 1].revents, watches[i].data);
		free(pfd);
		watches_compact();
		if (tick)
			tick();
	}
}

/* Background writes and reads */

struct writer {
	char *data;
	size_t len, off;
};

static size_t nwriters;

static void writer_ready(int fd, short revents, void *data)
{
	struct writer *w = data;
	ssize_t n = 0;

	if (revents & POLLOUT)
		n = write(fd, w->data + w->off, w->len - w->off);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0)
		w->off += n;
	if (n <= 0 || w->off == w->len || (revents & (POLLERR | POLLHUP))) {
		watch_remove(fd);
		close(fd);
		free(w->data);
		free(w);
		nwriters--;
	}
}

void writer_add(int fd, const char *data, size_t len)
{
	struct writer *w = calloc(1, sizeof(*w));

	if (!w || !(w->data = malloc(len ? len : 1)))
		die("out of memory");
	memcpy(w->data, data, len);
	w->len = len;
	if (len == 0) {
		close(fd);
		free(w->data);
		free(w);
		return;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	nwriters++;
	watch_add(fd, POLLOUT, writer_ready, w);
}

size_t writers_pending(void)
{
	return nwriters;
}

struct reader {
	char *data;
	size_t len, size;
	void (*fn)(char *data, size_t len, void *arg);
	void *arg;
};

static void reader_ready(int fd, short revents, void *data)
{
	struct reader *r = data;
	ssize_t n;

	(void)revents;
	if (r->size - r->len < 65536) {
		size_t size = r->size ? r->size * 2 : 131072;
		char *p = size <= MAX_TEXT + 65536 ? realloc(r->data, size) : NULL;

		if (!p) {
			n = -1;
			errno = EFBIG;
			goto end;
		}
		r->data = p;
		r->size = size;
	}
	n = read(fd, r->data + r->len, r->size - r->len);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0) {
		r->len += n;
		if (r->len <= MAX_TEXT)
			return;
		n = -1;
	}
end:
	watch_remove(fd);
	close(fd);
	if (n < 0) {
		free(r->data);
		r->fn(NULL, 0, r->arg);
	} else {
		r->fn(r->data, r->len, r->arg);
	}
	free(r);
}

void reader_add(int fd, void (*fn)(char *data, size_t len, void *arg), void *arg)
{
	struct reader *r = calloc(1, sizeof(*r));

	if (!r)
		die("out of memory");
	r->fn = fn;
	r->arg = arg;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	watch_add(fd, POLLIN, reader_ready, r);
}

/* clip copy */

static char *copy_text;
static size_t copy_len;
static bool copy_cancelled;

static void copy_send(void *data, struct ext_data_control_source_v1 *source, const char *mime,
		      int fd)
{
	(void)data;
	(void)source;
	(void)mime;
	writer_add(fd, copy_text, copy_len);
}

static void copy_cancel(void *data, struct ext_data_control_source_v1 *source)
{
	(void)data;
	ext_data_control_source_v1_destroy(source);
	copy_cancelled = true;
}

static const struct ext_data_control_source_v1_listener copy_listener = {
	.send = copy_send,
	.cancelled = copy_cancel,
};

/* Done once something else owns the clipboard and every paste is served. */
static bool copy_done(void)
{
	return copy_cancelled && writers_pending() == 0;
}

static void read_all(int fd, char **data, size_t *len)
{
	size_t size = 0;
	ssize_t n;

	*data = NULL;
	*len = 0;
	for (;;) {
		if (size - *len < 65536) {
			size = size ? size * 2 : 65536;
			if (size > MAX_TEXT + 65536 || !(*data = realloc(*data, size)))
				die("input too large (at most %u MiB)", MAX_TEXT >> 20);
		}
		n = read(fd, *data + *len, size - *len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			die("read: %s", strerror(errno));
		if (n == 0)
			break;
		*len += n;
		if (*len > MAX_TEXT)
			die("input too large (at most %u MiB)", MAX_TEXT >> 20);
	}
}

static int cmd_copy(int argc, char **argv)
{
	struct ext_data_control_source_v1 *source;
	pid_t pid;

	if (argc > 0) {
		size_t len = 0;

		for (int i = 0; i < argc; i++)
			len += strlen(argv[i]) + 1;
		if (!(copy_text = malloc(len)))
			die("out of memory");
		for (int i = 0; i < argc; i++) {
			size_t l = strlen(argv[i]);

			memcpy(copy_text + copy_len, argv[i], l);
			copy_len += l;
			if (i + 1 < argc)
				copy_text[copy_len++] = ' ';
		}
	} else {
		read_all(STDIN_FILENO, &copy_text, &copy_len);
	}
	if (clip_connect() < 0)
		die("no Wayland display (WAYLAND_DISPLAY is not set or the desktop is not running)");
	source = source_new(&copy_listener, false);
	ext_data_control_device_v1_set_selection(clip.device, source);
	wl_display_roundtrip(clip.display);
	if (copy_cancelled)
		die("the compositor did not take the copy");

	/* Serve pastes in the background, like any program that copied. */
	pid = fork();
	if (pid < 0)
		die("fork: %s", strerror(errno));
	if (pid > 0)
		_exit(0);
	setsid();
	int null = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		close(null);
	}
	loop_run(copy_done, NULL);
	return 0;
}

/* clip paste */

static int cmd_paste(void)
{
	const char *mime;
	char buf[65536];
	ssize_t n;
	int fd;

	if (clip_connect() < 0)
		die("no Wayland display (WAYLAND_DISPLAY is not set or the desktop is not running)");
	if (!clip.selection)
		die("the clipboard is empty");
	mime = offer_text_mime(clip.selection);
	if (!mime)
		die("the clipboard holds no text");
	fd = offer_receive(clip.selection, mime);
	if (fd < 0)
		die("pipe: %s", strerror(errno));
	while ((n = read(fd, buf, sizeof(buf))) != 0) {
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			die("read: %s", strerror(errno));
		if (fwrite(buf, 1, n, stdout) != (size_t)n)
			die("write: %s", strerror(errno));
	}
	close(fd);
	return fflush(stdout) == 0 ? 0 : 1;
}

static void usage(FILE *out)
{
	fputs("usage: clip copy [TEXT...]   copy TEXT, or standard input\n"
	      "       clip paste            print the clipboard\n"
	      "       clip agent [--no-clipboard] [PORT]\n"
	      "                             share the clipboard, pointer and display size\n"
	      "                             with the VM's host (SPICE agent)\n", out);
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	if (strcmp(argv[1], "copy") == 0)
		return cmd_copy(argc - 2, argv + 2);
	if (strcmp(argv[1], "paste") == 0 && argc == 2)
		return cmd_paste();
	if (strcmp(argv[1], "agent") == 0) {
		bool clipboard = !(argc > 2 && strcmp(argv[2], "--no-clipboard") == 0);
		int rest = clipboard ? 2 : 3;

		if (argc <= rest + 1)
			return agent_main(argc == rest + 1 ? argv[rest] : NULL, clipboard);
	}
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		usage(stdout);
		return 0;
	}
	usage(stderr);
	return 2;
}
