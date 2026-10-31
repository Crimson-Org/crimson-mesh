#pragma once

#include "math3d.h"

/* The eye circles a target point, so orbiting, panning and flying all move the same two things */
typedef struct {
	Vector3 target;
	float distance;
	/* Radians around the upward axis and above the horizon */
	float yaw, pitch;
	/* The size of the model in view, which sets how fast flying and panning move and the eye's view distance */
	float scale;
} Camera;

void frame_camera(Camera *camera, Vector3 minimum, Vector3 maximum);
Vector3 get_camera_eye(const Camera *camera);
/* The direction the eye looks, and the screen's right and up directions in the world */
void get_camera_axes(const Camera *camera, Vector3 *forward, Vector3 *right, Vector3 *up);
void orbit_camera(Camera *camera, float right, float up);
/* Turns the eye in place, as a head turns, moving the target instead of the eye */
void look_camera(Camera *camera, float right, float up);
/* Slides the view by a fraction of what it shows across and up */
void pan_camera(Camera *camera, float right, float up);
void zoom_camera(Camera *camera, float steps);
/* Moves eye and target together, by model sizes along the view, the screen's right and the world's up */
void fly_camera(Camera *camera, float forward, float right, float up);
Matrix4 get_view_matrix(const Camera *camera);
Matrix4 get_projection_matrix(const Camera *camera, float aspect);
