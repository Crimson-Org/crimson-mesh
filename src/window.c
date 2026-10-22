#include <string.h>

#include "../lang/en.h"
#include "common/loading.h"
#include "common/log.h"
#include "common/startup.h"
#include "model.h"
#include "state.h"
#include "view-options.h"
#include "viewer.h"
#include "window.h"

#define TIMELINE_WIDTH 320
#define TIMELINE_STEP 0.01

struct Window {
	GtkWidget *window, *title, *stack, *spinner, *error_page;
	GtkWidget *animation_bar, *animation_list, *timeline, *time_label, *play_button;
	GSimpleAction *skeleton_action, *play_action;
	Viewer *viewer;
	GFile *file;
	GCancellable *loading;
	/* The timeline is being moved by the animation and not by a drag */
	gboolean is_following_time;
	gboolean is_started;
};

/* A model on its way, which the window only takes while it still wants it */
typedef struct {
	Window *window;
	GFile *file;
	GCancellable *cancellable;
} LoadRequest;

static const char *const LIGHTING_TITLES[LIGHTING_COUNT] = {
	TEXT_LIGHT_STUDIO, TEXT_LIGHT_DAYLIGHT, TEXT_LIGHT_HEADLIGHT, TEXT_LIGHT_FLAT,
};
static const char *const BACKGROUND_TITLES[BACKGROUND_COUNT] = {
	TEXT_BACKGROUND_CHARCOAL, TEXT_BACKGROUND_MIDNIGHT, TEXT_BACKGROUND_STUDIO, TEXT_BACKGROUND_LIGHT,
	TEXT_BACKGROUND_BLACK,
};


static void update_time_label(Window *window, float time) {
	g_autofree char *text = g_strdup_printf(TEXT_TIME, time, get_viewer_duration(window->viewer));
	gtk_label_set_text(GTK_LABEL(window->time_label), text);
}


static void follow_time(float time, gpointer data) {
	Window *window = data;
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
	update_time_label(window, 0);
}


static void move_timeline(GtkRange *range, Window *window) {
	if (window->is_following_time)
		return;
	float time = (float)gtk_range_get_value(range);
	seek_viewer(window->viewer, time);
	update_time_label(window, time);
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


/* The bar appears only for models that move, listing their animations when there are several */
static void show_animations(Window *window, const Model *model) {
	guint count = model ? model->animations->len : 0;
	gtk_widget_set_visible(window->animation_bar, count > 0);
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


/* Large counts read in groups of three, as 1,234,567 */
static char *format_count(guint count) {
	g_autofree char *digits = g_strdup_printf("%u", count);
	GString *grouped = g_string_new(NULL);
	gsize length = strlen(digits);
	for (gsize index = 0; index < length; index++) {
		if (index > 0 && (length - index) % 3 == 0)
			g_string_append_c(grouped, ',');
		g_string_append_c(grouped, digits[index]);
	}
	return g_string_free(grouped, FALSE);
}


static void show_model_summary(Window *window, const Model *model) {
	g_autofree char *triangles = format_count(model->triangle_count);
	g_autofree char *summary = g_strdup_printf(TEXT_MODEL_SUMMARY, model->format, triangles);
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->title), summary);
}


static void finish_loading(GObject *source, GAsyncResult *result, gpointer data) {
	LoadRequest *request = data;
	g_autoptr(GError) error = NULL;
	Model *model = finish_loading_model(result, &error);
	gboolean is_wanted = !g_cancellable_is_cancelled(request->cancellable);
	Window *window = request->window;
	g_object_unref(request->cancellable);
	g_object_unref(request->file);
	g_free(request);
	if (!is_wanted) {
		g_clear_pointer(&model, free_model);
		return;
	}
	g_clear_object(&window->loading);
	gtk_widget_set_visible(window->spinner, FALSE);
	if (!model) {
		g_message("Model could not be opened: %s", error->message);
		adw_status_page_set_description(ADW_STATUS_PAGE(window->error_page), error->message);
		adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->title), NULL);
		gtk_stack_set_visible_child_name(GTK_STACK(window->stack), "error");
		return;
	}
	note_startup_moment("Model read");
	g_simple_action_set_enabled(window->skeleton_action, model->has_skeleton);
	show_model_summary(window, model);
	show_viewer_model(window->viewer, model);
	show_animations(window, model);
}


