#include <math.h>
#include <string.h>

#include "model/animation.h"


/* The last key at or before the time, found by halving, since long takes hold thousands of keys */
static guint find_key(const void *keys, gsize size, guint count, float time) {
	guint low = 0, high = count;
	while (high - low > 1) {
		guint middle = (low + high) / 2;
		float key_time = *(const float *)((const char *)keys + middle * size);
		if (key_time <= time)
			low = middle;
		else
			high = middle;
	}
	return low;
}


static float get_blend(float start, float end, float time) {
	return end > start ? CLAMP((time - start) / (end - start), 0, 1) : 0;
}


static Vector3 sample_vector(const VectorKey *keys, guint count, float time, Vector3 fallback) {
	if (count == 0)
		return fallback;
	guint key = find_key(keys, sizeof(VectorKey), count, time);
	if (key + 1 >= count)
		return keys[key].value;
	return mix_vectors(keys[key].value, keys[key + 1].value, get_blend(keys[key].time, keys[key + 1].time, time));
}


static Quaternion sample_rotation(const RotationKey *keys, guint count, float time, Quaternion fallback) {
	if (count == 0)
		return fallback;
	guint key = find_key(keys, sizeof(RotationKey), count, time);
	if (key + 1 >= count)
		return keys[key].value;
	return mix_quaternions(keys[key].value, keys[key + 1].value,
		get_blend(keys[key].time, keys[key + 1].time, time));
}


void pose_model(const Model *model, const ModelAnimation *animation, float time, Matrix4 *globals) {
	guint count = model->nodes->len;
	for (guint index = 0; index < count; index++)
		globals[index] = g_array_index(model->nodes, ModelNode, index).local;
	for (guint channel = 0; animation && channel < animation->channel_count; channel++) {
		const ModelChannel *keys = &animation->channels[channel];
		Vector3 position, scale;
		Quaternion rotation;
		split_matrix(&globals[keys->node], &position, &rotation, &scale);
		globals[keys->node] = compose_matrix(sample_vector(keys->positions, keys->position_count, time, position),
			sample_rotation(keys->rotations, keys->rotation_count, time, rotation),
			sample_vector(keys->scales, keys->scale_count, time, scale));
	}
	for (guint index = 0; index < count; index++) {
		int parent = g_array_index(model->nodes, ModelNode, index).parent;
		if (parent >= 0)
			globals[index] = multiply_matrices(&globals[parent], &globals[index]);
	}
}


void get_joint_matrices(const Model *model, const Matrix4 *globals, Matrix4 *joints) {
	for (guint index = 0; index < model->joints->len; index++) {
		const ModelJoint *joint = &g_array_index(model->joints, ModelJoint, index);
		joints[index] = multiply_matrices(&globals[joint->node], &joint->offset);
	}
}
