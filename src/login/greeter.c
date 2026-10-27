/*
 * wormos-greeter - the login screen and the first-boot setup.
 *
 * Runs full screen in its own labwc as the user "greeter", started by
 * wormlogin, which checks passwords and creates the first account; this
 * program only asks and passes the answers on (see wormlogin.c for the
 * messages). WORMOS_GREETER_MODE is "login" or "setup". It exits once a
 * login or the setup has succeeded, and wormlogin starts the desktop.
 */

#include <gtk/gtk.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define APP_ID      "org.wormos.Greeter"
#define SOCKET_PATH "/run/wormlogin/socket"
#define CSS         "/usr/share/wormos/greeter/greeter.css"
#define WALLPAPER   "/usr/share/backgrounds/wormos.png"

static GtkWidget *window, *clock_label, *date_label;
static GtkApplication *app;

/* ---- talking to wormlogin ---- */

/* Send the fields; the reply's message (or NULL for "ok") goes into msg. */
static gboolean ask(const char *const *fields, char *msg, size_t len)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	char buf[2048], reply[512];
	size_t n = 0;
	ssize_t r;
	int fd;

	for (int i = 0; fields[i]; i++) {
		size_t l = strlen(fields[i]);

		if (n + l + 1 > sizeof(buf)) {
			g_strlcpy(msg, "Too long.", len);
			return FALSE;
		}
		if (i)
			buf[n++] = '\0';
		memcpy(buf + n, fields[i], l);
		n += l;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    send(fd, buf, n, 0) < 0 || (r = recv(fd, reply, sizeof(reply) - 1, 0)) <= 0) {
		g_strlcpy(msg, "The login service does not answer.", len);
		if (fd >= 0)
			close(fd);
		explicit_bzero(buf, sizeof(buf));
		return FALSE;
	}
	close(fd);
	explicit_bzero(buf, sizeof(buf));
	reply[r] = '\0';
	if (!strcmp(reply, "ok"))
		return TRUE;
	g_strlcpy(msg, (size_t)r > strlen(reply) + 1 ? reply + strlen(reply) + 1 : "Failed.", len);
	return FALSE;
}

/*
 * wormlogin answers after checking (a wrong password takes two seconds),
 * so requests run in a thread and the screen stays responsive.
 */
struct request {
	char **fields;
	gboolean ok;
	char msg[256];
	void (*done)(struct request *r, gpointer data);
	gpointer data;
};

static void request_thread(GTask *task, gpointer source, gpointer data, GCancellable *c)
{
	struct request *r = data;

	(void)source;
	(void)c;
	r->ok = ask((const char *const *)r->fields, r->msg, sizeof(r->msg));
	g_task_return_boolean(task, TRUE);
}

static void request_finished(GObject *source, GAsyncResult *res, gpointer data)
{
	struct request *r = data;

	(void)source;
	(void)res;
	r->done(r, r->data);
	for (int i = 0; r->fields[i]; i++)
		explicit_bzero(r->fields[i], strlen(r->fields[i]));
	g_strfreev(r->fields);
	g_free(r);
}

static void send_request(char **fields, void (*done)(struct request *, gpointer), gpointer data)
{
	struct request *r = g_new0(struct request, 1);
	GTask *task = g_task_new(NULL, NULL, request_finished, r);

	r->fields = fields;
	r->done = done;
	r->data = data;
	g_task_set_task_data(task, r, NULL);
	g_task_run_in_thread(task, request_thread);
	g_object_unref(task);
}

/* ---- the frame: wallpaper, clock, power ---- */

static gboolean tick(gpointer data)
{
	GDateTime *now = g_date_time_new_now_local();
	char *time = g_date_time_format(now, "%H:%M"), *date = g_date_time_format(now, "%A %e %B");

	(void)data;
	gtk_label_set_text(GTK_LABEL(clock_label), time);
	gtk_label_set_text(GTK_LABEL(date_label), date);
	g_free(time);
	g_free(date);
	g_date_time_unref(now);
	return G_SOURCE_CONTINUE;
}

static void power_done(struct request *r, gpointer data)
{
	(void)r;
	(void)data;
}

static void power_clicked(GtkButton *button, gpointer data)
{
	(void)button;
	send_request(g_strdupv((char *[]){ "power", data, NULL }), power_done, NULL);
}