gboolean has_window_file(Window *window) {
	return window->file != NULL;
}


/* The worker starts after the window's next frame, so the window and its spinner are on screen first */
static gboolean start_loading(gpointer data) {
	LoadRequest *request = data;
	load_model(request->file, request->cancellable, finish_loading, request);
	return G_SOURCE_REMOVE;
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
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->title), TEXT_LOADING);
	gtk_window_set_title(GTK_WINDOW(window->window), name);
	show_viewer_model(window->viewer, NULL);
	show_animations(window, NULL);
	gtk_widget_set_visible(window->spinner, TRUE);
	gtk_stack_set_visible_child_name(GTK_STACK(window->stack), "view");
	window->loading = g_cancellable_new();
	LoadRequest *request = g_new0(LoadRequest, 1);
	request->window = window;
	request->cancellable = g_object_ref(window->loading);
	request->file = g_object_ref(file);
	run_after_frame(window->window, start_loading, request);
}


static void open_chosen_files(GObject *dialog, GAsyncResult *result, gpointer data) {
	Window *window = data;
	g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(dialog), result, NULL);
	for (guint index = 0; files && index < g_list_model_get_n_items(files); index++) {
		g_autoptr(GFile) file = g_list_model_get_item(files, index);
		GApplication *application = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window->window)));
		g_application_open(application, &file, 1, "");
	}
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
		if (g_str_has_prefix(*pattern, "*."))
			gtk_file_filter_add_suffix(models, *pattern + 2);
	g_autoptr(GtkFileFilter) everything = gtk_file_filter_new();
	gtk_file_filter_set_name(everything, TEXT_ALL_FILTER);
	gtk_file_filter_add_pattern(everything, "*");
	g_list_store_append(filters, models);
	g_list_store_append(filters, everything);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	g_autoptr(GFile) folder = window->file ? g_file_get_parent(window->file) : NULL;
	gtk_file_dialog_set_initial_folder(dialog, folder);
	gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(window->window), NULL, open_chosen_files, window);
}


static void frame_model(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	frame_viewer(window->viewer);
}


static gboolean *find_view_switch(const char *name) {
	ViewOptions *options = get_view_options();
	static const char *const NAMES[] = {"grid", "wireframe", "skeleton", "textures", "turntable"};
	gboolean *switches[] = {&options->shows_grid, &options->shows_wireframe, &options->shows_skeleton,
		&options->shows_textures, &options->is_turning};
	for (guint index = 0; index < G_N_ELEMENTS(NAMES); index++)
		if (g_strcmp0(NAMES[index], name) == 0)
			return switches[index];
	return NULL;
}


static void change_view_switch(GSimpleAction *action, GVariant *value, gpointer data) {
	Window *window = data;
	*find_view_switch(g_action_get_name(G_ACTION(action))) = g_variant_get_boolean(value);
	g_simple_action_set_state(action, value);
	save_view_options();
	redraw_viewer(window->viewer);
}


static void change_lighting(GSimpleAction *action, GVariant *value, gpointer data) {
	Window *window = data;
	get_view_options()->lighting = find_option_name(LIGHTING_NAMES, LIGHTING_COUNT,
		g_variant_get_string(value, NULL), LIGHTING_STUDIO);
	g_simple_action_set_state(action, value);
	save_view_options();
	redraw_viewer(window->viewer);
}


static void change_background(GSimpleAction *action, GVariant *value, gpointer data) {
	Window *window = data;
	get_view_options()->background = find_option_name(BACKGROUND_NAMES, BACKGROUND_COUNT,
		g_variant_get_string(value, NULL), BACKGROUND_CHARCOAL);
	g_simple_action_set_state(action, value);
	save_view_options();
	redraw_viewer(window->viewer);
}


/* L and B step through the choices that the header bar's menus offer */
static void step_choice(Window *window, const char *action_name, const char *const *names, int count, int current) {
	const char *next = names[(current + 1) % count];
	g_action_group_change_action_state(G_ACTION_GROUP(window->window), action_name, g_variant_new_string(next));
}


