/* Shared declarations for worm-shell, the WormOS desktop shell. */

#ifndef SHELL_H
#define SHELL_H

#include <gtk/gtk.h>

#define SHELL_APP_ID   "org.wormos.Shell"
#define SHELL_CSS      "/usr/share/wormos/shell/shell.css"

/* A window of any program, as the compositor reports it (toplevels.c). */
struct toplevel {
	void *handle;           /* struct zwlr_foreign_toplevel_handle_v1 */
	char *title;
	char *app_id;
	gboolean activated, minimized, maximized;
	gboolean ready;         /* the first "done" event arrived */
};

/* What the panel shows about the system (status.c). */
struct status {
	int snapshots;          /* -1: unknown */
	int last_id;
	gint64 last_date;
	char last_comment[256];
	gboolean auto_snapshots;
	char pending[16];       /* rewind, change, trial or empty */
	gboolean anon;          /* worm anon on */
	gboolean network_cut;   /* worm panic or safe mode */
	gboolean online;        /* a network device has a carrier */
	char address[64];       /* first IPv4 address, if any */
};

/* wayland.c */
void wayland_init(void);
void *wayland_seat(void);   /* struct wl_seat */

/* toplevels.c */
typedef void (*toplevels_changed_fn)(void);
void toplevels_bind(void *registry, uint32_t name, uint32_t version);
void toplevels_watch(toplevels_changed_fn fn);
GPtrArray *toplevels_list(void);   /* of struct toplevel, not owned */
void toplevel_activate(struct toplevel *t);
void toplevel_minimize(struct toplevel *t);
void toplevel_close(struct toplevel *t);

/* outputs.c */
void outputs_bind(void *registry, uint32_t name, uint32_t version);
double outputs_scale_setting(void);    /* 0: automatic */
void outputs_set_scale(double scale);  /* 0: automatic; saved for the user */

/* status.c */
void status_read(struct status *s);

/* panel.c */
void panels_init(GtkApplication *app);

/* launcher.c */
void launcher_toggle(GtkApplication *app);

/* notify.c */
void notify_init(GtkApplication *app);

/* power.c */
void power_logout(void);
gboolean power_request(const char *what);   /* "off" or "reboot" */

/* util */
char *relative_time(gint64 unix_time);       /* "5 minutes ago"; g_free */
GIcon *app_icon(const char *app_id);         /* for a window's app_id */

#endif
