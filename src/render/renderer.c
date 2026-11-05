#include <epoxy/gl.h>
#include <gio/gio.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "render/renderer.h"

#define SAMPLES 4
/* Each upload step stops after this long, so a model of many meshes and textures spreads over several frames, while
 * one mesh or texture still goes up whole */
#define UPLOAD_BUDGET_MICROSECONDS 6000
#define ANISOTROPY 8.0f
#define GRID_LINES 20
#define JOINT_POINT_SIZE 7.0f
#define JOINT_RIM_SIZE 10.0f
#define ATTRIBUTE_POSITION 0
#define ATTRIBUTE_NORMAL 1
#define ATTRIBUTE_UV 2
#define ATTRIBUTE_COLOUR 3
#define ATTRIBUTE_JOINTS 4
#define ATTRIBUTE_WEIGHTS 5
#define LIGHT_COUNT 3
#define TEXTURE_UNIT_BASE 0
#define TEXTURE_UNIT_JOINTS 1
/* Colours without alpha, such as lights and the background */
#define RGB_CHANNELS 3
#define SHADER_LOG_BYTES 1024
/* The grid spans a whole power of ten, with every fifth line strong and the rest faint */
#define GRID_SCALE_BASE 10
#define MAJOR_GRID_EVERY 5
#define MINOR_GRID_STRENGTH 0.45f
/* The grid fades out around the model, from this share of the grid's reach out to the grid's edge, so the grid
 * ends softly and the horizon and the view's edges stay clear, and the reach is a few model sizes but never under a
 * few grid steps */
#define GRID_FADE_START 0.3f
#define GRID_FADE_MODEL_SIZES 2.5f
#define GRID_FADE_LEAST_STEPS 4
/* A soft shadow on the ground under the model, a little wider than the model and darkest in the middle */
#define SHADOW_SPREAD 1.4f
#define SHADOW_CORNERS 6
/* Each grid step draws a line across each axis, two ends apiece */
#define GRID_LINE_ENDS 4
/* Joint matrices travel as a float texture, one texel per matrix column */
#define JOINT_TEXELS MATRIX_ORDER

/* The parts of a shader program the renderer sets each frame */
typedef struct {
	GLuint program;
	GLint model, view, projection, normal_matrix, is_skinned, joint_base, joints, base_colour, base_texture;
	GLint has_texture, eye, light_directions, light_colours, sky, ground, is_lit, is_wire, wire_colour;
} SurfaceShader;

typedef struct {
	GLuint program;
	GLint view_projection, point_size, tint, is_point, fade_centre, fade_radii, fade_start;
} LineShader;

typedef struct {
	GLuint program;
	GLint top, bottom, vignette;
} BackgroundShader;

typedef struct {
	GLuint program;
	GLint image;
} CopyShader;

/* One mesh's buffers on the graphics card */
typedef struct {
	GLuint array, edge_array, vertices, indices, edges;
	gboolean is_uploaded;
} MeshBuffers;

/* A line or point with its own colour, for the grid and the skeleton */
typedef struct {
	float position[POINT_COMPONENTS];
	float colour[COLOUR_CHANNELS];
} LineVertex;

struct Renderer {
	SurfaceShader surface;
	LineShader lines;
	BackgroundShader background;
	CopyShader copy;
	gboolean is_es;
	GLuint empty_array;
	/* The ground holds the grid's lines and then the shadow's two triangles */
	GLuint grid_array, grid_buffer;
	int grid_count;
	/* Where the ground is under the model, how far the grid reaches before fading out and the shadow's size */
	Vector3 ground_centre;
	float grid_fade_radius, shadow_radius_x, shadow_radius_z;
	GLuint skeleton_array, skeleton_buffer;
	GLuint joint_texture;
	/* Rows the joint texture holds now, and the largest texture side the card takes */
	GLint joint_rows, maximum_texture_size;
	/* Drawn with several samples a pixel, then resolved into a plain texture that is copied to the area */
	GLuint frame_buffer, colour_buffer, depth_buffer, resolve_buffer, resolve_texture;
	int frame_width, frame_height;
	float anisotropy;
	const Model *model;
	MeshBuffers *meshes;
	GLuint *textures;
	guint uploaded_meshes, uploaded_textures;
};

/* Light from three lamps and the sky, all in world directions except the headlight, which follows the eye */
typedef struct {
	float directions[LIGHT_COUNT][POINT_COMPONENTS];
	float colours[LIGHT_COUNT][RGB_CHANNELS];
	float sky[RGB_CHANNELS], ground[RGB_CHANNELS];
	gboolean is_view_relative, is_lit;
} LightingPreset;

/* A backdrop from top to bottom, with a vignette that darkens the backdrop's sides, so the model looks softly lit */
typedef struct {
	float top[RGB_CHANNELS], bottom[RGB_CHANNELS];
	float grid[COLOUR_CHANNELS], wire[COLOUR_CHANNELS];
	float vignette;
} BackgroundPreset;

static const LightingPreset LIGHTING_PRESETS[LIGHTING_COUNT] = {
	[LIGHTING_STUDIO] = {
		{{-0.45f, -0.7f, -0.55f}, {0.7f, -0.3f, -0.4f}, {0.2f, -0.4f, 0.9f}},
		{{1.5f, 1.45f, 1.38f}, {0.38f, 0.42f, 0.5f}, {0.7f, 0.72f, 0.8f}},
		{0.22f, 0.23f, 0.26f}, {0.09f, 0.085f, 0.08f}, FALSE, TRUE,
	},
	[LIGHTING_DAYLIGHT] = {
		{{-0.3f, -0.9f, -0.3f}, {0, 0, 0}, {0, 0, 0}},
		{{1.9f, 1.75f, 1.5f}, {0, 0, 0}, {0, 0, 0}},
		{0.26f, 0.32f, 0.44f}, {0.13f, 0.11f, 0.085f}, FALSE, TRUE,
	},
	[LIGHTING_HEADLIGHT] = {
		{{0, 0, 1}, {0, 0, 0}, {0, 0, 0}},
		{{1.4f, 1.4f, 1.4f}, {0, 0, 0}, {0, 0, 0}},
		{0.16f, 0.16f, 0.18f}, {0.1f, 0.1f, 0.11f}, TRUE, TRUE,
	},
	[LIGHTING_FLAT] = {{{0}}, {{0}}, {0}, {0}, FALSE, FALSE},
};

