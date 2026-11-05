#include <assimp/cimport.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <float.h>
#include <stdlib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <string.h>

#include "lang/en.h"
#include "model/model.h"
#include "model/animation.h"

/* Files that leave the timing out are played at the frame rate most exporters assume */
#define DEFAULT_TICKS_PER_SECOND 25.0
#define LOAD_STEPS (aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices \
	| aiProcess_LimitBoneWeights | aiProcess_SortByPType | aiProcess_FlipUVs | aiProcess_ImproveCacheLocality \
	| aiProcess_FindInvalidData | aiProcess_ValidateDataStructure | aiProcess_GenUVCoords | aiProcess_TransformUVCoords \
	| aiProcess_PopulateArmatureData)
/* The smoothing angle the Assimp documentation names as giving good results */
#define SMOOTHING_ANGLE 80.0f
/* A model file is untrusted, so its textures may only be regular files of a sane size, and pictures that would
 * decode into more pixels than this are refused from their header, before any pixel is made */
#define MAXIMUM_TEXTURE_FILE_BYTES (256 * 1024 * 1024)
#define MAXIMUM_TEXTURE_PIXELS (8192 * 8192)
/* The header is fed in pieces this large until it tells the picture's size */
#define PROBE_CHUNK_BYTES (64 * 1024)

/* Assimp keeps the last error in one string for every thread, so imports take turns */
static GMutex import_lock;


/* Assimp stores matrices row after row, and OpenGL reads them column after column */
static Matrix4 convert_matrix(const struct aiMatrix4x4 *matrix) {
	return (Matrix4){{
		matrix->a1, matrix->b1, matrix->c1, matrix->d1,
		matrix->a2, matrix->b2, matrix->c2, matrix->d2,
		matrix->a3, matrix->b3, matrix->c3, matrix->d3,
		matrix->a4, matrix->b4, matrix->c4, matrix->d4,
	}};
}


/* The scene tree flattened with parents first, with lookups by name for animations and by node for joints */
static void add_nodes(Model *model, GHashTable *names, GHashTable *places, const struct aiNode *node, int parent) {
	ModelNode entry = {
		.name = g_strndup(node->mName.data, node->mName.length),
		.parent = parent,
		.local = convert_matrix(&node->mTransformation),
		.meshes = g_memdup2(node->mMeshes, node->mNumMeshes * sizeof(guint)),
		.mesh_count = node->mNumMeshes,
	};
	int index = (int)model->nodes->len;
	g_array_append_val(model->nodes, entry);
	if (!g_hash_table_contains(names, entry.name))
		g_hash_table_insert(names, entry.name, GINT_TO_POINTER(index + 1));
	g_hash_table_insert(places, (gpointer)node, GINT_TO_POINTER(index + 1));
	for (guint child = 0; child < node->mNumChildren; child++)
		add_nodes(model, names, places, node->mChildren[child], index);
}


static int find_node(GHashTable *names, const struct aiString *name) {
	g_autofree char *text = g_strndup(name->data, name->length);
	return GPOINTER_TO_INT(g_hash_table_lookup(names, text)) - 1;
}


/* Each texture becomes plain pixels here, so the window only passes them to the graphics card */
static gboolean read_texture_pixels(GdkTexture *texture, ModelTexture *pixels) {
	if (!texture)
		return FALSE;
	g_autoptr(GdkTextureDownloader) downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
	gsize stride = 0;
	g_autoptr(GBytes) bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
	pixels->width = gdk_texture_get_width(texture);
	pixels->height = gdk_texture_get_height(texture);
	gsize row = (gsize)pixels->width * COLOUR_CHANNELS;
	if (stride == row) {
		pixels->pixels = g_bytes_ref(bytes);
		return TRUE;
	}
	const guint8 *source = g_bytes_get_data(bytes, NULL);
	guint8 *packed = g_malloc(row * (gsize)pixels->height);
	for (int line = 0; line < pixels->height; line++)
		memcpy(packed + row * (gsize)line, source + stride * (gsize)line, row);
	pixels->pixels = g_bytes_new_take(packed, row * (gsize)pixels->height);
	return TRUE;
}


