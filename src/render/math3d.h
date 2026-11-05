#pragma once

#include <glib.h>

/* Sizes of the values a vertex and a matrix hold */
#define POINT_COMPONENTS 3
#define COLOUR_CHANNELS 4
#define ALPHA_CHANNEL 3
#define TEXTURE_COORDINATES 2
#define MATRIX_ORDER 4
/* A normal matrix is the 3x3 corner of a 4x4 matrix */
#define NORMAL_MATRIX_VALUES 9
/* Sizes below this count as zero, so a flat or empty model never divides by nothing */
#define SMALLEST_SIZE 1e-4f

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
	float values[MATRIX_ORDER * MATRIX_ORDER];
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
/* Moves what a perspective shows up the screen, by a share of half the screen's height */
void lift_perspective(Matrix4 *matrix, float lift);
Matrix4 get_look_at_matrix(Vector3 eye, Vector3 target, Vector3 up);
Vector3 transform_point(const Matrix4 *matrix, Vector3 point);
Vector3 get_matrix_position(const Matrix4 *matrix);
/* The matrix that turns normals the same way the given matrix turns points */
void get_normal_matrix(const Matrix4 *matrix, float normal[NORMAL_MATRIX_VALUES]);
/* Splits a resting matrix into its position, rotation and scale, with no rotation for a part scaled to nothing */
void split_matrix(const Matrix4 *matrix, Vector3 *position, Quaternion *rotation, Vector3 *scale);