static const BackgroundPreset BACKGROUND_PRESETS[BACKGROUND_COUNT] = {
	[BACKGROUND_CHARCOAL] = {{0.17f, 0.18f, 0.2f}, {0.08f, 0.085f, 0.1f}, {1, 1, 1, 0.16f}, {0.31f, 0.78f, 0.78f, 0.7f},
		0.3f},
	[BACKGROUND_MIDNIGHT] = {{0.086f, 0.137f, 0.165f}, {0.04f, 0.07f, 0.086f}, {0.31f, 0.78f, 0.78f, 0.22f},
		{0.31f, 0.78f, 0.78f, 0.75f}, 0.35f},
	[BACKGROUND_STUDIO] = {{0.55f, 0.57f, 0.6f}, {0.27f, 0.28f, 0.31f}, {1, 1, 1, 0.22f}, {0.08f, 0.1f, 0.12f, 0.7f},
		0.3f},
	[BACKGROUND_LIGHT] = {{0.96f, 0.96f, 0.97f}, {0.82f, 0.84f, 0.87f}, {0, 0, 0, 0.16f}, {0.12f, 0.43f, 0.5f, 0.75f},
		0.1f},
	[BACKGROUND_BLACK] = {{0, 0, 0}, {0, 0, 0}, {1, 1, 1, 0.14f}, {0.31f, 0.78f, 0.78f, 0.7f}, 0},
};

static const float SHADOW_TINT[4] = {0, 0, 0, 0.42f};
/* Magenta shows up on nearly any model, and the dark rim keeps joints visible on light ones */
static const float SKELETON_COLOUR[4] = {1, 0.42f, 0.84f, 1};
static const float JOINT_RIM[4] = {0.05f, 0.02f, 0.04f, 0.85f};
static const float NO_TINT[4] = {1, 1, 1, 1};

static const char *SURFACE_VERTEX =
	"layout(location = " G_STRINGIFY(ATTRIBUTE_POSITION) ") in vec3 position;\n"
	"layout(location = " G_STRINGIFY(ATTRIBUTE_NORMAL) ") in vec3 normal;\n"
	"layout(location = " G_STRINGIFY(ATTRIBUTE_UV) ") in vec2 uv;\n"
	"layout(location = " G_STRINGIFY(ATTRIBUTE_COLOUR) ") in vec4 colour;\n"
	"layout(location = " G_STRINGIFY(ATTRIBUTE_JOINTS) ") in uvec4 joint_ids;\n"
	"layout(location = " G_STRINGIFY(ATTRIBUTE_WEIGHTS) ") in vec4 weights;\n"
	"uniform mat4 model, view, projection;\n"
	"uniform mat3 normal_matrix;\n"
	"uniform bool is_skinned;\n"
	"uniform int joint_base;\n"
	"uniform sampler2D joints;\n"
	"out vec3 world_position, world_normal;\n"
	"out vec2 texture_uv;\n"
	"out vec4 vertex_colour;\n"
	"mat4 get_joint(uint id) {\n"
	"	int row = joint_base + int(id);\n"
	"	return mat4(texelFetch(joints, ivec2(0, row), 0), texelFetch(joints, ivec2(1, row), 0),\n"
	"		texelFetch(joints, ivec2(2, row), 0), texelFetch(joints, ivec2(3, row), 0));\n"
	"}\n"
	"void main() {\n"
	"	mat4 place = model;\n"
	"	mat3 turn = normal_matrix;\n"
	"	float total = weights.x + weights.y + weights.z + weights.w;\n"
	"	if (is_skinned && total > 0.0) {\n"
	"		place = (get_joint(joint_ids.x) * weights.x + get_joint(joint_ids.y) * weights.y\n"
	"			+ get_joint(joint_ids.z) * weights.z + get_joint(joint_ids.w) * weights.w) / total;\n"
	"		turn = transpose(inverse(mat3(place)));\n"
	"	}\n"
	"	vec4 placed = place * vec4(position, 1.0);\n"
	"	world_position = placed.xyz;\n"
	"	world_normal = turn * normal;\n"
	"	texture_uv = uv;\n"
	"	vertex_colour = colour;\n"
	"	gl_Position = projection * view * placed;\n"
	"}\n";

