#pragma once

#include <gtk/gtk.h>

#include "model/model.h"

typedef struct Viewer Viewer;

/* Called each frame an animation plays, so the window can follow the time */
typedef void (*ViewerTimeChanged)(float time, gpointer data);

Viewer *create_viewer(ViewerTimeChanged time_changed, gpointer data);
GtkWidget *get_viewer_widget(Viewer *viewer);
/* Takes the model and frames it, or clears the view without one */
void show_viewer_model(Viewer *viewer, Model *model);
const Model *get_viewer_model(Viewer *viewer);
/* Listens on the window for the movement keys, so flying works wherever the focus is */
void add_viewer_keys(Viewer *viewer, GtkWidget *window);
void frame_viewer(Viewer *viewer);
/* Draws again after a change of what the view shows */
void redraw_viewer(Viewer *viewer);
void set_viewer_animation(Viewer *viewer, guint index);
void set_viewer_playing(Viewer *viewer, gboolean is_playing);
void seek_viewer(Viewer *viewer, float time);
float get_viewer_duration(Viewer *viewer);
void free_viewer(Viewer *viewer);