static GtkWidget *power_button(const char *icon, const char *tooltip, const char *action)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon);

	gtk_widget_set_tooltip_text(b, tooltip);
	gtk_widget_add_css_class(b, "power");
	g_signal_connect(b, "clicked", G_CALLBACK(power_clicked), (gpointer)action);
	return b;
}

/* Wallpaper behind everything, the clock above the card, power below. */
static void frame(GtkWidget *card)
{
	GtkWidget *overlay = gtk_overlay_new(), *picture, *column, *corner;

	picture = gtk_picture_new_for_filename(WALLPAPER);
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);

	column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_halign(column, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(column, GTK_ALIGN_CENTER);
	clock_label = gtk_label_new("");
	gtk_widget_add_css_class(clock_label, "clock");
	date_label = gtk_label_new("");
	gtk_widget_add_css_class(date_label, "date");
	gtk_box_append(GTK_BOX(column), clock_label);
	gtk_box_append(GTK_BOX(column), date_label);
	gtk_widget_set_margin_top(card, 28);
	gtk_box_append(GTK_BOX(column), card);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), column);

	corner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(corner, GTK_ALIGN_END);
	gtk_widget_set_valign(corner, GTK_ALIGN_END);
	gtk_widget_set_margin_end(corner, 24);
	gtk_widget_set_margin_bottom(corner, 24);
	gtk_box_append(GTK_BOX(corner), power_button("system-reboot-symbolic", "Restart", "reboot"));
	gtk_box_append(GTK_BOX(corner), power_button("system-shutdown-symbolic", "Shut down", "off"));
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), corner);

	gtk_window_set_child(GTK_WINDOW(window), overlay);
	tick(NULL);
	g_timeout_add_seconds(1, tick, NULL);
}

static GtkWidget *card_box(void)
{
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

	gtk_widget_add_css_class(card, "card");
	gtk_widget_set_size_request(card, 400, -1);
	gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
	return card;
}

static GtkWidget *text(const char *s, const char *style)
{
	GtkWidget *l = gtk_label_new(s);

	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 44);
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
	if (style)
		gtk_widget_add_css_class(l, style);
	return l;
}

/* ---- login ---- */

static GtkWidget *user_choice, *password, *login_error, *login_button;
static gboolean busy;           /* a request is waiting for its answer */
static GPtrArray *users;        /* user names, as shown in user_choice */
static GPtrArray *full_names;   /* their full names (or user names) */

static void login_done(struct request *r, gpointer data)
{
	(void)data;
	if (r->ok) {
		g_application_quit(G_APPLICATION(app));
		return;
	}
	busy = FALSE;
	gtk_label_set_text(GTK_LABEL(login_error), r->msg);
	gtk_widget_set_sensitive(password, TRUE);
	gtk_widget_set_sensitive(login_button, TRUE);
	gtk_editable_set_text(GTK_EDITABLE(password), "");
	gtk_widget_grab_focus(password);
}

static void login_clicked(GtkWidget *widget, gpointer data)
{
	guint i = user_choice ? gtk_drop_down_get_selected(GTK_DROP_DOWN(user_choice)) : 0;

	(void)widget;
	(void)data;
	if (i >= users->len || busy)
		return;
	busy = TRUE;
	gtk_label_set_text(GTK_LABEL(login_error), "");
	gtk_widget_set_sensitive(password, FALSE);
	gtk_widget_set_sensitive(login_button, FALSE);
	send_request(g_strdupv((char *[]){ "login", g_ptr_array_index(users, i),
					   (char *)gtk_editable_get_text(GTK_EDITABLE(password)),
					   NULL }),
		     login_done, NULL);
}

/* The people who can log in: accounts with IDs from 1000. */
static GtkWidget *user_list(void)
{
	GtkStringList *names = gtk_string_list_new(NULL);
	struct passwd *pw;

	users = g_ptr_array_new_with_free_func(g_free);
	full_names = g_ptr_array_new_with_free_func(g_free);
	setpwent();
	while ((pw = getpwent())) {
		char *shown;

		if (pw->pw_uid < 1000 || pw->pw_uid >= 65534)
			continue;
		g_ptr_array_add(users, g_strdup(pw->pw_name));
		g_ptr_array_add(full_names, g_strdup(pw->pw_gecos && *pw->pw_gecos ? pw->pw_gecos
										  : pw->pw_name));
		shown = pw->pw_gecos && *pw->pw_gecos && strcmp(pw->pw_gecos, pw->pw_name)
			? g_strdup_printf("%s (%s)", pw->pw_gecos, pw->pw_name)
			: g_strdup(pw->pw_name);
		gtk_string_list_append(names, shown);
		g_free(shown);
	}
	endpwent();
	return gtk_drop_down_new(G_LIST_MODEL(names), NULL);
}

