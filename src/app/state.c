#include "common/storage/key-store.h"
#include "app/state.h"

#define STATE_FILE "window.ini"
#define DEFAULT_WIDTH 1100
#define DEFAULT_HEIGHT 800


void load_window_state(WindowGeometry *geometry) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	load_window_geometry(file, geometry, DEFAULT_WIDTH, DEFAULT_HEIGHT);
}


void save_window_state(const WindowGeometry *geometry) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	store_window_geometry(file, geometry);
	save_key_file(file, path);
}
