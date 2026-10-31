#include <math.h>

#include "model/animation.h"
#include "render/camera.h"
#include "render/renderer.h"
#include "window/view-options.h"
#include "render/viewer.h"

/* Shaders need OpenGL 3.3 or OpenGL ES 3.0, matching what GTK draws the window with */
#define GL_MAJOR 3
#define GL_MINOR 3
#define ES_MAJOR 3
#define ES_MINOR 0
/* Radians the view turns for each pixel dragged at a mouse speed of 100 percent */
#define ORBIT_SPEED 0.002f
#define LOOK_SPEED 0.0012f
/* Model sizes flown each second at a fly speed of 100 percent, and how much faster Shift goes */
#define FLY_SPEED 0.6f
#define FAST_FACTOR 4.0f
#define TURNTABLE_SPEED 0.6f
/* A long frame, such as after the window was hidden, moves no further than this */
#define LONGEST_STEP 0.1f
/* Touchpads scroll in pixels, and this many make one wheel click */
#define PIXELS_PER_STEP 40.0
#define MICROSECONDS 1e6f

/* The keys that fly the eye, one bit each */
typedef enum {
	MOVE_FORWARD = 1 << 0,
	MOVE_BACK = 1 << 1,
	MOVE_LEFT = 1 << 2,
	MOVE_RIGHT = 1 << 3,
	MOVE_DOWN = 1 << 4,
	MOVE_UP = 1 << 5,
} Movement;

/* What a drag does, chosen by its button and Shift when it starts */
typedef enum {
	DRAG_ORBIT,
	DRAG_PAN,
	DRAG_LOOK,
} DragMode;

struct Viewer {
	GtkWidget *area;
	Renderer *renderer;
	Camera camera;
	/* The model to show, and the one the renderer uses until the next frame swaps them */
	Model *model, *shown, *leaving;
	Matrix4 *globals, *joints;
	guint animation;
	float time;
	gboolean is_playing, has_uploads, is_fast;
	Movement movement;
	DragMode drag;
	double drag_x, drag_y;
	guint tick;
	gint64 last_frame;
	ViewerTimeChanged time_changed;
	gpointer data;
};


static const ModelAnimation *get_current_animation(Viewer *viewer) {
	if (!viewer->model || viewer->animation >= viewer->model->animations->len)
		return NULL;
	return &g_array_index(viewer->model->animations, ModelAnimation, viewer->animation);
}


static gboolean is_moving(Viewer *viewer) {
	const ModelAnimation *animation = get_current_animation(viewer);
	gboolean is_animating = viewer->is_playing && animation && animation->duration > 0;
	return is_animating || viewer->has_uploads || viewer->movement || get_view_options()->is_turning;
}


static void fly(Viewer *viewer, float seconds) {
	float speed = FLY_SPEED * (float)get_view_options()->fly_speed / DEFAULT_SPEED;
	float distance = seconds * speed * (viewer->is_fast ? FAST_FACTOR : 1);
	Movement movement = viewer->movement;
	float forward = (movement & MOVE_FORWARD ? 1.0f : 0.0f) - (movement & MOVE_BACK ? 1.0f : 0.0f);
	float right = (movement & MOVE_RIGHT ? 1.0f : 0.0f) - (movement & MOVE_LEFT ? 1.0f : 0.0f);
	float up = (movement & MOVE_UP ? 1.0f : 0.0f) - (movement & MOVE_DOWN ? 1.0f : 0.0f);
	fly_camera(&viewer->camera, forward * distance, right * distance, up * distance);
}


