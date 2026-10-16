#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <poll.h>

#if defined(__has_include)
# if __has_include(<SDL2/SDL.h>)
#  include <SDL2/SDL.h>
# elif __has_include(<SDL.h>)
#  include <SDL.h>
# else
#  include <SDL2/SDL.h>
# endif
#else
# include <SDL2/SDL.h>
#endif

#define GL_SILENCE_DEPRECATION 1
#if defined(__has_include)
# if __has_include(<GL/gl.h>)
#  define USE_OPENGL 1
#  include <GL/gl.h>
# elif __has_include(<OpenGL/gl.h>)
#  define USE_OPENGL 1
#  include <OpenGL/gl.h>
# endif
#endif

#define WIDTH 800
#define HEIGHT 480
#define FRAME_SIZE (WIDTH * HEIGHT * 4)

#define FIFO_VGA "/tmp/tiny386_vga"
#define FIFO_KBD "/tmp/tiny386_kbd"

static const uint8_t sdl2_scancode_to_at[512] = {
	[SDL_SCANCODE_A] = 0x1E, [SDL_SCANCODE_B] = 0x30, [SDL_SCANCODE_C] = 0x2E,
	[SDL_SCANCODE_D] = 0x20, [SDL_SCANCODE_E] = 0x12, [SDL_SCANCODE_F] = 0x21,
	[SDL_SCANCODE_G] = 0x22, [SDL_SCANCODE_H] = 0x23, [SDL_SCANCODE_I] = 0x17,
	[SDL_SCANCODE_J] = 0x24, [SDL_SCANCODE_K] = 0x25, [SDL_SCANCODE_L] = 0x26,
	[SDL_SCANCODE_M] = 0x32, [SDL_SCANCODE_N] = 0x31, [SDL_SCANCODE_O] = 0x18,
	[SDL_SCANCODE_P] = 0x19, [SDL_SCANCODE_Q] = 0x10, [SDL_SCANCODE_R] = 0x13,
	[SDL_SCANCODE_S] = 0x1F, [SDL_SCANCODE_T] = 0x14, [SDL_SCANCODE_U] = 0x16,
	[SDL_SCANCODE_V] = 0x2F, [SDL_SCANCODE_W] = 0x11, [SDL_SCANCODE_X] = 0x2D,
	[SDL_SCANCODE_Y] = 0x15, [SDL_SCANCODE_Z] = 0x2C,

	[SDL_SCANCODE_1] = 0x02, [SDL_SCANCODE_2] = 0x03, [SDL_SCANCODE_3] = 0x04,
	[SDL_SCANCODE_4] = 0x05, [SDL_SCANCODE_5] = 0x06, [SDL_SCANCODE_6] = 0x07,
	[SDL_SCANCODE_7] = 0x08, [SDL_SCANCODE_8] = 0x09, [SDL_SCANCODE_9] = 0x0A,
	[SDL_SCANCODE_0] = 0x0B,

	[SDL_SCANCODE_RETURN] = 0x1C, [SDL_SCANCODE_ESCAPE] = 0x01,
	[SDL_SCANCODE_BACKSPACE] = 0x0E, [SDL_SCANCODE_TAB] = 0x0F,
	[SDL_SCANCODE_SPACE] = 0x39,

	[SDL_SCANCODE_MINUS] = 0x0C, [SDL_SCANCODE_EQUALS] = 0x0D,
	[SDL_SCANCODE_LEFTBRACKET] = 0x1A, [SDL_SCANCODE_RIGHTBRACKET] = 0x1B,
	[SDL_SCANCODE_BACKSLASH] = 0x2B, [SDL_SCANCODE_NONUSHASH] = 0x2B,
	[SDL_SCANCODE_SEMICOLON] = 0x27, [SDL_SCANCODE_APOSTROPHE] = 0x28,
	[SDL_SCANCODE_GRAVE] = 0x29, [SDL_SCANCODE_COMMA] = 0x33,
	[SDL_SCANCODE_PERIOD] = 0x34, [SDL_SCANCODE_SLASH] = 0x35,

	[SDL_SCANCODE_CAPSLOCK] = 0x3A,

	[SDL_SCANCODE_F1] = 0x3B, [SDL_SCANCODE_F2] = 0x3C, [SDL_SCANCODE_F3] = 0x3D,
	[SDL_SCANCODE_F4] = 0x3E, [SDL_SCANCODE_F5] = 0x3F, [SDL_SCANCODE_F6] = 0x40,
	[SDL_SCANCODE_F7] = 0x41, [SDL_SCANCODE_F8] = 0x42, [SDL_SCANCODE_F9] = 0x43,
	[SDL_SCANCODE_F10] = 0x44, [SDL_SCANCODE_F11] = 0x57, [SDL_SCANCODE_F12] = 0x58,

	[SDL_SCANCODE_PRINTSCREEN] = 0x63, [SDL_SCANCODE_SCROLLLOCK] = 0x46,
	[SDL_SCANCODE_PAUSE] = 0x77, [SDL_SCANCODE_INSERT] = 0x6E,
	[SDL_SCANCODE_HOME] = 0x66, [SDL_SCANCODE_PAGEUP] = 0x68,
	[SDL_SCANCODE_DELETE] = 0x6F, [SDL_SCANCODE_END] = 0x6B,
	[SDL_SCANCODE_PAGEDOWN] = 0x6D, [SDL_SCANCODE_RIGHT] = 0x6A,
	[SDL_SCANCODE_LEFT] = 0x69, [SDL_SCANCODE_DOWN] = 0x6C,
	[SDL_SCANCODE_UP] = 0x67,

	[SDL_SCANCODE_NUMLOCKCLEAR] = 0x45, [SDL_SCANCODE_KP_DIVIDE] = 0x62,
	[SDL_SCANCODE_KP_MULTIPLY] = 0x37, [SDL_SCANCODE_KP_MINUS] = 0x4A,
	[SDL_SCANCODE_KP_PLUS] = 0x4E, [SDL_SCANCODE_KP_ENTER] = 0x60,
	[SDL_SCANCODE_KP_1] = 0x4F, [SDL_SCANCODE_KP_2] = 0x50,
	[SDL_SCANCODE_KP_3] = 0x51, [SDL_SCANCODE_KP_4] = 0x4B,
	[SDL_SCANCODE_KP_5] = 0x4C, [SDL_SCANCODE_KP_6] = 0x4D,
	[SDL_SCANCODE_KP_7] = 0x47, [SDL_SCANCODE_KP_8] = 0x48,
	[SDL_SCANCODE_KP_9] = 0x49, [SDL_SCANCODE_KP_0] = 0x52,
	[SDL_SCANCODE_KP_PERIOD] = 0x53, [SDL_SCANCODE_NONUSBACKSLASH] = 0x56,

	[SDL_SCANCODE_LCTRL] = 0x1D, [SDL_SCANCODE_LSHIFT] = 0x2A,
	[SDL_SCANCODE_LALT] = 0x38, [SDL_SCANCODE_LGUI] = 0x5B,
	[SDL_SCANCODE_RCTRL] = 0x61, [SDL_SCANCODE_RSHIFT] = 0x36,
	[SDL_SCANCODE_RALT] = 0x64, [SDL_SCANCODE_RGUI] = 0x5C,
};

