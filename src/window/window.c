#include <string.h>

#include "lang/en.h"
#include "common/widgets/loading.h"
#include "common/app/log.h"
#include "common/app/startup.h"
#include "common/text/numbers.h"
#include "common/window/file-drop.h"
#include "model/model.h"
#include "app/state.h"
#include "app/view-options.h"
#include "render/viewer.h"
#include "window/window.h"
#include "common/widgets/spacing.h"

#define TIMELINE_WIDTH 320
/* A narrow window folds the view switches into one menu and shortens the timeline, down to the window's smallest
 * size */
#define NARROW_WINDOW "max-width: 800sp"
#define NARROW_TIMELINE_WIDTH 160
#define MINIMUM_WIDTH 480
#define MINIMUM_HEIGHT 360
#define TIMELINE_STEP 0.01
#define SUFFIX_PATTERN_START "*."
#define HOME_FOLDER_MARK "~"
/* A deep folder shows the folder's start and last few folders, with the ones between left out */
#define SHOWN_LAST_FOLDERS 2
#define LEFT_OUT_FOLDERS_MARK "..."
#define VIEW_PAGE "view"
#define ACCEL_ATTRIBUTE "accel"

/* Keys that act on the view, which reach it only when no focused button or dialog took them */
static const struct {
	const char *key, *action;
} VIEW_KEYS[] = {
	{"f", "win.frame"}, {"Home", "win.frame"}, {"g", "app.grid"}, {"z", "app.wireframe"}, {"j", "app.skeleton"},
	{"t", "app.textures"}, {"r", "app.turntable"}, {"l", "app.next-lighting"}, {"b", "app.next-background"},
	{"space", "win.play"},
};

struct Window {
	GtkWidget *window, *title, *stack, *spinner, *error_page;
	/* The header's controls, shown only while a model is in view, since the other pages lead on with Open */
	GtkWidget *view_switches, *view_choices;
	/* The switches stand in the header in a wide window and fold into the view menu in a narrow one */
	GtkWidget *switch_group, *view_menu_button;
	GtkWidget *animation_bar, *animation_list, *timeline, *time_label, *duration_label, *play_button, *skeleton_button;
	GSimpleAction *play_action;
	Viewer *viewer;
	GFile *file;
	GCancellable *loading;
	/* The timeline is being moved by the animation and not by a drag, or held by the pointer */
	gboolean is_following_time, is_timeline_held;
	gboolean is_started;
};

/* A model on its way, which the window only takes while it still wants it, so a closed window cancels it */
typedef struct {
	Window *window;
	GFile *file;
	GCancellable *cancellable;
	/* Handed to the worker, which frees it once it is done */
	gboolean is_started;
} LoadRequest;

static gboolean has_noted_model;

static const char *const LIGHTING_TITLES[LIGHTING_COUNT] = {
	TEXT_LIGHT_STUDIO, TEXT_LIGHT_DAYLIGHT, TEXT_LIGHT_HEADLIGHT, TEXT_LIGHT_FLAT,
};
static const char *const BACKGROUND_TITLES[BACKGROUND_COUNT] = {
	TEXT_BACKGROUND_BLACK, TEXT_BACKGROUND_MIDNIGHT, TEXT_BACKGROUND_CHARCOAL, TEXT_BACKGROUND_STUDIO,
	TEXT_BACKGROUND_LIGHT,
};


static void update_time_label(Window *window, float time) {
	g_autofree char *text = g_strdup_printf(TEXT_TIME, time);
	gtk_label_set_text(GTK_LABEL(window->time_label), text);
}


/* The time label stays as wide as the longest time, so the bar stays still while the animation plays, and the
 * digits all take the same width */
static void show_duration(Window *window, float duration) {
	g_autofree char *longest = g_strdup_printf(TEXT_TIME, duration);
	gtk_label_set_width_chars(GTK_LABEL(window->time_label), (int)g_utf8_strlen(longest, -1));
	g_autofree char *text = g_strdup_printf(TEXT_DURATION, duration);
	gtk_label_set_text(GTK_LABEL(window->duration_label), text);
}


