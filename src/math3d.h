#pragma once

#include <stdbool.h>

/* A point or direction in space */
typedef struct {
	float x, y, z;
} Vector3;

/* A rotation as a unit quaternion */
typedef struct {
	float x, y, z, w;
} Quaternion;

/* A 4x4 matrix stored column after column, the order OpenGL reads */
typedef struct {
	float values[16];
} Matrix4;

Vector3 make_vector(float x, float y, float z);
Vector3 add_vectors(Vector3 a, Vector3 b);
Vector3 subtract_vectors(Vector3 a, Vector3 b);
Vector3 scale_vector(Vector3 vector, float factor);
Vector3 mix_vectors(Vector3 a, Vector3 b, float amount);
Vector3 cross_vectors(Vector3 a, Vector3 b);
Vector3 normalize_vector(Vector3 vector);
Vector3 min_vectors(Vector3 a, Vector3 b);
Vector3 max_vectors(Vector3 a, Vector3 b);
float dot_vectors(Vector3 a, Vector3 b);
float get_vector_length(Vector3 vector);

Quaternion mix_quaternions(Quaternion a, Quaternion b, float amount);

Matrix4 get_identity_matrix(void);
Matrix4 multiply_matrices(const Matrix4 *a, const Matrix4 *b);
Matrix4 compose_matrix(Vector3 position, Quaternion rotation, Vector3 scale);
Matrix4 get_perspective_matrix(float field_of_view, float aspect, float near, float far);
Matrix4 get_look_at_matrix(Vector3 eye, Vector3 target, Vector3 up);
Vector3 transform_point(const Matrix4 *matrix, Vector3 point);
Vector3 get_matrix_position(const Matrix4 *matrix);
/* The matrix that turns normals the same way the given matrix turns points */
void get_normal_matrix(const Matrix4 *matrix, float normal[9]);
bool is_identity_matrix(const Matrix4 *matrix);
