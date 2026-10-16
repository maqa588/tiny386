#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pc.h"
#if defined(__has_include)
# if __has_include(<SDL2/SDL.h>)
#  include <SDL2/SDL.h>
# else
#  include "SDL.h"
# endif
#else
# include "SDL.h"
#endif
#include "osd/osd.h"
#if defined(__has_include)
# if __has_include(<GL/gl.h>)
#  define USE_OPENGL 1
#  include <GL/gl.h>
# elif __has_include(<OpenGL/gl.h>)
/* macOS ships the OpenGL headers inside a framework */
#  define GL_SILENCE_DEPRECATION 1
#  define USE_OPENGL 1
#  include <OpenGL/gl.h>
# endif
#endif

#include <time.h>
uint32_t get_uticks()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint32_t) ts.tv_sec * 1000000 +
		(uint32_t) ts.tv_nsec / 1000);
}

#ifndef _WIN32
#include <sys/mman.h>
void *bigmalloc(size_t size)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
#else
void *bigmalloc(size_t size)
{
	return malloc(size);
}
#endif

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	FILE *fp = fopen(file, "rb");
	if (fp == NULL) {
		fprintf(stderr, "load_rom: open %s failed: %s\n", file, strerror(errno));
		abort();
	}

	fseek(fp, 0, SEEK_END);
	int len = ftell(fp);
	fprintf(stderr, "load_rom: %s, len %d\n", file, len);
	rewind(fp);
	if (backward)
		fread(phys_mem + addr - len, 1, len, fp);
	else
		fread(phys_mem + addr, 1, len, fp);
	fclose(fp);
	return len;
}

typedef struct {
	int width, height;
#if SDL_MAJOR_VERSION >= 2
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
# if defined(USE_OPENGL)
	SDL_GLContext gl_ctx;
	bool use_opengl;
	GLuint gl_tex;
# endif
#endif
	SDL_Surface *screen;
	PC *pc;
	OSD *osd;
	bool osd_enabled;
	bool grabbed;
	float wheel_accum; /* fractional wheel remainder (SDL preciseY) */
} Console;

void console_send_kbd(void *opaque, int keypress, int keycode)
{
	Console *s = opaque;
	ps2_put_keycode(s->pc->kbd, keypress, keycode);
}

void console_power_off(void *opaque)
{
	Console *s = opaque;
	fprintf(stderr, "[PM] Power off requested from Control Panel, exiting...\n");
	if (s && s->pc)
		s->pc->shutdown_state = 8;
	exit(0);
}

void console_trigger_suspend(void *opaque)
{
	Console *s = opaque;
	if (s && s->pc) {
		fprintf(stderr, "[PM] Suspend requested from Control Panel...\n");
		/* Dismiss OSD overlay so screen is clean */
		s->osd_enabled = false;
		s->pc->full_update = 2;
		s->pc->apm_suspend = 2; /* trigger suspend */
	}
}


#ifndef BPP
#define BPP 32
#endif