/* One clock moves the animation, the turntable, flying and uploads, and stops when none of them needs it */
static gboolean step_frame(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	Viewer *viewer = data;
	gint64 now = gdk_frame_clock_get_frame_time(clock);
	float seconds = viewer->last_frame ? MIN((float)(now - viewer->last_frame) / MICROSECONDS, LONGEST_STEP) : 0;
	viewer->last_frame = now;
	const ModelAnimation *animation = get_current_animation(viewer);
	if (viewer->is_playing && animation && animation->duration > 0) {
		viewer->time = fmodf(viewer->time + seconds, animation->duration);
		viewer->time_changed(viewer->time, viewer->data);
	}
	if (get_view_options()->is_turning)
		viewer->camera.yaw += seconds * TURNTABLE_SPEED;
	if (viewer->movement)
		fly(viewer, seconds);
	gtk_gl_area_queue_render(GTK_GL_AREA(viewer->area));
	if (is_moving(viewer))
		return G_SOURCE_CONTINUE;
	viewer->tick = 0;
	return G_SOURCE_REMOVE;
}


static void keep_moving(Viewer *viewer) {
	if (viewer->tick || !is_moving(viewer))
		return;
	viewer->last_frame = 0;
	viewer->tick = gtk_widget_add_tick_callback(viewer->area, step_frame, viewer, NULL);
}


void redraw_viewer(Viewer *viewer) {
	gtk_gl_area_queue_render(GTK_GL_AREA(viewer->area));
	keep_moving(viewer);
}


static void realize_area(GtkGLArea *area, Viewer *viewer) {
	gtk_gl_area_make_current(area);
	if (gtk_gl_area_get_error(area))
		return;
	gboolean is_es = gtk_gl_area_get_api(area) == GDK_GL_API_GLES;
	int major = 0, minor = 0;
	gdk_gl_context_get_version(gtk_gl_area_get_context(area), &major, &minor);
	int needed_major = is_es ? ES_MAJOR : GL_MAJOR, needed_minor = is_es ? ES_MINOR : GL_MINOR;
	GError *error = NULL;
	if (major < needed_major || (major == needed_major && minor < needed_minor)) {
		error = g_error_new(GDK_GL_ERROR, GDK_GL_ERROR_NOT_AVAILABLE, "OpenGL %s%d.%d is needed, and %d.%d was found",
			is_es ? "ES " : "", needed_major, needed_minor, major, minor);
		gtk_gl_area_set_error(area, error);
		g_clear_error(&error);
		return;
	}
	g_message("Drawing with OpenGL %s%d.%d", is_es ? "ES " : "", major, minor);
	viewer->renderer = create_renderer(is_es, &error);
	if (!viewer->renderer) {
		g_warning("%s", error->message);
		gtk_gl_area_set_error(area, error);
		g_clear_error(&error);
	}
	viewer->shown = NULL;
}


static void unrealize_area(GtkGLArea *area, Viewer *viewer) {
	gtk_gl_area_make_current(area);
	if (gtk_gl_area_get_error(area) || !viewer->renderer)
		return;
	free_renderer(viewer->renderer);
	viewer->renderer = NULL;
	viewer->shown = NULL;
	g_clear_pointer(&viewer->leaving, free_model);
}


/* The renderer takes a new model inside its own frame, where its context is current, and lets the old one go */
static void swap_models(Viewer *viewer) {
	if (viewer->shown == viewer->model)
		return;
	set_renderer_model(viewer->renderer, viewer->model);
	viewer->shown = viewer->model;
	g_clear_pointer(&viewer->leaving, free_model);
}


static gboolean render_area(GtkGLArea *area, GdkGLContext *context, Viewer *viewer) {
	if (!viewer->renderer)
		return TRUE;
	swap_models(viewer);
	viewer->has_uploads = upload_renderer_step(viewer->renderer);
	keep_moving(viewer);
	int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
	int width = MAX(gtk_widget_get_width(GTK_WIDGET(area)) * scale, 1);
	int height = MAX(gtk_widget_get_height(GTK_WIDGET(area)) * scale, 1);
	RenderFrame frame = {.options = get_view_options(), .width = width, .height = height};
	if (viewer->model) {
		pose_model(viewer->model, get_current_animation(viewer), viewer->time, viewer->globals);
		get_joint_matrices(viewer->model, viewer->globals, viewer->joints);
		Vector3 right, up;
		get_camera_axes(&viewer->camera, &frame.forward, &right, &up);
		frame.view = get_view_matrix(&viewer->camera);
		frame.projection = get_projection_matrix(&viewer->camera, (float)width / (float)height);
		frame.eye = get_camera_eye(&viewer->camera);
		frame.globals = viewer->globals;
		frame.joints = viewer->joints;
	}
	draw_renderer(viewer->renderer, &frame);
	return TRUE;
}