static void follow_time(float time, gpointer data) {
	Window *window = data;
	/* The pointer holding the timeline sets the time itself, so the animation does not move the slider away */
	if (window->is_timeline_held)
		return;
	window->is_following_time = TRUE;
	gtk_range_set_value(GTK_RANGE(window->timeline), time);
	window->is_following_time = FALSE;
	update_time_label(window, time);
}


static void show_animation_range(Window *window) {
	float duration = get_viewer_duration(window->viewer);
	window->is_following_time = TRUE;
	gtk_range_set_range(GTK_RANGE(window->timeline), 0, MAX(duration, TIMELINE_STEP));
	gtk_range_set_value(GTK_RANGE(window->timeline), 0);
	window->is_following_time = FALSE;
	show_duration(window, duration);
	update_time_label(window, 0);
}


static void move_timeline(GtkRange *range, Window *window) {
	if (window->is_following_time)
		return;
	float time = (float)gtk_range_get_value(range);
	seek_viewer(window->viewer, time);
	update_time_label(window, time);
}


static void hold_timeline(Window *window) {
	window->is_timeline_held = TRUE;
}


static void release_timeline(Window *window) {
	window->is_timeline_held = FALSE;
}


static void choose_animation(GtkDropDown *list, GParamSpec *spec, Window *window) {
	set_viewer_animation(window->viewer, gtk_drop_down_get_selected(list));
	show_animation_range(window);
}


/* The action, the button's icon and the viewer always change together */
static void show_playing(Window *window, gboolean is_playing) {
	g_simple_action_set_state(window->play_action, g_variant_new_boolean(is_playing));
	set_viewer_playing(window->viewer, is_playing);
	gtk_button_set_icon_name(GTK_BUTTON(window->play_button),
		is_playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
}


/* The bar's whole height, margins included, which the model is framed above */
static int measure_animation_bar(Window *window) {
	int height = 0;
	gtk_widget_measure(window->animation_bar, GTK_ORIENTATION_VERTICAL, -1, NULL, &height, NULL, NULL);
	return height;
}


/* The bar appears only for models that move, listing their animations when there are several */
static void show_animations(Window *window, const Model *model) {
	guint count = model ? model->animations->len : 0;
	gtk_widget_set_visible(window->animation_bar, count > 0);
	set_viewer_cover(window->viewer, count > 0 ? measure_animation_bar(window) : 0);
	g_simple_action_set_enabled(window->play_action, count > 0);
	show_playing(window, count > 0);
	if (count == 0)
		return;
	g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
	for (guint index = 0; index < count; index++)
		gtk_string_list_append(names, g_array_index(model->animations, ModelAnimation, index).name);
	gtk_drop_down_set_model(GTK_DROP_DOWN(window->animation_list), G_LIST_MODEL(names));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(window->animation_list), 0);
	gtk_widget_set_visible(window->animation_list, count > 1);
	show_animation_range(window);
}


/* The folder as people write it, with the home folder as ~ */
static char *get_display_folder(GFile *file) {
	g_autoptr(GFile) parent = g_file_get_parent(file);
	g_autofree char *path = parent ? g_file_get_parse_name(parent) : g_strdup("");
	const char *home = g_get_home_dir();
	gsize home_length = strlen(home);
	if (g_str_has_prefix(path, home) && (path[home_length] == G_DIR_SEPARATOR || path[home_length] == 0))
		return g_strconcat(HOME_FOLDER_MARK, path + home_length, NULL);
	return g_steal_pointer(&path);
}


/* Such as ~/.../tmp/models, which keeps the subtitle short enough to sit centred under the title */
static char *shorten_folder(const char *folder) {
	g_auto(GStrv) parts = g_strsplit(folder, G_DIR_SEPARATOR_S, -1);
	guint count = g_strv_length(parts);
	/* The start, the mark and the last folders only save room when they replace two folders or more */
	const guint least_left_out = 2;
	if (count < 1 + SHOWN_LAST_FOLDERS + least_left_out)
		return g_strdup(folder);
	g_autofree char *last = g_strjoinv(G_DIR_SEPARATOR_S, parts + count - SHOWN_LAST_FOLDERS);
	return g_strjoin(G_DIR_SEPARATOR_S, parts[0], LEFT_OUT_FOLDERS_MARK, last, NULL);
}