Console *console_init(int width, int height)
{
	Console *s = malloc(sizeof(Console));
	memset(s, 0, sizeof(Console));
	s->osd = osd_init();
	s->osd_enabled = false;
#ifdef SWAPXY
	s->width = height;
	s->height = width;
#else
	s->width = width;
	s->height = height;
#endif
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
#if SDL_MAJOR_VERSION >= 2
# if defined(USE_OPENGL)
	const char *no_gl_env = getenv("TINY386_NO_GL");
	bool try_opengl = (no_gl_env == NULL || atoi(no_gl_env) == 0);
	if (try_opengl) {
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

		s->window = SDL_CreateWindow("tiny386 [OpenGL 2D Accelerated] - ctrl + ] to grab",
		                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		                             s->width, s->height,
		                             SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
		if (s->window) {
			s->gl_ctx = SDL_GL_CreateContext(s->window);
			if (s->gl_ctx) {
				SDL_GL_MakeCurrent(s->window, s->gl_ctx);
				SDL_GL_SetSwapInterval(0); /* Disable vsync for max emulator throughput */

				glGenTextures(1, &s->gl_tex);
				glBindTexture(GL_TEXTURE_2D, s->gl_tex);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s->width, s->height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

				s->use_opengl = true;
				printf("[OpenGL] 2D Hardware Acceleration active!\n");
				printf("[OpenGL] Renderer: %s | Vendor: %s | Version: %s\n",
				       (const char *)glGetString(GL_RENDERER),
				       (const char *)glGetString(GL_VENDOR),
				       (const char *)glGetString(GL_VERSION));
			} else {
				fprintf(stderr, "[OpenGL] Failed to create GL context: %s, falling back to SDL renderer\n", SDL_GetError());
				SDL_DestroyWindow(s->window);
				s->window = NULL;
			}
		}
	}
# endif

	if (!s->window) {
		s->window = SDL_CreateWindow("tiny386 - use ctrl + ] to grab/ungrab",
		                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		                             s->width, s->height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
		s->renderer = SDL_CreateRenderer(s->window, -1, SDL_RENDERER_ACCELERATED);
		if (!s->renderer)
			s->renderer = SDL_CreateRenderer(s->window, -1, SDL_RENDERER_SOFTWARE);
		SDL_RenderSetLogicalSize(s->renderer, s->width, s->height);
		s->texture = SDL_CreateTexture(s->renderer, SDL_PIXELFORMAT_ARGB8888,
		                               SDL_TEXTUREACCESS_STREAMING, s->width, s->height);
	}
	s->screen = SDL_CreateRGBSurfaceWithFormat(0, s->width, s->height, 32,
	                                           SDL_PIXELFORMAT_ARGB8888);
	if (s->screen)
		memset(s->screen->pixels, 0, s->screen->pitch * s->height);
#else
	s->screen = SDL_SetVideoMode(s->width, s->height, BPP, 0);
	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY,
			    SDL_DEFAULT_REPEAT_INTERVAL);
	SDL_WM_SetCaption("tiny386 - use ctrl + ] to grab/ungrab", NULL);
#endif
	osd_attach_console(s->osd, s);
	return s;
}

static void redraw(void *opaque,
		   int x, int y, int w, int h)
{
	Console *s = opaque;
	if (s->osd_enabled)
		osd_render(s->osd, s->screen->pixels,
			   s->screen->w, s->screen->h, s->screen->pitch);
#if SDL_MAJOR_VERSION >= 2
# if defined(USE_OPENGL)
	if (s->use_opengl) {
		int win_w, win_h;
		SDL_GetWindowSize(s->window, &win_w, &win_h);

		glBindTexture(GL_TEXTURE_2D, s->gl_tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->width, s->height,
		                GL_BGRA, GL_UNSIGNED_BYTE, s->screen->pixels);

		glViewport(0, 0, win_w, win_h);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, s->gl_tex);
		glBegin(GL_QUADS);
			glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
			glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
			glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
			glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
		glEnd();
		glDisable(GL_TEXTURE_2D);

		SDL_GL_SwapWindow(s->window);
		SDL_PumpEvents();
		return;
	}
# endif
	SDL_UpdateTexture(s->texture, NULL, s->screen->pixels, s->screen->pitch);
	SDL_RenderClear(s->renderer);
	SDL_RenderCopy(s->renderer, s->texture, NULL, NULL);
	SDL_RenderPresent(s->renderer);
	SDL_PumpEvents();
#else
	SDL_Flip(s->screen);
	SDL_PumpEvents();
#endif
}

