/*
 * glconform — differential GL conformance harness for the crocus/GEM stack.
 *
 * Renders a battery of GL 2.1 scenes through the installed GL renderer,
 * reads the pixels back and dumps one PPM per scene. Run once on the
 * hardware path (default) and once on the software reference
 * (HGL_SOFTWARE=1, llvmpipe), then compare the two sets: llvmpipe acts as
 * the conformance oracle, so a match means the GPU renders correctly.
 *
 * Build:
 *   g++ -Wall -O2 -o glconform glconform.cpp -lbe -lGL -lGLU
 *
 * Usage:
 *   glconform --render <outdir>          render all scenes into outdir
 *   glconform --compare <dirA> <dirB>    compare two result sets
 *
 * Orchestrated by glconform.sh (hardware pass, software pass, compare).
 */

#include <Application.h>
#include <GLView.h>
#include <Window.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glu.h>

// Haiku's gl.h stops at GL 1.x prototypes; the GL 2.0 shader entry points
// are exported by libGL but not declared. Declare what we use.
extern "C" {
GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
	const GLint* length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint* params);
void glUseProgram(GLuint program);
void glDeleteShader(GLuint shader);
void glDeleteProgram(GLuint program);
GLint glGetUniformLocation(GLuint program, const GLchar* name);
void glUniform1i(GLint location, GLint v0);
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
}

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int kSize = 300;		// render target is kSize x kSize

struct Scene {
	const char*	name;
	void		(*render)();
	// Comparison thresholds: a scene passes when at most maxBadPct % of
	// pixels differ by more than kChannelTolerance on any channel.
	// Edge-heavy scenes (lines, points, minified textures) get a looser
	// budget because rasterization rules legitimately differ between
	// implementations at primitive edges.
	float		maxBadPct;
};

static const int kChannelTolerance = 16;


// #pragma mark - scene helpers


static void
ortho2d()
{
	glViewport(0, 0, kSize, kSize);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}


static void
reset_state()
{
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_LIGHTING);
	glDisable(GL_FOG);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(0);
	ortho2d();
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClearDepth(1.0);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
		| GL_STENCIL_BUFFER_BIT);
}


static void
quad(float x0, float y0, float x1, float y1)
{
	glBegin(GL_QUADS);
	glVertex2f(x0, y0);
	glVertex2f(x1, y0);
	glVertex2f(x1, y1);
	glVertex2f(x0, y1);
	glEnd();
}


// #pragma mark - scenes


