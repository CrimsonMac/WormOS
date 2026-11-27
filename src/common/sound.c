/*
 * Sound choices from pw-dump (see sound.h).
 *
 * A sound card in PipeWire is a Device with routes (EnumRoute: one per
 * connector, with whether something is plugged in) and profiles; its nodes
 * (sinks and sources) belong to a profile's device numbers. On many
 * laptops the speakers and the headphone jack are in different profiles,
 * so only one of them has a node at a time: listing nodes (as "wpctl
 * status" does) loses the other. Listing routes keeps both, and choosing
 * one sets the profile, the route and the default node in turn.
 */

#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sound.h"

static char *route_label(const char *description, const char *name, gboolean output);

static const char *str(json_object *o, const char *key)
{
	json_object *v;

	return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string)
	       ? json_object_get_string(v) : NULL;
}

static int num(json_object *o, const char *key, int fallback)
{
	json_object *v;

	if (!o || !json_object_object_get_ex(o, key, &v))
		return fallback;
	if (json_object_is_type(v, json_type_int))
		return json_object_get_int(v);
	if (json_object_is_type(v, json_type_string))
		return atoi(json_object_get_string(v));
	return fallback;
}

static json_object *path(json_object *o, const char *a, const char *b)
{
	json_object *v = NULL;

	if (!o || !json_object_object_get_ex(o, a, &v))
		return NULL;
	if (b && !json_object_object_get_ex(v, b, &v))
		return NULL;
	return v;
}

char *sound_plain_name(const char *description, gboolean output)
{
	return description ? route_label(description, NULL, output) : NULL;
}

static void choice_free(gpointer p)
{
	struct sound_choice *c = p;

	g_free(c->label);
	g_free(c);
}

/* A connector's plain name. */
static char *route_label(const char *description, const char *name, gboolean output)
{
	const char *d = description ? description : name ? name : "";

	if (strstr(d, "Speaker"))
		return g_strdup("Speakers");
	if (strstr(d, "Headphone"))
		return g_strdup("Headphones");
	if (strstr(d, "Headset"))
		return g_strdup(output ? "Headset" : "Headset microphone");
	if (strstr(d, "Digital Microphone") || (name && strstr(name, "Dmic")))
		return g_strdup("Built-in microphone");
	if (!output && (strstr(d, "Microphone") || strstr(d, "Mic")))
		return g_strdup(name && strstr(name, "Mic1") && !strstr(d, "Stereo")
				? "Built-in microphone" : "Microphone jack");
	if (strstr(d, "HDMI") || strstr(d, "DisplayPort")) {
		char *s = g_strdup(strstr(d, "HDMI") ? strstr(d, "HDMI") : strstr(d, "DisplayPort"));
		char *out = strstr(s, " Output");

		if (out)
			*out = '\0';
		return s;
	}
	if (strstr(d, "Line"))
		return g_strdup(output ? "Line out" : "Line in");
	return g_strdup(d);
}

/* The volume PipeWire shows (wpctl's), from a node's linear channel volumes. */
static double node_volume(json_object *node)
{
	json_object *props = path(node, "info", "params"), *list, *vols;
	double sum = 0;
	size_t n;

	if (!props || !json_object_object_get_ex(props, "Props", &list) ||
	    !json_object_is_type(list, json_type_array))
		return -1;
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *p = json_object_array_get_idx(list, i);

		if (json_object_object_get_ex(p, "channelVolumes", &vols) &&
		    (n = json_object_array_length(vols)) > 0) {
			for (size_t j = 0; j < n; j++)
				sum += json_object_get_double(json_object_array_get_idx(vols, j));
			return cbrt(sum / (double)n);
		}
	}
	return -1;
}

/* The default sink's and source's node names, from the "default" metadata. */
static void defaults(json_object *all, char **sink, char **source)
{
	for (size_t i = 0; i < json_object_array_length(all); i++) {
		json_object *o = json_object_array_get_idx(all, i), *meta;
		const char *type = str(o, "type");

		if (!type || !strstr(type, "Metadata") ||
		    g_strcmp0(str(path(o, "props", NULL), "metadata.name"), "default"))
			continue;
		if (!json_object_object_get_ex(o, "metadata", &meta))
			continue;
		for (size_t j = 0; j < json_object_array_length(meta); j++) {
			json_object *m = json_object_array_get_idx(meta, j), *value;
			const char *key = str(m, "key"), *name = NULL;
			char **into = !g_strcmp0(key, "default.audio.sink") ? sink :
				      !g_strcmp0(key, "default.audio.source") ? source : NULL;

			if (!into || !json_object_object_get_ex(m, "value", &value))
				continue;
			if (json_object_is_type(value, json_type_object))
				name = str(value, "name");
			else if (json_object_is_type(value, json_type_string)) {
				json_object *parsed = json_tokener_parse(json_object_get_string(value));

				if (parsed) {
					g_free(*into);
					*into = g_strdup(str(parsed, "name"));
					json_object_put(parsed);
				}
				continue;
			}
			g_free(*into);
			*into = g_strdup(name);
		}
	}
}