/* Colours are lit in linear light with a soft highlight, then tone mapped with the ACES fit and turned back to sRGB */
static const char *SURFACE_FRAGMENT =
	"const int LIGHT_COUNT = " G_STRINGIFY(LIGHT_COUNT) ";\n"
	"const float GAMMA = 2.2;\n"
	"const float CUTOUT_ALPHA = 0.1;\n"
	"const float SHININESS = 48.0;\n"
	"const float SHINE_STRENGTH = 0.18;\n"
	"const float ACES_A = 2.51, ACES_B = 0.03, ACES_C = 2.43, ACES_D = 0.59, ACES_E = 0.14;\n"
	"in vec3 world_position, world_normal;\n"
	"in vec2 texture_uv;\n"
	"in vec4 vertex_colour;\n"
	"uniform vec4 base_colour, wire_colour;\n"
	"uniform sampler2D base_texture;\n"
	"uniform bool has_texture, is_lit, is_wire;\n"
	"uniform vec3 eye, sky, ground;\n"
	"uniform vec3 light_directions[LIGHT_COUNT], light_colours[LIGHT_COUNT];\n"
	"out vec4 result;\n"
	"void main() {\n"
	"	if (is_wire) {\n"
	"		result = wire_colour;\n"
	"		return;\n"
	"	}\n"
	"	vec4 colour = base_colour * vec4(pow(vertex_colour.rgb, vec3(GAMMA)), vertex_colour.a);\n"
	"	if (has_texture)\n"
	"		colour *= texture(base_texture, texture_uv);\n"
	"	if (colour.a < CUTOUT_ALPHA)\n"
	"		discard;\n"
	"	vec3 shade = colour.rgb;\n"
	"	if (is_lit) {\n"
	"		vec3 facing = normalize(world_normal);\n"
	"		if (!gl_FrontFacing)\n"
	"			facing = -facing;\n"
	"		vec3 toward_eye = normalize(eye - world_position);\n"
	"		vec3 light = mix(ground, sky, facing.y * 0.5 + 0.5);\n"
	"		vec3 shine = vec3(0.0);\n"
	"		for (int index = 0; index < LIGHT_COUNT; index++) {\n"
	"			vec3 toward_light = -normalize(light_directions[index] + vec3(1e-6));\n"
	"			float lit = max(dot(facing, toward_light), 0.0);\n"
	"			light += light_colours[index] * lit;\n"
	"			vec3 halfway = normalize(toward_light + toward_eye);\n"
	"			shine += light_colours[index] * pow(max(dot(facing, halfway), 0.0), SHININESS) * SHINE_STRENGTH\n"
	"				* step(0.0, lit);\n"
	"		}\n"
	"		shade = shade * light + shine;\n"
	"		shade = shade * (ACES_A * shade + ACES_B) / (shade * (ACES_C * shade + ACES_D) + ACES_E);\n"
	"	}\n"
	"	result = vec4(pow(clamp(shade, 0.0, 1.0), vec3(1.0 / GAMMA)), 1.0);\n"
	"}\n";

static const char *LINE_VERTEX =
	"layout(location = " G_STRINGIFY(ATTRIBUTE_POSITION) ") in vec3 position;\n"
	"layout(location = " G_STRINGIFY(ATTRIBUTE_COLOUR) ") in vec4 colour;\n"
	"uniform mat4 view_projection;\n"
	"uniform float point_size;\n"
	"out vec4 line_colour;\n"
	"out vec3 line_place;\n"
	"void main() {\n"
	"	line_colour = colour;\n"
	"	line_place = position;\n"
	"	gl_PointSize = point_size;\n"
	"	gl_Position = view_projection * vec4(position, 1.0);\n"
	"}\n";

/* Shapes on the ground, the grid and the shadow, fade out across the ground from a centre, over an ellipse with the
 * given radii, and other lines draw in full */
static const char *LINE_FRAGMENT =
	"in vec4 line_colour;\n"
	"in vec3 line_place;\n"
	"uniform vec4 tint;\n"
	"uniform bool is_point;\n"
	"uniform vec3 fade_centre;\n"
	"uniform vec2 fade_radii;\n"
	"uniform float fade_start;\n"
	"out vec4 result;\n"
	"void main() {\n"
	"	if (is_point && length(gl_PointCoord - vec2(0.5)) > 0.5)\n"
	"		discard;\n"
	"	float strength = 1.0;\n"
	"	if (fade_radii.x > 0.0)\n"
	"		strength -= smoothstep(fade_start, 1.0, length((line_place.xz - fade_centre.xz) / fade_radii));\n"
	"	result = line_colour * tint * vec4(1.0, 1.0, 1.0, strength);\n"
	"}\n";

/* One triangle larger than the screen, with place running from 0 to 1 across the screen, for the background and the
 * copy */
static const char *SCREEN_VERTEX =
	"out vec2 place;\n"
	"void main() {\n"
	"	vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	"	place = corner;\n"
	"	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* Copies the resolved frame into the area's own buffer, whatever format that buffer has, and always opaque, since
 * blended lines lower the frame's alpha and the window's colour would show through the lines */
static const char *COPY_FRAGMENT =
	"in vec2 place;\n"
	"uniform sampler2D image;\n"
	"out vec4 result;\n"
	"void main() {\n"
	"	result = vec4(texture(image, place).rgb, 1.0);\n"
	"}\n";

/* The colour runs from the bottom colour halfway to the top colour, so the top edge meets a dark title bar softly,
 * and the vignette darkens the sides more than the top and bottom */
static const char *BACKGROUND_FRAGMENT =
	"const vec2 VIGNETTE_SHAPE = vec2(2.0, 1.0);\n"
	"in vec2 place;\n"
	"uniform vec3 top, bottom;\n"
	"uniform float vignette;\n"
	"out vec4 result;\n"
	"void main() {\n"
	"	vec3 colour = mix(bottom, top, clamp(place.y * 0.5, 0.0, 1.0));\n"
	"	vec2 from_centre = (place - vec2(0.5)) * VIGNETTE_SHAPE;\n"
	"	colour *= 1.0 - vignette * dot(from_centre, from_centre);\n"
	"	result = vec4(colour, 1.0);\n"
	"}\n";


/* The same shaders run on desktop OpenGL and on OpenGL ES, which only differ in this first line */
static const char *get_shader_header(gboolean is_es) {
	return is_es ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
		: "#version 330 core\n";
}


static GLuint compile_stage(GLenum stage, const char *source, gboolean is_es, GError **error) {
	GLuint shader = glCreateShader(stage);
	const char *parts[] = {get_shader_header(is_es), source};
	glShaderSource(shader, G_N_ELEMENTS(parts), parts, NULL);
	glCompileShader(shader);
	GLint is_compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);
	if (is_compiled)
		return shader;
	char log[SHADER_LOG_BYTES] = "";
	glGetShaderInfoLog(shader, sizeof(log), NULL, log);
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "A shader did not compile: %s", log);
	glDeleteShader(shader);
	return 0;
}


static GLuint link_program(const char *vertex_source, const char *fragment_source, gboolean is_es, GError **error) {
	GLuint vertex = compile_stage(GL_VERTEX_SHADER, vertex_source, is_es, error);
	if (!vertex)
		return 0;
	GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source, is_es, error);
	if (!fragment) {
		glDeleteShader(vertex);
		return 0;
	}
	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint is_linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &is_linked);
	if (is_linked)
		return program;
	char log[SHADER_LOG_BYTES] = "";
	glGetProgramInfoLog(program, sizeof(log), NULL, log);
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "A shader did not link: %s", log);
	glDeleteProgram(program);
	return 0;
}