/* The subtitle always names the model's folder, after the model's kind and size, or while the model loads, and the
 * title's tooltip names the whole folder */
static void show_model_place(Window *window, const Model *model) {
	g_autofree char *whole_folder = get_display_folder(window->file);
	gtk_widget_set_tooltip_text(window->title, whole_folder);
	g_autofree char *folder = shorten_folder(whole_folder);
	g_autofree char *triangles = model ? format_count(model->triangle_count) : NULL;
	g_autofree char *place = model ? g_strdup_printf(TEXT_MODEL_SUMMARY, model->format, triangles, folder)
		: window->loading ? g_strdup_printf(TEXT_LOADING, folder) : g_strdup(folder);
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->title), place);
}


/* The view controls only show with the view, and fade with the page unless the page switches at once */
static void show_page(Window *window, const char *name, GtkStackTransitionType transition) {
	gboolean is_view = g_strcmp0(name, VIEW_PAGE) == 0;
	GtkRevealerTransitionType fade = transition == GTK_STACK_TRANSITION_TYPE_NONE ? GTK_REVEALER_TRANSITION_TYPE_NONE
		: GTK_REVEALER_TRANSITION_TYPE_CROSSFADE;
	GtkWidget *controls[] = {window->view_switches, window->view_choices};
	for (guint index = 0; index < G_N_ELEMENTS(controls); index++) {
		gtk_revealer_set_transition_type(GTK_REVEALER(controls[index]), fade);
		gtk_revealer_set_reveal_child(GTK_REVEALER(controls[index]), is_view);
	}
	gtk_stack_set_visible_child_full(GTK_STACK(window->stack), name, transition);
}


static void free_load_request(gpointer data) {
	LoadRequest *request = data;
	g_object_unref(request->cancellable);
	g_object_unref(request->file);
	g_free(request);
}


static void finish_loading(GObject *source, GAsyncResult *result, gpointer data) {
	LoadRequest *request = data;
	g_autoptr(GError) error = NULL;
	Model *model = finish_loading_model(result, &error);
	gboolean is_wanted = !g_cancellable_is_cancelled(request->cancellable);
	Window *window = request->window;
	free_load_request(request);
	if (!is_wanted) {
		g_clear_pointer(&model, free_model);
		return;
	}
	g_clear_object(&window->loading);
	gtk_widget_set_visible(window->spinner, FALSE);
	if (!model) {
		/* The reader's own words name the path and its internals, which belong in the log */
		g_message("Model could not be opened: %s", error->message);
		adw_status_page_set_description(ADW_STATUS_PAGE(window->error_page), TEXT_ERROR_DESCRIPTION);
		show_model_place(window, NULL);
		/* The load's result is a change of data, which switches without a fade */
		show_page(window, "error", GTK_STACK_TRANSITION_TYPE_NONE);
		return;
	}
	if (!has_noted_model)
		note_startup_moment("Model read");
	has_noted_model = TRUE;
	gtk_widget_set_sensitive(window->skeleton_button, model->has_skeleton);
	show_model_place(window, model);
	show_viewer_model(window->viewer, model);
	show_animations(window, model);
	gtk_widget_grab_focus(get_viewer_widget(window->viewer));
}


gboolean has_window_file(Window *window) {
	return window->file != NULL;
}


/* The worker starts after the window's next frame, so the window and its spinner are on screen first, and a load
 * replaced meanwhile never starts */
static gboolean start_loading(gpointer data) {
	LoadRequest *request = data;
	if (g_cancellable_is_cancelled(request->cancellable))
		return G_SOURCE_REMOVE;
	request->is_started = TRUE;
	load_model(request->file, request->cancellable, finish_loading, request);
	return G_SOURCE_REMOVE;
}


