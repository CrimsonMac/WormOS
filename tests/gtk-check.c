/*
 * A GTK 4 window for the desktop test. It prints what GTK found (theme,
 * icons, settings store, renderer), then shows a window with typical
 * widgets until it is killed or a minute has passed.
 *
 * Built with the WormOS toolchain by tests/desktop-test.py.
 */

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <gtk/gtk.h>
#include <stdio.h>

static gboolean quit(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void mapped(GtkWidget *window, gpointer data)
{
	GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));

	(void)data;
	printf("renderer=%s\n", renderer ? G_OBJECT_TYPE_NAME(renderer) : "none");
	printf("shown\n");
	fflush(stdout);
}

int main(void)
{
	GtkSettings *settings;
	GtkIconTheme *icons;
	GSettingsBackend *backend;
	GtkWidget *window, *box, *header, *entry, *button, *toggle;
	GMainLoop *loop;
	char *theme, *icon_theme, *cursor_theme, *font;

	if (!gtk_init_check()) {
		printf("gtk_init failed\n");
		return 1;
	}
	settings = gtk_settings_get_default();
	g_object_get(settings, "gtk-theme-name", &theme, "gtk-icon-theme-name", &icon_theme,
		     "gtk-cursor-theme-name", &cursor_theme, "gtk-font-name", &font, NULL);
	printf("theme=%s\nicons=%s\ncursor=%s\nfont=%s\n", theme, icon_theme, cursor_theme, font);

	icons = gtk_icon_theme_get_for_display(gdk_display_get_default());
	printf("folder icon=%s\n",
	       gtk_icon_theme_has_icon(icons, "folder-symbolic") ? "yes" : "no");

	backend = g_settings_backend_get_default();
	printf("settings store=%s\n", G_OBJECT_TYPE_NAME(backend));
	g_object_unref(backend);

	window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(window), "GTK check");
	gtk_window_set_default_size(GTK_WINDOW(window), 420, 260);
	header = gtk_header_bar_new();
	gtk_window_set_titlebar(GTK_WINDOW(window), header);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_top(box, 24);
	gtk_widget_set_margin_bottom(box, 24);
	gtk_widget_set_margin_start(box, 24);
	gtk_widget_set_margin_end(box, 24);
	gtk_box_append(GTK_BOX(box), gtk_label_new("WormOS"));
	entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Type here");
	gtk_box_append(GTK_BOX(box), entry);
	toggle = gtk_switch_new();
	gtk_switch_set_active(GTK_SWITCH(toggle), TRUE);
	gtk_widget_set_halign(toggle, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(box), toggle);
	button = gtk_button_new_with_label("Take a snapshot");
	gtk_widget_add_css_class(button, "suggested-action");
	gtk_box_append(GTK_BOX(box), button);
	gtk_window_set_child(GTK_WINDOW(window), box);

	loop = g_main_loop_new(NULL, FALSE);
	g_signal_connect(window, "map", G_CALLBACK(mapped), NULL);
	g_signal_connect_swapped(window, "destroy", G_CALLBACK(g_main_loop_quit), loop);
	gtk_window_present(GTK_WINDOW(window));
	g_timeout_add_seconds(60, quit, loop);
	g_main_loop_run(loop);
	return 0;
}