static gboolean create_shaders(Renderer *renderer, GError **error) {
	SurfaceShader *surface = &renderer->surface;
	gboolean is_es = renderer->is_es;
	surface->program = link_program(SURFACE_VERTEX, SURFACE_FRAGMENT, is_es, error);
	renderer->lines.program = surface->program ? link_program(LINE_VERTEX, LINE_FRAGMENT, is_es, error) : 0;
	renderer->background.program = renderer->lines.program ? link_program(SCREEN_VERTEX, BACKGROUND_FRAGMENT,
		is_es, error) : 0;
	renderer->copy.program = renderer->background.program ? link_program(SCREEN_VERTEX, COPY_FRAGMENT, is_es, error)
		: 0;
	if (!renderer->copy.program)
		return FALSE;
	GLuint program = surface->program;
	surface->model = glGetUniformLocation(program, "model");
	surface->view = glGetUniformLocation(program, "view");
	surface->projection = glGetUniformLocation(program, "projection");
	surface->normal_matrix = glGetUniformLocation(program, "normal_matrix");
	surface->is_skinned = glGetUniformLocation(program, "is_skinned");
	surface->joint_base = glGetUniformLocation(program, "joint_base");
	surface->joints = glGetUniformLocation(program, "joints");
	surface->base_colour = glGetUniformLocation(program, "base_colour");
	surface->base_texture = glGetUniformLocation(program, "base_texture");
	surface->has_texture = glGetUniformLocation(program, "has_texture");
	surface->eye = glGetUniformLocation(program, "eye");
	surface->light_directions = glGetUniformLocation(program, "light_directions");
	surface->light_colours = glGetUniformLocation(program, "light_colours");
	surface->sky = glGetUniformLocation(program, "sky");
	surface->ground = glGetUniformLocation(program, "ground");
	surface->is_lit = glGetUniformLocation(program, "is_lit");
	surface->is_wire = glGetUniformLocation(program, "is_wire");
	surface->wire_colour = glGetUniformLocation(program, "wire_colour");
	renderer->lines.view_projection = glGetUniformLocation(renderer->lines.program, "view_projection");
	renderer->lines.point_size = glGetUniformLocation(renderer->lines.program, "point_size");
	renderer->lines.tint = glGetUniformLocation(renderer->lines.program, "tint");
	renderer->lines.is_point = glGetUniformLocation(renderer->lines.program, "is_point");
	renderer->lines.fade_centre = glGetUniformLocation(renderer->lines.program, "fade_centre");
	renderer->lines.fade_radii = glGetUniformLocation(renderer->lines.program, "fade_radii");
	renderer->lines.fade_start = glGetUniformLocation(renderer->lines.program, "fade_start");
	renderer->background.top = glGetUniformLocation(renderer->background.program, "top");
	renderer->background.bottom = glGetUniformLocation(renderer->background.program, "bottom");
	renderer->background.vignette = glGetUniformLocation(renderer->background.program, "vignette");
	renderer->copy.image = glGetUniformLocation(renderer->copy.program, "image");
	return TRUE;
}


static void describe_line_vertices(GLuint array, GLuint buffer) {
	glBindVertexArray(array);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glEnableVertexAttribArray(ATTRIBUTE_POSITION);
	glVertexAttribPointer(ATTRIBUTE_POSITION, POINT_COMPONENTS, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
		(void *)offsetof(LineVertex, position));
	glEnableVertexAttribArray(ATTRIBUTE_COLOUR);
	glVertexAttribPointer(ATTRIBUTE_COLOUR, COLOUR_CHANNELS, GL_FLOAT, GL_FALSE, sizeof(LineVertex),
		(void *)offsetof(LineVertex, colour));
	glBindVertexArray(0);
}


Renderer *create_renderer(gboolean is_es, GError **error) {
	Renderer *renderer = g_new0(Renderer, 1);
	renderer->is_es = is_es;
	if (!create_shaders(renderer, error)) {
		free_renderer(renderer);
		return NULL;
	}
	glGenVertexArrays(1, &renderer->empty_array);
	glGenVertexArrays(1, &renderer->grid_array);
	glGenBuffers(1, &renderer->grid_buffer);
	describe_line_vertices(renderer->grid_array, renderer->grid_buffer);
	glGenVertexArrays(1, &renderer->skeleton_array);
	glGenBuffers(1, &renderer->skeleton_buffer);
	describe_line_vertices(renderer->skeleton_array, renderer->skeleton_buffer);
	/* Joint matrices are rows of 4 float pixels, which OpenGL and OpenGL ES 3 both fetch unfiltered */
	glGenTextures(1, &renderer->joint_texture);
	glBindTexture(GL_TEXTURE_2D, renderer->joint_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic"))
		renderer->anisotropy = ANISOTROPY;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &renderer->maximum_texture_size);
	return renderer;
}


static void release_model(Renderer *renderer) {
	for (guint index = 0; renderer->model && index < renderer->model->meshes->len; index++) {
		MeshBuffers *buffers = &renderer->meshes[index];
		if (!buffers->is_uploaded)
			continue;
		glDeleteVertexArrays(1, &buffers->array);
		glDeleteVertexArrays(1, &buffers->edge_array);
		glDeleteBuffers(1, &buffers->vertices);
		glDeleteBuffers(1, &buffers->indices);
		glDeleteBuffers(1, &buffers->edges);
	}
	if (renderer->uploaded_textures)
		glDeleteTextures((GLsizei)renderer->uploaded_textures, renderer->textures);
	g_clear_pointer(&renderer->meshes, g_free);
	g_clear_pointer(&renderer->textures, g_free);
	renderer->uploaded_meshes = renderer->uploaded_textures = 0;
	renderer->model = NULL;
}


