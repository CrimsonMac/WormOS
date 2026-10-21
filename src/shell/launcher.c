/*
 * The application launcher: a search field over a grid of the programs
 * that have desktop entries (the system's and the ones wh exported from
 * wormholes). It covers the screen; typing filters, Enter starts the first
 * match, Escape or a click outside closes it.
 */

#include <gio/gdesktopappinfo.h>
#include <gtk4-layer-shell.h>

#include "shell.h"

static GtkWidget *window, *content, *search, *grid;

static void hide(void)
{
	gtk_widget_set_visible(window, FALSE);
}

static gboolean matches(GAppInfo *info, const char *query)
{
	const char *fields[4];
	char *needle;
	gboolean found = FALSE;

	if (!*query)
		return TRUE;
	fields[0] = g_app_info_get_display_name(info);
	fields[1] = g_app_info_get_description(info);
	fields[2] = g_app_info_get_executable(info);
	fields[3] = G_IS_DESKTOP_APP_INFO(info)
		? g_desktop_app_info_get_generic_name(G_DESKTOP_APP_INFO(info)) : NULL;
	needle = g_utf8_casefold(query, -1);
	for (int i = 0; i < 4 && !found; i++) {
		char *hay;

		if (!fields[i])
			continue;
		hay = g_utf8_casefold(fields[i], -1);
		found = strstr(hay, needle) != NULL;
		g_free(hay);
	}
	if (!found && G_IS_DESKTOP_APP_INFO(info)) {
		const char *const *keywords =
			g_desktop_app_info_get_keywords(G_DESKTOP_APP_INFO(info));

		for (int i = 0; keywords && keywords[i] && !found; i++) {
			char *hay = g_utf8_casefold(keywords[i], -1);

			found = strstr(hay, needle) != NULL;
			g_free(hay);
		}
	}
	g_free(needle);
	return found;
}

static gboolean filter(GtkFlowBoxChild *child, gpointer data)
{
	GAppInfo *info = g_object_get_data(G_OBJECT(child), "app");

	(void)data;
	return matches(info, gtk_editable_get_text(GTK_EDITABLE(search)));
}

static void launch(GAppInfo *info)
{
	GdkAppLaunchContext *ctx =
		gdk_display_get_app_launch_context(gtk_widget_get_display(window));
	GError *error = NULL;

	if (!g_app_info_launch(info, NULL, G_APP_LAUNCH_CONTEXT(ctx), &error)) {
		g_warning("cannot start %s: %s", g_app_info_get_name(info), error->message);
		g_error_free(error);
	}
	g_object_unref(ctx);
	hide();
}

static void activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer data)
{
	(void)box;
	(void)data;
	launch(g_object_get_data(G_OBJECT(child), "app"));
}

static void search_changed(GtkSearchEntry *entry, gpointer data)
{
	(void)entry;
	(void)data;
	gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(grid));
}

/* Enter: the first program that matches what was typed. */
static void search_activated(GtkSearchEntry *entry, gpointer data)
{
	(void)data;
	if (!*gtk_editable_get_text(GTK_EDITABLE(entry)))
		return;
	for (GtkWidget *c = gtk_widget_get_first_child(grid); c; c = gtk_widget_get_next_sibling(c))
		if (gtk_widget_get_child_visible(c)) {
			launch(g_object_get_data(G_OBJECT(c), "app"));
			return;
		}
}

static void search_stopped(GtkSearchEntry *entry, gpointer data)
{
	(void)entry;
	(void)data;
	hide();
}

static gboolean key_pressed(GtkEventControllerKey *ctl, guint key, guint code,
			    GdkModifierType state, gpointer data)
{
	(void)ctl;
	(void)code;
	(void)state;
	(void)data;
	if (key == GDK_KEY_Escape) {
		hide();
		return TRUE;
	}
	return FALSE;
}

static void background_clicked(GtkGestureClick *gesture, int n, double x, double y,
			       gpointer data)
{
	GtkWidget *target = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);

	(void)gesture;
	(void)n;
	(void)data;
	if (!target || (target != content && !gtk_widget_is_ancestor(target, content)))
		hide();
}