/* GdkTexture reads PNG, JPEG and TIFF, and GdkPixbuf's loaders cover the TGA and BMP files older formats use */
static gboolean read_pixbuf_pixels(GdkPixbuf *source, ModelTexture *pixels) {
	if (!source)
		return FALSE;
	g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_add_alpha(source, FALSE, 0, 0, 0);
	g_object_unref(source);
	pixels->width = gdk_pixbuf_get_width(pixbuf);
	pixels->height = gdk_pixbuf_get_height(pixbuf);
	gsize row = (gsize)pixels->width * COLOUR_CHANNELS, stride = (gsize)gdk_pixbuf_get_rowstride(pixbuf);
	const guint8 *source_pixels = gdk_pixbuf_read_pixels(pixbuf);
	guint8 *packed = g_malloc(row * (gsize)pixels->height);
	for (int line = 0; line < pixels->height; line++)
		memcpy(packed + row * (gsize)line, source_pixels + stride * (gsize)line, row);
	pixels->pixels = g_bytes_new_take(packed, row * (gsize)pixels->height);
	return TRUE;
}


typedef struct {
	int width, height;
} PictureSize;


static void note_picture_size(GdkPixbufLoader *loader, int width, int height, PictureSize *size) {
	*size = (PictureSize){width, height};
}


static gboolean is_picture_small_enough(GBytes *bytes) {
	PictureSize size = {0};
	g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
	g_signal_connect(loader, "size-prepared", G_CALLBACK(note_picture_size), &size);
	const guint8 *data = g_bytes_get_data(bytes, NULL);
	gsize length = g_bytes_get_size(bytes);
	for (gsize offset = 0; offset < length && size.width == 0; offset += PROBE_CHUNK_BYTES)
		if (!gdk_pixbuf_loader_write(loader, data + offset, MIN(PROBE_CHUNK_BYTES, length - offset), NULL))
			break;
	/* Closing a picture fed only in part reports it unfinished, which is expected here */
	gdk_pixbuf_loader_close(loader, NULL);
	return size.width > 0 && size.height > 0 && (gint64)size.width * size.height <= MAXIMUM_TEXTURE_PIXELS;
}


static gboolean decode_texture_bytes(GBytes *bytes, ModelTexture *pixels) {
	if (!is_picture_small_enough(bytes))
		return FALSE;
	g_autoptr(GdkTexture) decoded = gdk_texture_new_from_bytes(bytes, NULL);
	if (decoded)
		return read_texture_pixels(decoded, pixels);
	g_autoptr(GInputStream) stream = g_memory_input_stream_new_from_bytes(bytes);
	return read_pixbuf_pixels(gdk_pixbuf_new_from_stream(stream, NULL, NULL), pixels);
}


/* A texture path may name anything, such as a device or a pipe, so only regular files of a sane size are read */
static gboolean decode_texture_file(GFile *file, ModelTexture *pixels) {
	g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR
		|| g_file_info_get_size(info) > MAXIMUM_TEXTURE_FILE_BYTES)
		return FALSE;
	g_autoptr(GBytes) bytes = g_file_load_bytes(file, NULL, NULL, NULL);
	return bytes && decode_texture_bytes(bytes, pixels);
}


/* Raw embedded texels are stored blue first, as Assimp keeps them */
static gboolean read_raw_texels(const struct aiTexture *texture, ModelTexture *pixels) {
	gsize count = (gsize)texture->mWidth * texture->mHeight;
	if (count > MAXIMUM_TEXTURE_PIXELS)
		return FALSE;
	guint8 *rgba = g_malloc(count * COLOUR_CHANNELS);
	for (gsize index = 0; index < count; index++) {
		const struct aiTexel *texel = &texture->pcData[index];
		guint8 *pixel = rgba + index * COLOUR_CHANNELS;
		pixel[0] = texel->r;
		pixel[1] = texel->g;
		pixel[2] = texel->b;
		pixel[3] = texel->a;
	}
	pixels->width = (int)texture->mWidth;
	pixels->height = (int)texture->mHeight;
	pixels->pixels = g_bytes_new_take(rgba, count * COLOUR_CHANNELS);
	return TRUE;
}


