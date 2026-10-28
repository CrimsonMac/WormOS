/*
 * A keyboard and an absolute pointer for tests that cannot send input from
 * the host (VMware): a uinput device driven by commands on a FIFO.
 *
 *   input-tool FIFO &          creates the devices, then reads FIFO
 *   echo 'type hello' > FIFO   types text (US layout)
 *   echo 'key ctrl+alt+t' > FIFO
 *   echo 'move 0.5 0.5' > FIFO moves to a position (fractions of the screen)
 *   echo 'click left' > FIFO   clicks a button (left, right, middle)
 *   echo 'hold leftshift' / 'release leftshift' > FIFO
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define ABS_MAXV 32767

static int fd;

static void emit(int type, int code, int value)
{
	struct input_event ev = {.type = type, .code = code, .value = value};
	if (write(fd, &ev, sizeof ev) != sizeof ev)
		perror("write");
}

static void sync_events(void)
{
	emit(EV_SYN, SYN_REPORT, 0);
	usleep(12000);
}

static const struct { const char *name; int code; } keys[] = {
	{"esc", KEY_ESC}, {"ret", KEY_ENTER}, {"enter", KEY_ENTER}, {"tab", KEY_TAB},
	{"backspace", KEY_BACKSPACE}, {"delete", KEY_DELETE}, {"space", KEY_SPACE},
	{"ctrl", KEY_LEFTCTRL}, {"leftctrl", KEY_LEFTCTRL}, {"shift", KEY_LEFTSHIFT},
	{"leftshift", KEY_LEFTSHIFT}, {"alt", KEY_LEFTALT}, {"super", KEY_LEFTMETA},
	{"meta", KEY_LEFTMETA}, {"capslock", KEY_CAPSLOCK}, {"up", KEY_UP},
	{"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"home", KEY_HOME},
	{"end", KEY_END}, {"pageup", KEY_PAGEUP}, {"pagedown", KEY_PAGEDOWN},
	{"insert", KEY_INSERT}, {"f1", KEY_F1}, {"f2", KEY_F2}, {"f4", KEY_F4},
	{"f5", KEY_F5}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
};

/* US layout: the key for each printable character, and whether it needs Shift. */
static const char plain[] = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
static const char shifted[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
static const int codes[] = {
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_MINUS, KEY_EQUAL, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I,
	KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G,
	KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
	KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA,
	KEY_DOT, KEY_SLASH,
};

static int key_code(const char *name)
{
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
		if (!strcmp(keys[i].name, name))
			return keys[i].code;
	if (strlen(name) == 1) {
		const char *p = strchr(plain, name[0]);
		if (p && *p)
			return codes[p - plain];
	}
	return -1;
}

static void tap(int code)
{
	emit(EV_KEY, code, 1);
	sync_events();
	emit(EV_KEY, code, 0);
	sync_events();
}

static void type_text(const char *s)
{
	for (; *s; s++) {
		const char *p;
		int shift = 0, code = -1;
		if (*s == ' ')
			code = KEY_SPACE;
		else if (*s == '\n')
			code = KEY_ENTER;
		else if ((p = strchr(plain, *s)) && *p)
			code = codes[p - plain];
		else if ((p = strchr(shifted, *s)) && *p) {
			code = codes[p - shifted];
			shift = 1;
		}
		if (code < 0) {
			fprintf(stderr, "input-tool: cannot type %c\n", *s);
			continue;
		}
		if (shift) {
			emit(EV_KEY, KEY_LEFTSHIFT, 1);
			sync_events();
		}
		tap(code);
		if (shift) {
			emit(EV_KEY, KEY_LEFTSHIFT, 0);
			sync_events();
		}
		/* About 25 keys a second, a fast typist: faster input garbles
		 * the first line of a new zsh (its suggestions start up). */
		usleep(15000);
	}
}

/* A combination such as ctrl+alt+t: press in order, release in reverse. */
static void combo(char *spec)
{
	int held[8], n = 0;
	for (char *name = strtok(spec, "+"); name && n < 8; name = strtok(NULL, "+")) {
		int code = key_code(name);
		if (code < 0) {
			fprintf(stderr, "input-tool: unknown key %s\n", name);
			break;
		}
		emit(EV_KEY, code, 1);
		sync_events();
		held[n++] = code;
	}
	while (n > 0) {
		emit(EV_KEY, held[--n], 0);
		sync_events();
	}
}

static int button_code(const char *name)
{
	if (!strcmp(name, "right"))
		return BTN_RIGHT;
	if (!strcmp(name, "middle"))
		return BTN_MIDDLE;
	return BTN_LEFT;
}

static void command(char *line)
{
	char *arg = strchr(line, ' ');
	if (arg)
		*arg++ = '\0';
	else
		arg = line + strlen(line);
	if (!strcmp(line, "type"))
		type_text(arg);
	else if (!strcmp(line, "key"))
		combo(arg);
	else if (!strcmp(line, "hold") || !strcmp(line, "release")) {
		int code = key_code(arg);
		if (code >= 0) {
			emit(EV_KEY, code, line[0] == 'h');
			sync_events();
		}
	} else if (!strcmp(line, "move")) {
		double x = 0, y = 0;
		if (sscanf(arg, "%lf %lf", &x, &y) == 2) {
			emit(EV_ABS, ABS_X, (int)(x * ABS_MAXV));
			emit(EV_ABS, ABS_Y, (int)(y * ABS_MAXV));
			sync_events();
		}
	} else if (!strcmp(line, "click")) {
		emit(EV_KEY, button_code(arg), 1);
		sync_events();
		emit(EV_KEY, button_code(arg), 0);
		sync_events();
	} else
		fprintf(stderr, "input-tool: unknown command %s\n", line);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: input-tool FIFO\n");
		return 2;
	}
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/uinput");
		return 1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	for (int code = 1; code < 128; code++)
		ioctl(fd, UI_SET_KEYBIT, code);
	ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);

	struct uinput_abs_setup abs = {.code = ABS_X, .absinfo = {.maximum = ABS_MAXV}};
	ioctl(fd, UI_ABS_SETUP, &abs);
	abs.code = ABS_Y;
	ioctl(fd, UI_ABS_SETUP, &abs);
	struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x1234,
					    .product = 0x5678}};
	strcpy(setup.name, "WormOS test input");
	ioctl(fd, UI_DEV_SETUP, &setup);
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("UI_DEV_CREATE");
		return 1;
	}

	mkfifo(argv[1], 0600);
	char line[4096];
	for (;;) {
		/* Reopened after every writer closes it. */
		FILE *in = fopen(argv[1], "r");
		if (!in) {
			perror(argv[1]);
			return 1;
		}
		while (fgets(line, sizeof line, in)) {
			line[strcspn(line, "\n")] = '\0';
			/* "type" keeps a trailing newline written as \n. */
			for (char *p = line; (p = strstr(p, "\\n")); )
				memmove(p, "\n", 1), memmove(p + 1, p + 2, strlen(p + 2) + 1);
			command(line);
		}
		fclose(in);
	}
}
