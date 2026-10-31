#include "common/key-store.h"
#include "window/view-options.h"

#define SETTINGS_FILE "settings.ini"
#define SETTINGS_GROUP "view"

const char *const LIGHTING_NAMES[LIGHTING_COUNT] = {"studio", "daylight", "headlight", "flat"};
const char *const BACKGROUND_NAMES[BACKGROUND_COUNT] = {"charcoal", "midnight", "studio", "light", "black"};

static ViewOptions options;
static gboolean is_loaded;


int find_option_name(const char *const *names, int count, const char *name, int fallback) {
	for (int index = 0; name && index < count; index++)
		if (g_strcmp0(names[index], name) == 0)
			return index;
	return fallback;
}


static gboolean read_boolean(GKeyFile *file, const char *key, gboolean fallback) {
	g_autoptr(GError) error = NULL;
	gboolean value = g_key_file_get_boolean(file, SETTINGS_GROUP, key, &error);
	return error ? fallback : value;
}


static void load_view_options(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	options.shows_wireframe = read_boolean(file, "wireframe", FALSE);
	options.shows_skeleton = read_boolean(file, "skeleton", FALSE);
	options.shows_grid = read_boolean(file, "grid", TRUE);
	options.shows_textures = read_boolean(file, "textures", TRUE);
	options.is_turning = read_boolean(file, "turntable", FALSE);
	g_autofree char *lighting = g_key_file_get_string(file, SETTINGS_GROUP, "lighting", NULL);
	g_autofree char *background = g_key_file_get_string(file, SETTINGS_GROUP, "background", NULL);
	options.lighting = find_option_name(LIGHTING_NAMES, LIGHTING_COUNT, lighting, LIGHTING_STUDIO);
	options.background = find_option_name(BACKGROUND_NAMES, BACKGROUND_COUNT, background, BACKGROUND_CHARCOAL);
	options.mouse_speed = read_key_integer(file, SETTINGS_GROUP, "mouse-speed", DEFAULT_SPEED, MINIMUM_SPEED,
		MAXIMUM_SPEED);
	options.fly_speed = read_key_integer(file, SETTINGS_GROUP, "fly-speed", DEFAULT_SPEED, MINIMUM_SPEED, MAXIMUM_SPEED);
	is_loaded = TRUE;
}


ViewOptions *get_view_options(void) {
	if (!is_loaded)
		load_view_options();
	return &options;
}


void save_view_options(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_set_boolean(file, SETTINGS_GROUP, "wireframe", options.shows_wireframe);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "skeleton", options.shows_skeleton);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "grid", options.shows_grid);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "textures", options.shows_textures);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "turntable", options.is_turning);
	g_key_file_set_string(file, SETTINGS_GROUP, "lighting", LIGHTING_NAMES[options.lighting]);
	g_key_file_set_string(file, SETTINGS_GROUP, "background", BACKGROUND_NAMES[options.background]);
	g_key_file_set_integer(file, SETTINGS_GROUP, "mouse-speed", options.mouse_speed);
	g_key_file_set_integer(file, SETTINGS_GROUP, "fly-speed", options.fly_speed);
	save_key_file(file, path);
}