static void build_login(void)
{
	GtkWidget *card = card_box(), *avatar, *choice;
	char letter[8] = "?";

	choice = user_list();
	if (users->len > 0) {
		gunichar c = g_unichar_toupper(g_utf8_get_char(g_ptr_array_index(users, 0)));

		letter[g_unichar_to_utf8(c, letter)] = '\0';
	}
	avatar = gtk_label_new(letter);
	gtk_widget_add_css_class(avatar, "avatar");
	gtk_widget_set_halign(avatar, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(card), avatar);
	if (users->len > 1) {
		user_choice = choice;
		gtk_box_append(GTK_BOX(card), user_choice);
	} else {
		gtk_box_append(GTK_BOX(card), text(users->len ? g_ptr_array_index(full_names, 0)
							      : "No accounts", "name"));
		g_object_ref_sink(choice);
		g_object_unref(choice);
	}

	password = gtk_password_entry_new();
	g_object_set(password, "placeholder-text", "Password", NULL);
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(password), TRUE);
	g_signal_connect(password, "activate", G_CALLBACK(login_clicked), NULL);
	gtk_box_append(GTK_BOX(card), password);
	login_button = gtk_button_new_with_label("Log in");
	gtk_widget_add_css_class(login_button, "suggested-action");
	g_signal_connect(login_button, "clicked", G_CALLBACK(login_clicked), NULL);
	gtk_box_append(GTK_BOX(card), login_button);
	login_error = text("", "error");
	gtk_box_append(GTK_BOX(card), login_error);

	frame(card);
	gtk_widget_grab_focus(password);
}

/* ---- first-boot setup ---- */

static GtkWidget *stack, *full_name, *user_name, *pass1, *pass2, *root1, *root2, *layout;
static GtkWidget *account_error, *root_error, *create_button, *progress;
static gboolean name_edited, suggesting;

static const char *const layouts[][2] = {
	{ "us", "English (US)" }, { "gb", "English (UK)" }, { "de", "German" },
	{ "fr", "French" }, { "es", "Spanish" }, { "it", "Italian" },
	{ "pt", "Portuguese" }, { "br", "Portuguese (Brazil)" }, { "nl", "Dutch" },
	{ "be", "Belgian" }, { "ch", "Swiss" }, { "at", "Austrian" },
	{ "se", "Swedish" }, { "no", "Norwegian" }, { "dk", "Danish" },
	{ "fi", "Finnish" }, { "pl", "Polish" }, { "cz", "Czech" },
	{ "hu", "Hungarian" }, { "ro", "Romanian" }, { "gr", "Greek" },
	{ "tr", "Turkish" }, { "ru", "Russian" }, { "ua", "Ukrainian" },
	{ "jp", "Japanese" }, { "latam", "Spanish (Latin America)" },
};

/* A user name from the full name: "Ada Lovelace" becomes "ada". */
static void name_changed(GtkEditable *e, gpointer data)
{
	const char *full = gtk_editable_get_text(e);
	char name[33];
	size_t n = 0;

	(void)data;
	if (name_edited)
		return;
	for (const char *p = full; *p && *p != ' ' && n < 32; p++) {
		char c = g_ascii_tolower(*p);

		if ((c >= 'a' && c <= 'z') || (n > 0 && ((c >= '0' && c <= '9') || c == '-' ||
							 c == '_')))
			name[n++] = c;
	}
	name[n] = '\0';
	suggesting = TRUE;
	gtk_editable_set_text(GTK_EDITABLE(user_name), name);
	suggesting = FALSE;
}

static void user_name_changed(GtkEditable *e, gpointer data)
{
	(void)e;
	(void)data;
	if (!suggesting)
		name_edited = TRUE;
}

/* Enter moves on to the next field; the user name's suggestion is selected
 * so that typing replaces it. */
static void next_field(GtkWidget *widget, gpointer data)
{
	(void)widget;
	gtk_widget_grab_focus(data);
	if (data == user_name)
		gtk_editable_select_region(GTK_EDITABLE(user_name), 0, -1);
}

