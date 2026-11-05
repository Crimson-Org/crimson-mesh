#include <glib.h>
#include <math.h>

#include "render/camera.h"

#define FIELD_OF_VIEW (G_PI / 4)
/* The model's first view comes from the front right and a little above, the angle product photos use */
#define FRAME_YAW (G_PI / 5)
#define FRAME_PITCH (G_PI / 9)
/* The model's box fills this share of the view's height or width, whichever the box reaches first, so the model looks
 * a little smaller than the box, with room for poses that reach outside the box */
#define FRAME_FILL 0.8f
#define BOX_CORNERS 8
/* Each bit of a corner's number picks the box's far side along one axis */
#define CORNER_X 1
#define CORNER_Y 2
#define CORNER_Z 4
#define PITCH_LIMIT (G_PI / 2 - 0.01)
#define ZOOM_STEP 0.12f
/* The eye keeps at least this share of the model from the target, or zooming would stop */
#define NEAREST_SHARE 0.002f
/* The near plane sits this share of the way to the model, and the far plane this many model sizes behind it */
#define NEAR_PLANE_SHARE 0.01f
#define FAR_PLANE_SIZES 20


static Vector3 get_box_corner(Vector3 minimum, Vector3 maximum, int corner) {
	return make_vector(corner & CORNER_X ? maximum.x : minimum.x, corner & CORNER_Y ? maximum.y : minimum.y,
		corner & CORNER_Z ? maximum.z : minimum.z);
}


/* The eye backs off until every corner of the box is inside the view, seen from the framing angle, so a long model
 * fills the width and a tall one the height */
void frame_camera(Camera *camera, Vector3 minimum, Vector3 maximum, float aspect, float open_share) {
	Vector3 size = subtract_vectors(maximum, minimum);
	camera->target = scale_vector(add_vectors(minimum, maximum), 0.5f);
	camera->yaw = (float)FRAME_YAW;
	camera->pitch = (float)FRAME_PITCH;
	camera->scale = fmaxf(get_vector_length(size) / 2, SMALLEST_SIZE) * 2;
	Vector3 forward, right, up;
	get_camera_axes(camera, &forward, &right, &up);
	float reach = tanf((float)FIELD_OF_VIEW / 2) * FRAME_FILL;
	float reach_up = reach * open_share;
	float reach_side = reach * aspect;
	/* The eye starts on the box's bounding sphere, which also frames a box of no size, such as a single point */
	camera->distance = camera->scale / 2;
	for (int corner = 0; corner < BOX_CORNERS; corner++) {
		Vector3 offset = subtract_vectors(get_box_corner(minimum, maximum, corner), camera->target);
		float depth = dot_vectors(offset, forward);
		camera->distance = fmaxf(camera->distance, fabsf(dot_vectors(offset, up)) / reach_up - depth);
		camera->distance = fmaxf(camera->distance, fabsf(dot_vectors(offset, right)) / reach_side - depth);
	}
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
Matrix4 get_projection_matrix(const Camera *camera, float aspect, float lift) {
	float near = fmaxf(camera->distance * NEAR_PLANE_SHARE, camera->scale * SMALLEST_SIZE);
	float far = camera->distance + camera->scale * FAR_PLANE_SIZES;
	Matrix4 projection = get_perspective_matrix((float)FIELD_OF_VIEW, aspect, near, far);
	lift_perspective(&projection, lift);
	return projection;
}