#if SDL_MAJOR_VERSION >= 2
/* Complete USB HID (SDL_Scancode) to IBM PC / AT Set 1 scancode lookup table */
static const uint8_t sdl2_scancode_to_at[512] = {
	[SDL_SCANCODE_A] = 0x1E,
	[SDL_SCANCODE_B] = 0x30,
	[SDL_SCANCODE_C] = 0x2E,
	[SDL_SCANCODE_D] = 0x20,
	[SDL_SCANCODE_E] = 0x12,
	[SDL_SCANCODE_F] = 0x21,
	[SDL_SCANCODE_G] = 0x22,
	[SDL_SCANCODE_H] = 0x23,
	[SDL_SCANCODE_I] = 0x17,
	[SDL_SCANCODE_J] = 0x24,
	[SDL_SCANCODE_K] = 0x25,
	[SDL_SCANCODE_L] = 0x26,
	[SDL_SCANCODE_M] = 0x32,
	[SDL_SCANCODE_N] = 0x31,
	[SDL_SCANCODE_O] = 0x18,
	[SDL_SCANCODE_P] = 0x19,
	[SDL_SCANCODE_Q] = 0x10,
	[SDL_SCANCODE_R] = 0x13,
	[SDL_SCANCODE_S] = 0x1F,
	[SDL_SCANCODE_T] = 0x14,
	[SDL_SCANCODE_U] = 0x16,
	[SDL_SCANCODE_V] = 0x2F,
	[SDL_SCANCODE_W] = 0x11,
	[SDL_SCANCODE_X] = 0x2D,
	[SDL_SCANCODE_Y] = 0x15,
	[SDL_SCANCODE_Z] = 0x2C,

	[SDL_SCANCODE_1] = 0x02,
	[SDL_SCANCODE_2] = 0x03,
	[SDL_SCANCODE_3] = 0x04,
	[SDL_SCANCODE_4] = 0x05,
	[SDL_SCANCODE_5] = 0x06,
	[SDL_SCANCODE_6] = 0x07,
	[SDL_SCANCODE_7] = 0x08,
	[SDL_SCANCODE_8] = 0x09,
	[SDL_SCANCODE_9] = 0x0A,
	[SDL_SCANCODE_0] = 0x0B,

	[SDL_SCANCODE_RETURN] = 0x1C,
	[SDL_SCANCODE_ESCAPE] = 0x01,
	[SDL_SCANCODE_BACKSPACE] = 0x0E,
	[SDL_SCANCODE_TAB] = 0x0F,
	[SDL_SCANCODE_SPACE] = 0x39,

	[SDL_SCANCODE_MINUS] = 0x0C,
	[SDL_SCANCODE_EQUALS] = 0x0D,
	[SDL_SCANCODE_LEFTBRACKET] = 0x1A,
	[SDL_SCANCODE_RIGHTBRACKET] = 0x1B,
	[SDL_SCANCODE_BACKSLASH] = 0x2B,
	[SDL_SCANCODE_NONUSHASH] = 0x2B,
	[SDL_SCANCODE_SEMICOLON] = 0x27,
	[SDL_SCANCODE_APOSTROPHE] = 0x28,
	[SDL_SCANCODE_GRAVE] = 0x29,
	[SDL_SCANCODE_COMMA] = 0x33,
	[SDL_SCANCODE_PERIOD] = 0x34,
	[SDL_SCANCODE_SLASH] = 0x35,

	[SDL_SCANCODE_CAPSLOCK] = 0x3A,

	[SDL_SCANCODE_F1] = 0x3B,
	[SDL_SCANCODE_F2] = 0x3C,
	[SDL_SCANCODE_F3] = 0x3D,
	[SDL_SCANCODE_F4] = 0x3E,
	[SDL_SCANCODE_F5] = 0x3F,
	[SDL_SCANCODE_F6] = 0x40,
	[SDL_SCANCODE_F7] = 0x41,
	[SDL_SCANCODE_F8] = 0x42,
	[SDL_SCANCODE_F9] = 0x43,
	[SDL_SCANCODE_F10] = 0x44,
	[SDL_SCANCODE_F11] = 0x57,
	[SDL_SCANCODE_F12] = 0x58,

	[SDL_SCANCODE_PRINTSCREEN] = 0x63,
	[SDL_SCANCODE_SCROLLLOCK] = 0x46,
	[SDL_SCANCODE_PAUSE] = 0x77,
	[SDL_SCANCODE_INSERT] = 0x6E,
	[SDL_SCANCODE_HOME] = 0x66,
	[SDL_SCANCODE_PAGEUP] = 0x68,
	[SDL_SCANCODE_DELETE] = 0x6F,
	[SDL_SCANCODE_END] = 0x6B,
	[SDL_SCANCODE_PAGEDOWN] = 0x6D,
	[SDL_SCANCODE_RIGHT] = 0x6A,
	[SDL_SCANCODE_LEFT] = 0x69,
	[SDL_SCANCODE_DOWN] = 0x6C,
	[SDL_SCANCODE_UP] = 0x67,

	[SDL_SCANCODE_NUMLOCKCLEAR] = 0x45,
	[SDL_SCANCODE_KP_DIVIDE] = 0x62,
	[SDL_SCANCODE_KP_MULTIPLY] = 0x37,
	[SDL_SCANCODE_KP_MINUS] = 0x4A,
	[SDL_SCANCODE_KP_PLUS] = 0x4E,
	[SDL_SCANCODE_KP_ENTER] = 0x60,
	[SDL_SCANCODE_KP_1] = 0x4F,
	[SDL_SCANCODE_KP_2] = 0x50,
	[SDL_SCANCODE_KP_3] = 0x51,
	[SDL_SCANCODE_KP_4] = 0x4B,
	[SDL_SCANCODE_KP_5] = 0x4C,
	[SDL_SCANCODE_KP_6] = 0x4D,
	[SDL_SCANCODE_KP_7] = 0x47,
	[SDL_SCANCODE_KP_8] = 0x48,
	[SDL_SCANCODE_KP_9] = 0x49,
	[SDL_SCANCODE_KP_0] = 0x52,
	[SDL_SCANCODE_KP_PERIOD] = 0x53,
	[SDL_SCANCODE_NONUSBACKSLASH] = 0x56,

	[SDL_SCANCODE_LCTRL] = 0x1D,
	[SDL_SCANCODE_LSHIFT] = 0x2A,
	[SDL_SCANCODE_LALT] = 0x38,
	[SDL_SCANCODE_LGUI] = 0x5B,
	[SDL_SCANCODE_RCTRL] = 0x61,
	[SDL_SCANCODE_RSHIFT] = 0x36,
	[SDL_SCANCODE_RALT] = 0x64,
	[SDL_SCANCODE_RGUI] = 0x5C,
};
#endif

