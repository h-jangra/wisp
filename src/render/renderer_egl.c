#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-egl.h>

#include "core/types.h"
#include "render/renderer.h"
#include "render/renderer_egl.h"

#define NANOVG_GLES2_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"

static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLConfig egl_cfg = NULL;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static NVGcontext *vg = NULL;
static int font_default = -1;
static bool egl_inited = false;

static const char *font_candidates[] = {
	"/usr/share/fonts/noto/NotoSans-Regular.ttf",
	"/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
	"/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	NULL
};

static void load_default_font(void)
{
	if (!vg || font_default >= 0)
		return;

	for (int i = 0; font_candidates[i]; i++) {
		if (access(font_candidates[i], R_OK) == 0) {
			font_default = nvgCreateFont(vg, "default", font_candidates[i]);
			if (font_default >= 0) {
				fprintf(stderr, "wisp: NanoVG loaded font '%s'\n", font_candidates[i]);
				return;
			}
		}
	}
	fprintf(stderr, "wisp: warning: no suitable TTF font found for NanoVG\n");
}

static bool egl_renderer_init(struct wl_display *display)
{
	if (egl_inited)
		return true;

	if (!display) {
		fprintf(stderr, "wisp: egl_renderer_init failed: NULL display\n");
		return false;
	}

	egl_dpy = eglGetDisplay((EGLNativeDisplayType)display);
	if (egl_dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "wisp: eglGetDisplay failed\n");
		return false;
	}

	EGLint major = 0, minor = 0;
	if (!eglInitialize(egl_dpy, &major, &minor)) {
		fprintf(stderr, "wisp: eglInitialize failed\n");
		egl_dpy = EGL_NO_DISPLAY;
		return false;
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "wisp: eglBindAPI(EGL_OPENGL_ES_API) failed\n");
		eglTerminate(egl_dpy);
		egl_dpy = EGL_NO_DISPLAY;
		return false;
	}

	const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 0,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};

	EGLint num_configs = 0;
	if (!eglChooseConfig(egl_dpy, config_attribs, &egl_cfg, 1, &num_configs) || num_configs < 1) {
		fprintf(stderr, "wisp: eglChooseConfig failed to find RGBA8888 stencil8 config\n");
		eglTerminate(egl_dpy);
		egl_dpy = EGL_NO_DISPLAY;
		return false;
	}

	const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, context_attribs);
	if (egl_ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "wisp: eglCreateContext failed\n");
		eglTerminate(egl_dpy);
		egl_dpy = EGL_NO_DISPLAY;
		return false;
	}

	egl_inited = true;
	fprintf(stderr, "wisp: EGL %d.%d initialized with GLES2 backend\n", major, minor);
	return true;
}

static void egl_renderer_shutdown(void)
{
	if (vg) {
		nvgDeleteGLES2(vg);
		vg = NULL;
		font_default = -1;
	}
	if (egl_dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (egl_ctx != EGL_NO_CONTEXT) {
			eglDestroyContext(egl_dpy, egl_ctx);
			egl_ctx = EGL_NO_CONTEXT;
		}
		eglTerminate(egl_dpy);
		egl_dpy = EGL_NO_DISPLAY;
	}
	egl_inited = false;
}

static bool egl_surface_create(struct widget *wgt, uint32_t width, uint32_t height)
{
	if (!wgt || !wgt->surface || !egl_inited)
		return false;

	if (wgt->egl_surface) {
		eglDestroySurface(egl_dpy, (EGLSurface)wgt->egl_surface);
		wgt->egl_surface = NULL;
	}
	if (wgt->egl_window) {
		wl_egl_window_destroy(wgt->egl_window);
		wgt->egl_window = NULL;
	}

	int pw = (int)width;
	int ph = (int)height;
	if (pw < 1) pw = 1;
	if (ph < 1) ph = 1;

	wgt->egl_window = wl_egl_window_create(wgt->surface, pw, ph);
	if (!wgt->egl_window) {
		fprintf(stderr, "wisp: wl_egl_window_create failed\n");
		return false;
	}

	wgt->egl_surface = (void *)eglCreateWindowSurface(egl_dpy, egl_cfg,
							  (EGLNativeWindowType)wgt->egl_window, NULL);
	if (!wgt->egl_surface) {
		fprintf(stderr, "wisp: eglCreateWindowSurface failed\n");
		wl_egl_window_destroy(wgt->egl_window);
		wgt->egl_window = NULL;
		return false;
	}

	wgt->use_egl = true;
	wgt->width = width;
	wgt->height = height;
	return true;
}

