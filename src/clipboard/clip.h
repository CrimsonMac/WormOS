#ifndef CLIP_H
#define CLIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

/* Offered by every source of ours, so the agent can tell its own copies. */
#define MARKER_MIME "application/x-wormos-clip"

/* Text formats, preferred first. */
extern const char *const text_mimes[];

/* Nothing larger is copied in either direction. */
#define MAX_TEXT (16u << 20)

struct offer {
	struct ext_data_control_offer_v1 *obj;
	char **mimes;
	size_t nmimes;
};

struct clip {
	struct wl_display *display;
	struct wl_seat *seat;
	struct ext_data_control_manager_v1 *manager;
	struct ext_data_control_device_v1 *device;
	struct offer *selection;	/* the clipboard, NULL when empty */
	struct offer *primary;
	/* Called whenever the clipboard changes; NULL until set. */
	void (*on_selection)(struct offer *offer);
	bool finished;
};

extern struct clip clip;

void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int clip_connect(void);
const char *offer_text_mime(const struct offer *offer);
bool offer_is_ours(const struct offer *offer);
int offer_receive(struct offer *offer, const char *mime);
struct ext_data_control_source_v1 *source_new(const struct ext_data_control_source_v1_listener *l,
					      bool marker);

/* A small poll loop shared by copy and the agent. */
typedef void (*watch_fn)(int fd, short revents, void *data);
int watch_add(int fd, short events, watch_fn fn, void *data);
void watch_events(int fd, short events);
void watch_remove(int fd);
void loop_run(bool (*done)(void), void (*tick)(void));

/* Writes data to fd in the background and closes it when done. */
void writer_add(int fd, const char *data, size_t len);
size_t writers_pending(void);

/* Reads fd to the end in the background, then calls fn (data NULL on error). */
void reader_add(int fd, void (*fn)(char *data, size_t len, void *arg), void *arg);

int agent_main(const char *port);

/* VMware: the clipboard through its backdoor (vmware.c). */
bool vmware_detected(void);
int vmware_main(void);

#endif