static void next_lighting(GSimpleAction *action, GVariant *parameter, gpointer data) {
	step_choice(data, "lighting", LIGHTING_NAMES, LIGHTING_COUNT, get_view_options()->lighting);
}


static void next_background(GSimpleAction *action, GVariant *parameter, gpointer data) {
	step_choice(data, "background", BACKGROUND_NAMES, BACKGROUND_COUNT, get_view_options()->background);
}


static void change_playing(GSimpleAction *action, GVariant *value, gpointer data) {
	show_playing(data, g_variant_get_boolean(value));
}


static void add_actions(Window *window) {
	ViewOptions *options = get_view_options();
	static const GActionEntry ENTRIES[] = {
		{"open", choose_files, NULL, NULL, NULL, {0}},
		{"frame", frame_model, NULL, NULL, NULL, {0}},
		{"grid", NULL, NULL, "true", change_view_switch, {0}},
		{"wireframe", NULL, NULL, "false", change_view_switch, {0}},
		{"skeleton", NULL, NULL, "false", change_view_switch, {0}},
		{"textures", NULL, NULL, "true", change_view_switch, {0}},
		{"turntable", NULL, NULL, "false", change_view_switch, {0}},
		{"lighting", NULL, "s", "'studio'", change_lighting, {0}},
		{"background", NULL, "s", "'charcoal'", change_background, {0}},
		{"next-lighting", next_lighting, NULL, NULL, NULL, {0}},
		{"next-background", next_background, NULL, NULL, NULL, {0}},
		{"play", NULL, NULL, "false", change_playing, {0}},
	};
	GActionMap *map = G_ACTION_MAP(window->window);
	g_action_map_add_action_entries(map, ENTRIES, G_N_ELEMENTS(ENTRIES), window);
	static const char *const SWITCHES[] = {"grid", "wireframe", "skeleton", "textures", "turntable"};
	for (guint index = 0; index < G_N_ELEMENTS(SWITCHES); index++)
		g_simple_action_set_state(G_SIMPLE_ACTION(g_action_map_lookup_action(map, SWITCHES[index])),
			g_variant_new_boolean(*find_view_switch(SWITCHES[index])));
	g_simple_action_set_state(G_SIMPLE_ACTION(g_action_map_lookup_action(map, "lighting")),
		g_variant_new_string(LIGHTING_NAMES[options->lighting]));
	g_simple_action_set_state(G_SIMPLE_ACTION(g_action_map_lookup_action(map, "background")),
		g_variant_new_string(BACKGROUND_NAMES[options->background]));
	window->skeleton_action = G_SIMPLE_ACTION(g_action_map_lookup_action(map, "skeleton"));
	window->play_action = G_SIMPLE_ACTION(g_action_map_lookup_action(map, "play"));
	g_simple_action_set_enabled(window->skeleton_action, FALSE);
	g_simple_action_set_enabled(window->play_action, FALSE);
}


static GMenu *create_choice_menu(const char *action, const char *const *names, const char *const *titles, int count) {
	GMenu *menu = g_menu_new();
	for (int index = 0; index < count; index++) {
		g_autofree char *detailed = g_strdup_printf("win.%s::%s", action, names[index]);
		g_menu_append(menu, titles[index], detailed);
	}
	return menu;
}


static GtkWidget *create_menu_button(const char *icon, const char *tooltip, GMenuModel *menu) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), icon);
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), menu);
	gtk_widget_set_tooltip_text(button, tooltip);
	return button;
}


static GtkWidget *create_toggle(const char *icon, const char *tooltip, const char *action) {
	GtkWidget *button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(button), icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	return button;
}


