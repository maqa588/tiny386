/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "../../ini.h"
#include "../../pc.h"
#include "common.h"

//
#include "esp_private/system_internal.h"
uint32_t get_uticks()
{
	return esp_system_get_time();
}

void *psmalloc(long size);
void *fbmalloc(long size);
void *bigmalloc(size_t size)
{
	return psmalloc(size);
}

static char *psram;
static long psram_off;
static long psram_len;

static char *pcram;
static long pcram_off;
static long pcram_len;
void *pcmalloc(long size)
{
	void *ret = pcram + pcram_off;

	size = (size + 31) / 32 * 32;
	if (pcram_off + size > pcram_len) {
		fprintf(stderr, "pcram error %ld %ld %ld\n", size, pcram_off, pcram_len);
		abort();
	}
	pcram_off += size;
	return ret;
}

void pcmalloc_init(void *ptr, long len)
{
	pcram = ptr;
	pcram_len = len;
}

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	if (file && file[0] == '/') {
		FILE *fp = fopen(file, "rb");
		assert(fp);
		fseek(fp, 0, SEEK_END);
		int len = ftell(fp);
		fprintf(stderr, "%s len %d\n", file, len);
		rewind(fp);
		if (backward)
			fread(phys_mem + addr - len, 1, len, fp);
		else
			fread(phys_mem + addr, 1, len, fp);
		fclose(fp);
		return len;
	}
	const esp_partition_t *part =
		esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
					 ESP_PARTITION_SUBTYPE_ANY,
					 file);
	assert(part);
	int len = part->size;
	fprintf(stderr, "%s len %d\n", file, len);
	if (backward)
		esp_partition_read(part, 0, phys_mem + addr - len, len);
	else
		esp_partition_read(part, 0, phys_mem + addr, len);
	return len;
}

//
EventGroupHandle_t global_event_group;
struct Globals globals;

typedef struct {
	PC *pc;
	u8 *fb;
} Console;

Console *console_init(int width, int height)
{
	Console *c = malloc(sizeof(Console));
	assert(c);
#if defined(USE_PPA_2D)
	/* PPA 2D Hardware Acceleration:
	 * Allocate an aligned linear buffer (800x480 RGB565) in PSRAM.
	 * 128-byte alignment perfectly aligns with ESP32-P4 L2 cache line
	 * and PPA_DATA_BURST_LENGTH_128 AXI DMA burst boundary. */
	c->fb = heap_caps_aligned_alloc(128, LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!c->fb) c->fb = bigmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
	globals.vga_fb = c->fb;
#else
	if (globals.panel_fb) {
		/* Zero-copy: VGA renders directly into MIPI-DSI DMA frame buffer */
		c->fb = globals.panel_fb;
	} else {
		c->fb = bigmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
	}
#endif
	return c;
}

static void redraw(void *opaque,
		   int x, int y, int w, int h)
{
	(void)opaque; (void)x; (void)y; (void)w; (void)h;
	/* Fully decoupled non-blocking dirty flag:
	 * Core 0 flags the screen dirty and immediately continues x86 CPU execution.
	 * Core 1 asynchronously executes D-Cache msync and hardware MIPI-DSI DMA push. */
	globals.vga_dirty = 1;
}

static int pc_main(const char *file)
{
	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	/* ESP32-P4 High-Performance Configuration: 16MB Guest RAM, 2MB VGA VRAM */
	conf.mem_size = 16 * 1024 * 1024;
	conf.vga_mem_size = 2 * 1024 * 1024;
	conf.cpu_gen = 4;
	conf.fpu = 0;

	int err = ini_parse(file, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "error %d\n", err);
		return err;
	}

	if (conf.width != LCD_WIDTH || conf.height != LCD_HEIGHT) {
		fprintf(stderr, "fixing width/height mismatch %dx%d => %dx%d\n",
			conf.width, conf.height, LCD_WIDTH, LCD_HEIGHT);
		conf.width = LCD_WIDTH;
		conf.height = LCD_HEIGHT;
	}

	Console *console = console_init(conf.width, conf.height);
	PC *pc = pc_new(redraw, console, console->fb, &conf);
	console->pc = pc;
	globals.pc = pc;
	globals.kbd = pc->kbd;
	globals.mouse = pc->mouse;
	xEventGroupSetBits(global_event_group, BIT0);

	/* PSRAM and Internal SRAM Watermark Monitoring */
	size_t spiram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
	size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	size_t spiram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
	size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	size_t sram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

	ESP_LOGI("MEM", "==================================================");
	ESP_LOGI("MEM", "Guest PC Memory Initialized (RAM: %u MB, VRAM: %u MB)",
	         (unsigned)(conf.mem_size / (1024 * 1024)),
	         (unsigned)(conf.vga_mem_size / (1024 * 1024)));
	ESP_LOGI("MEM", "PSRAM Total: %u KB, Free: %u KB, Min Free: %u KB",
	         (unsigned)(spiram_total / 1024),
	         (unsigned)(spiram_free / 1024),
	         (unsigned)(spiram_min_free / 1024));
	ESP_LOGI("MEM", "Internal SRAM Free: %u KB, Min Free: %u KB",
	         (unsigned)(sram_free / 1024),
	         (unsigned)(sram_min_free / 1024));
	ESP_LOGI("MEM", "PSRAM Pool: Allocated %ld KB / %ld KB | Remaining: %ld KB for JIT & Net",
	         psram_off / 1024, psram_len / 1024, (psram_len - psram_off) / 1024);
	ESP_LOGI("MEM", "==================================================");

	load_bios_and_reset(pc);

	pc->boot_start_time = get_uticks();
	for (; pc->shutdown_state != 8;) {
		pc_step(pc);
	}
	return 0;
}

