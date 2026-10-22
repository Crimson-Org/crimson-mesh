#pragma once

#include "model.h"

/* Places each node at the time in seconds, or in the resting pose without an animation */
void pose_model(const Model *model, const ModelAnimation *animation, float time, Matrix4 *globals);
/* The matrix that moves each joint's skin from its bind pose to the current one */
void get_joint_matrices(const Model *model, const Matrix4 *globals, Matrix4 *joints);