/* Runs after the frame, or when the window closed first, and frees a request the worker never took */
static void forget_load_request(gpointer data) {
	LoadRequest *request = data;
	if (!request->is_started)
		free_load_request(request);
}


/* The view shows at once with a spinner, and the model joins it once the worker has read it */
void open_window_file(Window *window, GFile *file) {
	if (window->loading)
		g_cancellable_cancel(window->loading);
	g_clear_object(&window->loading);
	g_set_object(&window->file, file);
	window->is_started = TRUE;
	g_autofree char *name = g_file_get_basename(file);
	adw_window_title_set_title(ADW_WINDOW_TITLE(window->title), name);
	gtk_window_set_title(GTK_WINDOW(window->window), name);
	show_viewer_model(window->viewer, NULL);
	show_animations(window, NULL);
	gtk_widget_set_visible(window->spinner, TRUE);
	show_page(window, VIEW_PAGE, gtk_stack_get_transition_type(GTK_STACK(window->stack)));
	window->loading = g_cancellable_new();
	show_model_place(window, NULL);
	LoadRequest *request = g_new0(LoadRequest, 1);
	request->window = window;
	request->cancellable = g_object_ref(window->loading);
	request->file = g_object_ref(file);
	run_after_frame(window->window, start_loading, request, forget_load_request);
}


/* Files chosen or dropped in a window show there, the first in that window and each further one in a new window */
static void open_files_here(Window *window, GFile **files, guint count) {
	for (guint index = 0; index < count; index++) {
		if (index == 0 && window)
			open_window_file(window, files[index]);
		else
			g_application_open(g_application_get_default(), &files[index], 1, "");
	}
}


/* The dialog holds its window, which may have closed while the dialog was open */
static void open_chosen_files(GObject *dialog, GAsyncResult *result, gpointer data) {
	g_autoptr(GtkWidget) widget = data;
	g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(dialog), result, NULL);
	g_autoptr(GPtrArray) chosen = g_ptr_array_new_with_free_func(g_object_unref);
	for (guint index = 0; files && index < g_list_model_get_n_items(files); index++)
		g_ptr_array_add(chosen, g_list_model_get_item(files, index));
	Window *window = gtk_window_get_application(GTK_WINDOW(widget)) ? g_object_get_data(G_OBJECT(widget), "window")
		: NULL;
	open_files_here(window, (GFile **)chosen->pdata, chosen->len);
}


static void choose_files(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_OPEN_TITLE);
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_autoptr(GtkFileFilter) models = gtk_file_filter_new();
	gtk_file_filter_set_name(models, TEXT_MODELS_FILTER);
	g_auto(GStrv) patterns = get_model_patterns();
	for (char **pattern = patterns; *pattern; pattern++)
		if (g_str_has_prefix(*pattern, SUFFIX_PATTERN_START))
			gtk_file_filter_add_suffix(models, *pattern + strlen(SUFFIX_PATTERN_START));
	g_autoptr(GtkFileFilter) everything = gtk_file_filter_new();
	gtk_file_filter_set_name(everything, TEXT_ALL_FILTER);
	gtk_file_filter_add_pattern(everything, "*");
	g_list_store_append(filters, models);
	g_list_store_append(filters, everything);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	g_autoptr(GFile) folder = window->file ? g_file_get_parent(window->file) : NULL;
	gtk_file_dialog_set_initial_folder(dialog, folder);
	gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(window->window), NULL, open_chosen_files,
		g_object_ref(window->window));
}


static void frame_model(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	frame_viewer(window->viewer);
}


static void change_playing(GSimpleAction *action, GVariant *value, gpointer data) {
	show_playing(data, g_variant_get_boolean(value));
}


