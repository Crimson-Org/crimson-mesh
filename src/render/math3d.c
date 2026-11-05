#include <math.h>
#include <string.h>

#include "render/math3d.h"

/* Quaternions closer than this are blended linearly, since the angle between them is too small to divide */
#define SLERP_LINEAR_LIMIT 0.9995f
/* Below this a matrix has no inverse that means anything */
#define SINGULAR_DETERMINANT 1e-12f


Vector3 make_vector(float x, float y, float z) {
	return (Vector3){x, y, z};
}


Vector3 add_vectors(Vector3 a, Vector3 b) {
	return (Vector3){a.x + b.x, a.y + b.y, a.z + b.z};
}


Vector3 subtract_vectors(Vector3 a, Vector3 b) {
	return (Vector3){a.x - b.x, a.y - b.y, a.z - b.z};
}


Vector3 scale_vector(Vector3 vector, float factor) {
	return (Vector3){vector.x * factor, vector.y * factor, vector.z * factor};
}


Vector3 mix_vectors(Vector3 a, Vector3 b, float amount) {
	return add_vectors(a, scale_vector(subtract_vectors(b, a), amount));
}


Vector3 cross_vectors(Vector3 a, Vector3 b) {
	return (Vector3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}


float dot_vectors(Vector3 a, Vector3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}


float get_vector_length(Vector3 vector) {
	return sqrtf(dot_vectors(vector, vector));
}


Vector3 normalize_vector(Vector3 vector) {
	float length = get_vector_length(vector);
	return length > 0 ? scale_vector(vector, 1 / length) : vector;
}


Vector3 min_vectors(Vector3 a, Vector3 b) {
	return (Vector3){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)};
}


Vector3 max_vectors(Vector3 a, Vector3 b) {
	return (Vector3){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)};
}


/* Turns along the shorter arc, so a key pair stored with inverted signs does not spin the long way round */
Quaternion mix_quaternions(Quaternion a, Quaternion b, float amount) {
	float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (cosine < 0) {
		b = (Quaternion){-b.x, -b.y, -b.z, -b.w};
		cosine = -cosine;
	}
	float from = 1 - amount, to = amount;
	if (cosine < SLERP_LINEAR_LIMIT) {
		float angle = acosf(cosine);
		float sine = sinf(angle);
		from = sinf((1 - amount) * angle) / sine;
		to = sinf(amount * angle) / sine;
	}
	Quaternion mixed = {from * a.x + to * b.x, from * a.y + to * b.y, from * a.z + to * b.z, from * a.w + to * b.w};
	float length = sqrtf(mixed.x * mixed.x + mixed.y * mixed.y + mixed.z * mixed.z + mixed.w * mixed.w);
	/* Keys of zero length, which a broken file may hold, mix to no rotation instead of NaN */
	if (length < SMALLEST_SIZE)
		return (Quaternion){0, 0, 0, 1};
	return (Quaternion){mixed.x / length, mixed.y / length, mixed.z / length, mixed.w / length};
}


Matrix4 get_identity_matrix(void) {
	return (Matrix4){{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
}


Matrix4 multiply_matrices(const Matrix4 *a, const Matrix4 *b) {
	Matrix4 product;
	for (int column = 0; column < MATRIX_ORDER; column++) {
		for (int row = 0; row < MATRIX_ORDER; row++) {
			float sum = 0;
			for (int index = 0; index < MATRIX_ORDER; index++)
				sum += a->values[index * MATRIX_ORDER + row] * b->values[column * MATRIX_ORDER + index];
			product.values[column * MATRIX_ORDER + row] = sum;
		}
	}
	return product;
}


Matrix4 compose_matrix(Vector3 position, Quaternion rotation, Vector3 scale) {
	float x = rotation.x, y = rotation.y, z = rotation.z, w = rotation.w;
	return (Matrix4){{
		(1 - 2 * (y * y + z * z)) * scale.x, 2 * (x * y + z * w) * scale.x, 2 * (x * z - y * w) * scale.x, 0,
		2 * (x * y - z * w) * scale.y, (1 - 2 * (x * x + z * z)) * scale.y, 2 * (y * z + x * w) * scale.y, 0,
		2 * (x * z + y * w) * scale.z, 2 * (y * z - x * w) * scale.z, (1 - 2 * (x * x + y * y)) * scale.z, 0,
		position.x, position.y, position.z, 1,
	}};
}


Matrix4 get_perspective_matrix(float field_of_view, float aspect, float near, float far) {
	float focal = 1 / tanf(field_of_view / 2);
	Matrix4 matrix = {{0}};
	matrix.values[0] = focal / aspect;
	matrix.values[5] = focal;
	matrix.values[10] = (far + near) / (near - far);
	matrix.values[11] = -1;
	matrix.values[14] = 2 * far * near / (near - far);
	return matrix;
}


Matrix4 get_look_at_matrix(Vector3 eye, Vector3 target, Vector3 up) {
	Vector3 forward = normalize_vector(subtract_vectors(target, eye));
	Vector3 side = normalize_vector(cross_vectors(forward, up));
	Vector3 above = cross_vectors(side, forward);
	return (Matrix4){{
		side.x, above.x, -forward.x, 0,
		side.y, above.y, -forward.y, 0,
		side.z, above.z, -forward.z, 0,
		-dot_vectors(side, eye), -dot_vectors(above, eye), dot_vectors(forward, eye), 1,
	}};
}


Vector3 transform_point(const Matrix4 *matrix, Vector3 point) {
	const float *m = matrix->values;
	return (Vector3){
		m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12],
		m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13],
		m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14],
	};
}