static void release_frame_buffer(Renderer *renderer) {
	if (!renderer->frame_buffer)
		return;
	glDeleteFramebuffers(1, &renderer->frame_buffer);
	glDeleteRenderbuffers(1, &renderer->colour_buffer);
	glDeleteRenderbuffers(1, &renderer->depth_buffer);
	glDeleteFramebuffers(1, &renderer->resolve_buffer);
	glDeleteTextures(1, &renderer->resolve_texture);
	renderer->frame_buffer = renderer->colour_buffer = renderer->depth_buffer = 0;
	renderer->resolve_buffer = renderer->resolve_texture = 0;
}


void free_renderer(Renderer *renderer) {
	release_model(renderer);
	release_frame_buffer(renderer);
	glDeleteProgram(renderer->surface.program);
	glDeleteProgram(renderer->lines.program);
	glDeleteProgram(renderer->background.program);
	glDeleteProgram(renderer->copy.program);
	glDeleteVertexArrays(1, &renderer->empty_array);
	glDeleteVertexArrays(1, &renderer->grid_array);
	glDeleteBuffers(1, &renderer->grid_buffer);
	glDeleteVertexArrays(1, &renderer->skeleton_array);
	glDeleteBuffers(1, &renderer->skeleton_buffer);
	glDeleteTextures(1, &renderer->joint_texture);
	g_free(renderer);
}


/* Two triangles under the model, wide enough for the shadow's fade to reach nothing at their edges */
static void add_shadow(Renderer *renderer, GArray *vertices, Vector3 centre) {
	float x = renderer->shadow_radius_x, z = renderer->shadow_radius_z;
	LineVertex corners[SHADOW_CORNERS] = {
		{{centre.x - x, centre.y, centre.z - z}, {1, 1, 1, 1}}, {{centre.x + x, centre.y, centre.z - z}, {1, 1, 1, 1}},
		{{centre.x + x, centre.y, centre.z + z}, {1, 1, 1, 1}}, {{centre.x - x, centre.y, centre.z - z}, {1, 1, 1, 1}},
		{{centre.x + x, centre.y, centre.z + z}, {1, 1, 1, 1}}, {{centre.x - x, centre.y, centre.z + z}, {1, 1, 1, 1}},
	};
	g_array_append_vals(vertices, corners, SHADOW_CORNERS);
}


/* Twenty lines each way, with stronger lines at each 5th, sized to the power of ten above the model's largest side, so
 * a tall model stands on as wide a grid as a long one, then the shadow */
static void build_ground(Renderer *renderer, const Model *model) {
	Vector3 size = subtract_vectors(model->maximum, model->minimum);
	float largest = fmaxf(fmaxf(fmaxf(size.x, size.y), size.z), SMALLEST_SIZE);
	float extent = powf(GRID_SCALE_BASE, ceilf(log10f(largest)));
	float step = extent * 2 / GRID_LINES;
	float centre_x = roundf((model->minimum.x + model->maximum.x) / 2 / step) * step;
	float centre_z = roundf((model->minimum.z + model->maximum.z) / 2 / step) * step;
	float ground = model->minimum.y;
	Vector3 middle = scale_vector(add_vectors(model->minimum, model->maximum), 0.5f);
	renderer->ground_centre = make_vector(middle.x, ground, middle.z);
	float reach = get_vector_length(size) * GRID_FADE_MODEL_SIZES;
	renderer->grid_fade_radius = fminf(fmaxf(reach, step * GRID_FADE_LEAST_STEPS), extent);
	renderer->shadow_radius_x = fmaxf(size.x / 2 * SHADOW_SPREAD, SMALLEST_SIZE);
	renderer->shadow_radius_z = fmaxf(size.z / 2 * SHADOW_SPREAD, SMALLEST_SIZE);
	GArray *vertices = g_array_new(FALSE, FALSE, sizeof(LineVertex));
	for (int line = -GRID_LINES / 2; line <= GRID_LINES / 2; line++) {
		float strength = line % MAJOR_GRID_EVERY == 0 ? 1.0f : MINOR_GRID_STRENGTH;
		float offset = (float)line * step;
		LineVertex ends[GRID_LINE_ENDS] = {
			{{centre_x + offset, ground, centre_z - extent}, {1, 1, 1, strength}},
			{{centre_x + offset, ground, centre_z + extent}, {1, 1, 1, strength}},
			{{centre_x - extent, ground, centre_z + offset}, {1, 1, 1, strength}},
			{{centre_x + extent, ground, centre_z + offset}, {1, 1, 1, strength}},
		};
		g_array_append_vals(vertices, ends, GRID_LINE_ENDS);
	}
	renderer->grid_count = (int)vertices->len;
	add_shadow(renderer, vertices, renderer->ground_centre);
	glBindBuffer(GL_ARRAY_BUFFER, renderer->grid_buffer);
	glBufferData(GL_ARRAY_BUFFER, vertices->len * sizeof(LineVertex), vertices->data, GL_STATIC_DRAW);
	g_array_unref(vertices);
}


void set_renderer_model(Renderer *renderer, const Model *model) {
	release_model(renderer);
	renderer->model = model;
	renderer->grid_count = 0;
	if (!model)
		return;
	renderer->meshes = g_new0(MeshBuffers, model->meshes->len);
	renderer->textures = g_new0(GLuint, MAX(model->textures->len, 1));
	build_ground(renderer, model);
}