static int sdl_get_keycode(const SDL_KeyboardEvent *ev)
{
#if SDL_MAJOR_VERSION >= 2
	if (ev->keysym.scancode < 512)
		return sdl2_scancode_to_at[ev->keysym.scancode];
	return 0;
#else
	int keycode = ev->keysym.scancode;
	int sym = ev->keysym.sym;
	switch (sym) {
	case SDLK_UP: return 0x67;
	case SDLK_DOWN: return 0x6c;
	case SDLK_LEFT: return 0x69;
	case SDLK_RIGHT: return 0x6a;
	case SDLK_HOME: return 0x66;
	case SDLK_END: return 0x6b;
	case SDLK_PAGEUP: return 0x68;
	case SDLK_PAGEDOWN: return 0x6d;
	case SDLK_INSERT: return 0x6e;
	case SDLK_DELETE: return 0x6f;
	case SDLK_KP_DIVIDE: return 0x62;
	case SDLK_KP_ENTER: return 0x60;
	case SDLK_RCTRL: return 0x61;
	case SDLK_PAUSE: return 0x77;
	case SDLK_PRINT: return 0x63;
	case SDLK_RALT: return 0x64;
	default: if (keycode == 0) return 0;
	}
#ifndef _WIN32
	if (keycode < 9) {
		keycode = 0;
	} else if (keycode < 127 + 9) {
		keycode -= 8;
	} else {
		keycode = 0;
	}
#endif
	return keycode;
#endif
}