static int by_name(gconstpointer a, gconstpointer b)
{
	return g_utf8_collate(g_app_info_get_display_name(*(GAppInfo **)a),
			      g_app_info_get_display_name(*(GAppInfo **)b));
}

static void fill(void)
{
	GList *all = g_app_info_get_all();
	GPtrArray *apps = g_ptr_array_new_with_free_func(g_object_unref);
	GtkWidget *child;

	while ((child = gtk_widget_get_first_child(grid)))
		gtk_flow_box_remove(GTK_FLOW_BOX(grid), child);
	for (GList *l = all; l; l = l->next)
		if (g_app_info_should_show(l->data))
			g_ptr_array_add(apps, g_object_ref(l->data));
	g_list_free_full(all, g_object_unref);
	g_ptr_array_sort(apps, by_name);

	for (guint i = 0; i < apps->len; i++) {
		GAppInfo *info = g_ptr_array_index(apps, i);
		GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *label, *image;
		GIcon *icon = g_app_info_get_icon(info);

		image = icon ? gtk_image_new_from_gicon(icon)
			     : gtk_image_new_from_icon_name("application-x-executable");
		gtk_image_set_pixel_size(GTK_IMAGE(image), 56);
		gtk_box_append(GTK_BOX(tile), image);
		label = gtk_label_new(g_app_info_get_display_name(info));
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 14);
		gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
		gtk_box_append(GTK_BOX(tile), label);
		gtk_widget_add_css_class(tile, "app");
		if (g_app_info_get_description(info))
			gtk_widget_set_tooltip_text(tile, g_app_info_get_description(info));
		gtk_flow_box_append(GTK_FLOW_BOX(grid), tile);
		child = gtk_widget_get_parent(tile);
		g_object_set_data_full(G_OBJECT(child), "app", g_object_ref(info), g_object_unref);
	}
	g_ptr_array_free(apps, TRUE);
}

static void build(GtkApplication *app)
{
	GtkWidget *scroll;
	GtkEventController *keys;
	GtkGesture *click;

	window = gtk_application_window_new(app);
	gtk_layer_init_for_window(GTK_WINDOW(window));
	gtk_layer_set_namespace(GTK_WINDOW(window), "wormos-launcher");
	gtk_layer_set_layer(GTK_WINDOW(window), GTK_LAYER_SHELL_LAYER_OVERLAY);
	for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++)
		gtk_layer_set_anchor(GTK_WINDOW(window), e, TRUE);
	gtk_layer_set_exclusive_zone(GTK_WINDOW(window), -1);
	gtk_layer_set_keyboard_mode(GTK_WINDOW(window), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
	gtk_widget_add_css_class(window, "launcher");

	content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
	gtk_widget_add_css_class(content, "launcher-content");
	gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(content, GTK_ALIGN_FILL);

	search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Search programs");
	gtk_widget_set_halign(search, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(search, "launcher-search");
	g_signal_connect(search, "search-changed", G_CALLBACK(search_changed), NULL);
	g_signal_connect(search, "activate", G_CALLBACK(search_activated), NULL);
	g_signal_connect(search, "stop-search", G_CALLBACK(search_stopped), NULL);
	gtk_box_append(GTK_BOX(content), search);

	grid = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
	gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(grid), TRUE);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 7);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 3);
	gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid), TRUE);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 8);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 8);
	gtk_widget_set_valign(grid, GTK_ALIGN_START);
	gtk_flow_box_set_filter_func(GTK_FLOW_BOX(grid), filter, NULL, NULL);
	g_signal_connect(grid, "child-activated", G_CALLBACK(activated), NULL);
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(content), scroll);

	gtk_window_set_child(GTK_WINDOW(window), content);
	keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), NULL);
	gtk_widget_add_controller(window, keys);
	click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(background_clicked), NULL);
	gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(click));
}

void launcher_toggle(GtkApplication *app)
{
	if (!window)
		build(app);
	if (gtk_widget_get_visible(window)) {
		hide();
		return;
	}
	fill();
	gtk_editable_set_text(GTK_EDITABLE(search), "");
	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_grab_focus(search);
}
