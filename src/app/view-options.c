#include "common/storage/key-store.h"
#include "app/view-options.h"

#define SETTINGS_FILE "settings.ini"
#define SETTINGS_GROUP "view"
#define MOUSE_SPEED_KEY "mouse-speed"
#define FLY_SPEED_KEY "fly-speed"

const char *const LIGHTING_NAMES[LIGHTING_COUNT] = {"studio", "daylight", "headlight", "flat"};
const char *const BACKGROUND_NAMES[BACKGROUND_COUNT] = {"black", "midnight", "charcoal", "studio", "light"};

/* Each switch once, with the name its action and its settings key share */
static const struct {
	const char *name;
	gsize offset;
	gboolean fallback;
} SWITCHES[] = {
	{"grid", G_STRUCT_OFFSET(ViewOptions, shows_grid), TRUE},
	{"wireframe", G_STRUCT_OFFSET(ViewOptions, shows_wireframe), FALSE},
	{"skeleton", G_STRUCT_OFFSET(ViewOptions, shows_skeleton), FALSE},
	{"textures", G_STRUCT_OFFSET(ViewOptions, shows_textures), TRUE},
	{"turntable", G_STRUCT_OFFSET(ViewOptions, is_turning), FALSE},
};

/* Each choice once, with the action that steps to the next one */
static const struct {
	const char *name, *next_name;
	const char *const *names;
	int count;
	gsize offset;
	int fallback;
} CHOICES[] = {
	{"lighting", "next-lighting", LIGHTING_NAMES, LIGHTING_COUNT, G_STRUCT_OFFSET(ViewOptions, lighting),
		LIGHTING_STUDIO},
	{"background", "next-background", BACKGROUND_NAMES, BACKGROUND_COUNT, G_STRUCT_OFFSET(ViewOptions, background),
		BACKGROUND_CHARCOAL},
};

static ViewOptions options;
static gboolean is_loaded;
static ViewOptionsChanged notify_change;
static gpointer change_data;


static gboolean *get_switch(guint index) {
	return G_STRUCT_MEMBER_P(&options, SWITCHES[index].offset);
}


static int *get_choice(guint index) {
	return G_STRUCT_MEMBER_P(&options, CHOICES[index].offset);
}


static int find_choice_name(guint index, const char *name) {
	for (int choice = 0; name && choice < CHOICES[index].count; choice++)
		if (g_strcmp0(CHOICES[index].names[choice], name) == 0)
			return choice;
	return CHOICES[index].fallback;
}


static void load_view_options(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	for (guint index = 0; index < G_N_ELEMENTS(SWITCHES); index++)
		*get_switch(index) = read_key_boolean(file, SETTINGS_GROUP, SWITCHES[index].name, SWITCHES[index].fallback);
	for (guint index = 0; index < G_N_ELEMENTS(CHOICES); index++) {
		g_autofree char *name = g_key_file_get_string(file, SETTINGS_GROUP, CHOICES[index].name, NULL);
		*get_choice(index) = find_choice_name(index, name);
	}
	options.mouse_speed = read_key_integer(file, SETTINGS_GROUP, MOUSE_SPEED_KEY, DEFAULT_SPEED, MINIMUM_SPEED,
		MAXIMUM_SPEED);
	options.fly_speed = read_key_integer(file, SETTINGS_GROUP, FLY_SPEED_KEY, DEFAULT_SPEED, MINIMUM_SPEED,
		MAXIMUM_SPEED);
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
	for (guint index = 0; index < G_N_ELEMENTS(SWITCHES); index++)
		g_key_file_set_boolean(file, SETTINGS_GROUP, SWITCHES[index].name, *get_switch(index));
	for (guint index = 0; index < G_N_ELEMENTS(CHOICES); index++)
		g_key_file_set_string(file, SETTINGS_GROUP, CHOICES[index].name, CHOICES[index].names[*get_choice(index)]);
	g_key_file_set_integer(file, SETTINGS_GROUP, MOUSE_SPEED_KEY, options.mouse_speed);
	g_key_file_set_integer(file, SETTINGS_GROUP, FLY_SPEED_KEY, options.fly_speed);
	save_key_file(file, path);
}


static void finish_change(GSimpleAction *action, GVariant *value) {
	g_simple_action_set_state(action, value);
	save_view_options();
	if (notify_change)
		notify_change(change_data);
}


static void change_switch(GSimpleAction *action, GVariant *value, gpointer data) {
	*get_switch(GPOINTER_TO_UINT(data)) = g_variant_get_boolean(value);
	finish_change(action, value);
}


static void change_choice(GSimpleAction *action, GVariant *value, gpointer data) {
	guint index = GPOINTER_TO_UINT(data);
	*get_choice(index) = find_choice_name(index, g_variant_get_string(value, NULL));
	finish_change(action, value);
}


/* The step goes through the choice's own action, so its menus and every window follow */
static void step_choice(GSimpleAction *action, GVariant *parameter, gpointer data) {
	guint index = GPOINTER_TO_UINT(data);
	GActionMap *map = G_ACTION_MAP(g_object_get_data(G_OBJECT(action), "application"));
	const char *next = CHOICES[index].names[(*get_choice(index) + 1) % CHOICES[index].count];
	g_action_change_state(g_action_map_lookup_action(map, CHOICES[index].name), g_variant_new_string(next));
}


void add_view_actions(GtkApplication *application, ViewOptionsChanged changed, gpointer data) {
	get_view_options();
	notify_change = changed;
	change_data = data;
	for (guint index = 0; index < G_N_ELEMENTS(SWITCHES); index++) {
		g_autoptr(GSimpleAction) action = g_simple_action_new_stateful(SWITCHES[index].name, NULL,
			g_variant_new_boolean(*get_switch(index)));
		g_signal_connect(action, "change-state", G_CALLBACK(change_switch), GUINT_TO_POINTER(index));
		g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(action));
	}
	for (guint index = 0; index < G_N_ELEMENTS(CHOICES); index++) {
		g_autoptr(GSimpleAction) action = g_simple_action_new_stateful(CHOICES[index].name, G_VARIANT_TYPE_STRING,
			g_variant_new_string(CHOICES[index].names[*get_choice(index)]));
		g_signal_connect(action, "change-state", G_CALLBACK(change_choice), GUINT_TO_POINTER(index));
		g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(action));
		g_autoptr(GSimpleAction) next = g_simple_action_new(CHOICES[index].next_name, NULL);
		g_object_set_data(G_OBJECT(next), "application", application);
		g_signal_connect(next, "activate", G_CALLBACK(step_choice), GUINT_TO_POINTER(index));
		g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(next));
	}
}