/* release all pressed keys */
#define KEYCODE_MAX 127
static uint8_t key_pressed[KEYCODE_MAX + 1];

static void sdl_reset_keys(PC *pc)
{
	int i;
	for(i = 1; i <= KEYCODE_MAX; i++) {
		if (key_pressed[i]) {
			ps2_put_keycode(pc->kbd, 0, i);
			key_pressed[i] = 0;
		}
	}
}

static void sdl_handle_key_event(const SDL_KeyboardEvent *ev, PC *pc)
{
	int keycode, keypress;

	keycode = sdl_get_keycode(ev);
	if (keycode) {
#if SDL_PATCHLEVEL < 50 /* not sdl12-compat */
		if (keycode == 0x3a || keycode ==0x45) {
			/* SDL does not generate key up for numlock & caps lock */
			ps2_put_keycode(pc->kbd, 1, keycode);
			ps2_put_keycode(pc->kbd, 0, keycode);
		} else
#endif
		{
			keypress = (ev->type == SDL_KEYDOWN);
			if (keycode <= KEYCODE_MAX)
				key_pressed[keycode] = keypress;
			ps2_put_keycode(pc->kbd, keypress, keycode);
		}
	} else if (ev->type == SDL_KEYUP) {
		/* workaround to reset the keyboard state (used when changing
		   desktop with ctrl-alt-x on Linux) */
		sdl_reset_keys(pc);
	}
}

static void sdl_send_mouse_event(PC *pc, int x1, int y1,
				 int dz, int state, bool is_absolute)
{
	int buttons, x, y;

	buttons = 0;
	if (state & SDL_BUTTON(SDL_BUTTON_LEFT))
		buttons |= (1 << 0);
	if (state & SDL_BUTTON(SDL_BUTTON_RIGHT))
		buttons |= (1 << 1);
	if (state & SDL_BUTTON(SDL_BUTTON_MIDDLE))
		buttons |= (1 << 2);
	if (is_absolute) {
		x = 0;//(x1 * 32768) / screen_width;
		y = 0;//(y1 * 32768) / screen_height;
	} else {
		x = x1;
		y = y1;
	}
	ps2_mouse_event(pc->mouse, x, y, dz, buttons);
}

static void sdl_handle_mouse_motion_event(const SDL_Event *ev, PC *pc)
{
	bool is_absolute = 0; //vm_mouse_is_absolute(m);
	int x, y;
	if (is_absolute) {
		x = ev->motion.x;
		y = ev->motion.y;
	} else {
		x = ev->motion.xrel;
		y = ev->motion.yrel;
	}
	sdl_send_mouse_event(pc, x, y, 0, ev->motion.state, is_absolute);
}

static void sdl_handle_mouse_button_event(const SDL_Event *ev, PC *pc)
{
	bool is_absolute = 0; //vm_mouse_is_absolute(m);
	int state, dz;

	dz = 0;
#if SDL_MAJOR_VERSION < 2
	if (ev->type == SDL_MOUSEBUTTONDOWN) {
		if (ev->button.button == SDL_BUTTON_WHEELUP) {
			dz = -1;
		} else if (ev->button.button == SDL_BUTTON_WHEELDOWN) {
			dz = 1;
		}
	}
#endif

	state = SDL_GetMouseState(NULL, NULL);
	/* just in case */
	if (ev->type == SDL_MOUSEBUTTONDOWN)
		state |= SDL_BUTTON(ev->button.button);
	else
		state &= ~SDL_BUTTON(ev->button.button);

	if (is_absolute) {
		sdl_send_mouse_event(pc, ev->button.x, ev->button.y,
				     dz, state, is_absolute);
	} else {
		sdl_send_mouse_event(pc, 0, 0, dz, state, is_absolute);
	}
}