static gboolean read_embedded_texture(const struct aiTexture *texture, ModelTexture *pixels) {
	if (texture->mHeight > 0)
		return read_raw_texels(texture, pixels);
	g_autoptr(GBytes) bytes = g_bytes_new(texture->pcData, texture->mWidth);
	return decode_texture_bytes(bytes, pixels);
}


/* Texture paths are relative to the model file, and exporters often write Windows separators */
static gboolean read_texture_file(GFile *folder, const char *path, ModelTexture *pixels) {
	g_autofree char *local = g_strdelimit(g_strdup(path), "\\", '/');
	g_autoptr(GFile) file = g_path_is_absolute(local) ? g_file_new_for_path(local) : g_file_resolve_relative_path(folder,
		local);
	if (decode_texture_file(file, pixels))
		return TRUE;
	g_autofree char *name = g_path_get_basename(local);
	g_autoptr(GFile) nearby = g_file_get_child(folder, name);
	return decode_texture_file(nearby, pixels);
}


/* Textures shared by several materials are read once, looked up by their path */
static int add_texture(Model *model, GHashTable *paths, const struct aiScene *scene, GFile *folder, const char *path) {
	gpointer known = g_hash_table_lookup(paths, path);
	if (known)
		return GPOINTER_TO_INT(known) - 1;
	ModelTexture pixels = {0};
	const struct aiTexture *embedded = aiGetEmbeddedTexture(scene, path);
	gboolean is_read = embedded ? read_embedded_texture(embedded, &pixels) : read_texture_file(folder, path, &pixels);
	int index = -1;
	if (is_read) {
		index = (int)model->textures->len;
		g_array_append_val(model->textures, pixels);
	} else {
		g_message("Texture %s could not be read", path);
	}
	g_hash_table_insert(paths, g_strdup(path), GINT_TO_POINTER(index + 1));
	return index;
}


static int find_material_texture(Model *model, GHashTable *paths, const struct aiScene *scene, GFile *folder,
	const struct aiMaterial *material) {
	static const enum aiTextureType TYPES[] = {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE};
	for (guint type = 0; type < G_N_ELEMENTS(TYPES); type++) {
		if (aiGetMaterialTextureCount(material, TYPES[type]) == 0)
			continue;
		struct aiString path;
		if (aiGetMaterialTexture(material, TYPES[type], 0, &path, NULL, NULL, NULL, NULL, NULL, NULL) == aiReturn_SUCCESS)
			return add_texture(model, paths, scene, folder, path.data);
	}
	return -1;
}


static void add_materials(Model *model, const struct aiScene *scene, GFile *folder) {
	g_autoptr(GHashTable) paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (guint index = 0; index < scene->mNumMaterials; index++) {
		const struct aiMaterial *source = scene->mMaterials[index];
		ModelMaterial material = {{1, 1, 1, 1}, -1};
		struct aiColor4D colour;
		if (aiGetMaterialColor(source, AI_MATKEY_BASE_COLOR, &colour) == aiReturn_SUCCESS
			|| aiGetMaterialColor(source, AI_MATKEY_COLOR_DIFFUSE, &colour) == aiReturn_SUCCESS)
			memcpy(material.colour, &colour, sizeof(material.colour));
		float opacity = 1;
		unsigned int count = 1;
		if (aiGetMaterialFloatArray(source, AI_MATKEY_OPACITY, &opacity, &count) == aiReturn_SUCCESS)
			material.colour[ALPHA_CHANNEL] *= opacity;
		material.texture = find_material_texture(model, paths, scene, folder, source);
		g_array_append_val(model->materials, material);
	}
	if (model->materials->len == 0) {
		ModelMaterial plain = {{1, 1, 1, 1}, -1};
		g_array_append_val(model->materials, plain);
	}
}


