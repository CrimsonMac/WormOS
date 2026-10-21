/* Icons for windows, from the desktop entry of the program they belong to. */

#include <gio/gdesktopappinfo.h>

#include "shell.h"

/*
 * Programs set their Wayland app_id to the name of their desktop entry
 * (org.gnome.TextEditor), or at least something close to it (foot, Firefox
 * as "firefox"). Try the exact name, then lower case, then a search.
 */
static GDesktopAppInfo *find_entry(const char *app_id)
{
	GDesktopAppInfo *info;
	char *name, *lower;
	gchar ***found;

	if (!app_id || !*app_id)
		return NULL;
	name = g_strconcat(app_id, ".desktop", NULL);
	info = g_desktop_app_info_new(name);
	g_free(name);
	if (info)
		return info;
	lower = g_ascii_strdown(app_id, -1);
	name = g_strconcat(lower, ".desktop", NULL);
	info = g_desktop_app_info_new(name);
	g_free(name);
	g_free(lower);
	if (info)
		return info;
	found = g_desktop_app_info_search(app_id);
	if (found && found[0] && found[0][0])
		info = g_desktop_app_info_new(found[0][0]);
	for (int i = 0; found && found[i]; i++)
		g_strfreev(found[i]);
	g_free(found);
	return info;
}

GIcon *app_icon(const char *app_id)
{
	GDesktopAppInfo *info = find_entry(app_id);
	GIcon *icon = NULL;

	if (info) {
		icon = g_app_info_get_icon(G_APP_INFO(info));
		if (icon)
			g_object_ref(icon);
		g_object_unref(info);
	}
	return icon ? icon : g_themed_icon_new("application-x-executable");
}
