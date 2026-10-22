/*
 * glbench - minimal GLES2 fill-rate / draw-throughput probe
 *
 * Used to verify that the GPU driver actually renders and to measure
 * sustained fill rate and driver submission overhead.
 *
 * Build (Android NDK):
 *   aarch64-linux-android30-clang -O2 -o glbench glbench.c -lEGL -lGLESv2 -lm
 *
 * Run:
 *   adb push glbench /data/local/tmp/ && adb shell "chmod 755 /data/local/tmp/glbench"
 *   adb shell "/data/local/tmp/glbench 1280 720 300"
 *
 * Note: run it several times in a row before trusting the numbers. The first
 * run can be limited by the governor ramping up, not by the GPU itself.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static GLuint make_shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(s, sizeof log, NULL, log);
		printf("shader error: %s\n", log);
		exit(1);
	}
	return s;
}

static const float VERTS[] = { -1, -1, 3, -1, -1, 3 };

int main(int argc, char **argv)
{
	int W = argc > 1 ? atoi(argv[1]) : 1280;
	int H = argc > 2 ? atoi(argv[2]) : 720;
	int ITER = argc > 3 ? atoi(argv[3]) : 200;

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLConfig cfg;
	EGLint n = 0;
	EGLSurface surf;
	EGLContext ctx;
	GLuint vs, fs, prog;
	GLint linked = 0;
	double t0, t1;
	const char *ext;

	EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLint pb[] = { EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE };
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

	const char *vs_src =
		"attribute vec2 p;\n"
		"void main(){ gl_Position = vec4(p,0.0,1.0); }\n";
	const char *fs_src =
		"precision mediump float;\n"
		"void main(){ gl_FragColor = vec4(0.5,0.4,0.3,0.5); }\n";

	if (!eglInitialize(dpy, NULL, NULL)) {
		printf("eglInitialize failed 0x%x\n", eglGetError());
		return 1;
	}
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n == 0) {
		printf("no EGL config\n");
		return 1;
	}
	pb[1] = W;
	pb[3] = H;
	surf = eglCreatePbufferSurface(dpy, cfg, pb);
	if (surf == EGL_NO_SURFACE) {
		printf("pbuffer failed 0x%x\n", eglGetError());
		return 1;
	}
	ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT) {
		printf("context failed 0x%x\n", eglGetError());
		return 1;
	}
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
		printf("makeCurrent failed 0x%x\n", eglGetError());
		return 1;
	}

	printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
	printf("GL_VENDOR  : %s\n", (const char *)glGetString(GL_VENDOR));
	printf("GL_VERSION : %s\n", (const char *)glGetString(GL_VERSION));

	ext = (const char *)glGetString(GL_EXTENSIONS);
	if (ext && strstr(ext, "GL_ARM_shader_core_properties")) {
		GLint v;

		printf("shader_core_properties: yes\n");
		v = -1;
		glGetIntegerv(0x93B3, &v);
		printf("core_count(0x93B3): %d\n", v);
		v = -1;
		glGetIntegerv(0x93B4, &v);
		printf("core_active(0x93B4): %d\n", v);
		v = -1;
		glGetIntegerv(0x93B5, &v);
		printf("core_mask(0x93B5): 0x%x\n", v);
	}

	vs = make_shader(GL_VERTEX_SHADER, vs_src);
	fs = make_shader(GL_FRAGMENT_SHADER, fs_src);
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		printf("link failed\n");
		return 1;
	}
	glUseProgram(prog);

	glViewport(0, 0, W, H);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, VERTS);
	glEnableVertexAttribArray(0);

	for (n = 0; n < 20; n++)	/* warm up */
		glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();

	t0 = now_ms();
	for (n = 0; n < ITER; n++)
		glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	t1 = now_ms();

	printf("resolution: %dx%d  iter: %d\n", W, H, ITER);
	printf("time_ms: %.2f\n", t1 - t0);
	printf("fill_rate: %.1f Mpix/s\n",
	       (double)W * H * ITER / ((t1 - t0) / 1000.0) / 1e6);

	{
		static const float TINY[] = {
			-0.01f, -0.01f, 0.01f, -0.01f, -0.01f, 0.01f
		};
		double a, b;

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, TINY);
		for (n = 0; n < 1000; n++)
			glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
		a = now_ms();
		for (n = 0; n < 20000; n++)
			glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
		b = now_ms();
		printf("draw20k_ms: %.2f  draws/s: %.0f\n",
		       b - a, 20000.0 / ((b - a) / 1000.0));
	}

	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return 0;
}