static void add_actions(Window *window) {
	static const GActionEntry ENTRIES[] = {
		{"open", choose_files, NULL, NULL, NULL, {0}},
		{"frame", frame_model, NULL, NULL, NULL, {0}},
		{"play", NULL, NULL, "false", change_playing, {0}},
	};
	GActionMap *map = G_ACTION_MAP(window->window);
	g_action_map_add_action_entries(map, ENTRIES, G_N_ELEMENTS(ENTRIES), window);
	window->play_action = G_SIMPLE_ACTION(g_action_map_lookup_action(map, "play"));
	g_simple_action_set_enabled(window->play_action, FALSE);
}


/* The view takes these keys while it has the focus, which a click on it gives, so a focused button or a dialog
 * keeps its own keys, such as Space pressing a button */
static void add_view_keys(GtkWidget *view) {
	GtkEventController *keys = gtk_shortcut_controller_new();
	for (guint index = 0; index < G_N_ELEMENTS(VIEW_KEYS); index++)
		gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys), gtk_shortcut_new(
			gtk_shortcut_trigger_parse_string(VIEW_KEYS[index].key), gtk_named_action_new(VIEW_KEYS[index].action)));
	gtk_widget_add_controller(view, keys);
	gtk_widget_set_focusable(view, TRUE);
}


static GMenu *create_choice_menu(const char *action, const char *const *names, const char *const *titles, int count) {
	GMenu *menu = g_menu_new();
	for (int index = 0; index < count; index++) {
		g_autofree char *detailed = g_strdup_printf("app.%s::%s", action, names[index]);
		g_menu_append(menu, titles[index], detailed);
	}
	return menu;
}


static const char *find_view_key(const char *action) {
	for (guint index = 0; index < G_N_ELEMENTS(VIEW_KEYS); index++)
		if (g_strcmp0(VIEW_KEYS[index].action, action) == 0)
			return VIEW_KEYS[index].key;
	return NULL;
}


/* A view action's menu item shows the action's key, as items with application shortcuts do */
static void append_view_item(GMenu *menu, const char *label, const char *action) {
	g_autoptr(GMenuItem) item = g_menu_item_new(label, action);
	const char *key = find_view_key(action);
	if (key)
		g_menu_item_set_attribute(item, ACCEL_ATTRIBUTE, "s", key);
	g_menu_append_item(menu, item);
}


static GMenu *create_view_menu(void) {
	GMenu *view = g_menu_new();
	append_view_item(view, TEXT_GRID, "app.grid");
	append_view_item(view, TEXT_WIREFRAME, "app.wireframe");
	append_view_item(view, TEXT_SKELETON, "app.skeleton");
	append_view_item(view, TEXT_TEXTURES, "app.textures");
	append_view_item(view, TEXT_TURNTABLE, "app.turntable");
	return view;
}


/* Lighting and background share one button, each list of choices under a heading */
static GMenu *create_look_menu(GMenuModel *lighting, GMenuModel *backgrounds) {
	GMenu *looks = g_menu_new();
	g_menu_append_section(looks, TEXT_LIGHTING, lighting);
	g_menu_append_section(looks, TEXT_BACKGROUND, backgrounds);
	return looks;
}


/* Laid out as Crimson Code's menu, the everyday commands first, then how the model is seen, then the program */
static GMenu *create_main_menu(GMenuModel *view, GMenuModel *lighting, GMenuModel *backgrounds) {
	g_autoptr(GMenu) everyday = g_menu_new();
	g_menu_append(everyday, TEXT_OPEN, "win.open");
	g_autoptr(GMenu) submenus = g_menu_new();
	append_view_item(submenus, TEXT_FRAME, "win.frame");
	g_menu_append_submenu(submenus, TEXT_VIEW, view);
	g_menu_append_submenu(submenus, TEXT_LIGHTING, lighting);
	g_menu_append_submenu(submenus, TEXT_BACKGROUND, backgrounds);
	g_autoptr(GMenu) program = g_menu_new();
	g_menu_append(program, TEXT_SETTINGS, "app.settings");
	g_menu_append(program, TEXT_ABOUT, "app.about");
	g_menu_append(program, TEXT_QUIT, "app.quit");
	GMenu *menu = g_menu_new();
	g_menu_append_section(menu, NULL, G_MENU_MODEL(everyday));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(submenus));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(program));
	return menu;
}