//

void *esp_psram_get(size_t *size);
void vga_task(void *arg);
void i2s_main();
void wifi_main(const char *, const char *);
void storage_init(void);
void usb_setup(void);

struct esp_ini_config {
	const char *filename;
	char ssid[16];
	char pass[32];
	int enable_usb;
};

static void i386_task(void *arg)
{
	struct esp_ini_config *config = arg;
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "x86 CPU emulation runs dedicated on Core %d\n", core_id);
	/* Wait for LCD panel (and panel_fb) to be ready before starting the
	 * PC emulator.  console_init() uses globals.panel_fb if set. */
	xEventGroupWaitBits(global_event_group,
	                    BIT1,
	                    pdFALSE,
	                    pdFALSE,
	                    portMAX_DELAY);
	pc_main(config->filename);
	vTaskDelete(NULL);
}

void *psmalloc(long size)
{
	void *ret = psram + psram_off;

	size = (size + 4095) / 4096 * 4096;
	if (psram_off + size > psram_len) {
		fprintf(stderr, "psram error %ld %ld %ld\n", size, psram_off, psram_len);
		abort();
	}
	psram_off += size;
	return ret;
}

void *fbmalloc(long size)
{
	void *fb = (uint8_t *) heap_caps_calloc(1, size, MALLOC_CAP_DMA);
	if (!fb) {
		fprintf(stderr, "fbmalloc error %ld\n", size);
		abort();
	}
	return fb;
}

static int parse_ini(void* user, const char* section,
		     const char* name, const char* value)
{
	struct esp_ini_config *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	if (SEC("esp")) {
		if (NAME("ssid")) {
			if (strlen(value) < 32)
				strcpy(conf->ssid, value);
		} else if (NAME("pass")) {
			if (strlen(value) < 64)
				strcpy(conf->pass, value);
		} else if (NAME("enable_usb")) {
			conf->enable_usb = atoi(value);
		}
	}
#undef SEC
#undef NAME
	return 1;
}

void app_main(void)
{
	global_event_group = xEventGroupCreate();

#ifdef ESPDEBUG
	uart_config_t uart_config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity	= UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	uart_param_config(UART_NUM_0, &uart_config);
	if (uart_driver_install(UART_NUM_0, 2 * 1024, 0, 0, NULL, 0) != ESP_OK) {
		assert(false);
	}
#endif

	i2s_main();
	storage_init();

	esp_psram_init();
#ifndef PSRAM_ALLOC_LEN
	// use the whole psram
	size_t len;
	psram = esp_psram_get(&len);
	psram_len = len;
#else
	psram_len = PSRAM_ALLOC_LEN;
	size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	if (free_spiram > 0 && psram_len > free_spiram - 2 * 1024 * 1024) {
		psram_len = (free_spiram > 4 * 1024 * 1024) ? (free_spiram - 2 * 1024 * 1024) : (free_spiram / 2);
	}
	psram = heap_caps_calloc(1, psram_len, MALLOC_CAP_SPIRAM);
#endif

	const static char *files[] = {
		"/sdcard/tiny386.ini",
		"/spiflash/tiny386.ini",
		NULL,
	};
	static struct esp_ini_config config;
	for (int i = 0; files[i]; i++) {
		if (ini_parse(files[i], parse_ini, &config) == 0) {
			config.filename = files[i];
			break;
		}
	}

	if (config.enable_usb) {
		vTaskDelay(2000 / portTICK_PERIOD_MS);
		usb_setup();
	}

	if (config.ssid[0]) {
		wifi_main(config.ssid, config.pass);
	}

	if (psram) {
		/* Core 0: Dedicated pure CPU emulation (highest priority 5) */
		xTaskCreatePinnedToCore(i386_task, "i386_main", 8192, &config, 5, NULL, 0);
		/* Core 1: VGA refresh, MIPI-DSI DMA, and background I/O (priority 2) */
		xTaskCreatePinnedToCore(vga_task, "vga_task", 4096, NULL, 2, NULL, 1);
	}
}
