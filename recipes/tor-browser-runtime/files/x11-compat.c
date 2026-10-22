/*
 * Stand-ins for the X11 functions Tor Browser links against.
 *
 * Tor Browser (Firefox) is built for X11 and Wayland, and its libxul is
 * linked with immediate binding against GDK's and cairo's X11 functions.
 * WormOS's GTK 3 has only the Wayland backend, so those functions do not
 * exist and the browser would not start. Firefox calls them only on an
 * X11 display; on Wayland they are never reached. The launcher preloads
 * this library so that the names resolve; each returns zero (no display,
 * no window, no atom).
 */

#define STUB(name) unsigned long name(void) { return 0; }

STUB(cairo_xlib_surface_create)
STUB(gdk_x11_atom_to_xatom)
STUB(gdk_x11_display_get_user_time)
STUB(gdk_x11_display_get_xdisplay)
STUB(gdk_x11_get_default_root_xwindow)
STUB(gdk_x11_get_default_xdisplay)
STUB(gdk_x11_get_server_time)
STUB(gdk_x11_get_xatom_by_name_for_display)
STUB(gdk_x11_lookup_xdisplay)
STUB(gdk_x11_screen_get_screen_number)
STUB(gdk_x11_screen_lookup_visual)
STUB(gdk_x11_screen_supports_net_wm_hint)
STUB(gdk_x11_set_sm_client_id)
STUB(gdk_x11_window_get_xid)
STUB(gdk_x11_window_lookup_for_display)
STUB(gdk_x11_xatom_to_atom)