static GtkWidget *create_menu_button(const char *icon, const char *tooltip, GMenuModel *menu) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), icon);
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), menu);
	gtk_widget_set_tooltip_text(button, tooltip);
	return button;
}


static GtkWidget *create_button(const char *icon, const char *tooltip, const char *action) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	return button;
}


static GtkWidget *create_toggle(const char *icon, const char *tooltip, const char *action) {
	GtkWidget *button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(button), icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	return button;
}


/* Controls that belong together sit close, and groups keep the header's wider gap between them */
static GtkWidget *create_group(GtkWidget *const *controls, guint count) {
	GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_HAIRLINE);
	for (guint index = 0; index < count; index++)
		gtk_box_append(GTK_BOX(group), controls[index]);
	return group;
}


/* The view controls fade as long as the pages do, so both arrive together */
static GtkWidget *create_view_controls(Window *window, GtkWidget *child) {
	GtkWidget *revealer = gtk_revealer_new();
	gtk_revealer_set_transition_duration(GTK_REVEALER(revealer),
		gtk_stack_get_transition_duration(GTK_STACK(window->stack)));
	gtk_revealer_set_child(GTK_REVEALER(revealer), child);
	return revealer;
}


/* What the model shows sits by Open, and the camera, lighting and backdrop sit by the menu */
static GtkWidget *create_header(Window *window) {
	GtkWidget *header = adw_header_bar_new();
	window->title = adw_window_title_new(TEXT_APPLICATION_NAME, NULL);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), window->title);
	GtkWidget *open = gtk_button_new_with_label(TEXT_OPEN);
	gtk_widget_set_tooltip_text(open, TEXT_OPEN_TOOLTIP);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open");
	/* The skeleton switch is the same for every window, but only a model with a skeleton can show one */
	window->skeleton_button = create_toggle("crimson-skeleton-symbolic", TEXT_SKELETON_TOOLTIP, "app.skeleton");
	gtk_widget_set_sensitive(window->skeleton_button, FALSE);
	GtkWidget *const shown[] = {
		create_toggle("crimson-grid-symbolic", TEXT_GRID_TOOLTIP, "app.grid"),
		create_toggle("crimson-wireframe-symbolic", TEXT_WIREFRAME_TOOLTIP, "app.wireframe"),
		window->skeleton_button,
		create_toggle("crimson-textures-symbolic", TEXT_TEXTURES_TOOLTIP, "app.textures"),
	};
	g_autoptr(GMenu) view = create_view_menu();
	window->switch_group = create_group(shown, G_N_ELEMENTS(shown));
	window->view_menu_button = create_menu_button("view-reveal-symbolic", TEXT_VIEW_TOOLTIP, G_MENU_MODEL(view));
	gtk_widget_set_visible(window->view_menu_button, FALSE);
	GtkWidget *switches = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_box_append(GTK_BOX(switches), open);
	gtk_box_append(GTK_BOX(switches), window->switch_group);
	gtk_box_append(GTK_BOX(switches), window->view_menu_button);
	window->view_switches = create_view_controls(window, switches);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), window->view_switches);
	g_autoptr(GMenu) lighting = create_choice_menu("lighting", LIGHTING_NAMES, LIGHTING_TITLES, LIGHTING_COUNT);
	g_autoptr(GMenu) backgrounds = create_choice_menu("background", BACKGROUND_NAMES, BACKGROUND_TITLES,
		BACKGROUND_COUNT);
	g_autoptr(GMenu) looks = create_look_menu(G_MENU_MODEL(lighting), G_MENU_MODEL(backgrounds));
	g_autoptr(GMenu) menu = create_main_menu(G_MENU_MODEL(view), G_MENU_MODEL(lighting), G_MENU_MODEL(backgrounds));
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_menu_button("open-menu-symbolic", TEXT_MENU,
		G_MENU_MODEL(menu)));
	GtkWidget *const camera[] = {
		create_button("crimson-frame-symbolic", TEXT_FRAME_TOOLTIP, "win.frame"),
		create_toggle("crimson-turntable-symbolic", TEXT_TURNTABLE_TOOLTIP, "app.turntable"),
	};
	GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_box_append(GTK_BOX(choices), create_group(camera, G_N_ELEMENTS(camera)));
	gtk_box_append(GTK_BOX(choices), create_menu_button("crimson-look-symbolic", TEXT_LOOK_TOOLTIP,
		G_MENU_MODEL(looks)));
	window->view_choices = create_view_controls(window, choices);
	gtk_widget_set_margin_end(window->view_choices, SPACING_SMALL);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), window->view_choices);
	return header;
}