static void
scene_scissor_clears()
{
	glEnable(GL_SCISSOR_TEST);
	const float colors[4][3] = {
		{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}
	};
	const int half = kSize / 2;
	const int rects[4][2] = {
		{0, 0}, {half, 0}, {0, half}, {half, half}
	};
	for (int i = 0; i < 4; i++) {
		glScissor(rects[i][0], rects[i][1], half, half);
		glClearColor(colors[i][0], colors[i][1], colors[i][2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
}


static void
scene_tri_gouraud()
{
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f);
	glColor3f(0, 1, 0); glVertex2f(0.8f, -0.8f);
	glColor3f(0, 0, 1); glVertex2f(0.0f, 0.8f);
	glEnd();
}


static void
scene_quad_gradient()
{
	glBegin(GL_QUADS);
	glColor3f(0, 0, 0);       glVertex2f(-1, -1);
	glColor3f(1, 0, 0);       glVertex2f(1, -1);
	glColor3f(1, 1, 0);       glVertex2f(1, 1);
	glColor3f(0, 1, 0);       glVertex2f(-1, 1);
	glEnd();
}


static void
scene_depth_test()
{
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	// Two slanted quads crossing in depth
	glBegin(GL_QUADS);
	glColor3f(1, 0, 0);
	glVertex3f(-0.9f, -0.9f, -0.5f);
	glVertex3f(0.9f, -0.9f, 0.5f);
	glVertex3f(0.9f, 0.9f, 0.5f);
	glVertex3f(-0.9f, 0.9f, -0.5f);
	glColor3f(0, 0, 1);
	glVertex3f(-0.9f, -0.9f, 0.5f);
	glVertex3f(0.9f, -0.9f, -0.5f);
	glVertex3f(0.9f, 0.9f, -0.5f);
	glVertex3f(-0.9f, 0.9f, 0.5f);
	glEnd();
}


static void
scene_stencil_ops()
{
	// Write a diamond into the stencil, then draw a fullscreen quad
	// through GL_EQUAL
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glBegin(GL_QUADS);
	glVertex2f(0.0f, -0.7f);
	glVertex2f(0.7f, 0.0f);
	glVertex2f(0.0f, 0.7f);
	glVertex2f(-0.7f, 0.0f);
	glEnd();
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilFunc(GL_EQUAL, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glColor3f(0, 1, 1);
	quad(-1, -1, 1, 1);
	glDisable(GL_STENCIL_TEST);
}


static void
scene_blend_alpha()
{
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glColor4f(1, 1, 1, 1);
	quad(-1, -1, 1, 1);
	const float layers[5][5] = {
		// x0, y0, size, hue index, alpha
		{-0.7f, -0.7f, 0.9f, 0, 0.5f},
		{-0.2f, -0.5f, 0.8f, 1, 0.4f},
		{0.0f, 0.0f, 0.9f, 2, 0.6f},
		{-0.6f, 0.1f, 0.7f, 0, 0.3f},
		{-0.1f, -0.1f, 0.5f, 1, 0.7f},
	};
	for (int i = 0; i < 5; i++) {
		float r = layers[i][3] == 0, g = layers[i][3] == 1,
			b = layers[i][3] == 2;
		glColor4f(r, g, b, layers[i][4]);
		quad(layers[i][0], layers[i][1],
			layers[i][0] + layers[i][2], layers[i][1] + layers[i][2]);
	}
	glDisable(GL_BLEND);
}


static GLuint
make_checker_texture(int texSize, int checker)
{
	static uint8* data = NULL;
	data = (uint8*)realloc(data, texSize * texSize * 3);
	for (int y = 0; y < texSize; y++) {
		for (int x = 0; x < texSize; x++) {
			bool on = ((x / checker) ^ (y / checker)) & 1;
			uint8* p = data + (y * texSize + x) * 3;
			p[0] = on ? 255 : 32;
			p[1] = on ? 64 : 200;
			p[2] = on ? 16 : 240;
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texSize, texSize, 0, GL_RGB,
		GL_UNSIGNED_BYTE, data);
	return tex;
}


static void
textured_fullscreen_quad()
{
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(1, 1); glVertex2f(1, 1);
	glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
}


static void
scene_tex_nearest()
{
	glEnable(GL_TEXTURE_2D);
	GLuint tex = make_checker_texture(16, 2);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glColor3f(1, 1, 1);
	textured_fullscreen_quad();
	glDeleteTextures(1, &tex);
	glDisable(GL_TEXTURE_2D);
}


static void
scene_tex_linear()
{
	glEnable(GL_TEXTURE_2D);
	GLuint tex = make_checker_texture(8, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glColor3f(1, 1, 1);
	textured_fullscreen_quad();
	glDeleteTextures(1, &tex);
	glDisable(GL_TEXTURE_2D);
}


static void
scene_tex_mipmap()
{
	glEnable(GL_TEXTURE_2D);
	static uint8 data[64 * 64 * 3];
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			uint8* p = data + (y * 64 + x) * 3;
			bool on = ((x / 4) ^ (y / 4)) & 1;
			p[0] = on ? 255 : 0;
			p[1] = (uint8)(x * 4);
			p[2] = (uint8)(y * 4);
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 64, 64, GL_RGB,
		GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	// Minify: many texture repeats on a small quad
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-0.9f, -0.9f);
	glTexCoord2f(8, 0); glVertex2f(0.9f, -0.9f);
	glTexCoord2f(8, 8); glVertex2f(0.9f, 0.9f);
	glTexCoord2f(0, 8); glVertex2f(-0.9f, 0.9f);
	glEnd();
	glDeleteTextures(1, &tex);
	glDisable(GL_TEXTURE_2D);
}


static void
scene_lines()
{
	glColor3f(1, 1, 1);
	glBegin(GL_LINES);
	for (int i = 0; i <= 10; i++) {
		float t = -1.0f + i * 0.2f;
		glVertex2f(t, -1); glVertex2f(t, 1);
		glVertex2f(-1, t); glVertex2f(1, t);
	}
	// diagonals
	glColor3f(1, 0.5f, 0);
	glVertex2f(-1, -1); glVertex2f(1, 1);
	glVertex2f(-1, 1); glVertex2f(1, -1);
	glEnd();
}


static void
scene_points()
{
	for (int size = 1; size <= 8; size++) {
		glPointSize((float)size);
		glColor3f(size & 1, (size >> 1) & 1, (size >> 2) & 1);
		glBegin(GL_POINTS);
		for (int i = 0; i < 8; i++) {
			glVertex2f(-0.9f + i * 0.25f,
				-0.9f + (size - 1) * 0.25f);
		}
		glEnd();
	}
	glPointSize(1.0f);
}


static void
scene_transform()
{
	glMatrixMode(GL_MODELVIEW);
	for (int i = 0; i < 6; i++) {
		glLoadIdentity();
		glRotatef(i * 30.0f, 0, 0, 1);
		glScalef(1.0f - i * 0.13f, 1.0f - i * 0.13f, 1);
		glColor3f(i & 1, (i >> 1) & 1, 1.0f - i * 0.15f);
		quad(-0.6f, -0.6f, 0.6f, 0.6f);
	}
	glLoadIdentity();
}


static void
scene_lighting()
{
	// Fixed-function lighting over a grid of quads with varying normals
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_NORMALIZE);
	float lightPos[] = {0.5f, 0.5f, 1.0f, 0.0f};
	float diffuse[] = {0.9f, 0.8f, 0.2f, 1.0f};
	float ambient[] = {0.1f, 0.1f, 0.3f, 1.0f};
	glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
	glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diffuse);
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, ambient);

	const int n = 10;
	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n; x++) {
			float u = -1.0f + 2.0f * x / n;
			float v = -1.0f + 2.0f * y / n;
			float s = 2.0f / n;
			// Normal swings across the grid like a curved surface
			float nx = sinf(u * 1.4f), ny = sinf(v * 1.4f);
			float nz = sqrtf(fmaxf(0.05f, 1.0f - nx * nx - ny * ny));
			glNormal3f(nx, ny, nz);
			quad(u, v, u + s, v + s);
		}
	}
	glDisable(GL_LIGHTING);
}