static gboolean valid_user_name(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n > 32 || !((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
		return FALSE;
	for (size_t i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
		      s[i] == '-' || s[i] == '_'))
			return FALSE;
	/* first-boot refuses group names too (video, wheel...). */
	return !getpwnam(s) && !getgrnam(s);
}

static const char *password_problem(GtkWidget *a, GtkWidget *b)
{
	const char *p = gtk_editable_get_text(GTK_EDITABLE(a));

	if (!*p)
		return "Choose a password.";
	if (strcmp(p, gtk_editable_get_text(GTK_EDITABLE(b))))
		return "The two passwords are not the same.";
	if (strchr(p, '\n'))
		return "The password cannot contain line breaks.";
	return NULL;
}

static void account_next(GtkWidget *widget, gpointer data)
{
	const char *name = gtk_editable_get_text(GTK_EDITABLE(user_name));
	const char *full = gtk_editable_get_text(GTK_EDITABLE(full_name));
	const char *problem = NULL;

	(void)widget;
	(void)data;
	if (!valid_user_name(name))
		problem = getpwnam(name) || getgrnam(name) ? "That user name is taken."
			: "The user name: lowercase letters, digits, - and _, starting with a letter.";
	else if (strpbrk(full, ":,"))
		problem = "The name cannot contain : or ,";
	else
		problem = password_problem(pass1, pass2);
	gtk_label_set_text(GTK_LABEL(account_error), problem ? problem : "");
	if (problem)
		return;
	gtk_stack_set_visible_child_name(GTK_STACK(stack), "root");
	gtk_widget_grab_focus(root1);
}

static void setup_done(struct request *r, gpointer data)
{
	(void)data;
	if (r->ok) {
		g_application_quit(G_APPLICATION(app));
		return;
	}
	busy = FALSE;
	gtk_spinner_stop(GTK_SPINNER(progress));
	gtk_widget_set_visible(progress, FALSE);
	gtk_widget_set_sensitive(create_button, TRUE);
	gtk_label_set_text(GTK_LABEL(root_error), r->msg);
}

static void create_clicked(GtkWidget *widget, gpointer data)
{
	const char *problem = password_problem(root1, root2);
	guint l = gtk_drop_down_get_selected(GTK_DROP_DOWN(layout));

	(void)widget;
	(void)data;
	if (busy)
		return;
	gtk_label_set_text(GTK_LABEL(root_error), problem ? problem : "");
	if (problem)
		return;
	busy = TRUE;
	gtk_widget_set_sensitive(create_button, FALSE);
	gtk_widget_set_visible(progress, TRUE);
	gtk_spinner_start(GTK_SPINNER(progress));
	gtk_label_set_text(GTK_LABEL(root_error), "Creating your account and the first snapshot...");
	send_request(g_strdupv((char *[]){
			     "setup", (char *)gtk_editable_get_text(GTK_EDITABLE(user_name)),
			     (char *)gtk_editable_get_text(GTK_EDITABLE(full_name)),
			     (char *)gtk_editable_get_text(GTK_EDITABLE(pass1)),
			     (char *)gtk_editable_get_text(GTK_EDITABLE(root1)),
			     (char *)layouts[MIN(l, G_N_ELEMENTS(layouts) - 1)][0], NULL }),
		     setup_done, NULL);
}

static void show_page(GtkButton *button, gpointer data)
{
	(void)button;
	gtk_stack_set_visible_child_name(GTK_STACK(stack), data);
	if (!strcmp(data, "account"))
		gtk_widget_grab_focus(full_name);
}

static GtkWidget *entry(GtkWidget *box, const char *placeholder, gboolean secret)
{
	GtkWidget *e = secret ? gtk_password_entry_new() : gtk_entry_new();

	if (secret)
		gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
	g_object_set(e, "placeholder-text", placeholder, NULL);
	gtk_box_append(GTK_BOX(box), e);
	return e;
}

static GtkWidget *label(GtkWidget *box, const char *s)
{
	GtkWidget *l = gtk_label_new(s);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_add_css_class(l, "field");
	gtk_box_append(GTK_BOX(box), l);
	return l;
}

static GtkWidget *button_row(GtkWidget *box, const char *back_page, const char *next,
			     GCallback next_cb)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *b;

	gtk_widget_set_margin_top(row, 8);
	if (back_page) {
		b = gtk_button_new_with_label("Back");
		g_signal_connect(b, "clicked", G_CALLBACK(show_page), (gpointer)back_page);
		gtk_box_append(GTK_BOX(row), b);
	}
	b = gtk_button_new_with_label(next);
	gtk_widget_add_css_class(b, "suggested-action");
	gtk_widget_set_hexpand(b, TRUE);
	g_signal_connect(b, "clicked", next_cb, NULL);
	gtk_box_append(GTK_BOX(row), b);
	gtk_box_append(GTK_BOX(box), row);
	return b;
}