static void describe_mesh_vertices(GLuint vertices) {
	glBindBuffer(GL_ARRAY_BUFFER, vertices);
	GLsizei stride = sizeof(ModelVertex);
	glEnableVertexAttribArray(ATTRIBUTE_POSITION);
	glVertexAttribPointer(ATTRIBUTE_POSITION, POINT_COMPONENTS, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(ModelVertex, position));
	glEnableVertexAttribArray(ATTRIBUTE_NORMAL);
	glVertexAttribPointer(ATTRIBUTE_NORMAL, POINT_COMPONENTS, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(ModelVertex, normal));
	glEnableVertexAttribArray(ATTRIBUTE_UV);
	glVertexAttribPointer(ATTRIBUTE_UV, TEXTURE_COORDINATES, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(ModelVertex, uv));
	glEnableVertexAttribArray(ATTRIBUTE_COLOUR);
	glVertexAttribPointer(ATTRIBUTE_COLOUR, COLOUR_CHANNELS, GL_FLOAT, GL_FALSE, stride, (void *)offsetof(ModelVertex, colour));
	glEnableVertexAttribArray(ATTRIBUTE_JOINTS);
	glVertexAttribIPointer(ATTRIBUTE_JOINTS, JOINTS_PER_VERTEX, GL_UNSIGNED_SHORT, stride,
		(void *)offsetof(ModelVertex, joints));
	glEnableVertexAttribArray(ATTRIBUTE_WEIGHTS);
	glVertexAttribPointer(ATTRIBUTE_WEIGHTS, JOINTS_PER_VERTEX, GL_FLOAT, GL_FALSE, stride,
		(void *)offsetof(ModelVertex, weights));
}


/* The triangles and the wireframe edges share one vertex buffer, each with its own index list */
static void upload_mesh(MeshBuffers *buffers, const ModelMesh *mesh) {
	glGenVertexArrays(1, &buffers->array);
	glGenVertexArrays(1, &buffers->edge_array);
	glGenBuffers(1, &buffers->vertices);
	glGenBuffers(1, &buffers->indices);
	glGenBuffers(1, &buffers->edges);
	glBindBuffer(GL_ARRAY_BUFFER, buffers->vertices);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(mesh->vertex_count * sizeof(ModelVertex)), mesh->vertices,
		GL_STATIC_DRAW);
	glBindVertexArray(buffers->array);
	describe_mesh_vertices(buffers->vertices);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers->indices);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(mesh->index_count * sizeof(uint32_t)), mesh->indices,
		GL_STATIC_DRAW);
	glBindVertexArray(buffers->edge_array);
	describe_mesh_vertices(buffers->vertices);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers->edges);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(mesh->edge_count * sizeof(uint32_t)), mesh->edges,
		GL_STATIC_DRAW);
	glBindVertexArray(0);
	buffers->is_uploaded = TRUE;
}


/* Textures hold colours, so the card reads them as sRGB and the shader lights them in linear light, while one
 * larger than the card takes shows as plain white rather than failing without a word */
static GLuint upload_texture(const Renderer *renderer, const ModelTexture *texture) {
	static const guint8 WHITE[] = {G_MAXUINT8, G_MAXUINT8, G_MAXUINT8, G_MAXUINT8};
	gboolean fits = texture->width <= renderer->maximum_texture_size && texture->height <= renderer->maximum_texture_size;
	if (!fits)
		g_message("A %dx%d texture is larger than the graphics card takes", texture->width, texture->height);
	GLuint name;
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, fits ? texture->width : 1, fits ? texture->height : 1, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, fits ? g_bytes_get_data(texture->pixels, NULL) : WHITE);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	if (renderer->anisotropy > 0)
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, renderer->anisotropy);
	return name;
}


/* Meshes go first so the geometry appears at once, and the textures follow to paint it */
gboolean upload_renderer_step(Renderer *renderer) {
	const Model *model = renderer->model;
	if (!model)
		return FALSE;
	gint64 deadline = g_get_monotonic_time() + UPLOAD_BUDGET_MICROSECONDS;
	while (renderer->uploaded_meshes < model->meshes->len && g_get_monotonic_time() < deadline) {
		guint index = renderer->uploaded_meshes++;
		upload_mesh(&renderer->meshes[index], &g_array_index(model->meshes, ModelMesh, index));
	}
	while (renderer->uploaded_meshes == model->meshes->len && renderer->uploaded_textures < model->textures->len
		&& g_get_monotonic_time() < deadline) {
		guint index = renderer->uploaded_textures;
		renderer->textures[index] = upload_texture(renderer, &g_array_index(model->textures, ModelTexture, index));
		renderer->uploaded_textures++;
	}
	return renderer->uploaded_meshes < model->meshes->len || renderer->uploaded_textures < model->textures->len;
}


/* The frame is drawn with several samples a pixel for smooth edges */
static void prepare_frame_buffer(Renderer *renderer, int width, int height) {
	if (renderer->frame_buffer && renderer->frame_width == width && renderer->frame_height == height)
		return;
	release_frame_buffer(renderer);
	glGenFramebuffers(1, &renderer->frame_buffer);
	glGenRenderbuffers(1, &renderer->colour_buffer);
	glGenRenderbuffers(1, &renderer->depth_buffer);
	glBindRenderbuffer(GL_RENDERBUFFER, renderer->colour_buffer);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, SAMPLES, GL_RGBA8, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, renderer->depth_buffer);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, SAMPLES, GL_DEPTH_COMPONENT24, width, height);
	glBindFramebuffer(GL_FRAMEBUFFER, renderer->frame_buffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderer->colour_buffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderer->depth_buffer);
	glGenTextures(1, &renderer->resolve_texture);
	glBindTexture(GL_TEXTURE_2D, renderer->resolve_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &renderer->resolve_buffer);
	glBindFramebuffer(GL_FRAMEBUFFER, renderer->resolve_buffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderer->resolve_texture, 0);
	renderer->frame_width = width;
	renderer->frame_height = height;
}


static void draw_background(Renderer *renderer, const BackgroundPreset *preset) {
	glDisable(GL_DEPTH_TEST);
	glUseProgram(renderer->background.program);
	glUniform3fv(renderer->background.top, 1, preset->top);
	glUniform3fv(renderer->background.bottom, 1, preset->bottom);
	glUniform1f(renderer->background.vignette, preset->vignette);
	glBindVertexArray(renderer->empty_array);
	glDrawArrays(GL_TRIANGLES, 0, TRIANGLE_CORNERS);
}