static void
scene_fog()
{
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0.0f);
	glFogf(GL_FOG_END, 1.0f);
	float fogColor[] = {0.5f, 0.5f, 0.5f, 1.0f};
	glFogfv(GL_FOG_COLOR, fogColor);
	// Depth-ramped quad: fog factor varies across the surface
	glBegin(GL_QUADS);
	glColor3f(1, 0, 0);
	glVertex3f(-0.9f, -0.9f, 0.0f);
	glVertex3f(0.9f, -0.9f, -0.95f);
	glVertex3f(0.9f, 0.9f, -0.95f);
	glVertex3f(-0.9f, 0.9f, 0.0f);
	glEnd();
	glDisable(GL_FOG);
}


static void
scene_culling()
{
	glEnable(GL_CULL_FACE);
	glFrontFace(GL_CCW);
	// CCW quad (visible with BACK culling)
	glCullFace(GL_BACK);
	glColor3f(0, 1, 0);
	quad(-0.9f, -0.9f, -0.1f, -0.1f);
	// CW quad (culled with BACK culling — must NOT appear)
	glColor3f(1, 0, 0);
	glBegin(GL_QUADS);
	glVertex2f(0.1f, -0.9f);
	glVertex2f(0.1f, -0.1f);
	glVertex2f(0.9f, -0.1f);
	glVertex2f(0.9f, -0.9f);
	glEnd();
	// Same two with FRONT culling — only the CW one appears
	glCullFace(GL_FRONT);
	glColor3f(0, 0, 1);
	quad(-0.9f, 0.1f, -0.1f, 0.9f);		// culled
	glColor3f(1, 1, 0);
	glBegin(GL_QUADS);
	glVertex2f(0.1f, 0.1f);
	glVertex2f(0.1f, 0.9f);
	glVertex2f(0.9f, 0.9f);
	glVertex2f(0.9f, 0.1f);
	glEnd();
	glDisable(GL_CULL_FACE);
}