/* Grab/ungrab the keyboard and mouse. Relative mouse mode is required so the
   guest receives proper movement deltas and the host pointer is exclusively
   captured (SDL_SetWindowGrab alone does not confine the pointer on macOS). */
static void sdl_set_grab(Console *s, bool grab)
{
#if SDL_MAJOR_VERSION >= 2
	SDL_SetRelativeMouseMode(grab ? SDL_TRUE : SDL_FALSE);
	SDL_ShowCursor(grab ? SDL_DISABLE : SDL_ENABLE);
	SDL_SetWindowGrab(s->window, grab ? SDL_TRUE : SDL_FALSE);
#else
	SDL_ShowCursor(grab ? SDL_DISABLE : SDL_ENABLE);
	SDL_WM_GrabInput(grab ? SDL_GRAB_ON : SDL_GRAB_OFF);
#endif
	s->grabbed = grab;
}

static void poll(void *opaque)
{
	Console *s = opaque;
	SDL_Event ev;
	int keycode;

	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_KEYDOWN:
			keycode = sdl_get_keycode(&(ev.key));
			if (keycode == 0x1a && key_pressed[0x1d]) {
				s->osd_enabled = !s->osd_enabled;
				osd_attach_emulink(s->osd, s->pc->emulink);
				osd_attach_ide(s->osd, s->pc->ide, s->pc->ide2);
				s->pc->full_update = s->osd_enabled ? 1 : 2;
				break;
			}
			if (keycode == 0x1b && key_pressed[0x1d]) {
				sdl_set_grab(s, !s->grabbed);
				break;
			}
			/* fall through */
		case SDL_KEYUP:
			if (s->osd_enabled)
				osd_handle_key(s->osd, sdl_get_keycode(&(ev.key)),
					       ev.type == SDL_KEYDOWN);
			else
				sdl_handle_key_event(&(ev.key), s->pc);
			break;
		case SDL_MOUSEMOTION:
			if (s->osd_enabled)
				osd_handle_mouse_motion(s->osd,
							ev.motion.x, ev.motion.y);
			else
				sdl_handle_mouse_motion_event(&ev, s->pc);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if (s->osd_enabled)
				osd_handle_mouse_button(
					s->osd,
					ev.button.x, ev.button.y,
					ev.type == SDL_MOUSEBUTTONDOWN,
					1 /* XXX */);
			else
				sdl_handle_mouse_button_event(&ev, s->pc);
			break;
#if SDL_MAJOR_VERSION >= 2
		case SDL_MOUSEWHEEL: {
			/* SDL >= 2.0.18: on macOS (trackpad/Magic Mouse) and other
			   hi-res wheels the integer ev.wheel.y is truncated to 0 and
			   the real delta only appears in ev.wheel.preciseY.  Accumulate
			   the fractional amount and emit one PS/2 step per notch. */
			float wy;
#if SDL_VERSION_ATLEAST(2, 0, 18)
			wy = ev.wheel.preciseY;
			if (wy == 0.0f)
				wy = (float)ev.wheel.y;
#else
			wy = (float)ev.wheel.y;
#endif
			if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
				wy = -wy;
			s->wheel_accum += wy;
			int dz = 0;
			while (s->wheel_accum >= 1.0f) {
				dz -= 1; /* wheel up -> negative IMPS/2 z */
				s->wheel_accum -= 1.0f;
			}
			while (s->wheel_accum <= -1.0f) {
				dz += 1; /* wheel down -> positive IMPS/2 z */
				s->wheel_accum += 1.0f;
			}
			if (dz != 0)
				sdl_send_mouse_event(s->pc, 0, 0, dz,
						     SDL_GetMouseState(NULL, NULL), 0);
			break;
		}
		case SDL_WINDOWEVENT:
			/* release the pointer when the window loses focus so the user
			   is never trapped in relative mouse mode */
			if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST && s->grabbed)
				sdl_set_grab(s, false);
			break;