static void begin_drag(GtkGestureDrag *gesture, double x, double y, Viewer *viewer) {
	guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
	GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
	gboolean is_shifted = (state & GDK_SHIFT_MASK) != 0;
	gboolean is_panning = button == GDK_BUTTON_MIDDLE || (button == GDK_BUTTON_PRIMARY && is_shifted);
	viewer->drag = button == GDK_BUTTON_SECONDARY ? DRAG_LOOK : is_panning ? DRAG_PAN : DRAG_ORBIT;
	viewer->drag_x = viewer->drag_y = 0;
	static const char *CURSORS[] = {[DRAG_ORBIT] = "grabbing", [DRAG_PAN] = "move", [DRAG_LOOK] = "crosshair"};
	gtk_widget_set_cursor_from_name(viewer->area, CURSORS[viewer->drag]);
}


static void update_drag(GtkGestureDrag *gesture, double offset_x, double offset_y, Viewer *viewer) {
	float right = (float)(offset_x - viewer->drag_x), down = (float)(offset_y - viewer->drag_y);
	viewer->drag_x = offset_x;
	viewer->drag_y = offset_y;
	float height = (float)MAX(gtk_widget_get_height(viewer->area), 1);
	float speed = (float)get_view_options()->mouse_speed / DEFAULT_SPEED;
	if (viewer->drag == DRAG_ORBIT)
		orbit_camera(&viewer->camera, right * ORBIT_SPEED * speed, down * ORBIT_SPEED * speed);
	else if (viewer->drag == DRAG_PAN)
		pan_camera(&viewer->camera, right / height, down / height);
	else
		look_camera(&viewer->camera, right * LOOK_SPEED * speed, down * LOOK_SPEED * speed);
	gtk_gl_area_queue_render(GTK_GL_AREA(viewer->area));
}


static void end_drag(GtkGestureDrag *gesture, double offset_x, double offset_y, Viewer *viewer) {
	gtk_widget_set_cursor(viewer->area, NULL);
}


static gboolean scroll_view(GtkEventControllerScroll *controller, double dx, double dy, Viewer *viewer) {
	double steps = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL ? dy
		: dy / PIXELS_PER_STEP;
	zoom_camera(&viewer->camera, (float)steps);
	gtk_gl_area_queue_render(GTK_GL_AREA(viewer->area));
	return TRUE;
}


static Movement get_key_movement(guint keyval) {
	switch (gdk_keyval_to_lower(keyval)) {
	case GDK_KEY_w:
		return MOVE_FORWARD;
	case GDK_KEY_s:
		return MOVE_BACK;
	case GDK_KEY_a:
		return MOVE_LEFT;
	case GDK_KEY_d:
		return MOVE_RIGHT;
	case GDK_KEY_q:
		return MOVE_DOWN;
	case GDK_KEY_e:
		return MOVE_UP;
	default:
		return 0;
	}
}


static gboolean press_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
	Viewer *viewer) {
	viewer->is_fast = (state & GDK_SHIFT_MASK) || keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R;
	Movement movement = get_key_movement(keyval);
	if (!movement || !viewer->model || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)))
		return FALSE;
	viewer->movement |= movement;
	keep_moving(viewer);
	return TRUE;
}


static void release_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state,
	Viewer *viewer) {
	if (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R)
		viewer->is_fast = FALSE;
	viewer->movement &= ~get_key_movement(keyval);
}


/* A key let go while another window has the focus does not report its release, so leaving stops the flight */
static void notice_window_activity(GtkWindow *window, GParamSpec *spec, Viewer *viewer) {
	if (gtk_window_is_active(window))
		return;
	viewer->movement = 0;
	viewer->is_fast = FALSE;
}


