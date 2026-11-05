#pragma once

#include <gtk/gtk.h>

/* How the model is lit, from studio lamps to no lighting at all */
typedef enum {
	LIGHTING_STUDIO,
	LIGHTING_DAYLIGHT,
	LIGHTING_HEADLIGHT,
	LIGHTING_FLAT,
	LIGHTING_COUNT,
} Lighting;

/* The colours behind the model */
typedef enum {
	BACKGROUND_CHARCOAL,
	BACKGROUND_MIDNIGHT,
	BACKGROUND_STUDIO,
	BACKGROUND_LIGHT,
	BACKGROUND_BLACK,
	BACKGROUND_COUNT,
} Background;

#define DEFAULT_SPEED 100
#define MINIMUM_SPEED 10
#define MAXIMUM_SPEED 400

/* What the view shows, chosen from the header bar, and how fast it moves, chosen in Settings, kept between launches
 * and shared by every window */
typedef struct {
	gboolean shows_wireframe, shows_skeleton, shows_grid, shows_textures, is_turning;
	Lighting lighting;
	Background background;
	/* Percentages of the default turning speed for dragging and of the default flying speed */
	int mouse_speed, fly_speed;
} ViewOptions;

/* The names the settings file and the actions use */
extern const char *const LIGHTING_NAMES[LIGHTING_COUNT];
extern const char *const BACKGROUND_NAMES[BACKGROUND_COUNT];

/* Called after any view option changed, so every window can draw again */
typedef void (*ViewOptionsChanged)(gpointer data);

ViewOptions *get_view_options(void);
void save_view_options(void);
/* Puts the view switches and choices on the application as actions, such as app.grid and app.lighting, so every
 * window shows the same state, with app.next-lighting and app.next-background stepping through the choices */
void add_view_actions(GtkApplication *application, ViewOptionsChanged changed, gpointer data);