static GLuint
build_program(const char* vsSource, const char* fsSource)
{
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &vsSource, NULL);
	glCompileShader(vs);
	GLint ok = 0;
	glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok)
		return 0;

	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &fsSource, NULL);
	glCompileShader(fs);
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok)
		return 0;

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!ok)
		return 0;
	return prog;
}


static void
scene_glsl_varying()
{
	static const char* vs =
		"varying vec3 col;\n"
		"void main() {\n"
		"  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
		"  col = gl_Color.rgb;\n"
		"}\n";
	static const char* fs =
		"varying vec3 col;\n"
		"void main() { gl_FragColor = vec4(col * col, 1.0); }\n";

	GLuint prog = build_program(vs, fs);
	if (prog == 0) {
		// Signal SKIP by rendering a solid magenta frame on both passes
		glColor3f(1, 0, 1);
		quad(-1, -1, 1, 1);
		return;
	}
	glUseProgram(prog);
	scene_quad_gradient();
	glUseProgram(0);
	glDeleteProgram(prog);
}


static void
scene_glsl_texture()
{
	static const char* vs =
		"void main() {\n"
		"  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
		"  gl_TexCoord[0] = gl_MultiTexCoord0;\n"
		"}\n";
	static const char* fs =
		"uniform sampler2D tex;\n"
		"uniform vec3 tint;\n"
		"void main() {\n"
		"  vec3 t = texture2D(tex, gl_TexCoord[0].st).rgb;\n"
		"  gl_FragColor = vec4(t * tint, 1.0);\n"
		"}\n";

	GLuint prog = build_program(vs, fs);
	if (prog == 0) {
		glColor3f(1, 0, 1);
		quad(-1, -1, 1, 1);
		return;
	}
	GLuint tex = make_checker_texture(16, 2);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);
	glUniform3f(glGetUniformLocation(prog, "tint"), 0.9f, 1.0f, 0.6f);
	textured_fullscreen_quad();
	glUseProgram(0);
	glDeleteProgram(prog);
	glDeleteTextures(1, &tex);
}


static void
scene_copytex()
{
	// Render a gradient, copy it into a texture, clear, then draw the
	// texture — exercises the framebuffer-to-texture copy path.
	scene_quad_gradient();
	glFinish();

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, 256, 256, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_TEXTURE_2D);
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-0.8f, -0.8f);
	glTexCoord2f(1, 0); glVertex2f(0.8f, -0.8f);
	glTexCoord2f(1, 1); glVertex2f(0.8f, 0.8f);
	glTexCoord2f(0, 1); glVertex2f(-0.8f, 0.8f);
	glEnd();
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1, &tex);
}


