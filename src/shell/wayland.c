/*
 * The compositor's protocols that GTK does not use itself: the list of
 * windows (wlr-foreign-toplevel-management) and output configuration
 * (wlr-output-management). They are bound on GTK's own Wayland connection,
 * so their events are dispatched with GTK's.
 */

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include "shell.h"

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	(void)data;
	if (strcmp(interface, "zwlr_foreign_toplevel_manager_v1") == 0)
		toplevels_bind(registry, name, version);
	else if (strcmp(interface, "zwlr_output_manager_v1") == 0)
		outputs_bind(registry, name, version);
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

void wayland_init(void)
{
	GdkDisplay *display = gdk_display_get_default();
	struct wl_registry *registry;

	if (!GDK_IS_WAYLAND_DISPLAY(display)) {
		g_warning("not running on Wayland: no window list or display settings");
		return;
	}
	registry = wl_display_get_registry(gdk_wayland_display_get_wl_display(display));
	wl_registry_add_listener(registry, &registry_listener, NULL);
}

void *wayland_seat(void)
{
	GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());

	return seat ? gdk_wayland_seat_get_wl_seat(seat) : NULL;
}