static inline void render_frame(SDL_Window *win, bool use_gl, unsigned int tex, SDL_Renderer *rend, SDL_Texture *t, const void *pixels)
{
#if defined(USE_OPENGL)
	if (use_gl) {
		int w, h;
		SDL_GetWindowSize(win, &w, &h);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, WIDTH, HEIGHT, GL_BGRA, GL_UNSIGNED_BYTE, pixels);

		glViewport(0, 0, w, h);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, tex);
		glBegin(GL_QUADS);
			glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f,  1.0f);
			glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f,  1.0f);
			glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f, -1.0f);
			glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f, -1.0f);
		glEnd();
		glDisable(GL_TEXTURE_2D);

		SDL_GL_SwapWindow(win);
		return;
	}
#endif
	SDL_UpdateTexture(t, NULL, pixels, WIDTH * 4);
	SDL_RenderCopy(rend, t, NULL, NULL);
	SDL_RenderPresent(rend);
}

int main(int argc, char *argv[])
{
	mkfifo(FIFO_VGA, 0666);
	mkfifo(FIFO_KBD, 0666);

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init Error: %s\n", SDL_GetError());
		return 1;
	}

	bool use_opengl = false;
	SDL_Window *window = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Texture *texture = NULL;
	unsigned int gl_tex = 0;

#if defined(USE_OPENGL)
	const char *no_gl_env = getenv("TINY386_NO_GL");
	if (no_gl_env == NULL || atoi(no_gl_env) == 0) {
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
		SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);

		window = SDL_CreateWindow(
			"tiny386 - RV32 QEMU OpenGL 2D Viewer (800x480)",
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			WIDTH, HEIGHT, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
		if (window) {
			SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
			if (gl_ctx) {
				SDL_GL_MakeCurrent(window, gl_ctx);
				SDL_GL_SetSwapInterval(0);
				use_opengl = true;

				glGenTextures(1, &gl_tex);
				glBindTexture(GL_TEXTURE_2D, gl_tex);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, WIDTH, HEIGHT, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

				printf("[Viewer] OpenGL 2D Hardware Acceleration active! (Renderer: %s)\n",
				       (const char *)glGetString(GL_RENDERER));
			} else {
				SDL_DestroyWindow(window);
				window = NULL;
			}
		}
	}