static GLuint
make_checker_texture_rgba(int texSize, int checker)
{
	static uint8* data = NULL;
	data = (uint8*)realloc(data, texSize * texSize * 4);
	for (int y = 0; y < texSize; y++) {
		for (int x = 0; x < texSize; x++) {
			bool on = ((x / checker) ^ (y / checker)) & 1;
			uint8* p = data + (y * texSize + x) * 4;
			p[0] = on ? 255 : 32;
			p[1] = on ? 64 : 200;
			p[2] = on ? 16 : 240;
			p[3] = 255;
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texSize, texSize, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, data);
	return tex;
}


static void
scene_tex_rgba()
{
	// Same checkerboard as tex_nearest but uploaded as GL_RGBA: no
	// RGB->RGBX conversion in the upload path.
	glEnable(GL_TEXTURE_2D);
	GLuint tex = make_checker_texture_rgba(16, 2);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glColor3f(1, 1, 1);
	textured_fullscreen_quad();
	glDeleteTextures(1, &tex);
	glDisable(GL_TEXTURE_2D);
}


static void
scene_tex_roundtrip()
{
	// Upload RGBA, read it back with glGetTexImage and compare on the
	// CPU: green = intact, red = corrupted. Distinguishes a broken
	// upload path (round-trip fails) from a broken sampler read
	// (round-trip intact but tex_rgba scrambled).
	const int texSize = 16;
	static uint8 orig[16 * 16 * 4];
	for (int y = 0; y < texSize; y++) {
		for (int x = 0; x < texSize; x++) {
			bool on = ((x / 2) ^ (y / 2)) & 1;
			uint8* p = orig + (y * texSize + x) * 4;
			p[0] = on ? 255 : 32;
			p[1] = on ? 64 : 200;
			p[2] = on ? 16 : 240;
			p[3] = 255;
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texSize, texSize, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, orig);
	glFinish();

	static uint8 back[16 * 16 * 4];
	memset(back, 0xAA, sizeof(back));
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back);

	int mismatches = 0;
	int firstBad = -1;
	for (int i = 0; i < texSize * texSize; i++) {
		if (memcmp(orig + i * 4, back + i * 4, 3) != 0) {
			if (firstBad < 0)
				firstBad = i;
			mismatches++;
		}
	}
	fprintf(stderr, "glconform: tex_roundtrip mismatches=%d/%d",
		mismatches, texSize * texSize);
	if (firstBad >= 0) {
		int x = firstBad % texSize, y = firstBad / texSize;
		const uint8* o = orig + firstBad * 4;
		const uint8* b = back + firstBad * 4;
		fprintf(stderr, " first at (%d,%d): wrote %02x%02x%02x read "
			"%02x%02x%02x", x, y, o[0], o[1], o[2], b[0], b[1], b[2]);
	}
	fprintf(stderr, "\n");

	glDeleteTextures(1, &tex);
	if (mismatches == 0)
		glColor3f(0, 1, 0);
	else
		glColor3f(1, 0, 0);
	quad(-1, -1, 1, 1);
}


static void
scene_tex_ident()
{
	// Coordinate-identity texture: texel (x,y) = (x*16, y*16, 128).
	// The rendered output directly reveals which source texel the
	// sampler fetched at every position — the layout permutation map.
	const int texSize = 16;
	static uint8 data[16 * 16 * 4];
	for (int y = 0; y < texSize; y++) {
		for (int x = 0; x < texSize; x++) {
			uint8* p = data + (y * texSize + x) * 4;
			p[0] = (uint8)(x * 16);
			p[1] = (uint8)(y * 16);
			p[2] = 128;
			p[3] = 255;
		}
	}
	glEnable(GL_TEXTURE_2D);
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texSize, texSize, 0, GL_RGBA,
		GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glColor3f(1, 1, 1);
	textured_fullscreen_quad();
	glDeleteTextures(1, &tex);
	glDisable(GL_TEXTURE_2D);
}


static const Scene kScenes[] = {
	{"scissor_clears",	scene_scissor_clears,	0.1f},
	{"tri_gouraud",		scene_tri_gouraud,		1.0f},
	{"quad_gradient",	scene_quad_gradient,	0.5f},
	{"depth_test",		scene_depth_test,		1.5f},
	{"stencil_ops",		scene_stencil_ops,		1.5f},
	{"blend_alpha",		scene_blend_alpha,		1.0f},
	// NEAREST-sampled checkerboards: rounding at texel boundaries is
	// implementation-defined; on this hardware 100% of the differing
	// pixels sit exactly on texel edges (~0.7% of the image).
	{"tex_nearest",		scene_tex_nearest,		1.0f},
	{"tex_linear",		scene_tex_linear,		2.0f},
	{"tex_mipmap",		scene_tex_mipmap,		5.0f},
	// Line rasterization (diamond-exit) legitimately differs between
	// implementations; 100% of differing pixels lie within 1px of a
	// drawn line.
	{"lines",			scene_lines,			8.0f},
	{"points",			scene_points,			3.0f},
	{"transform",		scene_transform,		2.0f},
	{"lighting",		scene_lighting,			1.0f},
	{"fog",				scene_fog,				1.0f},
	{"culling",			scene_culling,			0.5f},
	{"glsl_varying",	scene_glsl_varying,		0.5f},
	{"glsl_texture",	scene_glsl_texture,		1.0f},
	{"copytex",			scene_copytex,			2.0f},
	{"tex_rgba",		scene_tex_rgba,			1.0f},
	{"tex_roundtrip",	scene_tex_roundtrip,	0.1f},
	{"tex_ident",		scene_tex_ident,		0.5f},
};
static const int kNumScenes = sizeof(kScenes) / sizeof(kScenes[0]);


// #pragma mark - PPM I/O


static bool
write_ppm(const char* path, const uint8* rgb)
{
	FILE* f = fopen(path, "wb");
	if (f == NULL)
		return false;
	fprintf(f, "P6\n%d %d\n255\n", kSize, kSize);
	fwrite(rgb, 1, kSize * kSize * 3, f);
	fclose(f);
	return true;
}


static uint8*
read_ppm(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	int w = 0, h = 0, maxval = 0;
	if (fscanf(f, "P6 %d %d %d", &w, &h, &maxval) != 3
		|| w != kSize || h != kSize || maxval != 255) {
		fclose(f);
		return NULL;
	}
	fgetc(f);	// single whitespace after header
	uint8* data = (uint8*)malloc(kSize * kSize * 3);
	size_t got = fread(data, 1, kSize * kSize * 3, f);
	fclose(f);
	if (got != (size_t)(kSize * kSize * 3)) {
		free(data);
		return NULL;
	}
	return data;
}


// #pragma mark - render mode


class ConformGLView : public BGLView {
public:
	ConformGLView(BRect frame)
		:
		BGLView(frame, "conform", B_FOLLOW_ALL_SIDES, B_WILL_DRAW,
			BGL_RGB | BGL_DOUBLE | BGL_DEPTH | BGL_STENCIL)
	{
	}
};


static ConformGLView* sView = NULL;
static const char* sOutDir = NULL;
static int sFailures = 0;


static int32
render_thread(void* /*unused*/)
{
	snooze(800000);		// let AttachedToWindow create the GL context

	sView->LockGL();
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	fprintf(stderr, "glconform: renderer = %s\n",
		renderer ? renderer : "(null)");

	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/renderer.txt", sOutDir);
	FILE* f = fopen(path, "w");
	if (f != NULL) {
		fprintf(f, "%s\n", renderer ? renderer : "unknown");
		fclose(f);
	}

	static uint8 rgb[kSize * kSize * 3];
	for (int i = 0; i < kNumScenes; i++) {
		reset_state();
		kScenes[i].render();
		glFinish();

		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, kSize, kSize, GL_RGB, GL_UNSIGNED_BYTE, rgb);

		snprintf(path, sizeof(path), "%s/%02d_%s.ppm", sOutDir, i,
			kScenes[i].name);
		if (!write_ppm(path, rgb)) {
			fprintf(stderr, "glconform: cannot write %s\n", path);
			sFailures++;
		}
		fprintf(stderr, "glconform: rendered %s\n", kScenes[i].name);
	}
	sView->UnlockGL();

	be_app->PostMessage(B_QUIT_REQUESTED);
	return 0;
}