static void set_lighting(const Renderer *renderer, const RenderFrame *frame) {
	const SurfaceShader *surface = &renderer->surface;
	const LightingPreset *preset = &LIGHTING_PRESETS[frame->options->lighting];
	float directions[LIGHT_COUNT][POINT_COMPONENTS];
	memcpy(directions, preset->directions, sizeof(directions));
	if (preset->is_view_relative) {
		directions[0][0] = frame->forward.x;
		directions[0][1] = frame->forward.y;
		directions[0][2] = frame->forward.z;
	}
	glUniform3fv(surface->light_directions, LIGHT_COUNT, &directions[0][0]);
	glUniform3fv(surface->light_colours, LIGHT_COUNT, &preset->colours[0][0]);
	glUniform3fv(surface->sky, 1, preset->sky);
	glUniform3fv(surface->ground, 1, preset->ground);
	glUniform1i(surface->is_lit, preset->is_lit);
	glUniform3f(surface->eye, frame->eye.x, frame->eye.y, frame->eye.z);
}


/* Each joint is one row, so the texture is made once for the model and only its values change each frame */
static void upload_joints(Renderer *renderer, const RenderFrame *frame) {
	guint count = renderer->model->joints->len;
	Matrix4 identity = get_identity_matrix();
	GLint rows = (GLint)MAX(count, 1);
	const float *values = count ? frame->joints->values : identity.values;
	glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT_JOINTS);
	glBindTexture(GL_TEXTURE_2D, renderer->joint_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, sizeof(float));
	if (rows > renderer->maximum_texture_size)
		return;
	if (rows != renderer->joint_rows) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, JOINT_TEXELS, rows, 0, GL_RGBA, GL_FLOAT, values);
		renderer->joint_rows = rows;
		return;
	}
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, JOINT_TEXELS, rows, GL_RGBA, GL_FLOAT, values);
}


/* A rig with more joints than the joint texture can hold is drawn in its resting pose */
static gboolean can_skin(const Renderer *renderer) {
	return (GLint)renderer->model->joints->len <= renderer->maximum_texture_size;
}


static void draw_mesh(const Renderer *renderer, const RenderFrame *frame, guint mesh_index, const Matrix4 *place,
	gboolean is_wire) {
	const SurfaceShader *surface = &renderer->surface;
	const ModelMesh *mesh = &g_array_index(renderer->model->meshes, ModelMesh, mesh_index);
	if (mesh_index >= renderer->uploaded_meshes || mesh->index_count == 0)
		return;
	const ModelMaterial *material = &g_array_index(renderer->model->materials, ModelMaterial, mesh->material);
	gboolean has_texture = frame->options->shows_textures && material->texture >= 0
		&& (guint)material->texture < renderer->uploaded_textures;
	Matrix4 identity = get_identity_matrix();
	gboolean is_skinned = mesh->is_skinned && can_skin(renderer);
	const Matrix4 *model = is_skinned ? &identity : place;
	float normal[NORMAL_MATRIX_VALUES];
	get_normal_matrix(model, normal);
	glUniformMatrix4fv(surface->model, 1, GL_FALSE, model->values);
	glUniformMatrix3fv(surface->normal_matrix, 1, GL_FALSE, normal);
	glUniform1i(surface->is_skinned, is_skinned);
	glUniform1i(surface->joint_base, (GLint)mesh->joint_base);
	glUniform4fv(surface->base_colour, 1, material->colour);
	glUniform1i(surface->has_texture, has_texture);
	glUniform1i(surface->is_wire, is_wire);
	if (has_texture) {
		glActiveTexture(GL_TEXTURE0 + TEXTURE_UNIT_BASE);
		glBindTexture(GL_TEXTURE_2D, renderer->textures[material->texture]);
	}
	if (is_wire) {
		glBindVertexArray(renderer->meshes[mesh_index].edge_array);
		glDrawElements(GL_LINES, (GLsizei)mesh->edge_count, GL_UNSIGNED_INT, NULL);
	} else {
		glBindVertexArray(renderer->meshes[mesh_index].array);
		glDrawElements(GL_TRIANGLES, (GLsizei)mesh->index_count, GL_UNSIGNED_INT, NULL);
	}
}


static void draw_model(Renderer *renderer, const RenderFrame *frame, gboolean is_wire) {
	const Model *model = renderer->model;
	for (guint index = 0; index < model->nodes->len; index++) {
		const ModelNode *node = &g_array_index(model->nodes, ModelNode, index);
		for (guint mesh = 0; mesh < node->mesh_count; mesh++)
			if (node->meshes[mesh] < model->meshes->len)
				draw_mesh(renderer, frame, node->meshes[mesh], &frame->globals[index], is_wire);
	}
}