#endif

	if (!window) {
		window = SDL_CreateWindow(
			"tiny386 - RV32 QEMU SDL2 Viewer (800x480)",
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			WIDTH, HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
		if (!window) {
			fprintf(stderr, "SDL_CreateWindow Error: %s\n", SDL_GetError());
			return 1;
		}
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
		if (!renderer) {
			renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
		}
		texture = SDL_CreateTexture(
			renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
	}

#include <sys/mman.h>

#define SHM_FB_FILE "/dev/shm/tiny386_fb"
#define SHM_FB_MAGIC 0x54333836

typedef struct {
	volatile uint32_t magic;
	volatile uint32_t width;
	volatile uint32_t height;
	volatile uint32_t stride;
	volatile uint32_t frame_seq;
	uint8_t  pixels[WIDTH * HEIGHT * 4];
} Tiny386ShmFB;

	int vga_fd = open(FIFO_VGA, O_RDWR | O_NONBLOCK);
	int kbd_fd = open(FIFO_KBD, O_RDWR | O_NONBLOCK);

	int shm_fd = open(SHM_FB_FILE, O_RDWR | O_CREAT, 0666);
	Tiny386ShmFB *shm = NULL;
	if (shm_fd >= 0) {
		ftruncate(shm_fd, sizeof(Tiny386ShmFB));
		shm = (Tiny386ShmFB *)mmap(NULL, sizeof(Tiny386ShmFB), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (shm == MAP_FAILED) shm = NULL;
	}

	uint8_t *frame_buf = malloc(FRAME_SIZE);
	memset(frame_buf, 0, FRAME_SIZE);

	printf("[Viewer] Listening for frame updates (800x480 ARGB8888)\n");

	bool running = true;
	size_t cur_bytes = 0;
	uint32_t last_seq = 0;

	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) {
				running = false;
				break;
			} else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
				if (ev.key.keysym.scancode < 512) {
					uint8_t at_code = sdl2_scancode_to_at[ev.key.keysym.scancode];
					if (at_code && kbd_fd >= 0) {
						uint8_t pkt[2];
						pkt[0] = (ev.type == SDL_KEYDOWN) ? 1 : 0;
						pkt[1] = at_code;
						write(kbd_fd, pkt, 2);
					}
				}
			}
		}

		/* 1. Prefer Shared Memory (Zero-copy, 60 FPS, No tearing/flickering) */
		if (shm == NULL) {
			shm_fd = open(SHM_FB_FILE, O_RDWR | O_CREAT, 0666);
			if (shm_fd >= 0) {
				ftruncate(shm_fd, sizeof(Tiny386ShmFB));
				shm = (Tiny386ShmFB *)mmap(NULL, sizeof(Tiny386ShmFB), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
				if (shm == MAP_FAILED) shm = NULL;
			}
		}

		if (shm && shm->magic == SHM_FB_MAGIC) {
			/* Seqlock read: the writer flips frame_seq to odd while it draws
			   into the framebuffer. Only copy when it is even and unchanged
			   during the copy, so we never present a half-drawn frame. */
			uint32_t seq = shm->frame_seq;
			if (!(seq & 1) && seq != last_seq) {
				memcpy(frame_buf, (const void *)shm->pixels, FRAME_SIZE);
				if (shm->frame_seq == seq) {
					last_seq = seq;
					render_frame(window, use_opengl, gl_tex, renderer, texture, frame_buf);
				}
			}
			SDL_Delay(16);
			continue;
		}

		/* 2. Fallback to FIFO pipe if SHM is not active */
		if (vga_fd >= 0) {
			struct pollfd pfd = { .fd = vga_fd, .events = POLLIN };
			int ret = poll(&pfd, 1, 2);
			if (ret > 0 && (pfd.revents & POLLIN)) {
				while (cur_bytes < FRAME_SIZE) {
					ssize_t n = read(vga_fd, frame_buf + cur_bytes, FRAME_SIZE - cur_bytes);
					if (n > 0) {
						cur_bytes += n;
					} else {
						break;
					}
				}
				if (cur_bytes >= FRAME_SIZE) {
					render_frame(window, use_opengl, gl_tex, renderer, texture, frame_buf);
					cur_bytes = 0;
				}
			}
		} else {
			vga_fd = open(FIFO_VGA, O_RDWR | O_NONBLOCK);
			SDL_Delay(16);
		}
	}

	close(vga_fd);
	close(kbd_fd);
	free(frame_buf);
	SDL_DestroyTexture(texture);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}
