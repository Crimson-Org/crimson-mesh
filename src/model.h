#pragma once

#include <gio/gio.h>
#include <stdint.h>

#include "math3d.h"

/* Each vertex bends with up to this many joints */
#define JOINTS_PER_VERTEX 4

/* One corner of a triangle as the renderer uploads it */
typedef struct {
	float position[3];
	float normal[3];
	float uv[2];
	float colour[4];
	uint16_t joints[JOINTS_PER_VERTEX];
	float weights[JOINTS_PER_VERTEX];
} ModelVertex;

/* A batch of triangles that share one material */
typedef struct {
	ModelVertex *vertices;
	/* Triangles, and the same corners as pairs for drawing each edge once */
	uint32_t *indices, *edges;
	guint vertex_count, index_count, edge_count;
	guint material;
	/* Skinned meshes follow their joints, with matrices starting at joint_base in the model's joint list */
	gboolean is_skinned;
	guint joint_base;
} ModelMesh;

/* Decoded texture pixels, 4 bytes of red, green, blue and alpha per pixel from the top row down */
typedef struct {
	int width, height;
	GBytes *pixels;
} ModelTexture;

/* How a material colours its triangles, with texture minus one when it has none */
typedef struct {
	float colour[4];
	int texture;
} ModelMaterial;

/* A place in the scene tree, listed after its parent so a single pass works out each node's position */
typedef struct {
	char *name;
	int parent;
	Matrix4 local;
	/* The meshes drawn at this node */
	guint *meshes;
	guint mesh_count;
	/* Skin follows this node or an animation moves it, so it is drawn as part of the skeleton */
	gboolean is_bone;
} ModelNode;

/* A joint bends skin toward its node from the bind pose of the skin */
typedef struct {
	guint node;
	Matrix4 offset;
} ModelJoint;

/* Key times are in seconds */
typedef struct {
	float time;
	Vector3 value;
} VectorKey;

typedef struct {
	float time;
	Quaternion value;
} RotationKey;

/* The keys that move one node through an animation */
typedef struct {
	guint node;
	VectorKey *positions, *scales;
	RotationKey *rotations;
	guint position_count, rotation_count, scale_count;
} ModelChannel;

/* A named motion, such as running or an idle loop */
typedef struct {
	char *name;
	float duration;
	ModelChannel *channels;
	guint channel_count;
} ModelAnimation;

/* What the viewer shows from a model file, read without touching the window */
typedef struct {
	GArray *meshes, *materials, *textures, *nodes, *joints, *animations;
	Vector3 minimum, maximum;
	guint triangle_count, vertex_count;
	/* Some nodes are bones, and a file of only bones and animations has no triangles at all */
	gboolean has_skeleton;
	char *format;
} Model;

/* Reads the file on a worker thread and passes a Model, or an error with the loader's own message */
void load_model(GFile *file, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data);
Model *finish_loading_model(GAsyncResult *result, GError **error);
void free_model(Model *model);
/* Patterns such as "*.glb" for each format the loader reads */
char **get_model_patterns(void);