void add_viewer_keys(Viewer *viewer, GtkWidget *window) {
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_key), viewer);
	g_signal_connect(keys, "key-released", G_CALLBACK(release_key), viewer);
	gtk_widget_add_controller(window, keys);
	g_signal_connect(window, "notify::is-active", G_CALLBACK(notice_window_activity), viewer);
}


Viewer *create_viewer(ViewerTimeChanged time_changed, gpointer data) {
	Viewer *viewer = g_new0(Viewer, 1);
	viewer->time_changed = time_changed;
	viewer->data = data;
	viewer->area = gtk_gl_area_new();
	/* All APIs are allowed, so the area always shares the API GTK draws the window with */
	gtk_gl_area_set_required_version(GTK_GL_AREA(viewer->area), ES_MAJOR, ES_MINOR);
	/* Frames are drawn only when something changes, and the renderer keeps its own depth buffer */
	gtk_gl_area_set_auto_render(GTK_GL_AREA(viewer->area), FALSE);
	gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(viewer->area), FALSE);
	gtk_widget_set_hexpand(viewer->area, TRUE);
	gtk_widget_set_vexpand(viewer->area, TRUE);
	g_signal_connect(viewer->area, "realize", G_CALLBACK(realize_area), viewer);
	g_signal_connect(viewer->area, "unrealize", G_CALLBACK(unrealize_area), viewer);
	g_signal_connect(viewer->area, "render", G_CALLBACK(render_area), viewer);
	GtkGesture *drag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(begin_drag), viewer);
	g_signal_connect(drag, "drag-update", G_CALLBACK(update_drag), viewer);
	g_signal_connect(drag, "drag-end", G_CALLBACK(end_drag), viewer);
	gtk_widget_add_controller(viewer->area, GTK_EVENT_CONTROLLER(drag));
	GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_view), viewer);
	gtk_widget_add_controller(viewer->area, scroll);
	return viewer;
}


GtkWidget *get_viewer_widget(Viewer *viewer) {
	return viewer->area;
}


/* The model the renderer still uses waits for the next frame to let it go, and any other is freed at once */
void show_viewer_model(Viewer *viewer, Model *model) {
	if (viewer->model && viewer->model == viewer->shown)
		viewer->leaving = viewer->model;
	else if (viewer->model)
		free_model(viewer->model);
	viewer->model = model;
	viewer->animation = 0;
	viewer->time = 0;
	viewer->is_playing = FALSE;
	g_free(viewer->globals);
	g_free(viewer->joints);
	viewer->globals = model ? g_new(Matrix4, MAX(model->nodes->len, 1)) : NULL;
	viewer->joints = model ? g_new(Matrix4, MAX(model->joints->len, 1)) : NULL;
	if (model)
		frame_camera(&viewer->camera, model->minimum, model->maximum);
	redraw_viewer(viewer);
}


const Model *get_viewer_model(Viewer *viewer) {
	return viewer->model;
}


void frame_viewer(Viewer *viewer) {
	if (!viewer->model)
		return;
	frame_camera(&viewer->camera, viewer->model->minimum, viewer->model->maximum);
	redraw_viewer(viewer);
}


void set_viewer_animation(Viewer *viewer, guint index) {
	viewer->animation = index;
	viewer->time = 0;
	redraw_viewer(viewer);
}


void set_viewer_playing(Viewer *viewer, gboolean is_playing) {
	viewer->is_playing = is_playing;
	redraw_viewer(viewer);
}


void seek_viewer(Viewer *viewer, float time) {
	viewer->time = time;
	redraw_viewer(viewer);
}


float get_viewer_duration(Viewer *viewer) {
	const ModelAnimation *animation = get_current_animation(viewer);
	return animation ? animation->duration : 0;
}


void free_viewer(Viewer *viewer) {
	g_clear_pointer(&viewer->model, free_model);
	g_clear_pointer(&viewer->leaving, free_model);
	g_free(viewer->globals);
	g_free(viewer->joints);
	g_free(viewer);
}