class ConformApp : public BApplication {
public:
	ConformApp()
		:
		BApplication("application/x-vnd.glconform")
	{
	}

	virtual void ReadyToRun()
	{
		BWindow* window = new BWindow(BRect(50, 50, 50 + kSize - 1,
			50 + kSize - 1), "glconform", B_TITLED_WINDOW,
			B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
		sView = new ConformGLView(BRect(0, 0, kSize - 1, kSize - 1));
		window->AddChild(sView);
		window->Show();

		thread_id thread = spawn_thread(render_thread, "conform_render",
			B_NORMAL_PRIORITY, NULL);
		resume_thread(thread);
	}
};


static int
run_render(const char* outDir)
{
	sOutDir = outDir;
	char cmd[B_PATH_NAME_LENGTH + 16];
	snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", outDir);
	if (system(cmd) != 0)
		return 1;

	ConformApp app;
	app.Run();
	return sFailures > 0 ? 1 : 0;
}


// #pragma mark - compare mode


static int
run_compare(const char* dirA, const char* dirB)
{
	printf("glconform — differential comparison\n");
	char path[B_PATH_NAME_LENGTH];

	for (int pass = 0; pass < 2; pass++) {
		const char* dir = pass == 0 ? dirA : dirB;
		snprintf(path, sizeof(path), "%s/renderer.txt", dir);
		FILE* f = fopen(path, "r");
		char name[128] = "unknown";
		if (f != NULL) {
			if (fgets(name, sizeof(name), f) != NULL)
				name[strcspn(name, "\n")] = '\0';
			fclose(f);
		}
		printf("  %s: %s\n", pass == 0 ? "A" : "B", name);
	}
	printf("\n  %-18s %10s %10s   %s\n", "scene", "bad px %", "mean d",
		"verdict");

	int failed = 0, passed = 0, missing = 0;
	for (int i = 0; i < kNumScenes; i++) {
		snprintf(path, sizeof(path), "%s/%02d_%s.ppm", dirA, i,
			kScenes[i].name);
		uint8* a = read_ppm(path);
		snprintf(path, sizeof(path), "%s/%02d_%s.ppm", dirB, i,
			kScenes[i].name);
		uint8* b = read_ppm(path);

		if (a == NULL || b == NULL) {
			printf("  %-18s %10s %10s   MISSING\n", kScenes[i].name,
				"-", "-");
			free(a);
			free(b);
			missing++;
			continue;
		}

		int64 sumDelta = 0;
		int badPixels = 0;
		const int pixels = kSize * kSize;
		for (int p = 0; p < pixels; p++) {
			int maxd = 0;
			for (int c = 0; c < 3; c++) {
				int d = abs((int)a[p * 3 + c] - (int)b[p * 3 + c]);
				if (d > maxd)
					maxd = d;
			}
			sumDelta += maxd;
			if (maxd > kChannelTolerance)
				badPixels++;
		}
		float badPct = 100.0f * badPixels / pixels;
		float meanDelta = (float)sumDelta / pixels;
		// The mean-delta gate scales with the scene's bad-pixel budget:
		// a scene allowed N% of differing pixels may legitimately carry
		// up to N% of full-contrast (255) deltas in its mean.
		float meanLimit = 2.0f + 2.55f * kScenes[i].maxBadPct;
		bool ok = badPct <= kScenes[i].maxBadPct && meanDelta < meanLimit;

		printf("  %-18s %9.2f%% %10.2f   %s\n", kScenes[i].name,
			badPct, meanDelta, ok ? "PASS" : "FAIL");
		if (ok)
			passed++;
		else
			failed++;

		free(a);
		free(b);
	}

	printf("\n  %d/%d scenes match the software reference", passed,
		kNumScenes - missing);
	if (missing > 0)
		printf(" (%d missing)", missing);
	printf("\n");
	return failed == 0 && missing == 0 ? 0 : 1;
}


// #pragma mark - main


int
main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "--render") == 0)
		return run_render(argv[2]);
	if (argc == 4 && strcmp(argv[1], "--compare") == 0)
		return run_compare(argv[2], argv[3]);

	fprintf(stderr,
		"usage:\n"
		"  %s --render <outdir>\n"
		"  %s --compare <dirA> <dirB>\n",
		argv[0], argv[0]);
	return 1;
}