static GtkWidget *create_header(Window *window) {
	GtkWidget *header = adw_header_bar_new();
	window->title = adw_window_title_new(TEXT_APPLICATION_NAME, NULL);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), window->title);
	GtkWidget *open = gtk_button_new_from_icon_name("document-open-symbolic");
	gtk_widget_set_tooltip_text(open, TEXT_OPEN_TOOLTIP);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(open), "win.open");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), open);
	g_autoptr(GMenu) lighting = create_choice_menu("lighting", LIGHTING_NAMES, LIGHTING_TITLES, LIGHTING_COUNT);
	g_autoptr(GMenu) backgrounds = create_choice_menu("background", BACKGROUND_NAMES, BACKGROUND_TITLES,
		BACKGROUND_COUNT);
	/* Laid out as Crimson Code's menu, the everyday commands first, the rest in submenus, then the program */
	g_autoptr(GMenu) everyday = g_menu_new();
	g_menu_append(everyday, TEXT_OPEN, "win.open");
	g_menu_append(everyday, TEXT_FRAME, "win.frame");
	g_autoptr(GMenu) view = g_menu_new();
	g_menu_append(view, TEXT_GRID, "win.grid");
	g_menu_append(view, TEXT_WIREFRAME, "win.wireframe");
	g_menu_append(view, TEXT_SKELETON, "win.skeleton");
	g_menu_append(view, TEXT_TEXTURES, "win.textures");
	g_menu_append(view, TEXT_TURNTABLE, "win.turntable");
	g_autoptr(GMenu) submenus = g_menu_new();
	g_menu_append_submenu(submenus, TEXT_VIEW, G_MENU_MODEL(view));
	g_menu_append_submenu(submenus, TEXT_LIGHTING, G_MENU_MODEL(lighting));
	g_menu_append_submenu(submenus, TEXT_BACKGROUND, G_MENU_MODEL(backgrounds));
	g_autoptr(GMenu) program = g_menu_new();
	g_menu_append(program, TEXT_SETTINGS, "app.settings");
	g_menu_append(program, TEXT_ABOUT, "app.about");
	g_menu_append(program, TEXT_QUIT, "app.quit");
	g_autoptr(GMenu) menu = g_menu_new();
	g_menu_append_section(menu, NULL, G_MENU_MODEL(everyday));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(submenus));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(program));
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_menu_button("open-menu-symbolic", TEXT_MENU,
		G_MENU_MODEL(menu)));
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_menu_button("crimson-background-symbolic",
		TEXT_BACKGROUND_TOOLTIP, G_MENU_MODEL(backgrounds)));
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_menu_button("display-brightness-symbolic",
		TEXT_LIGHTING_TOOLTIP, G_MENU_MODEL(lighting)));
	GtkWidget *switches = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(switches, "linked");
	gtk_box_append(GTK_BOX(switches), create_toggle("crimson-grid-symbolic", TEXT_GRID_TOOLTIP, "win.grid"));
	gtk_box_append(GTK_BOX(switches), create_toggle("crimson-wireframe-symbolic", TEXT_WIREFRAME_TOOLTIP,
		"win.wireframe"));
	gtk_box_append(GTK_BOX(switches), create_toggle("crimson-skeleton-symbolic", TEXT_SKELETON_TOOLTIP, "win.skeleton"));
	gtk_box_append(GTK_BOX(switches), create_toggle("crimson-textures-symbolic", TEXT_TEXTURES_TOOLTIP, "win.textures"));
	gtk_box_append(GTK_BOX(switches), create_toggle("object-rotate-left-symbolic", TEXT_TURNTABLE_TOOLTIP,
		"win.turntable"));
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), switches);
	GtkWidget *frame = gtk_button_new_from_icon_name("zoom-fit-best-symbolic");
	gtk_widget_set_tooltip_text(frame, TEXT_FRAME_TOOLTIP);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(frame), "win.frame");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), frame);
	return header;
}


/* Floating video player style controls along the bottom of the view */
static GtkWidget *create_animation_bar(Window *window) {
	window->play_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(window->play_button), "media-playback-start-symbolic");
	gtk_widget_set_tooltip_text(window->play_button, TEXT_PLAY);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(window->play_button), "win.play");
	window->animation_list = gtk_drop_down_new(NULL, NULL);
	gtk_widget_set_tooltip_text(window->animation_list, TEXT_ANIMATION);
	g_signal_connect(window->animation_list, "notify::selected", G_CALLBACK(choose_animation), window);
	window->timeline = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, TIMELINE_STEP);
	gtk_scale_set_draw_value(GTK_SCALE(window->timeline), FALSE);
	gtk_widget_set_size_request(window->timeline, TIMELINE_WIDTH, -1);
	gtk_accessible_update_property(GTK_ACCESSIBLE(window->timeline), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_TIMELINE,
		-1);
	g_signal_connect(window->timeline, "value-changed", G_CALLBACK(move_timeline), window);
	window->time_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(window->time_label, "numeric");
	window->animation_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(window->animation_bar, "toolbar");
	gtk_widget_add_css_class(window->animation_bar, "osd");
	gtk_widget_add_css_class(window->animation_bar, "animation-bar");
	gtk_widget_set_halign(window->animation_bar, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(window->animation_bar, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(window->animation_bar), window->play_button);
	gtk_box_append(GTK_BOX(window->animation_bar), window->animation_list);
	gtk_box_append(GTK_BOX(window->animation_bar), window->timeline);
	gtk_box_append(GTK_BOX(window->animation_bar), window->time_label);
	gtk_widget_set_visible(window->animation_bar, FALSE);
	return window->animation_bar;
}


