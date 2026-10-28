/*
 * clip agent: shares the clipboard with the machine the VM runs on, over
 * the SPICE agent's virtio port (com.redhat.spice.0). The same protocol
 * reaches a SPICE client (virt-manager, remote-viewer) through the SPICE
 * server, and QEMU's own windows and VNC server through its qemu-vdagent
 * device. In VMware, which has no such port, vmware.c takes over.
 *
 * Only text, and only the clipboard (not the primary selection). Both
 * sides announce what they hold (grab) and send it when asked (request),
 * so nothing crosses until someone pastes. Copies made by the agent itself
 * carry MARKER_MIME, so they are never announced back.
 *
 * The protocol (spice-protocol's vd_agent.h): the port carries chunks, a
 * header {port, size} and up to 2048 bytes; messages {protocol, type,
 * opaque, size} followed by their data span one or more chunks. All
 * fields are little-endian.
 */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clip.h"

#define DEFAULT_PORT "/dev/virtio-ports/com.redhat.spice.0"

#define VDP_CLIENT_PORT 1
#define VDP_SERVER_PORT 2
#define VD_AGENT_PROTOCOL 1
#define CHUNK_MAX 2048
#define CHUNK_HEADER 8
#define MSG_HEADER 20

enum {
	MSG_CLIPBOARD = 4,
	MSG_ANNOUNCE_CAPABILITIES = 6,
	MSG_CLIPBOARD_GRAB = 7,
	MSG_CLIPBOARD_REQUEST = 8,
	MSG_CLIPBOARD_RELEASE = 9,
	MSG_CLIENT_DISCONNECTED = 13,
};

enum {
	CAP_CLIPBOARD_BY_DEMAND = 5,
	CAP_CLIPBOARD_SELECTION = 6,
	CAP_GUEST_LINEEND_LF = 8,
};

#define TYPE_NONE 0
#define TYPE_UTF8_TEXT 1
#define SELECTION_CLIPBOARD 0

/* A host waiting this long for its own text gets nothing. */
#define REQUEST_TIMEOUT 5

static int port = -1;
static const char *port_path;

static struct {
	char *data;
	size_t len, size;
} out;

/* Message bytes collected from chunks, per source port. */
static struct {
	char *data;
	size_t len, size;
} in[3];

static char chunk[CHUNK_HEADER + CHUNK_MAX];
static size_t chunk_len;

static uint32_t host_caps;
static bool have_caps;

/* The guest's clipboard, when it holds text that is not the host's. */
static struct offer *guest_offer;
static bool guest_grabbed;	/* the host was told */
static unsigned host_requests;	/* the host asked; a read is under way */
static bool guest_reading;

/* The host's clipboard, offered in the guest. */
static struct ext_data_control_source_v1 *host_source;
static char *host_text;
static size_t host_len;
static bool host_text_valid;
static int *waiting;
static size_t nwaiting;
static time_t request_time;	/* 0: no request to the host open */

static bool host_has(int cap)
{
	return have_caps && (host_caps & (1u << cap));
}

static void put32(char *p, uint32_t v)
{
	v = htole32(v);
	memcpy(p, &v, 4);
}

static uint32_t get32(const char *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return le32toh(v);
}

static void out_append(const void *data, size_t len)
{
	if (out.size - out.len < len) {
		size_t size = out.size ? out.size : 65536;

		while (size - out.len < len)
			size *= 2;
		if (!(out.data = realloc(out.data, size)))
			die("out of memory");
		out.size = size;
	}
	memcpy(out.data + out.len, data, len);
	out.len += len;
}

