#pragma once

#include <adwaita.h>

typedef struct Window Window;

Window *create_window(GtkApplication *application);
GtkWindow *get_gtk_window(Window *window);
/* Each window shows one model, and a window that has none yet takes the next file opened */
gboolean has_window_file(Window *window);
void open_window_file(Window *window, GFile *file);
/* Fades the empty page in on the next frame when the window was opened without a model */
void show_window_start(Window *window);