/* Floating video player style controls along the bottom of the view */
static GtkWidget *create_animation_bar(Window *window) {
	window->play_button = create_toggle("media-playback-start-symbolic", TEXT_PLAY, "win.play");
	gtk_widget_add_css_class(window->play_button, "circular");
	gtk_widget_add_css_class(window->play_button, "play-button");
	window->animation_list = gtk_drop_down_new(NULL, NULL);
	gtk_widget_add_css_class(window->animation_list, "flat");
	gtk_widget_set_tooltip_text(window->animation_list, TEXT_ANIMATION);
	g_signal_connect(window->animation_list, "notify::selected", G_CALLBACK(choose_animation), window);
	window->timeline = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, TIMELINE_STEP);
	gtk_scale_set_draw_value(GTK_SCALE(window->timeline), FALSE);
	gtk_widget_set_size_request(window->timeline, TIMELINE_WIDTH, -1);
	gtk_accessible_update_property(GTK_ACCESSIBLE(window->timeline), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_TIMELINE,
		-1);
	g_signal_connect(window->timeline, "value-changed", G_CALLBACK(move_timeline), window);
	GtkGesture *hold = gtk_gesture_click_new();
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(hold), GTK_PHASE_CAPTURE);
	g_signal_connect_swapped(hold, "pressed", G_CALLBACK(hold_timeline), window);
	g_signal_connect_swapped(hold, "stopped", G_CALLBACK(release_timeline), window);
	g_signal_connect_swapped(hold, "released", G_CALLBACK(release_timeline), window);
	gtk_widget_add_controller(window->timeline, GTK_EVENT_CONTROLLER(hold));
	window->time_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(window->time_label), 1);
	gtk_widget_add_css_class(window->time_label, "numeric");
	window->duration_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(window->duration_label, "numeric");
	gtk_widget_add_css_class(window->duration_label, "dimmed");
	GtkWidget *times = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_TIGHT);
	gtk_widget_add_css_class(times, "animation-time");
	gtk_box_append(GTK_BOX(times), window->time_label);
	gtk_box_append(GTK_BOX(times), window->duration_label);
	window->animation_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_add_css_class(window->animation_bar, "toolbar");
	gtk_widget_add_css_class(window->animation_bar, "osd");
	gtk_widget_add_css_class(window->animation_bar, "animation-bar");
	gtk_widget_set_halign(window->animation_bar, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(window->animation_bar, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(window->animation_bar), window->play_button);
	gtk_box_append(GTK_BOX(window->animation_bar), window->animation_list);
	gtk_box_append(GTK_BOX(window->animation_bar), window->timeline);
	gtk_box_append(GTK_BOX(window->animation_bar), times);
	gtk_widget_set_visible(window->animation_bar, FALSE);
	return window->animation_bar;
}


static GtkWidget *create_view(Window *window) {
	window->viewer = create_viewer(follow_time, window);
	add_view_keys(get_viewer_widget(window->viewer));
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), get_viewer_widget(window->viewer));
	window->spinner = create_loading_spinner();
	gtk_widget_set_visible(window->spinner, FALSE);
	gtk_widget_set_halign(window->spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(window->spinner, GTK_ALIGN_CENTER);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), window->spinner);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), create_animation_bar(window));
	return overlay;
}