static void port_flush(void)
{
	while (port >= 0 && out.len > 0) {
		ssize_t n = write(port, out.data, out.len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		if (n < 0) {
			/* No one on the host side: what was queued is stale. */
			out.len = 0;
			break;
		}
		memmove(out.data, out.data + n, out.len - n);
		out.len -= n;
	}
	if (port >= 0)
		watch_events(port, POLLIN | (out.len ? POLLOUT : 0));
}

/* Sends a message; sel adds the selection header when the host knows it. */
static void send_msg(uint32_t type, bool sel, const void *data, size_t len)
{
	char head[MSG_HEADER + 4] = { 0 };
	size_t hlen = MSG_HEADER, total, done = 0;
	const char *p = data;

	if (sel && host_has(CAP_CLIPBOARD_SELECTION))
		hlen += 4;	/* selection 0 (clipboard) and three reserved bytes */
	put32(head, VD_AGENT_PROTOCOL);
	put32(head + 4, type);
	put32(head + 16, hlen - MSG_HEADER + len);
	total = hlen + len;

	while (done < total) {
		size_t n = total - done < CHUNK_MAX ? total - done : CHUNK_MAX;
		char ch[CHUNK_HEADER];
		size_t from_head = done < hlen ? (hlen - done < n ? hlen - done : n) : 0;

		put32(ch, VDP_CLIENT_PORT);
		put32(ch + 4, n);
		out_append(ch, sizeof(ch));
		if (from_head)
			out_append(head + done, from_head);
		if (n > from_head)
			out_append(p + (done + from_head - hlen), n - from_head);
		done += n;
	}
	port_flush();
}

static void send_caps(bool request)
{
	char data[8];

	put32(data, request);
	put32(data + 4, (1u << CAP_CLIPBOARD_BY_DEMAND) | (1u << CAP_CLIPBOARD_SELECTION) |
			 (1u << CAP_GUEST_LINEEND_LF));
	send_msg(MSG_ANNOUNCE_CAPABILITIES, false, data, sizeof(data));
}

static void send_type(uint32_t msg, uint32_t type)
{
	char data[4];

	put32(data, type);
	send_msg(msg, true, data, sizeof(data));
}

static void send_clipboard(uint32_t type, const char *text, size_t len)
{
	char *data = malloc(4 + len);

	if (!data)
		die("out of memory");
	put32(data, type);
	if (len)
		memcpy(data + 4, text, len);
	send_msg(MSG_CLIPBOARD, true, data, 4 + len);
	free(data);
}

/* Tells the host about the guest's clipboard, if it can take it. */
static void guest_announce(void)
{
	if (!host_has(CAP_CLIPBOARD_BY_DEMAND))
		return;
	if (guest_offer && !guest_grabbed) {
		send_type(MSG_CLIPBOARD_GRAB, TYPE_UTF8_TEXT);
		guest_grabbed = true;
	} else if (!guest_offer && guest_grabbed) {
		send_msg(MSG_CLIPBOARD_RELEASE, true, NULL, 0);
		guest_grabbed = false;
	}
}

/* The guest's text arrived for the host. */
static void guest_text_read(char *data, size_t len, void *arg)
{
	(void)arg;
	guest_reading = false;
	for (; host_requests > 0; host_requests--) {
		if (data)
			send_clipboard(TYPE_UTF8_TEXT, data, len);
		else
			send_clipboard(TYPE_NONE, NULL, 0);
	}
	free(data);
}

/* The guest's clipboard changed; the previous offer is gone. */
static void guest_selection(struct offer *offer)
{
	bool grabbed = guest_grabbed;

	if (offer_is_ours(offer)) {
		guest_offer = NULL;	/* the host's text, just offered */
		return;
	}
	guest_offer = offer_text_mime(offer) ? offer : NULL;
	/* A new copy in the guest: the host must ask again. */
	guest_grabbed = false;
	if (guest_offer)
		guest_announce();
	else if (grabbed && host_has(CAP_CLIPBOARD_BY_DEMAND))
		send_msg(MSG_CLIPBOARD_RELEASE, true, NULL, 0);
}

static void waiting_close(void)
{
	for (size_t i = 0; i < nwaiting; i++)
		close(waiting[i]);
	nwaiting = 0;
	request_time = 0;
}

static void host_forget(void)
{
	free(host_text);
	host_text = NULL;
	host_len = 0;
	host_text_valid = false;
	waiting_close();
}

static void host_send(void *data, struct ext_data_control_source_v1 *source, const char *mime,
		      int fd)
{
	int *w;

	(void)data;
	if (source != host_source || strcmp(mime, MARKER_MIME) == 0) {
		close(fd);
		return;
	}
	if (host_text_valid) {
		writer_add(fd, host_text, host_len);
		return;
	}
	if (!(w = realloc(waiting, (nwaiting + 1) * sizeof(*w))))
		die("out of memory");
	waiting = w;
	waiting[nwaiting++] = fd;
	if (!request_time) {
		send_type(MSG_CLIPBOARD_REQUEST, TYPE_UTF8_TEXT);
		request_time = time(NULL);
	}
}

static void host_cancelled(void *data, struct ext_data_control_source_v1 *source)
{
	(void)data;
	if (source == host_source) {
		host_source = NULL;
		host_forget();
	}
	ext_data_control_source_v1_destroy(source);
}

static const struct ext_data_control_source_v1_listener host_listener = {
	.send = host_send,
	.cancelled = host_cancelled,
};

/* The host copied text: offer it in the guest. */
static void host_grab(void)
{
	if (host_source)
		ext_data_control_source_v1_destroy(host_source);
	host_forget();
	host_source = source_new(&host_listener, true);
	ext_data_control_device_v1_set_selection(clip.device, host_source);
	guest_offer = NULL;
	guest_grabbed = false;
}

static void host_release(void)
{
	if (!host_source)
		return;
	ext_data_control_source_v1_destroy(host_source);
	host_source = NULL;
	host_forget();
	ext_data_control_device_v1_set_selection(clip.device, NULL);
}

static void handle_msg(uint32_t type, const char *data, size_t len)
{
	bool sel = host_has(CAP_CLIPBOARD_SELECTION);

	if (type == MSG_ANNOUNCE_CAPABILITIES) {
		if (len < 8)
			return;
		host_caps = get32(data + 4);
		have_caps = true;
		if (get32(data))
			send_caps(false);
		/* A new client knows nothing about the guest's clipboard. */
		guest_grabbed = false;
		guest_announce();
		return;
	}
	if (type != MSG_CLIPBOARD && type != MSG_CLIPBOARD_GRAB &&
	    type != MSG_CLIPBOARD_REQUEST && type != MSG_CLIPBOARD_RELEASE) {
		if (type == MSG_CLIENT_DISCONNECTED) {
			host_release();
			guest_grabbed = false;
		}
		return;
	}
	if (sel) {
		if (len < 4)
			return;
		if ((unsigned char)data[0] != SELECTION_CLIPBOARD)
			return;
		data += 4;
		len -= 4;
	}
	switch (type) {
	case MSG_CLIPBOARD_GRAB:
		for (size_t i = 0; i + 4 <= len; i += 4)
			if (get32(data + i) == TYPE_UTF8_TEXT) {
				host_grab();
				break;
			}
		break;
	case MSG_CLIPBOARD_REQUEST:
		if (len < 4 || get32(data) != TYPE_UTF8_TEXT || !guest_offer) {
			send_clipboard(TYPE_NONE, NULL, 0);
			break;
		}
		host_requests++;
		if (!guest_reading) {
			int fd = offer_receive(guest_offer, offer_text_mime(guest_offer));

			if (fd < 0) {
				guest_text_read(NULL, 0, NULL);
				break;
			}
			guest_reading = true;
			reader_add(fd, guest_text_read, NULL);
		}
		break;
	case MSG_CLIPBOARD:
		if (!request_time)
			break;
		if (len >= 4 && get32(data) == TYPE_UTF8_TEXT && host_source) {
			free(host_text);
			host_len = len - 4;
			if (!(host_text = malloc(host_len ? host_len : 1)))
				die("out of memory");
			memcpy(host_text, data + 4, host_len);
			host_text_valid = true;
			for (size_t i = 0; i < nwaiting; i++)
				writer_add(waiting[i], host_text, host_len);
			nwaiting = 0;
			request_time = 0;
		} else {
			waiting_close();
		}
		break;
	case MSG_CLIPBOARD_RELEASE:
		host_release();
		break;
	}
}

/* Takes the complete messages out of a port's buffer. */
static void messages_take(unsigned from)
{
	size_t off = 0;

	while (in[from].len - off >= MSG_HEADER) {
		const char *m = in[from].data + off;
		uint32_t size = get32(m + 16);

		if (get32(m) != VD_AGENT_PROTOCOL || size > MAX_TEXT + 64) {
			warn("bad message from the host, starting over");
			in[from].len = 0;
			return;
		}
		if (in[from].len - off < MSG_HEADER + (size_t)size)
			break;
		handle_msg(get32(m + 4), m + MSG_HEADER, size);
		off += MSG_HEADER + size;
	}
	memmove(in[from].data, in[from].data + off, in[from].len - off);
	in[from].len -= off;
}

static void chunk_take(void)
{
	uint32_t from = get32(chunk), size = get32(chunk + 4);

	if (from != VDP_CLIENT_PORT && from != VDP_SERVER_PORT)
		return;
	if (in[from].size - in[from].len < size) {
		size_t n = in[from].size ? in[from].size : 65536;

		while (n - in[from].len < size)
			n *= 2;
		if (!(in[from].data = realloc(in[from].data, n)))
			die("out of memory");
		in[from].size = n;
	}
	memcpy(in[from].data + in[from].len, chunk + CHUNK_HEADER, size);
	in[from].len += size;
	messages_take(from);
}

static void port_close(void);

static void port_ready(int fd, short revents, void *data)
{
	(void)data;
	if (revents & POLLOUT)
		port_flush();
	if (revents & POLLIN) {
		for (;;) {
			size_t want = chunk_len < CHUNK_HEADER ? CHUNK_HEADER - chunk_len
				: CHUNK_HEADER + get32(chunk + 4) - chunk_len;
			ssize_t n = read(fd, chunk + chunk_len, want);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			chunk_len += n;
			if (chunk_len == CHUNK_HEADER && get32(chunk + 4) > CHUNK_MAX) {
				warn("bad chunk from the host, starting over");
				chunk_len = 0;
				for (size_t i = 0; i < 3; i++)
					in[i].len = 0;
				continue;
			}
			if (chunk_len >= CHUNK_HEADER &&
			    chunk_len == CHUNK_HEADER + get32(chunk + 4)) {
				chunk_take();
				chunk_len = 0;
			}
		}
	}
	/* Nothing on the host side: try again later instead of spinning. */
	if ((revents & (POLLHUP | POLLERR)) && !(revents & POLLIN))
		port_close();
}

static time_t port_retry;

static void port_open(void)
{
	port = open(port_path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
	if (port < 0) {
		port_retry = time(NULL) + 2;
		return;
	}
	chunk_len = 0;
	for (size_t i = 0; i < 3; i++)
		in[i].len = 0;
	out.len = 0;
	have_caps = false;
	host_caps = 0;
	guest_grabbed = false;
	watch_add(port, POLLIN, port_ready, NULL);
	send_caps(true);
}

static void port_close(void)
{
	watch_remove(port);
	close(port);
	port = -1;
	port_retry = time(NULL) + 2;
	have_caps = false;
	host_requests = 0;
	waiting_close();
}

static void agent_tick(void)
{
	time_t now = time(NULL);

	if (port < 0 && now >= port_retry)
		port_open();
	if (request_time && now - request_time > REQUEST_TIMEOUT)
		waiting_close();
}

int agent_main(const char *path)
{
	port_path = path ? path : DEFAULT_PORT;
	if (access(port_path, F_OK) < 0) {
		if (!path && vmware_detected())
			return vmware_main();
		return 0;	/* not in a VM with an agent channel: nothing to do */
	}
	if (clip_connect() < 0)
		die("no Wayland display");
	clip.on_selection = guest_selection;
	guest_selection(clip.selection);
	port_open();
	if (port < 0)
		die("%s: %s", port_path, strerror(errno));
	loop_run(NULL, agent_tick);
	return clip.finished ? 1 : 0;
}