/* Hands each vertex the strongest joints that bend it, keeping the slots in order of weight */
static void add_joint_weight(ModelVertex *vertex, uint16_t joint, float weight) {
	int weakest = 0;
	for (int slot = 1; slot < JOINTS_PER_VERTEX; slot++)
		if (vertex->weights[slot] < vertex->weights[weakest])
			weakest = slot;
	if (weight <= vertex->weights[weakest])
		return;
	vertex->joints[weakest] = joint;
	vertex->weights[weakest] = weight;
}


/* Two armatures may give joints the same names, so a joint finds its own node through the armature data first */
static void add_mesh_joints(Model *model, GHashTable *names, GHashTable *places, const struct aiMesh *source,
	ModelMesh *mesh) {
	mesh->joint_base = model->joints->len;
	for (guint index = 0; index < source->mNumBones; index++) {
		const struct aiBone *bone = source->mBones[index];
		int node = bone->mNode ? GPOINTER_TO_INT(g_hash_table_lookup(places, bone->mNode)) - 1 : -1;
		if (node < 0)
			node = find_node(names, &bone->mName);
		ModelJoint joint = {node < 0 ? 0 : (guint)node, convert_matrix(&bone->mOffsetMatrix)};
		g_array_append_val(model->joints, joint);
		for (guint weight = 0; weight < bone->mNumWeights; weight++) {
			const struct aiVertexWeight *entry = &bone->mWeights[weight];
			if (entry->mVertexId < mesh->vertex_count)
				add_joint_weight(&mesh->vertices[entry->mVertexId], (uint16_t)index, entry->mWeight);
		}
	}
	mesh->is_skinned = source->mNumBones > 0;
}


static int compare_edges(gconstpointer a, gconstpointer b) {
	guint64 first = *(const guint64 *)a, second = *(const guint64 *)b;
	return first < second ? -1 : first > second;
}


/* Each edge shared by two triangles is listed once, found by sorting all edges by their two corners */
static void add_mesh_edges(ModelMesh *mesh) {
	guint64 *keys = g_new(guint64, mesh->index_count);
	guint key_count = 0;
	for (guint triangle = 0; triangle + TRIANGLE_CORNERS <= mesh->index_count; triangle += TRIANGLE_CORNERS) {
		for (guint corner = 0; corner < TRIANGLE_CORNERS; corner++) {
			uint32_t from = mesh->indices[triangle + corner], to = mesh->indices[triangle + (corner + 1) % TRIANGLE_CORNERS];
			keys[key_count++] = ((guint64)MIN(from, to) << 32) | MAX(from, to);
		}
	}
	qsort(keys, key_count, sizeof(guint64), compare_edges);
	mesh->edges = g_new(uint32_t, (gsize)key_count * 2);
	mesh->edge_count = 0;
	for (guint index = 0; index < key_count; index++) {
		if (index > 0 && keys[index] == keys[index - 1])
			continue;
		mesh->edges[mesh->edge_count++] = (uint32_t)(keys[index] >> 32);
		mesh->edges[mesh->edge_count++] = (uint32_t)keys[index];
	}
	g_free(keys);
}


static void add_mesh(Model *model, GHashTable *names, GHashTable *places, const struct aiMesh *source) {
	ModelMesh mesh = {0};
	mesh.vertex_count = source->mNumVertices;
	mesh.vertices = g_new0(ModelVertex, mesh.vertex_count);
	mesh.material = MIN(source->mMaterialIndex, model->materials->len - 1);
	for (guint index = 0; index < mesh.vertex_count; index++) {
		ModelVertex *vertex = &mesh.vertices[index];
		memcpy(vertex->position, &source->mVertices[index], sizeof(vertex->position));
		if (source->mNormals)
			memcpy(vertex->normal, &source->mNormals[index], sizeof(vertex->normal));
		if (source->mTextureCoords[0]) {
			vertex->uv[0] = source->mTextureCoords[0][index].x;
			vertex->uv[1] = source->mTextureCoords[0][index].y;
		}
		if (source->mColors[0])
			memcpy(vertex->colour, &source->mColors[0][index], sizeof(vertex->colour));
		else
			vertex->colour[0] = vertex->colour[1] = vertex->colour[2] = vertex->colour[3] = 1;
	}
	GArray *indices = g_array_sized_new(FALSE, FALSE, sizeof(uint32_t), source->mNumFaces * TRIANGLE_CORNERS);
	for (guint face = 0; face < source->mNumFaces; face++) {
		if (source->mFaces[face].mNumIndices == TRIANGLE_CORNERS)
			g_array_append_vals(indices, source->mFaces[face].mIndices, TRIANGLE_CORNERS);
	}
	mesh.index_count = indices->len;
	mesh.indices = (uint32_t *)g_array_free(indices, FALSE);
	add_mesh_edges(&mesh);
	add_mesh_joints(model, names, places, source, &mesh);
	model->triangle_count += mesh.index_count / TRIANGLE_CORNERS;
	model->vertex_count += mesh.vertex_count;
	g_array_append_val(model->meshes, mesh);
}


