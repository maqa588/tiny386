#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include "pc.h"
#include "jit.h"

#define FIFO_VGA "/tmp/tiny386_vga"
#define FIFO_KBD "/tmp/tiny386_kbd"

void rv32_syscalls_init(void);
void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int sys_ftruncate(int fd, off_t length);
int sys_rt_sigaction(int sig, void *act, void *oldact);
struct timespec64 {
	long long tv_sec;
	long long tv_nsec;
};
int sys_clock_gettime(int clk_id, struct timespec64 *ts);

/* Set by the SIGINT/SIGTERM handler so the CPU loop can exit on Ctrl+C. */
static volatile sig_atomic_t s_stop = 0;

static void rv32_sig_handler(int sig)
{
	(void)sig;
	s_stop = 1;
}

/* Install a handler using the raw rt_sigaction syscall (the newlib toolchain
   used for this target has no working signal() glue). */
static void install_sig_handler(int sig, void (*handler)(int))
{
	struct {
		void *handler;
		unsigned long flags;
		void *restorer;
		uint32_t mask[2];
	} sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = (void *)handler;
	sys_rt_sigaction(sig, &sa, NULL);
}

uint32_t get_uticks(void)
{
	struct timespec64 ts;
	sys_clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
	return ((uint32_t) ts.tv_sec * 1000000 + (uint32_t)(ts.tv_nsec / 1000));
}

void *bigmalloc(size_t size)
{
	void *ptr = sys_mmap(NULL, size, 3 /* PROT_READ|PROT_WRITE */, 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
	if ((long)ptr < 0 && (long)ptr >= -4096) {
		ptr = malloc(size);
	}
	return ptr;
}

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
		fread((char *)phys_mem + addr - len, 1, len, fp);
	else
		fread((char *)phys_mem + addr, 1, len, fp);
	fclose(fp);
	return len;
}

static int s_vga_fd = -1;
static int s_kbd_fd = -1;
static uint8_t *s_fb = NULL;
static size_t s_frame_bytes = 800 * 480 * 4;

#define SHM_FB_FILE "/dev/shm/tiny386_fb"
#define SHM_FB_MAGIC 0x54333836

typedef struct {
	volatile uint32_t magic;
	volatile uint32_t width;
	volatile uint32_t height;
	volatile uint32_t stride;
	volatile uint32_t frame_seq; /* seqlock: odd while the framebuffer is being drawn */
	uint8_t  pixels[800 * 480 * 4];
} Tiny386ShmFB;

static Tiny386ShmFB *s_shm_fb = NULL;

/* Called just before the VGA starts modifying the framebuffer: mark it busy
   (odd sequence) so the front-end will not sample a half-drawn frame. */
static void frame_begin(void *opaque)
{
	(void)opaque;
	if (s_shm_fb)
		s_shm_fb->frame_seq++;
}

/* Called once the framebuffer is consistent again: mark it stable (even). */
static void frame_end(void *opaque)
{
	(void)opaque;
	if (s_shm_fb) {
		s_shm_fb->frame_seq++;
	} else if (s_vga_fd >= 0 && s_fb) {
		size_t total = 0;
		while (total < s_frame_bytes) {
			ssize_t n = write(s_vga_fd, s_fb + total, s_frame_bytes - total);
			if (n > 0) {
				total += n;
			} else {
				close(s_vga_fd);
				s_vga_fd = -1;
				break;
			}
		}
	}
}

static void redraw(void *opaque, int x, int y, int w, int h)
{
	(void)opaque; (void)x; (void)y; (void)w; (void)h;
}

int main(int argc, char *argv[])
{
	rv32_syscalls_init();
	jit_init();

	/* Ctrl+C / kill must terminate the emulator cleanly. */
	install_sig_handler(SIGINT, rv32_sig_handler);
	install_sig_handler(SIGTERM, rv32_sig_handler);

	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	conf.mem_size = 16 * 1024 * 1024;
	conf.vga_mem_size = 2 * 1024 * 1024;
	conf.width = 800;
	conf.height = 480;
	conf.cpu_gen = 4;
	conf.fpu = 0;

	bool enable_jit = true;
	const char *inifile = "tiny386_pc.ini";
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-jit") == 0) {
			enable_jit = false;
		} else {
			inifile = argv[i];
		}
	}
	jit_enable(enable_jit);

	int err = ini_parse(inifile, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "ini_parse(%s) error %d\n", inifile, err);
		return err;
	}

	s_frame_bytes = conf.width * conf.height * 4;

	/* Setup Zero-Copy Shared Memory Framebuffer */
	int shm_fd = open(SHM_FB_FILE, O_RDWR | O_CREAT, 0666);
	if (shm_fd >= 0) {
		sys_ftruncate(shm_fd, sizeof(Tiny386ShmFB));
		void *ptr = sys_mmap(NULL, sizeof(Tiny386ShmFB), 3 /* PROT_READ|PROT_WRITE */, 1 /* MAP_SHARED */, shm_fd, 0);
		if ((long)ptr > 0 && (long)ptr < 0x7fffffff) {
			s_shm_fb = (Tiny386ShmFB *)ptr;
			s_shm_fb->magic = SHM_FB_MAGIC;
			s_shm_fb->width = conf.width;
			s_shm_fb->height = conf.height;
			s_shm_fb->stride = conf.width * 4;
			s_shm_fb->frame_seq = 0;
			s_fb = s_shm_fb->pixels;
			memset(s_fb, 0, s_frame_bytes);
			fprintf(stderr, "[RV32] Zero-Copy Shared Memory Framebuffer ready: %s (%dx%d, ptr=%p)\n",
				SHM_FB_FILE, conf.width, conf.height, (void *)s_shm_fb);
		}
	}

	if (!s_fb) {
		s_fb = bigmalloc(s_frame_bytes);
		memset(s_fb, 0, s_frame_bytes);
		fprintf(stderr, "[RV32] Falling back to local memory framebuffer\n");
		s_vga_fd = open(FIFO_VGA, O_WRONLY | O_NONBLOCK);
	}

	PC *pc = pc_new(redraw, NULL, s_fb, &conf);
	vga_set_frame_notify(pc->vga, frame_begin, frame_end, NULL);
	load_bios_and_reset(pc);

	/* Open keyboard FIFO in non-blocking mode */
	s_kbd_fd = open(FIFO_KBD, O_RDONLY | O_NONBLOCK);

	if (enable_jit) {
		printf("[RV32] tiny386_rv32 initialized. Starting CPU loop with RV32 JIT ENABLED!\n");
	} else {
		printf("[RV32] tiny386_rv32 initialized. Starting CPU loop in pure INTERPRETER mode (JIT disabled)!\n");
	}

	pc->boot_start_time = get_uticks();
	for (; pc->shutdown_state != 8 && !s_stop;) {
		/* Process host keyboard events from FIFO */
		if (s_kbd_fd >= 0) {
			uint8_t pkt[2];
			ssize_t n = read(s_kbd_fd, pkt, 2);
			if (n == 2) {
				ps2_put_keycode(pc->kbd, pkt[0], pkt[1]);
			}
		} else {
			s_kbd_fd = open(FIFO_KBD, O_RDONLY | O_NONBLOCK);
		}

		pc_step(pc);
		pc_vga_step(pc);

	}

	if (s_stop)
		fprintf(stderr, "\n[RV32] Interrupted, shutting down.\n");

	return 0;
}
