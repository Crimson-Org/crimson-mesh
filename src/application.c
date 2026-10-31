#include <adwaita.h>

#include "../lang/en.h"
#include "application.h"
#include "settings-dialog.h"
#include "window.h"


/* A window still waiting for its first model takes the file, and each other file gets a window of its own */
static Window *find_empty_window(GtkApplication *application) {
	for (GList *item = gtk_application_get_windows(application); item; item = item->next) {
		Window *window = g_object_get_data(G_OBJECT(item->data), "window");
		if (window && !has_window_file(window))
			return window;
	}
	return NULL;
}


static void activate(GApplication *application) {
	GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(application));
	Window *window = active ? g_object_get_data(G_OBJECT(active), "window") : NULL;
	if (!window)
		window = create_window(GTK_APPLICATION(application));
	gtk_window_present(get_gtk_window(window));
	show_window_start(window);
}


/* Each window shows itself first and reads its model after its first frame, so something is on screen at once */
static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	for (int index = 0; index < count; index++) {
		Window *window = find_empty_window(GTK_APPLICATION(application));
		if (!window)
			window = create_window(GTK_APPLICATION(application));
		open_window_file(window, files[index]);
		gtk_window_present(get_gtk_window(window));
	}
}


static void show_about(GSimpleAction *action, GVariant *parameter, gpointer application) {
	AdwDialog *dialog = adw_about_dialog_new();
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), get_application_identity()->name);
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog), get_application_identity()->id);
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), get_application_identity()->version);
	adw_dialog_present(dialog, GTK_WIDGET(gtk_application_get_active_window(application)));
}


static void open_settings(GSimpleAction *action, GVariant *parameter, gpointer application) {
	show_settings(GTK_WIDGET(gtk_application_get_active_window(application)));
}


static void quit(GSimpleAction *action, GVariant *parameter, gpointer application) {
	GtkWindow *window = gtk_application_get_active_window(application);
	if (window)
		gtk_window_close(window);
}


static void start_up(GApplication *application) {
	static const GActionEntry ENTRIES[] = {
		{"about", show_about, NULL, NULL, NULL, {0}},
		{"settings", open_settings, NULL, NULL, NULL, {0}},
		{"quit", quit, NULL, NULL, NULL, {0}},
	};
	static const struct {
		const char *action, *accelerators[3];
	} SHORTCUTS[] = {
		{"win.open", {"<Control>o", NULL}},
		{"win.frame", {"f", "Home", NULL}},
		{"win.grid", {"g", NULL}},
		{"win.wireframe", {"z", NULL}},
		{"win.skeleton", {"j", NULL}},
		{"win.textures", {"t", NULL}},
		{"win.turntable", {"r", NULL}},
		{"win.next-lighting", {"l", NULL}},
		{"win.next-background", {"b", NULL}},
		{"win.play", {"space", NULL}},
		{"app.settings", {"<Control>comma", NULL}},
		{"app.quit", {"<Control>q", "<Control>w", NULL}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(application), ENTRIES, G_N_ELEMENTS(ENTRIES), application);
	for (guint index = 0; index < G_N_ELEMENTS(SHORTCUTS); index++)
		gtk_application_set_accels_for_action(GTK_APPLICATION(application), SHORTCUTS[index].action,
			SHORTCUTS[index].accelerators);
}


const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonMesh", TEXT_APPLICATION_NAME, "crimson-mesh", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.parameter_summary = TEXT_FILES_OPTION,
};
