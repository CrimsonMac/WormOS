/*
 * The volume of the default sound output, through WirePlumber's wpctl.
 * It is read every few seconds and whenever something changes it here (the
 * panel's menu, scrolling on its button, the volume keys). Changes run one
 * at a time; a new level replaces one still waiting, so dragging the slider
 * never builds a queue. Until every change is done, readings are dropped:
 * they would move the slider back under the pointer.
 */

#include <stdio.h>

#include "shell.h"

#define SINK "@DEFAULT_AUDIO_SINK@"
#define STEP 0.05

static struct volume state;
static volume_changed_fn changed;
static GQueue pending = G_QUEUE_INIT;   /* of char ** (argument lists) */
static gboolean running;                /* a change is running */
static gboolean reading;                /* a reading is running */
static gboolean read_again;             /* read once more when it finishes */
static guint generation;                /* bumped by every change */

static void run_next(void);

static void spawn(const char *const *argv, GAsyncReadyCallback done, gpointer data)
{
	GError *error = NULL;
	GSubprocess *proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
					      G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);

	if (!proc) {
		/* No wpctl: no sound server installed. */
		GTask *task = g_task_new(NULL, NULL, done, data);

		g_task_return_error(task, error);
		g_object_unref(task);
		return;
	}
	g_subprocess_communicate_utf8_async(proc, NULL, NULL, done, data);
	g_object_unref(proc);
}

/* Output of the finished program, or NULL if it failed. */
static char *finish(GObject *source, GAsyncResult *res)
{
	char *out = NULL;

	/* spawn() failed: its task has no source object. */
	if (!source) {
		g_task_propagate_pointer(G_TASK(res), NULL);
		return NULL;
	}
	if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, &out, NULL, NULL) ||
	    !g_subprocess_get_successful(G_SUBPROCESS(source))) {
		g_free(out);
		return NULL;
	}
	return out;
}

/* ---- reading ---- */

static void description_read(GObject *source, GAsyncResult *res, gpointer data)
{
	char *out = finish(source, res), *line;

	(void)data;
	if (out && (line = strstr(out, "node.description = \""))) {
		line += strlen("node.description = \"");
		line[strcspn(line, "\"")] = '\0';
		if (g_strcmp0(state.output, line)) {
			g_free(state.output);
			state.output = g_strdup(line);
			if (changed)
				changed();
		}
	}
	g_free(out);
}

static void level_read(GObject *source, GAsyncResult *res, gpointer data)
{
	guint started = GPOINTER_TO_UINT(data);
	char *out = finish(source, res);
	double level;

	reading = FALSE;
	if (started == generation && g_queue_is_empty(&pending) && !running) {
		/* "Volume: 0.40" or "Volume: 0.40 [MUTED]" */
		if (out && sscanf(out, "Volume: %lf", &level) == 1) {
			state.available = TRUE;
			state.level = level;
			state.muted = strstr(out, "[MUTED]") != NULL;
		} else {
			state.available = FALSE;
		}
		if (changed)
			changed();
	}
	g_free(out);
	if (read_again) {
		read_again = FALSE;
		volume_refresh(FALSE);
	}
}

void volume_refresh(gboolean details)
{
	static const char *const get[] = { "wpctl", "get-volume", SINK, NULL };
	static const char *const inspect[] = { "wpctl", "inspect", SINK, NULL };

	if (details)
		spawn(inspect, description_read, NULL);
	if (reading) {
		read_again = TRUE;
		return;
	}
	reading = TRUE;
	spawn(get, level_read, GUINT_TO_POINTER(generation));
}

/* ---- changing ---- */

static void change_done(GObject *source, GAsyncResult *res, gpointer data)
{
	(void)data;
	g_free(finish(source, res));
	running = FALSE;
	run_next();
}

static void run_next(void)
{
	char **argv = g_queue_pop_head(&pending);

	if (!argv) {
		volume_refresh(FALSE);
		return;
	}
	running = TRUE;
	spawn((const char *const *)argv, change_done, NULL);
	g_strfreev(argv);
}

/* An absolute level: "wpctl set-volume -l 1 SINK LEVEL". */
static gboolean sets_level(char **argv)
{
	return !strcmp(argv[1], "set-volume") && !strpbrk(argv[5], "+-");
}

static void change(char **argv)
{
	char **last = g_queue_peek_tail(&pending);

	generation++;
	/* A level still waiting is replaced by the new one. */
	if (last && sets_level(last) && sets_level(argv))
		g_strfreev(g_queue_pop_tail(&pending));
	g_queue_push_tail(&pending, argv);
	if (!running)
		run_next();
}

void volume_set(double level)
{
	char value[32];

	level = CLAMP(level, 0, 1);
	g_ascii_formatd(value, sizeof(value), "%.3f", level);
	state.level = level;
	change(g_strdupv((char *[]){ "wpctl", "set-volume", "-l", "1", SINK, value, NULL }));
}

void volume_step(int direction)
{
	char value[32];

	g_snprintf(value, sizeof(value), "%d%%%c", (int)(STEP * 100), direction > 0 ? '+' : '-');
	state.level = CLAMP(state.level + direction * STEP, 0, 1);
	change(g_strdupv((char *[]){ "wpctl", "set-volume", "-l", "1", SINK, value, NULL }));
}

void volume_set_muted(gboolean muted)
{
	state.muted = muted;
	change(g_strdupv((char *[]){ "wpctl", "set-mute", SINK, muted ? "1" : "0", NULL }));
}

void volume_toggle_mute(void)
{
	state.muted = !state.muted;
	change(g_strdupv((char *[]){ "wpctl", "set-mute", SINK, "toggle", NULL }));
}

const struct volume *volume_get(void)
{
	return &state;
}

void volume_watch(volume_changed_fn fn)
{
	changed = fn;
	volume_refresh(TRUE);
}
