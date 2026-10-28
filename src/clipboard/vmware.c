/*
 * clip agent in VMware: shares text with the host through VMware's
 * backdoor, with the copy and paste of its first tools (version 1), which
 * VMware still answers and which needs no X11 (the current one lives in
 * open-vm-tools' X11 desktop plugin).
 *
 * The guest's copies go to the host as soon as they are made; VMware puts
 * them on the host's clipboard. The host's clipboard comes the other way
 * when the pointer enters the VM's window (VMware "grabs" the input): only
 * then does VMware hand over its text. The agent polls where the pointer
 * is; outside the window VMware reports -100,-100.
 *
 * Only UTF-8 text, at most 65436 bytes (VMware's limit). Windows line
 * endings from the host become \n.
 */

#include <cpuid.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "clip.h"

#define BDOOR_MAGIC 0x564D5868
#define BDOOR_PORT 0x5658
#define CMD_GETPTRLOCATION 4
#define CMD_GETSELLENGTH 6
#define CMD_GETNEXTPIECE 7
#define CMD_SETSELLENGTH 8
#define CMD_SETNEXTPIECE 9
#define CMD_GETVERSION 10
#define UNGRABBED (-100)
#define MAX_SELECTION (65536 - 100)

static uint32_t backdoor(uint32_t cmd, uint32_t arg, uint32_t *bx_out)
{
	uint32_t ax = BDOOR_MAGIC, bx = arg, cx = cmd, dx = BDOOR_PORT;

	__asm__ volatile("inl %%dx, %%eax" : "+a"(ax), "+b"(bx), "+c"(cx), "+d"(dx));
	if (bx_out)
		*bx_out = bx;
	return ax;
}

/*
 * The backdoor is an I/O port only VMware answers: elsewhere reading it
 * faults. So it is used only when the CPU says VMware is the hypervisor.
 */
bool vmware_detected(void)
{
	unsigned a, b, c, d;
	char vendor[13];
	uint32_t magic;

	__cpuid(1, a, b, c, d);
	if (!(c & (1u << 31)))
		return false;
	__cpuid(0x40000000, a, b, c, d);
	memcpy(vendor, &b, 4);
	memcpy(vendor + 4, &c, 4);
	memcpy(vendor + 8, &d, 4);
	vendor[12] = '\0';
	if (strcmp(vendor, "VMwareVMware") != 0)
		return false;
	backdoor(CMD_GETVERSION, 0, &magic);
	return magic == BDOOR_MAGIC;
}

/* The text last exchanged, so neither side's copy is sent back. */
static char *last;
static size_t last_len;
static bool reading;
static bool grabbed = true;	/* the first check sees whether it is */

static struct ext_data_control_source_v1 *host_source;
static char *host_text;
static size_t host_len;

static void remember(const char *text, size_t len)
{
	free(last);
	if (!(last = malloc(len ? len : 1)))
		die("out of memory");
	memcpy(last, text, len);
	last_len = len;
}

static bool is_last(const char *text, size_t len)
{
	return last && len == last_len && memcmp(text, last, len) == 0;
}

static void host_put(const char *text, size_t len)
{
	backdoor(CMD_SETSELLENGTH, len, NULL);
	for (size_t i = 0; i < len; i += 4) {
		uint32_t piece = 0;

		memcpy(&piece, text + i, len - i < 4 ? len - i : 4);
		backdoor(CMD_SETNEXTPIECE, piece, NULL);
	}
}

static void guest_text_read(char *data, size_t len, void *arg)
{
	(void)arg;
	reading = false;
	if (data && len > 0 && len <= MAX_SELECTION && !is_last(data, len)) {
		host_put(data, len);
		remember(data, len);
	}
	free(data);
}

/* The guest's clipboard changed: send new text to the host. */
static void guest_selection(struct offer *offer)
{
	int fd;

	if (!offer || offer_is_ours(offer) || !offer_text_mime(offer) || reading)
		return;
	fd = offer_receive(offer, offer_text_mime(offer));
	if (fd < 0)
		return;
	reading = true;
	reader_add(fd, guest_text_read, NULL);
}

static void host_send(void *data, struct ext_data_control_source_v1 *source, const char *mime,
		      int fd)
{
	(void)data;
	if (source != host_source || strcmp(mime, MARKER_MIME) == 0) {
		close(fd);
		return;
	}
	writer_add(fd, host_text, host_len);
}

static void host_cancelled(void *data, struct ext_data_control_source_v1 *source)
{
	(void)data;
	if (source == host_source)
		host_source = NULL;
	ext_data_control_source_v1_destroy(source);
}

static const struct ext_data_control_source_v1_listener host_listener = {
	.send = host_send,
	.cancelled = host_cancelled,
};

/* The pointer just entered the window: take the host's text, if new. */
static void host_fetch(void)
{
	int32_t len = (int32_t)backdoor(CMD_GETSELLENGTH, 0, NULL);
	char *text;
	size_t n = 0;

	if (len <= 0 || len > MAX_SELECTION)
		return;
	if (!(text = malloc((size_t)len + 4)))
		die("out of memory");
	for (int32_t i = 0; i < len; i += 4) {
		uint32_t piece = backdoor(CMD_GETNEXTPIECE, 0, NULL);

		memcpy(text + i, &piece, 4);
	}
	/* Windows line endings become \n; a trailing NUL is dropped. */
	for (int32_t i = 0; i < len; i++)
		if (!(text[i] == '\r' && i + 1 < len && text[i + 1] == '\n'))
			text[n++] = text[i];
	while (n > 0 && text[n - 1] == '\0')
		n--;
	if (n == 0 || is_last(text, n)) {
		free(text);
		return;
	}
	remember(text, n);
	free(host_text);
	host_text = text;
	host_len = n;
	if (host_source)
		ext_data_control_source_v1_destroy(host_source);
	host_source = source_new(&host_listener, true);
	ext_data_control_device_v1_set_selection(clip.device, host_source);
}

static void vmware_tick(void)
{
	uint32_t where = backdoor(CMD_GETPTRLOCATION, 0, NULL);
	bool now = !((int16_t)(where >> 16) == UNGRABBED && (int16_t)(where & 0xffff) == UNGRABBED);

	if (now && !grabbed)
		host_fetch();
	grabbed = now;
}

int vmware_main(void)
{
	if (clip_connect() < 0)
		die("no Wayland display");
	clip.on_selection = guest_selection;
	guest_selection(clip.selection);
	loop_run(NULL, vmware_tick);
	return clip.finished ? 1 : 0;
}