#endif
		case SDL_QUIT:
			exit(0);
		}
	}
}

/* Put host thread into sleep (SDL_WaitEvent) while the guest APM is suspended/standby.
   Wakes up immediately upon keyboard or mouse activity and clears pc->apm_suspend. */
static void console_suspend(Console *s)
{
	PC *pc = s->pc;
	fprintf(stderr, "[PM] Guest APM %s: host entering sleep (SDL_WaitEvent)...\n",
		pc->apm_suspend == 2 ? "Suspend" : "Standby");

	/* Pause audio output so the mixer thread does not loop old buffers during standby */
	SDL_PauseAudio(1);

	while (pc->apm_suspend && pc->shutdown_state != 8) {
		SDL_Event ev;
		if (!SDL_WaitEvent(&ev))
			break;

		switch (ev.type) {
		case SDL_QUIT:
			exit(0);

		case SDL_KEYDOWN:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEMOTION:
#if SDL_MAJOR_VERSION >= 2
		case SDL_MOUSEWHEEL:
#endif
			/* User activity detected: wake up guest */
			pc->apm_suspend = 0;
			fprintf(stderr, "[PM] Guest wake event (type=0x%x), resuming emulation...\n", ev.type);
			/* Put the waking event back into queue so normal poll() handles it */
			SDL_PushEvent(&ev);
			break;

#if SDL_MAJOR_VERSION >= 2
		case SDL_WINDOWEVENT:
			if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
			    ev.window.event == SDL_WINDOWEVENT_RESTORED) {
				redraw(s, 0, 0, s->width, s->height);
			} else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST && s->grabbed) {
				sdl_set_grab(s, false);
			}
			break;
#endif

		default:
			break;
		}
	}

	/* Resume audio output */
	SDL_PauseAudio(0);
}

void console_set_audio(Console *console)
{
	SDL_AudioSpec audio_spec = {0};
	audio_spec.freq = 44100;
	audio_spec.format = AUDIO_S16SYS;
	audio_spec.channels = 2;
	audio_spec.samples = 512;
	audio_spec.callback = mixer_callback;
	audio_spec.userdata = console->pc;
	SDL_OpenAudio(&audio_spec, 0);
	SDL_PauseAudio(0);
}

u8 *console_get_fb(Console *console)
{
	return console->screen->pixels;
}

int main(int argc, char *argv[])
{
	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	conf.mem_size = 8 * 1024 * 1024;
	conf.vga_mem_size = 256 * 1024;
	conf.width = 640;
	conf.height = 480;
	conf.cpu_gen = 4;
	conf.fpu = 0;


	const char *argv1;
	bool enable_kvm = false;
	if (argc == 2) {
		argv1 = argv[1];
	} else if (argc == 3) {
		if (strcmp(argv[1], "-kvm") == 0)
			enable_kvm = true;
		argv1 = argv[2];
		ne2000_set_config_file(argv1);
	} else {
		return 1;
	}

	int err = ini_parse(argv1, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "error %d\n", err);
		return err;
	}
	if (enable_kvm)
		conf.cpu_gen = -1;

	Console *console = console_init(conf.width, conf.height);
	u8 *fb = console_get_fb(console);
	PC *pc = pc_new(redraw, console, fb, &conf);
	console->pc = pc;
	osd_attach_pc(console->osd, pc);
	osd_attach_emulink(console->osd, pc->emulink);
	osd_attach_ide(console->osd, pc->ide, pc->ide2);
	console_set_audio(console);

	load_bios_and_reset(pc);

	pc->boot_start_time = get_uticks();
	for (; pc->shutdown_state != 8;) {
		pc_step(pc);
		poll(console);
		pc_vga_step(pc);
		if (pc->apm_suspend)
			console_suspend(console);
		if (pc_guest_powered_off(pc))
			break;
	}
	return 0;
}
