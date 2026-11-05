#include <adwaita.h>

#include "lang/en.h"
#include "app/application.h"
#include "app/settings-dialog.h"
#include "app/view-options.h"
#include "window/window.h"


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


/* The view options are the same in every window, so each one draws again after a change */
static void redraw_windows(gpointer application) {
	for (GList *item = gtk_application_get_windows(application); item; item = item->next) {
		Window *window = g_object_get_data(G_OBJECT(item->data), "window");
		if (window)
			redraw_window(window);
	}
}


static void start_up(GApplication *application) {
	add_view_actions(GTK_APPLICATION(application), redraw_windows, application);
}


static const Shortcut SHORTCUTS[] = {
	{"win.open", {"<Control>o", NULL}},
	{"window.close", {"<Control>w", NULL}},
};

const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonMesh", TEXT_APPLICATION_NAME, "crimson-mesh", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.parameter_summary = TEXT_FILES_OPTION,
	.show_settings = show_settings,
	.shortcuts = SHORTCUTS,
	.shortcut_count = G_N_ELEMENTS(SHORTCUTS),
};
