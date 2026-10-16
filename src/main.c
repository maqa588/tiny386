// "headless" tiny386
// for SDL port, see `sdl/main.c`
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pc.h"
#include "jit.h"

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

	/* Run the same binary in pure-interpreter mode for A/B measurements:
	 * TINY386_NO_JIT=1 disables the JIT at runtime, so both sides of the
	 * comparison share identical code generation and build flags. */
	if (getenv("TINY386_NO_JIT"))
		jit_enable(false);

	pc->boot_start_time = get_uticks();
	for (; g_running && pc->shutdown_state != 8;) {
		pc_step(pc);
		pc_vga_step(pc);
		if (getenv("TINY386_AUTO_ENTER")) {
			static int s_auto_stage = 0;
			/* The GUI half of the script (mouse moves, clicks, Win+E) is opt-in:
			 * plain TINY386_AUTO_ENTER only dismisses the logon prompt so that a
			 * clean desktop (with any auto-started app window) can be inspected. */
			bool auto_gui = getenv("TINY386_AUTO_GUI") != NULL;
			uint32_t elapsed = get_uticks() - pc->boot_start_time;
			int t_target = atoi(getenv("TINY386_AUTO_ENTER"));
			int login_period = getenv("TINY386_AUTO_LOGIN_PERIOD") ?
				atoi(getenv("TINY386_AUTO_LOGIN_PERIOD")) : 0;
			if (login_period > 0) {
				/* Robust logon handling: the Windows/network logon dialog
				 * appears at a highly variable point in the boot, so instead
				 * of one hard-coded moment, pulse Enter at a fixed interval
				 * across a wide window to dismiss it whenever it shows up. */
				static int s_pulse_on = 0;
				static int s_pulse_count = 0;
				static uint32_t s_pulse_next = 0;
				if (elapsed >= (uint32_t)t_target * 1000000 &&
				    elapsed < (uint32_t)(t_target + 90) * 1000000 &&
				    s_pulse_count < 40) {
					if (!s_pulse_on && elapsed >= s_pulse_next) {
						s_pulse_on = 1;
						s_pulse_next = elapsed + 100000;
						fprintf(stderr, "[AUTO-INPUT] login Enter down #%d t=%us...\n",
							s_pulse_count + 1, (unsigned)(elapsed / 1000000));
						ps2_put_keycode(pc->kbd, 1, 0x1c);
					} else if (s_pulse_on && elapsed >= s_pulse_next) {
						s_pulse_on = 0;
						s_pulse_count++;
						s_pulse_next = elapsed + (uint32_t)login_period * 1000000;
						ps2_put_keycode(pc->kbd, 0, 0x1c);
					}
				}
			} else if (s_auto_stage == 0 && elapsed >= (uint32_t)t_target * 1000000) {
				s_auto_stage = 1;
				fprintf(stderr, "[AUTO-INPUT] Enter #1 (Login) make at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_put_keycode(pc->kbd, 1, 0x1c);
			} else if (s_auto_stage == 1 && elapsed >= (uint32_t)t_target * 1000000 + 200000) {
				s_auto_stage = 2;
				fprintf(stderr, "[AUTO-INPUT] Enter #1 (Login) break at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_put_keycode(pc->kbd, 0, 0x1c);
			} else if (s_auto_stage == 2 && elapsed >= (uint32_t)(t_target + 8) * 1000000) {
				s_auto_stage = 3;
				fprintf(stderr, "[AUTO-INPUT] Enter #2 (Dismiss) make at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_put_keycode(pc->kbd, 1, 0x1c);
			} else if (s_auto_stage == 3 && elapsed >= (uint32_t)(t_target + 8) * 1000000 + 200000) {
				s_auto_stage = 4;
				fprintf(stderr, "[AUTO-INPUT] Enter #2 (Dismiss) break at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_put_keycode(pc->kbd, 0, 0x1c);
			} else if (auto_gui && s_auto_stage >= 4 && s_auto_stage < 14 && elapsed >= (uint32_t)(t_target + 10) * 1000000 + (s_auto_stage - 4) * 100000) {
				int step = s_auto_stage - 4;
				s_auto_stage++;
				fprintf(stderr, "[AUTO-INPUT] Mouse move step %d/10 at t=%us...\n", step + 1, (unsigned)(elapsed / 1000000));
				ps2_mouse_event(pc->mouse, -27, -19, 0, 0);
			} else if (auto_gui && s_auto_stage == 14 && elapsed >= (uint32_t)(t_target + 12) * 1000000) {
				s_auto_stage = 15;
				fprintf(stderr, "[AUTO-INPUT] Mouse Click #1 down at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_mouse_event(pc->mouse, 0, 0, 0, 1);
			} else if (auto_gui && s_auto_stage == 15 && elapsed >= (uint32_t)(t_target + 12) * 1000000 + 100000) {
				s_auto_stage = 16;
				fprintf(stderr, "[AUTO-INPUT] Mouse Click #1 up at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_mouse_event(pc->mouse, 0, 0, 0, 0);
			} else if (auto_gui && s_auto_stage == 16 && elapsed >= (uint32_t)(t_target + 12) * 1000000 + 200000) {
				s_auto_stage = 17;
				fprintf(stderr, "[AUTO-INPUT] Mouse Click #2 down at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_mouse_event(pc->mouse, 0, 0, 0, 1);
			} else if (auto_gui && s_auto_stage == 17 && elapsed >= (uint32_t)(t_target + 12) * 1000000 + 300000) {
				s_auto_stage = 18;
				fprintf(stderr, "[AUTO-INPUT] Mouse Click #2 up at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_mouse_event(pc->mouse, 0, 0, 0, 0);
			} else if (auto_gui && s_auto_stage == 18 && elapsed >= (uint32_t)(t_target + 15) * 1000000) {
				s_auto_stage = 19;
				fprintf(stderr, "[AUTO-INPUT] Win+E make at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_put_keycode(pc->kbd, 1, 0xe05b);
				ps2_put_keycode(pc->kbd, 1, 0x12);
			} else if (auto_gui && s_auto_stage == 19 && elapsed >= (uint32_t)(t_target + 15) * 1000000 + 200000) {
				s_auto_stage = 20;
				fprintf(stderr, "[AUTO-INPUT] Win+E break at t=%us...\n", (unsigned)(elapsed / 1000000));
				ps2_put_keycode(pc->kbd, 0, 0x12);
				ps2_put_keycode(pc->kbd, 0, 0xe05b);
			}
		}
		if (getenv("TINY386_DUMP_BDA")) {
			static uint32_t last_bda = 0;
			uint32_t now = get_uticks();
			if (now - last_bda >= 5000000) {
				last_bda = now;
				fprintf(stderr, "BDA @%us:\n",
					(unsigned)((now - pc->boot_start_time) / 1000000));
				for (int a = 0x400; a < 0x500; a += 16) {
					fprintf(stderr, "  %04x:", a);
					for (int i = 0; i < 16; i++)
						fprintf(stderr, " %02x",
							(unsigned char)pc->phys_mem[a + i]);
					fprintf(stderr, "\n");
				}
				fprintf(stderr, "  IVT int11=%04x:%04x int15=%04x:%04x\n",
					*(uint16_t *)(pc->phys_mem + 0x40),
					*(uint16_t *)(pc->phys_mem + 0x42),
					*(uint16_t *)(pc->phys_mem + 0x54),
					*(uint16_t *)(pc->phys_mem + 0x56));
			}
		}
		if (getenv("TINY386_DUMP_RAM")) { /* TEMP-RAMDUMP: debug only */
			static uint32_t last_ram = 0;
			uint32_t now = get_uticks();
			if (now - last_ram >= 30000000) {
				last_ram = now;
				FILE *fp = fopen(getenv("TINY386_DUMP_RAM"), "wb");
				if (fp) {
					fwrite(pc->phys_mem, 1, conf.mem_size, fp);
					fclose(fp);
					fprintf(stderr, "RAM dumped at %us\n",
						(unsigned)((now - pc->boot_start_time) / 1000000));
				}
			}
		}
		if (pc_guest_powered_off(pc)) {
			fprintf(stderr, "[EXIT] powered_off t=%us\n",
				(unsigned)((get_uticks() - pc->boot_start_time) / 1000000));
			break;
		}
		if (use_term)
			term_step(term);
	}
	fprintf(stderr, "[DONE] shutdown_state=%d\n", pc->shutdown_state);
	return 0;
}