static void build_setup(void)
{
	GtkWidget *card = card_box(), *page, *start;
	GtkStringList *names = gtk_string_list_new(NULL);

	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	gtk_stack_set_vhomogeneous(GTK_STACK(stack), FALSE);
	gtk_box_append(GTK_BOX(card), stack);

	/* Welcome. */
	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_box_append(GTK_BOX(page), text("Welcome to WormOS", "title"));
	gtk_box_append(GTK_BOX(page), text(
		"A system you cannot break: it is read-only, every change can be undone "
		"with a snapshot, and untrusted programs run in sandboxes.", NULL));
	gtk_box_append(GTK_BOX(page), text(
		"First, create your account.", "dim"));
	start = gtk_button_new_with_label("Get started");
	gtk_widget_add_css_class(start, "suggested-action");
	g_signal_connect(start, "clicked", G_CALLBACK(show_page), "account");
	gtk_box_append(GTK_BOX(page), start);
	gtk_stack_add_named(GTK_STACK(stack), page, "welcome");

	/* The account. */
	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_append(GTK_BOX(page), text("Your account", "title"));
	label(page, "Your name");
	full_name = entry(page, "Ada Lovelace", FALSE);
	label(page, "User name");
	user_name = entry(page, "ada", FALSE);
	label(page, "Password");
	pass1 = entry(page, "Password", TRUE);
	pass2 = entry(page, "The same password again", TRUE);
	label(page, "Keyboard layout");
	for (guint i = 0; i < G_N_ELEMENTS(layouts); i++)
		gtk_string_list_append(names, layouts[i][1]);
	layout = gtk_drop_down_new(G_LIST_MODEL(names), NULL);
	gtk_box_append(GTK_BOX(page), layout);
	account_error = text("", "error");
	gtk_box_append(GTK_BOX(page), account_error);
	button_row(page, "welcome", "Next", G_CALLBACK(account_next));
	g_signal_connect(full_name, "changed", G_CALLBACK(name_changed), NULL);
	g_signal_connect(user_name, "changed", G_CALLBACK(user_name_changed), NULL);
	g_signal_connect(full_name, "activate", G_CALLBACK(next_field), user_name);
	g_signal_connect(user_name, "activate", G_CALLBACK(next_field), pass1);
	g_signal_connect(pass1, "activate", G_CALLBACK(next_field), pass2);
	g_signal_connect(pass2, "activate", G_CALLBACK(account_next), NULL);
	gtk_stack_add_named(GTK_STACK(stack), page, "account");

	/* The administrator. */
	page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_append(GTK_BOX(page), text("The root password", "title"));
	gtk_box_append(GTK_BOX(page), text(
		"Root administers the system: installing software, rewinding or deleting "
		"snapshots. Programs you run cannot do that without this password.", NULL));
	root1 = entry(page, "Root password", TRUE);
	root2 = entry(page, "The same password again", TRUE);
	root_error = text("", "error");
	gtk_box_append(GTK_BOX(page), root_error);
	progress = gtk_spinner_new();
	gtk_widget_set_visible(progress, FALSE);
	gtk_box_append(GTK_BOX(page), progress);
	create_button = button_row(page, "account", "Create account", G_CALLBACK(create_clicked));
	g_signal_connect(root1, "activate", G_CALLBACK(next_field), root2);
	g_signal_connect(root2, "activate", G_CALLBACK(create_clicked), NULL);
	gtk_stack_add_named(GTK_STACK(stack), page, "root");

	frame(card);
	gtk_widget_grab_focus(start);
}

/* ---- start ---- */

static void activate(GtkApplication *application, gpointer data)
{
	GtkCssProvider *css = gtk_css_provider_new();
	const char *mode = g_getenv("WORMOS_GREETER_MODE");

	(void)data;
	gtk_css_provider_load_from_path(css, CSS);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
						   GTK_STYLE_PROVIDER(css),
						   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
	window = gtk_application_window_new(application);
	gtk_window_set_title(GTK_WINDOW(window), "WormOS");
	gtk_widget_add_css_class(window, "greeter");
	if (mode && !strcmp(mode, "setup"))
		build_setup();
	else
		build_login();
	gtk_window_fullscreen(GTK_WINDOW(window));
	gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv)
{
	int status;

	app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