Vector3 get_matrix_position(const Matrix4 *matrix) {
	return (Vector3){matrix->values[12], matrix->values[13], matrix->values[14]};
}


/* The inverse transpose of the upper 3x3, which keeps normals upright under uneven scaling */
void get_normal_matrix(const Matrix4 *matrix, float normal[NORMAL_MATRIX_VALUES]) {
	const float *m = matrix->values;
	float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
	float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (fabsf(determinant) < SINGULAR_DETERMINANT) {
		float identity[NORMAL_MATRIX_VALUES] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
		memcpy(normal, identity, sizeof(identity));
		return;
	}
	float inverse = 1 / determinant;
	normal[0] = (e * i - f * h) * inverse;
	normal[1] = (c * h - b * i) * inverse;
	normal[2] = (b * f - c * e) * inverse;
	normal[3] = (f * g - d * i) * inverse;
	normal[4] = (a * i - c * g) * inverse;
	normal[5] = (c * d - a * f) * inverse;
	normal[6] = (d * h - e * g) * inverse;
	normal[7] = (b * g - a * h) * inverse;
	normal[8] = (a * e - b * d) * inverse;
}


void split_matrix(const Matrix4 *matrix, Vector3 *position, Quaternion *rotation, Vector3 *scale) {
	const float *m = matrix->values;
	*position = make_vector(m[12], m[13], m[14]);
	*scale = make_vector(get_vector_length(make_vector(m[0], m[1], m[2])),
		get_vector_length(make_vector(m[4], m[5], m[6])), get_vector_length(make_vector(m[8], m[9], m[10])));
	if (scale->x < SMALLEST_SIZE || scale->y < SMALLEST_SIZE || scale->z < SMALLEST_SIZE) {
		*rotation = (Quaternion){0, 0, 0, 1};
		return;
	}
	float r[NORMAL_MATRIX_VALUES] = {
		m[0] / scale->x, m[1] / scale->x, m[2] / scale->x,
		m[4] / scale->y, m[5] / scale->y, m[6] / scale->y,
		m[8] / scale->z, m[9] / scale->z, m[10] / scale->z,
	};
	float trace = r[0] + r[4] + r[8];
	if (trace > 0) {
		float s = sqrtf(trace + 1) * 2;
		*rotation = (Quaternion){(r[5] - r[7]) / s, (r[6] - r[2]) / s, (r[1] - r[3]) / s, s / 4};
	} else if (r[0] > r[4] && r[0] > r[8]) {
		float s = sqrtf(1 + r[0] - r[4] - r[8]) * 2;
		*rotation = (Quaternion){s / 4, (r[3] + r[1]) / s, (r[6] + r[2]) / s, (r[5] - r[7]) / s};
	} else if (r[4] > r[8]) {
		float s = sqrtf(1 + r[4] - r[0] - r[8]) * 2;
		*rotation = (Quaternion){(r[3] + r[1]) / s, s / 4, (r[7] + r[5]) / s, (r[6] - r[2]) / s};
	} else {
		float s = sqrtf(1 + r[8] - r[0] - r[4]) * 2;
		*rotation = (Quaternion){(r[6] + r[2]) / s, (r[7] + r[5]) / s, s / 4, (r[1] - r[3]) / s};
	}
}