/* The empty and error pages both lead on with Open */
static GtkWidget *create_status_page(const char *icon, const char *title) {
	GtkWidget *button = gtk_button_new_with_label(TEXT_OPEN);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.open");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "suggested-action");
	GtkWidget *page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), icon);
	adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), button);
	return page;
}


static GtkWidget *create_empty_page(void) {
	GtkWidget *page = create_status_page(get_application_identity()->id, TEXT_EMPTY_TITLE);
	adw_status_page_set_description(ADW_STATUS_PAGE(page), TEXT_EMPTY_DESCRIPTION);
	return page;
}


static GtkWidget *create_content(Window *window) {
	window->error_page = create_status_page("crimson-model-broken-symbolic", TEXT_ERROR_TITLE);
	window->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(window->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	/* The window opens blank, so a model opened with it skips the empty page */
	gtk_stack_add_named(GTK_STACK(window->stack), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "blank");
	gtk_stack_add_named(GTK_STACK(window->stack), create_empty_page(), "empty");
	gtk_stack_add_named(GTK_STACK(window->stack), window->error_page, "error");
	gtk_stack_add_named(GTK_STACK(window->stack), create_view(window), VIEW_PAGE);
	gtk_stack_set_visible_child_name(GTK_STACK(window->stack), "blank");
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_header(window));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), window->stack);
	return toolbar;
}


static void open_dropped_files(GFile **files, guint count, gpointer window) {
	open_files_here(window, files, count);
}


static gboolean close_window(GtkWindow *gtk_window, Window *window) {
	WindowGeometry geometry;
	read_window_geometry(gtk_window, &geometry);
	save_window_state(&geometry);
	hide_closing_window(gtk_window);
	return GDK_EVENT_PROPAGATE;
}


static void free_window(gpointer data) {
	Window *window = data;
	if (window->loading)
		g_cancellable_cancel(window->loading);
	g_clear_object(&window->loading);
	g_clear_object(&window->file);
	free_viewer(window->viewer);
	g_free(window);
}


/* A narrow window keeps the title readable by folding the view switches into one menu and shortening the timeline */
static void add_narrow_layout(Window *window) {
	AdwBreakpoint *narrow = adw_breakpoint_new(adw_breakpoint_condition_parse(NARROW_WINDOW));
	adw_breakpoint_add_setters(narrow, G_OBJECT(window->switch_group), "visible", FALSE,
		G_OBJECT(window->view_menu_button), "visible", TRUE,
		G_OBJECT(window->timeline), "width-request", NARROW_TIMELINE_WIDTH, NULL);
	adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window->window), narrow);
	gtk_widget_set_size_request(window->window, MINIMUM_WIDTH, MINIMUM_HEIGHT);
}


Window *create_window(GtkApplication *application) {
	Window *window = g_new0(Window, 1);
	WindowGeometry geometry;
	load_window_state(&geometry);
	window->window = adw_application_window_new(application);
	g_object_set_data_full(G_OBJECT(window->window), "window", window, free_window);
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
	watch_window_frames(window->window);
	apply_window_geometry(GTK_WINDOW(window->window), &geometry);
	add_actions(window);
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(window->window), create_content(window));
	add_narrow_layout(window);
	add_viewer_keys(window->viewer, window->window);
	add_file_drop(window->window, open_dropped_files, window);
	g_signal_connect(window->window, "close-request", G_CALLBACK(close_window), window);
	return window;
}


GtkWindow *get_gtk_window(Window *window) {
	return GTK_WINDOW(window->window);
}


static gboolean fade_in_empty_page(gpointer data) {
	Window *window = data;
	if (!window->is_started)
		show_page(window, "empty", gtk_stack_get_transition_type(GTK_STACK(window->stack)));
	return G_SOURCE_REMOVE;
}


void show_window_start(Window *window) {
	if (!window->is_started)
		run_after_frame(window->window, fade_in_empty_page, window, NULL);
}


void redraw_window(Window *window) {
	redraw_viewer(window->viewer);
}
