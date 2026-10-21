/*
 * The windows of all programs, for the panel's task list
 * (wlr-foreign-toplevel-management). The compositor announces each window
 * and its changes; the panel is told after every complete update ("done").
 */

#include <wayland-client.h>

#include "shell.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

static GPtrArray *toplevels;            /* of struct toplevel */
static GPtrArray *watchers;             /* of toplevels_changed_fn */

static void changed(void)
{
	for (guint i = 0; watchers && i < watchers->len; i++)
		((toplevels_changed_fn)g_ptr_array_index(watchers, i))();
}

static void handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
			 const char *title)
{
	struct toplevel *t = data;

	(void)h;
	g_free(t->title);
	t->title = g_strdup(title);
}

static void handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
			  const char *app_id)
{
	struct toplevel *t = data;

	(void)h;
	g_free(t->app_id);
	t->app_id = g_strdup(app_id);
}

static void handle_output_enter(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
				struct wl_output *output)
{
	(void)data;
	(void)h;
	(void)output;
}

static void handle_output_leave(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
				struct wl_output *output)
{
	(void)data;
	(void)h;
	(void)output;
}

static void handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
			 struct wl_array *state)
{
	struct toplevel *t = data;
	uint32_t *s;

	(void)h;
	t->activated = t->minimized = t->maximized = FALSE;
	wl_array_for_each(s, state) {
		if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
			t->activated = TRUE;
		else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
			t->minimized = TRUE;
		else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED)
			t->maximized = TRUE;
	}
}

static void handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t = data;

	(void)h;
	t->ready = TRUE;
	changed();
}

static void handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct toplevel *t = data;

	g_ptr_array_remove(toplevels, t);
	zwlr_foreign_toplevel_handle_v1_destroy(h);
	g_free(t->title);
	g_free(t->app_id);
	g_free(t);
	changed();
}

static void handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
			  struct zwlr_foreign_toplevel_handle_v1 *parent)
{
	(void)data;
	(void)h;
	(void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
	.title = handle_title,
	.app_id = handle_app_id,
	.output_enter = handle_output_enter,
	.output_leave = handle_output_leave,
	.state = handle_state,
	.done = handle_done,
	.closed = handle_closed,
	.parent = handle_parent,
};

static void manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
			     struct zwlr_foreign_toplevel_handle_v1 *handle)
{
	struct toplevel *t = g_new0(struct toplevel, 1);

	(void)data;
	(void)m;
	t->handle = handle;
	g_ptr_array_add(toplevels, t);
	zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, t);
}

static void manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *m)
{
	(void)data;
	zwlr_foreign_toplevel_manager_v1_destroy(m);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
	.toplevel = manager_toplevel,
	.finished = manager_finished,
};

void toplevels_bind(void *registry, uint32_t name, uint32_t version)
{
	struct zwlr_foreign_toplevel_manager_v1 *m;

	if (!toplevels)
		toplevels = g_ptr_array_new();
	m = wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface,
			     MIN(version, 3));
	zwlr_foreign_toplevel_manager_v1_add_listener(m, &manager_listener, NULL);
}

void toplevels_watch(toplevels_changed_fn fn)
{
	if (!watchers)
		watchers = g_ptr_array_new();
	g_ptr_array_add(watchers, fn);
}

GPtrArray *toplevels_list(void)
{
	if (!toplevels)
		toplevels = g_ptr_array_new();
	return toplevels;
}

void toplevel_activate(struct toplevel *t)
{
	struct wl_seat *seat = wayland_seat();

	if (t->minimized)
		zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
	if (seat)
		zwlr_foreign_toplevel_handle_v1_activate(t->handle, seat);
}

void toplevel_minimize(struct toplevel *t)
{
	zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
}

void toplevel_close(struct toplevel *t)
{
	zwlr_foreign_toplevel_handle_v1_close(t->handle);
}
