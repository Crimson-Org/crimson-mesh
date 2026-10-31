#pragma once

#include <glib.h>

#include "model.h"
#include "view-options.h"

typedef struct Renderer Renderer;

/* Everything one frame needs, worked out by the view before it draws */
typedef struct {
	Matrix4 view, projection;
	Vector3 eye, forward;
	/* Node positions for the current pose, and the joint matrices made from them */
	const Matrix4 *globals, *joints;
	const ViewOptions *options;
	int width, height;
} RenderFrame;

/* Made and freed while the drawing area's context is current, for desktop OpenGL or OpenGL ES */
Renderer *create_renderer(gboolean is_es, GError **error);
void free_renderer(Renderer *renderer);
/* Starts passing the model to the graphics card a few pieces at a time, or clears the view without one */
void set_renderer_model(Renderer *renderer, const Model *model);
/* Uploads the next pieces within a few milliseconds and returns whether more remain */
gboolean upload_renderer_step(Renderer *renderer);
void draw_renderer(Renderer *renderer, const RenderFrame *frame);