static GtkWidget *create_view(Window *window) {
	window->viewer = create_viewer(follow_time, window);
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


static GtkWidget *create_empty_page(void) {
	GtkWidget *button = gtk_button_new_with_label(TEXT_OPEN);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.open");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_add_css_class(button, "suggested-action");
	GtkWidget *page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), get_application_identity()->id);
	adw_status_page_set_title(ADW_STATUS_PAGE(page), TEXT_EMPTY_TITLE);
	adw_status_page_set_description(ADW_STATUS_PAGE(page), TEXT_EMPTY_DESCRIPTION);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), button);
	return page;
}


static GtkWidget *create_content(Window *window) {
	window->error_page = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(window->error_page), "dialog-error-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(window->error_page), TEXT_ERROR_TITLE);
	window->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(window->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	/* The window opens blank, so a model opened with it skips the empty page */
	gtk_stack_add_named(GTK_STACK(window->stack), gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), "blank");
	gtk_stack_add_named(GTK_STACK(window->stack), create_empty_page(), "empty");
	gtk_stack_add_named(GTK_STACK(window->stack), window->error_page, "error");
	gtk_stack_add_named(GTK_STACK(window->stack), create_view(window), "view");
	gtk_stack_set_visible_child_name(GTK_STACK(window->stack), "blank");
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), create_header(window));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), window->stack);
	return toolbar;
}


static gboolean drop_files(GtkDropTarget *target, const GValue *value, double x, double y, Window *window) {
	GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
	GApplication *application = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window->window)));
	/* The first model let go over a window replaces its model, and each further one opens in a new window */
	for (GSList *item = files; item; item = item->next) {
		if (item == files)
			open_window_file(window, item->data);
		else
			g_application_open(application, (GFile **)&item->data, 1, "");
	}
	gboolean has_files = files != NULL;
	g_slist_free(files);
	return has_files;
}


static gboolean close_window(GtkWindow *gtk_window, Window *window) {
	WindowGeometry geometry;
	read_window_geometry(gtk_window, &geometry);
	save_window_state(&geometry);
	return GDK_EVENT_PROPAGATE;
}


static void note_window_shown(GtkWidget *widget) {
	note_startup_moment("Window shown");
	note_first_frame(widget);
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


Window *create_window(GtkApplication *application) {
	Window *window = g_new0(Window, 1);
	WindowGeometry geometry;
	load_window_state(&geometry);
	window->window = adw_application_window_new(application);
	g_object_set_data_full(G_OBJECT(window->window), "window", window, free_window);
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
	note_startup_moment("Window created");
	apply_window_geometry(GTK_WINDOW(window->window), &geometry);
	add_actions(window);
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(window->window), create_content(window));
	add_viewer_keys(window->viewer, window->window);
	GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(drop_files), window);
	gtk_widget_add_controller(window->window, GTK_EVENT_CONTROLLER(drop));
	g_signal_connect(window->window, "close-request", G_CALLBACK(close_window), window);
	g_signal_connect(window->window, "map", G_CALLBACK(note_window_shown), NULL);
	return window;
}


GtkWindow *get_gtk_window(Window *window) {
	return GTK_WINDOW(window->window);
}


static gboolean fade_in_empty_page(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	Window *window = data;
	if (!window->is_started)
		gtk_stack_set_visible_child_name(GTK_STACK(window->stack), "empty");
	return G_SOURCE_REMOVE;
}


void show_window_start(Window *window) {
	if (!window->is_started)
		gtk_widget_add_tick_callback(window->window, fade_in_empty_page, window, NULL);
}