static void add_animations(Model *model, GHashTable *names, const struct aiScene *scene) {
	for (guint index = 0; index < scene->mNumAnimations; index++) {
		const struct aiAnimation *source = scene->mAnimations[index];
		double ticks = source->mTicksPerSecond > 0 ? source->mTicksPerSecond : DEFAULT_TICKS_PER_SECOND;
		ModelAnimation animation = {0};
		animation.name = source->mName.length ? g_strndup(source->mName.data, source->mName.length)
			: g_strdup_printf("Animation %u", index + 1);
		animation.duration = (float)(source->mDuration / ticks);
		animation.channels = g_new0(ModelChannel, source->mNumChannels);
		for (guint channel = 0; channel < source->mNumChannels; channel++) {
			const struct aiNodeAnim *keys = source->mChannels[channel];
			int node = find_node(names, &keys->mNodeName);
			if (node < 0)
				continue;
			ModelChannel *entry = &animation.channels[animation.channel_count++];
			entry->node = (guint)node;
			entry->position_count = keys->mNumPositionKeys;
			entry->positions = g_new(VectorKey, entry->position_count);
			for (guint key = 0; key < entry->position_count; key++) {
				const struct aiVectorKey *value = &keys->mPositionKeys[key];
				entry->positions[key] = (VectorKey){(float)(value->mTime / ticks),
					{value->mValue.x, value->mValue.y, value->mValue.z}};
			}
			entry->scale_count = keys->mNumScalingKeys;
			entry->scales = g_new(VectorKey, entry->scale_count);
			for (guint key = 0; key < entry->scale_count; key++) {
				const struct aiVectorKey *value = &keys->mScalingKeys[key];
				entry->scales[key] = (VectorKey){(float)(value->mTime / ticks),
					{value->mValue.x, value->mValue.y, value->mValue.z}};
			}
			entry->rotation_count = keys->mNumRotationKeys;
			entry->rotations = g_new(RotationKey, entry->rotation_count);
			for (guint key = 0; key < entry->rotation_count; key++) {
				const struct aiQuatKey *value = &keys->mRotationKeys[key];
				entry->rotations[key] = (RotationKey){(float)(value->mTime / ticks),
					{value->mValue.x, value->mValue.y, value->mValue.z, value->mValue.w}};
			}
		}
		g_array_append_val(model->animations, animation);
	}
}


/* The bounds of the pose the model is first shown in, which is the first animation's start since animations play by
 * themselves, or the resting pose, with skinned vertices placed where their joints put them */