static json_object *dump(void)
{
	char *out = NULL;
	json_object *all;

	if (!g_spawn_sync(NULL, (char *[]){ "pw-dump", NULL }, NULL,
			  G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
			  NULL, NULL) || !out)
		return NULL;
	all = json_tokener_parse(out);
	g_free(out);
	if (all && !json_object_is_type(all, json_type_array)) {
		json_object_put(all);
		return NULL;
	}
	return all;
}

static gint outputs_first(gconstpointer a, gconstpointer b)
{
	const struct sound_choice *x = *(const struct sound_choice *const *)a;
	const struct sound_choice *y = *(const struct sound_choice *const *)b;

	return y->output - x->output;
}

GPtrArray *sound_choices(char **signature)
{
	json_object *all = dump();
	GPtrArray *list;
	GString *sig;
	char *def_sink = NULL, *def_source = NULL;
	gboolean *used;
	size_t count;

	if (!all)
		return NULL;
	list = g_ptr_array_new_with_free_func(choice_free);
	sig = g_string_new(NULL);
	count = json_object_array_length(all);
	used = g_new0(gboolean, count);
	defaults(all, &def_sink, &def_source);

	/* Each device's connectors that may be in use (not unplugged). */
	for (size_t i = 0; i < count; i++) {
		json_object *dev = json_object_array_get_idx(all, i), *props, *params, *routes, *active;
		const char *type = str(dev, "type"), *api, *desc;
		int id = num(dev, "id", -1), cur_profile = -1, visible[2] = { 0 };
		gboolean own_name;

		props = path(dev, "info", "props");
		if (!type || !strstr(type, "Interface:Device") ||
		    g_strcmp0(str(props, "media.class"), "Audio/Device"))
			continue;
		params = path(dev, "info", "params");
		if (!params || !json_object_object_get_ex(params, "EnumRoute", &routes))
			continue;
		if (!json_object_object_get_ex(params, "Route", &active))
			active = NULL;
		{
			json_object *profiles;

			if (json_object_object_get_ex(params, "Profile", &profiles) &&
			    json_object_array_length(profiles) > 0)
				cur_profile = num(json_object_array_get_idx(profiles, 0), "index", -1);
		}
		api = str(props, "device.api");
		desc = str(props, "device.description");
		/* Bluetooth and USB devices go by their own names. */
		own_name = !g_strcmp0(api, "bluez5") || !g_strcmp0(str(props, "device.bus"), "usb");
		for (size_t r = 0; r < json_object_array_length(routes); r++)
			if (g_strcmp0(str(json_object_array_get_idx(routes, r), "available"), "no"))
				visible[!g_strcmp0(str(json_object_array_get_idx(routes, r), "direction"),
						   "Output")]++;
		for (size_t r = 0; r < json_object_array_length(routes); r++) {
			json_object *route = json_object_array_get_idx(routes, r), *devs, *profs;
			struct sound_choice *c;
			int profile = -1, rdev = -1;
			gboolean output = !g_strcmp0(str(route, "direction"), "Output");
			char *plain;

			if (!g_strcmp0(str(route, "available"), "no"))
				continue;
			if (json_object_object_get_ex(route, "profiles", &profs) &&
			    json_object_array_length(profs) > 0) {
				profile = json_object_get_int(json_object_array_get_idx(profs, 0));
				for (size_t p = 0; p < json_object_array_length(profs); p++)
					if (json_object_get_int(json_object_array_get_idx(profs, p)) == cur_profile)
						profile = cur_profile;
			}
			if (json_object_object_get_ex(route, "devices", &devs) &&
			    json_object_array_length(devs) > 0) {
				size_t k = 0;

				/* devices and profiles go in pairs when the counts match */
				if (profs && json_object_array_length(profs) == json_object_array_length(devs))
					for (size_t p = 0; p < json_object_array_length(profs); p++)
						if (json_object_get_int(json_object_array_get_idx(profs, p)) == profile)
							k = p;
				rdev = json_object_get_int(json_object_array_get_idx(devs, k));
			}
			c = g_new0(struct sound_choice, 1);
			c->output = output;
			c->device = id;
			c->route = num(route, "index", -1);
			c->route_device = rdev;
			c->profile = profile;
			c->node = -1;
			c->volume = -1;
			plain = route_label(str(route, "description"), str(route, "name"), output);
			if (own_name && desc)
				c->label = visible[output] > 1 ? g_strdup_printf("%s: %s", desc, plain)
							       : g_strdup(desc);
			else
				c->label = g_strdup(plain);
			g_free(plain);
			/* Its node, when its profile is the one in use. */
			for (size_t n = 0; n < count && profile == cur_profile; n++) {
				json_object *node = json_object_array_get_idx(all, n);
				json_object *np = path(node, "info", "props");
				const char *mc = str(np, "media.class");

				if (!mc || g_strcmp0(str(node, "type"), "PipeWire:Interface:Node") ||
				    num(np, "device.id", -2) != id ||
				    num(np, "card.profile.device", -2) != rdev ||
				    strcmp(mc, output ? "Audio/Sink" : "Audio/Source"))
					continue;
				c->node = num(node, "id", -1);
				c->volume = node_volume(node);
				c->active = !g_strcmp0(str(np, "node.name"), output ? def_sink : def_source);
				used[n] = TRUE;
			}
			/* Without a default recorded, the connector in use counts. */
			if (!(output ? def_sink : def_source) && active)
				for (size_t a = 0; a < json_object_array_length(active); a++) {
					json_object *ar = json_object_array_get_idx(active, a);

					if (num(ar, "index", -1) == c->route && num(ar, "device", -1) == rdev)
						c->active = TRUE;
				}
			g_ptr_array_add(list, c);
		}
	}

	/* Sinks and sources of their own (virtual, network...). */
	for (size_t n = 0; n < count; n++) {
		json_object *node = json_object_array_get_idx(all, n);
		json_object *np = path(node, "info", "props");
		const char *mc = str(np, "media.class"), *name = str(np, "node.description");
		struct sound_choice *c;
		gboolean output;

		if (used[n] || !mc || g_strcmp0(str(node, "type"), "PipeWire:Interface:Node") ||
		    (strcmp(mc, "Audio/Sink") && strcmp(mc, "Audio/Source")) ||
		    num(np, "device.id", -1) >= 0)
			continue;
		output = !strcmp(mc, "Audio/Sink");
		c = g_new0(struct sound_choice, 1);
		c->output = output;
		c->device = -1;
		c->node = num(node, "id", -1);
		c->volume = node_volume(node);
		c->label = g_strdup(name ? name : str(np, "node.name"));
		c->active = !g_strcmp0(str(np, "node.name"), output ? def_sink : def_source);
		g_ptr_array_add(list, c);
	}
	g_ptr_array_sort(list, outputs_first);
	for (guint i = 0; i < list->len; i++) {
		struct sound_choice *c = g_ptr_array_index(list, i);

		g_string_append_printf(sig, "%d:%s:%d:%d:%d;", c->output, c->label, c->active,
				       c->device, c->route);
	}
	if (signature)
		*signature = g_string_free(sig, FALSE);
	else
		g_string_free(sig, TRUE);
	g_free(used);
	g_free(def_sink);
	g_free(def_source);
	json_object_put(all);
	return list;
}

