#include <glib.h>
#include <math.h>

#include "camera.h"

#define FIELD_OF_VIEW (G_PI / 4)
/* The model's first view comes from the front right and a little above, the angle product photos use */
#define FRAME_YAW (G_PI / 5)
#define FRAME_PITCH (G_PI / 9)
#define FRAME_MARGIN 1.15f
#define PITCH_LIMIT (G_PI / 2 - 0.01)
#define ZOOM_STEP 0.12f
/* The eye keeps at least this share of the model from the target, or zooming would stop */
#define NEAREST_SHARE 0.002f


void frame_camera(Camera *camera, Vector3 minimum, Vector3 maximum) {
	Vector3 size = subtract_vectors(maximum, minimum);
	float radius = fmaxf(get_vector_length(size) / 2, 1e-4f);
	camera->target = scale_vector(add_vectors(minimum, maximum), 0.5f);
	camera->distance = radius / sinf((float)FIELD_OF_VIEW / 2) * FRAME_MARGIN;
	camera->yaw = (float)FRAME_YAW;
	camera->pitch = (float)FRAME_PITCH;
	camera->scale = radius * 2;
}


static Vector3 get_offset_direction(const Camera *camera) {
	return make_vector(cosf(camera->pitch) * sinf(camera->yaw), sinf(camera->pitch),
		cosf(camera->pitch) * cosf(camera->yaw));
}


Vector3 get_camera_eye(const Camera *camera) {
	return add_vectors(camera->target, scale_vector(get_offset_direction(camera), camera->distance));
}


void get_camera_axes(const Camera *camera, Vector3 *forward, Vector3 *right, Vector3 *up) {
	*forward = scale_vector(get_offset_direction(camera), -1);
	*right = normalize_vector(cross_vectors(*forward, make_vector(0, 1, 0)));
	*up = cross_vectors(*right, *forward);
}


void orbit_camera(Camera *camera, float right, float up) {
	camera->yaw -= right;
	camera->pitch = CLAMP(camera->pitch + up, (float)-PITCH_LIMIT, (float)PITCH_LIMIT);
}


void look_camera(Camera *camera, float right, float up) {
	Vector3 eye = get_camera_eye(camera);
	camera->yaw -= right;
	camera->pitch = CLAMP(camera->pitch + up, (float)-PITCH_LIMIT, (float)PITCH_LIMIT);
	camera->target = subtract_vectors(eye, scale_vector(get_offset_direction(camera), camera->distance));
}


/* A fraction of the view's width moves the target that share of the view, at the target's distance */
void pan_camera(Camera *camera, float right, float up) {
	Vector3 forward, side, above;
	get_camera_axes(camera, &forward, &side, &above);
	float view_height = 2 * camera->distance * tanf((float)FIELD_OF_VIEW / 2);
	camera->target = add_vectors(camera->target, scale_vector(side, -right * view_height));
	camera->target = add_vectors(camera->target, scale_vector(above, up * view_height));
}


void zoom_camera(Camera *camera, float steps) {
	camera->distance = fmaxf(camera->distance * expf(steps * ZOOM_STEP), camera->scale * NEAREST_SHARE);
}


void fly_camera(Camera *camera, float forward, float right, float up) {
	Vector3 ahead, side, above;
	get_camera_axes(camera, &ahead, &side, &above);
	Vector3 step = add_vectors(scale_vector(ahead, forward), scale_vector(side, right));
	step = add_vectors(step, make_vector(0, up, 0));
	camera->target = add_vectors(camera->target, scale_vector(step, camera->scale));
}


Matrix4 get_view_matrix(const Camera *camera) {
	return get_look_at_matrix(get_camera_eye(camera), camera->target, make_vector(0, 1, 0));
}


/* The near plane follows the eye's distance so close-ups keep their depth precision */
Matrix4 get_projection_matrix(const Camera *camera, float aspect) {
	float near = fmaxf(camera->distance * 0.01f, camera->scale * 1e-4f);
	float far = camera->distance + camera->scale * 20;
	return get_perspective_matrix((float)FIELD_OF_VIEW, aspect, near, far);
}