static void measure_bounds(Model *model) {
	model->minimum = make_vector(FLT_MAX, FLT_MAX, FLT_MAX);
	model->maximum = make_vector(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	Matrix4 *globals = g_new(Matrix4, MAX(model->nodes->len, 1));
	const ModelAnimation *first = model->animations->len ? &g_array_index(model->animations, ModelAnimation, 0) : NULL;
	pose_model(model, first, 0, globals);
	for (guint index = 0; index < model->nodes->len; index++) {
		ModelNode *node = &g_array_index(model->nodes, ModelNode, index);
		for (guint mesh_index = 0; mesh_index < node->mesh_count; mesh_index++) {
			if (node->meshes[mesh_index] >= model->meshes->len)
				continue;
			ModelMesh *mesh = &g_array_index(model->meshes, ModelMesh, node->meshes[mesh_index]);
			for (guint vertex_index = 0; vertex_index < mesh->vertex_count; vertex_index++) {
				ModelVertex *vertex = &mesh->vertices[vertex_index];
				Vector3 point = make_vector(vertex->position[0], vertex->position[1], vertex->position[2]);
				/* A skinned mesh is drawn by its joints alone, so a vertex no joint bends stays where it is */
				Vector3 placed = mesh->is_skinned ? point : transform_point(&globals[index], point);
				if (mesh->is_skinned && vertex->weights[0] + vertex->weights[1] + vertex->weights[2]
					+ vertex->weights[3] > 0) {
					placed = make_vector(0, 0, 0);
					for (int slot = 0; slot < JOINTS_PER_VERTEX; slot++) {
						if (vertex->weights[slot] <= 0)
							continue;
						ModelJoint *joint = &g_array_index(model->joints, ModelJoint, mesh->joint_base
							+ vertex->joints[slot]);
						Matrix4 bend = multiply_matrices(&globals[joint->node], &joint->offset);
						placed = add_vectors(placed, scale_vector(transform_point(&bend, point), vertex->weights[slot]));
					}
				}
				model->minimum = min_vectors(model->minimum, placed);
				model->maximum = max_vectors(model->maximum, placed);
			}
		}
	}
	/* A file with bones and no geometry is framed by the bone positions */
	for (guint index = 0; model->vertex_count == 0 && index < model->nodes->len; index++) {
		if (!g_array_index(model->nodes, ModelNode, index).is_bone)
			continue;
		Vector3 place = get_matrix_position(&globals[index]);
		model->minimum = min_vectors(model->minimum, place);
		model->maximum = max_vectors(model->maximum, place);
	}
	g_free(globals);
	if (model->minimum.x > model->maximum.x) {
		model->minimum = make_vector(-1, -1, -1);
		model->maximum = make_vector(1, 1, 1);
	}
}


/* Bones are the nodes skin follows and the nodes animations move, which covers rigs without a mesh */
static void mark_bones(Model *model) {
	for (guint index = 0; index < model->joints->len; index++)
		g_array_index(model->nodes, ModelNode, g_array_index(model->joints, ModelJoint, index).node).is_bone = TRUE;
	for (guint index = 0; index < model->animations->len; index++) {
		ModelAnimation *animation = &g_array_index(model->animations, ModelAnimation, index);
		for (guint channel = 0; channel < animation->channel_count; channel++)
			g_array_index(model->nodes, ModelNode, animation->channels[channel].node).is_bone = TRUE;
	}
	for (guint index = 0; index < model->nodes->len; index++)
		model->has_skeleton = model->has_skeleton || g_array_index(model->nodes, ModelNode, index).is_bone;
}


static Model *create_model(void) {
	Model *model = g_new0(Model, 1);
	model->meshes = g_array_new(FALSE, TRUE, sizeof(ModelMesh));
	model->materials = g_array_new(FALSE, TRUE, sizeof(ModelMaterial));
	model->textures = g_array_new(FALSE, TRUE, sizeof(ModelTexture));
	model->nodes = g_array_new(FALSE, TRUE, sizeof(ModelNode));
	model->joints = g_array_new(FALSE, TRUE, sizeof(ModelJoint));
	model->animations = g_array_new(FALSE, TRUE, sizeof(ModelAnimation));
	return model;
}


static Model *read_model(GFile *file, GCancellable *cancellable, GError **error) {
	g_autofree char *path = g_file_get_path(file);
	if (!path) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_NOT_LOCAL);
		return NULL;
	}
	struct aiPropertyStore *properties = aiCreatePropertyStore();
	aiSetImportPropertyInteger(properties, AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
	aiSetImportPropertyInteger(properties, AI_CONFIG_PP_LBW_MAX_WEIGHTS, JOINTS_PER_VERTEX);
	aiSetImportPropertyFloat(properties, AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, SMOOTHING_ANGLE);
	g_mutex_lock(&import_lock);
	/* A load replaced while it waited for the one before is not read either */
	if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
		g_mutex_unlock(&import_lock);
		aiReleasePropertyStore(properties);
		return NULL;
	}
	const struct aiScene *scene = aiImportFileExWithProperties(path, LOAD_STEPS, NULL, properties);
	if (!scene || !scene->mRootNode)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, aiGetErrorString());
	g_mutex_unlock(&import_lock);
	aiReleasePropertyStore(properties);
	if (!scene || !scene->mRootNode) {
		if (scene)
			aiReleaseImport(scene);
		return NULL;
	}
	Model *model = create_model();
	g_autoptr(GHashTable) names = g_hash_table_new(g_str_hash, g_str_equal);
	g_autoptr(GHashTable) places = g_hash_table_new(g_direct_hash, g_direct_equal);
	add_nodes(model, names, places, scene->mRootNode, -1);
	g_autoptr(GFile) folder = g_file_get_parent(file);
	add_materials(model, scene, folder);
	for (guint index = 0; index < scene->mNumMeshes; index++)
		add_mesh(model, names, places, scene->mMeshes[index]);
	add_animations(model, names, scene);
	mark_bones(model);
	measure_bounds(model);
	/* The format comes from the file's own name, as a folder name may hold a dot too */
	g_autofree char *name = g_path_get_basename(path);
	const char *extension = strrchr(name, '.');
	model->format = g_ascii_strup(extension ? extension + 1 : "", -1);
	aiReleaseImport(scene);
	return model;
}