static void draw_surfaces(Renderer *renderer, const RenderFrame *frame) {
	const SurfaceShader *surface = &renderer->surface;
	glUseProgram(surface->program);
	glUniformMatrix4fv(surface->view, 1, GL_FALSE, frame->view.values);
	glUniformMatrix4fv(surface->projection, 1, GL_FALSE, frame->projection.values);
	glUniform1i(surface->base_texture, TEXTURE_UNIT_BASE);
	glUniform1i(surface->joints, TEXTURE_UNIT_JOINTS);
	set_lighting(renderer, frame);
	upload_joints(renderer, frame);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	/* The filled surface is drawn a little behind its own wireframe, so the lines show in full */
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(1, 1);
	draw_model(renderer, frame, FALSE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	if (!frame->options->shows_wireframe)
		return;
	glUniform4fv(surface->wire_colour, 1, BACKGROUND_PRESETS[frame->options->background].wire);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	draw_model(renderer, frame, TRUE);
	glDepthMask(GL_TRUE);
}


static void use_line_shader(const Renderer *renderer, const RenderFrame *frame, float point_size, const float *tint) {
	Matrix4 view_projection = multiply_matrices(&frame->projection, &frame->view);
	glUseProgram(renderer->lines.program);
	glUniformMatrix4fv(renderer->lines.view_projection, 1, GL_FALSE, view_projection.values);
	glUniform1f(renderer->lines.point_size, point_size);
	glUniform4fv(renderer->lines.tint, 1, tint);
	glUniform1i(renderer->lines.is_point, FALSE);
	glUniform2f(renderer->lines.fade_radii, 0, 0);
}


static void fade_on_ground(const Renderer *renderer, float radius_x, float radius_z, float start) {
	Vector3 centre = renderer->ground_centre;
	glUniform3f(renderer->lines.fade_centre, centre.x, centre.y, centre.z);
	glUniform2f(renderer->lines.fade_radii, radius_x, radius_z);
	glUniform1f(renderer->lines.fade_start, start);
}


/* The shadow lies on the ground with the grid, under the model and seen only from above */
static void draw_shadow(Renderer *renderer, const RenderFrame *frame) {
	gboolean is_wanted = frame->options->shows_grid && renderer->model->triangle_count > 0;
	if (!is_wanted || renderer->grid_count == 0 || frame->eye.y <= renderer->ground_centre.y)
		return;
	use_line_shader(renderer, frame, 1, SHADOW_TINT);
	fade_on_ground(renderer, renderer->shadow_radius_x, renderer->shadow_radius_z, 0);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(renderer->grid_array);
	glDrawArrays(GL_TRIANGLES, renderer->grid_count, SHADOW_CORNERS);
	glDepthMask(GL_TRUE);
}


static void draw_grid(Renderer *renderer, const RenderFrame *frame) {
	if (!frame->options->shows_grid || renderer->grid_count == 0)
		return;
	use_line_shader(renderer, frame, 1, BACKGROUND_PRESETS[frame->options->background].grid);
	fade_on_ground(renderer, renderer->grid_fade_radius, renderer->grid_fade_radius, GRID_FADE_START);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(renderer->grid_array);
	glDrawArrays(GL_LINES, 0, renderer->grid_count);
	glDepthMask(GL_TRUE);
}


static gboolean is_bone(const Model *model, int node) {
	return g_array_index(model->nodes, ModelNode, node).is_bone;
}


/* Bones run from each bone to the nearest bone above it, drawn over the model so they always show,
 * and a file with no triangles always shows them since it has no geometry */
static void draw_skeleton(Renderer *renderer, const RenderFrame *frame) {
	const Model *model = renderer->model;
	gboolean is_wanted = frame->options->shows_skeleton || model->triangle_count == 0;
	if (!is_wanted || !model->has_skeleton)
		return;
	GArray *bones = g_array_new(FALSE, FALSE, sizeof(LineVertex));
	GArray *points = g_array_new(FALSE, FALSE, sizeof(LineVertex));
	for (guint index = 0; index < model->nodes->len; index++) {
		if (!is_bone(model, (int)index))
			continue;
		Vector3 here = get_matrix_position(&frame->globals[index]);
		LineVertex point = {{here.x, here.y, here.z}, {SKELETON_COLOUR[0], SKELETON_COLOUR[1], SKELETON_COLOUR[2], 1}};
		g_array_append_val(points, point);
		int parent = g_array_index(model->nodes, ModelNode, index).parent;
		while (parent >= 0 && !is_bone(model, parent))
			parent = g_array_index(model->nodes, ModelNode, parent).parent;
		if (parent < 0)
			continue;
		Vector3 above = get_matrix_position(&frame->globals[parent]);
		LineVertex top = {{above.x, above.y, above.z}, {SKELETON_COLOUR[0], SKELETON_COLOUR[1], SKELETON_COLOUR[2], 1}};
		g_array_append_val(bones, top);
		g_array_append_val(bones, point);
	}
	use_line_shader(renderer, frame, JOINT_POINT_SIZE, NO_TINT);
	glDisable(GL_DEPTH_TEST);
	/* OpenGL ES always takes the point size from the shader, desktop OpenGL only once enabled */
	if (!renderer->is_es)
		glEnable(GL_PROGRAM_POINT_SIZE);
	glBindVertexArray(renderer->skeleton_array);
	glBindBuffer(GL_ARRAY_BUFFER, renderer->skeleton_buffer);
	glBufferData(GL_ARRAY_BUFFER, bones->len * (GLsizeiptr)sizeof(LineVertex), bones->data, GL_STREAM_DRAW);
	glDrawArrays(GL_LINES, 0, (GLsizei)bones->len);
	glBufferData(GL_ARRAY_BUFFER, points->len * (GLsizeiptr)sizeof(LineVertex), points->data, GL_STREAM_DRAW);
	glUniform1i(renderer->lines.is_point, TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUniform1f(renderer->lines.point_size, JOINT_RIM_SIZE);
	glUniform4fv(renderer->lines.tint, 1, JOINT_RIM);
	glDrawArrays(GL_POINTS, 0, (GLsizei)points->len);
	glUniform1f(renderer->lines.point_size, JOINT_POINT_SIZE);
	glUniform4fv(renderer->lines.tint, 1, NO_TINT);
	glDrawArrays(GL_POINTS, 0, (GLsizei)points->len);
	if (!renderer->is_es)
		glDisable(GL_PROGRAM_POINT_SIZE);
	g_array_unref(bones);
	g_array_unref(points);
}


void draw_renderer(Renderer *renderer, const RenderFrame *frame) {
	GLint target = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
	prepare_frame_buffer(renderer, frame->width, frame->height);
	glBindFramebuffer(GL_FRAMEBUFFER, renderer->frame_buffer);
	glViewport(0, 0, frame->width, frame->height);
	glClear(GL_DEPTH_BUFFER_BIT);
	draw_background(renderer, &BACKGROUND_PRESETS[frame->options->background]);
	if (renderer->model) {
		draw_shadow(renderer, frame);
		draw_surfaces(renderer, frame);
		draw_grid(renderer, frame);
		draw_skeleton(renderer, frame);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, renderer->frame_buffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, renderer->resolve_buffer);
	glBlitFramebuffer(0, 0, frame->width, frame->height, 0, 0, frame->width, frame->height, GL_COLOR_BUFFER_BIT,
		GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glUseProgram(renderer->copy.program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, renderer->resolve_texture);
	glUniform1i(renderer->copy.image, 0);
	glBindVertexArray(renderer->empty_array);
	glDrawArrays(GL_TRIANGLES, 0, TRIANGLE_CORNERS);
	glBindVertexArray(0);
}
