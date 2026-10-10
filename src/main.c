// "headless" tiny386
// for SDL port, see `sdl/main.c`
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pc.h"

// platform HAL implementation
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

static uint8_t *s_fb = NULL;
static int s_fb_w = 800, s_fb_h = 480;

static void dump_screenshot(void) {
	if (!s_fb) return;
	FILE *fp = fopen("/tmp/tiny386_screen.ppm", "wb");
	if (fp) {
		fprintf(fp, "P6\n%d %d\n255\n", s_fb_w, s_fb_h);
		uint8_t rgb[s_fb_w * 3];
		for (int y = 0; y < s_fb_h; y++) {
			uint32_t *line = (uint32_t *)(s_fb + y * s_fb_w * 4);
			for (int x = 0; x < s_fb_w; x++) {
				uint32_t p = line[x];
				rgb[x * 3 + 0] = (p >> 16) & 0xff;
				rgb[x * 3 + 1] = (p >> 8) & 0xff;
				rgb[x * 3 + 2] = p & 0xff;
			}
			fwrite(rgb, 1, s_fb_w * 3, fp);
		}
		fclose(fp);
	}
}

static void redraw(void *opaque,
		   int x, int y, int w, int h)
{
	(void)opaque; (void)x; (void)y; (void)w; (void)h;
	static uint32_t last_dump = 0;
	uint32_t now = get_uticks();
	if (now - last_dump >= 1000000) { // every 1 sec
		last_dump = now;
		dump_screenshot();
	}
}

#include "term.h"
static void put_key(void *o, unsigned char scan_code, int is_pressed)
{
	ps2_put_keycode(o, is_pressed, scan_code);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [-kvm] [-term] inifile\n",
		argv0);
}

#ifndef _WIN32
#include <signal.h>
volatile int g_running = 1;
static void sig_handler(int _)
{
	g_running = 0;
}
static void set_sig_handler()
{
	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
}
#else
#define g_running 1
#define set_sig_handler()
#endif

int main(int argc, char *argv[])
{
	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	conf.mem_size = 8 * 1024 * 1024;
	conf.vga_mem_size = 256 * 1024;
	conf.width = 720;
	conf.height = 480;
	conf.cpu_gen = 4;
	conf.fpu = 0;

	const char *argv1;
	bool enable_kvm = false;
	bool use_term = false;
	if (argc > 1) {
		for (int i = 1; i < argc - 1; i++) {
			if (strcmp(argv[i], "-kvm") == 0)
				enable_kvm = true;
			else if (strcmp(argv[i], "-term") == 0)
				use_term = true;
			else {
				usage(argv[0]);
				return 1;
			}
		}
		argv1 = argv[argc - 1];
		ne2000_set_config_file(argv1);
	} else {
		usage(argv[0]);
		return 1;
	}

	int err = ini_parse(argv1, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "error %d\n", err);
		return err;
	}
	if (enable_kvm)
		conf.cpu_gen = -1;

	set_sig_handler();

	s_fb_w = conf.width;
	s_fb_h = conf.height;
	void *fb = bigmalloc(conf.width * conf.height * 4);
	s_fb = (uint8_t *)fb;
	PC *pc = pc_new(redraw, NULL, fb, &conf);
	Term *term = NULL;
	if (use_term)
		term = term_init(pc->vga, put_key, pc->kbd);
	load_bios_and_reset(pc);

	pc->boot_start_time = get_uticks();
	for (; g_running && pc->shutdown_state != 8;) {
		pc_step(pc);
		pc_vga_step(pc);
		if (use_term)
			term_step(term);
	}
	return 0;
}