static void run_loading(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	/* A load replaced before its worker ran is never read */
	if (g_task_return_error_if_cancelled(task))
		return;
	GError *error = NULL;
	Model *model = read_model(G_FILE(data), cancellable, &error);
	if (model)
		g_task_return_pointer(task, model, (GDestroyNotify)free_model);
	else
		g_task_return_error(task, error);
}


void load_model(GFile *file, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data) {
	g_autoptr(GTask) task = g_task_new(NULL, cancellable, callback, data);
	g_task_set_task_data(task, g_object_ref(file), g_object_unref);
	g_task_run_in_thread(task, run_loading);
}


Model *finish_loading_model(GAsyncResult *result, GError **error) {
	return g_task_propagate_pointer(G_TASK(result), error);
}


void free_model(Model *model) {
	for (guint index = 0; index < model->meshes->len; index++) {
		ModelMesh *mesh = &g_array_index(model->meshes, ModelMesh, index);
		g_free(mesh->vertices);
		g_free(mesh->indices);
		g_free(mesh->edges);
	}
	for (guint index = 0; index < model->textures->len; index++)
		g_bytes_unref(g_array_index(model->textures, ModelTexture, index).pixels);
	for (guint index = 0; index < model->nodes->len; index++) {
		ModelNode *node = &g_array_index(model->nodes, ModelNode, index);
		g_free(node->name);
		g_free(node->meshes);
	}
	for (guint index = 0; index < model->animations->len; index++) {
		ModelAnimation *animation = &g_array_index(model->animations, ModelAnimation, index);
		for (guint channel = 0; channel < animation->channel_count; channel++) {
			g_free(animation->channels[channel].positions);
			g_free(animation->channels[channel].rotations);
			g_free(animation->channels[channel].scales);
		}
		g_free(animation->channels);
		g_free(animation->name);
	}
	g_array_unref(model->meshes);
	g_array_unref(model->materials);
	g_array_unref(model->textures);
	g_array_unref(model->nodes);
	g_array_unref(model->joints);
	g_array_unref(model->animations);
	g_free(model->format);
	g_free(model);
}


/* Assimp adds its list to the end of the string it gets, so the string starts out empty */
char **get_model_patterns(void) {
	struct aiString list = {0};
	aiGetExtensionList(&list);
	char **patterns = g_strsplit(list.data, ";", -1);
	for (char **pattern = patterns; *pattern; pattern++)
		g_strstrip(*pattern);
	return patterns;
}