static void run(char **argv)
{
	g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
		     G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
}

void sound_choose(const struct sound_choice *c)
{
	char dev[16], node[16], param[128];

	if (c->device >= 0) {
		snprintf(dev, sizeof(dev), "%d", c->device);
		if (c->profile >= 0) {
			snprintf(param, sizeof(param), "{ index: %d, save: true }", c->profile);
			run((char *[]){ "pw-cli", "set-param", dev, "Profile", param, NULL });
		}
		if (c->route >= 0 && c->route_device >= 0) {
			snprintf(param, sizeof(param), "{ index: %d, device: %d, save: true }",
				 c->route, c->route_device);
			run((char *[]){ "pw-cli", "set-param", dev, "Route", param, NULL });
		}
	}
	/* The node, which a new profile only brings after a moment. */
	for (int tries = 0; tries < 20; tries++) {
		GPtrArray *now = sound_choices(NULL);
		int id = c->node;

		for (guint i = 0; now && id < 0 && i < now->len; i++) {
			struct sound_choice *n = g_ptr_array_index(now, i);

			if (n->device == c->device && n->route == c->route && n->output == c->output)
				id = n->node;
		}
		if (now)
			g_ptr_array_free(now, TRUE);
		if (id >= 0) {
			snprintf(node, sizeof(node), "%d", id);
			run((char *[]){ "wpctl", "set-default", node, NULL });
			return;
		}
		g_usleep(100000);
	}
}