static void egl_surface_destroy(struct widget *wgt)
{
	if (!wgt)
		return;

	if (wgt->egl_surface) {
		eglDestroySurface(egl_dpy, (EGLSurface)wgt->egl_surface);
		wgt->egl_surface = NULL;
	}
	if (wgt->egl_window) {
		wl_egl_window_destroy(wgt->egl_window);
		wgt->egl_window = NULL;
	}
	wgt->use_egl = false;
}

static void egl_surface_resize(struct widget *wgt, uint32_t width, uint32_t height)
{
	if (!wgt || !wgt->egl_window)
		return;

	int pw = (int)width;
	int ph = (int)height;
	if (pw < 1) pw = 1;
	if (ph < 1) ph = 1;

	wl_egl_window_resize(wgt->egl_window, pw, ph, 0, 0);
	wgt->width = width;
	wgt->height = height;
}

static bool egl_begin_frame(struct widget *wgt)
{
	if (!wgt || !wgt->egl_surface || !egl_inited)
		return false;

	if (!eglMakeCurrent(egl_dpy, (EGLSurface)wgt->egl_surface,
			    (EGLSurface)wgt->egl_surface, egl_ctx)) {
		fprintf(stderr, "wisp: eglMakeCurrent failed\n");
		return false;
	}

	if (!vg) {
		vg = nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
		if (!vg) {
			fprintf(stderr, "wisp: fatal: nvgCreateGLES2 failed\n");
			return false;
		}
		load_default_font();
	}

	int pw = (int)wgt->width;
	int ph = (int)wgt->height;
	glViewport(0, 0, pw, ph);

	float cr = 0.0f, cg = 0.0f, cb = 0.0f, ca = 0.0f;
	uint32_t clear_color = 0;
	if (wgt && !wgt->transparent) {
		if (wgt->bg_color != 0)
			clear_color = wgt->bg_color;
		else if (wgt->surf_type == SURFACE_TOPLEVEL)
			clear_color = 0xFF181825u;
	}
	if (clear_color != 0) {
		ca = (float)((clear_color >> 24) & 0xFF) / 255.0f;
		cr = (float)((clear_color >> 16) & 0xFF) / 255.0f;
		cg = (float)((clear_color >> 8) & 0xFF) / 255.0f;
		cb = (float)(clear_color & 0xFF) / 255.0f;
	}
	glClearColor(cr, cg, cb, ca);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	float scale = (wgt->scale > 0) ? (float)wgt->scale : 1.0f;
	float lw = (float)pw / scale;
	float lh = (float)ph / scale;

	nvgBeginFrame(vg, lw, lh, scale);
	return true;
}

static void egl_end_frame(struct widget *wgt)
{
	if (!wgt || !wgt->egl_surface || !vg)
		return;
	/* layer-shell & xdg protocols forbid a buffer before configure. */
	if (wgt->layer_surface && !wgt->layer_configured)
		return;
	if (wgt->xdg_surface && !wgt->xdg_configured)
		return;

	nvgEndFrame(vg);
	eglSwapBuffers(egl_dpy, (EGLSurface)wgt->egl_surface);
	wgt->draws++;
}

static NVGcontext *egl_get_vg(void)
{
	return vg;
}

const struct wisp_renderer egl_renderer = {
	.name = "egl_gles2",
	.init = egl_renderer_init,
	.shutdown = egl_renderer_shutdown,
	.surface_create = egl_surface_create,
	.surface_destroy = egl_surface_destroy,
	.surface_resize = egl_surface_resize,
	.begin_frame = egl_begin_frame,
	.end_frame = egl_end_frame,
	.get_vg = (void *(*)(void))egl_get_vg,
};

const struct wisp_renderer *g_renderer = &native_renderer;

void renderer_set_backend(const struct wisp_renderer *backend)
{
	g_renderer = backend ? backend : &native_renderer;
}

bool renderer_init(struct wl_display *display)
{
	return g_renderer && g_renderer->init ? g_renderer->init(display) : false;
}

void renderer_shutdown(void)
{
	if (g_renderer && g_renderer->shutdown)
		g_renderer->shutdown();
}

void *renderer_get_vg(void)
{
	return g_renderer && g_renderer->get_vg ? g_renderer->get_vg() : NULL;
}
