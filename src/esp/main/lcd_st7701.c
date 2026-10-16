#ifdef USE_LCD_ST7701
// Based on: https://github.com/csvke/esp32_p4_jc4880p433c_bsp
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bsp_display.c
 * @brief Display hardware abstraction for JC4880P433C board
 * 
 * This module handles the ST7701 LCD controller via MIPI DSI interface,
 * backlight control, and direct DMA framebuffer exposure for zero-copy VGA rendering.
 * 
 * Hardware:
 * - LCD: 480x800 IPS panel with ST7701 controller
 * - Interface: MIPI DSI (2 data lanes)
 * - Backlight: PWM controlled via LEDC
 * - Power: Internal LDO for DSI PHY
 */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#include "common.h"

#define CONFIG_BSP_JC4880P443C_LCD_H_RES 480
#define CONFIG_BSP_JC4880P443C_LCD_V_RES 800

#define CONFIG_BSP_JC4880P443C_DSI_LANE_BITRATE 500
#define CONFIG_BSP_JC4880P443C_DSI_PHY_LDO_CHANNEL 3
#define CONFIG_BSP_JC4880P443C_DSI_PHY_VOLTAGE_MV 2500

#define CONFIG_BSP_JC4880P443C_DPI_CLOCK_MHZ 34
#define CONFIG_BSP_JC4880P443C_LCD_BL_GPIO 23
#define CONFIG_BSP_JC4880P443C_LCD_RST_GPIO 5
#define CONFIG_BSP_JC4880P443C_NUM_FB 1
#define CONFIG_BSP_JC4880P443C_BACKLIGHT_CHANNEL 1
#define CONFIG_BSP_JC4880P443C_BACKLIGHT_TIMER 1
#define CONFIG_BSP_JC4880P443C_BACKLIGHT_PWM_FREQ 20000

/* Display configuration */
#define BSP_LCD_H_RES                   CONFIG_BSP_JC4880P443C_LCD_H_RES
#define BSP_LCD_V_RES                   CONFIG_BSP_JC4880P443C_LCD_V_RES
#define BSP_LCD_COLOR_SPACE             LCD_RGB_ELEMENT_ORDER_RGB

/* LCD GPIO pins */
#define BSP_LCD_BACKLIGHT               ((gpio_num_t)CONFIG_BSP_JC4880P443C_LCD_BL_GPIO)
#define BSP_LCD_RST                     ((gpio_num_t)CONFIG_BSP_JC4880P443C_LCD_RST_GPIO)

static const char *TAG = "lcd";

// ST7701 vendor initialization sequence for 480x800 panel
static const st7701_lcd_init_cmd_t st7701_lcd_cmds[] = {
	{0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
	{0xEF, (uint8_t[]){0x08}, 1, 0},
	{0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
	{0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
	{0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
	{0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
	{0xCC, (uint8_t[]){0x10}, 1, 0},

	{0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
	{0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
	{0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

	{0xB0, (uint8_t[]){0x5D}, 1, 0},
	{0xB1, (uint8_t[]){0x58}, 1, 0},
	{0xB2, (uint8_t[]){0x87}, 1, 0},
	{0xB3, (uint8_t[]){0x80}, 1, 0},
	{0xB5, (uint8_t[]){0x4E}, 1, 0},
	{0xB7, (uint8_t[]){0x85}, 1, 0},
	{0xB8, (uint8_t[]){0x21}, 1, 0},
	{0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
	{0xBB, (uint8_t[]){0x03}, 1, 0},
	{0xBC, (uint8_t[]){0x00}, 1, 0},

	{0xC1, (uint8_t[]){0x78}, 1, 0},
	{0xC2, (uint8_t[]){0x78}, 1, 0},
	{0xD0, (uint8_t[]){0x88}, 1, 0},

	{0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
	{0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
	{0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
	{0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
	{0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
	{0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
	{0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
	{0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
	{0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

	{0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
	{0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},

	{0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
	{0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
	{0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

	{0x11, (uint8_t[]){0x00}, 1, 120},
	{0x29, (uint8_t[]){0x00}, 1, 20},
};

/**
 * @brief Power up the MIPI DSI PHY using internal LDO (VO3) at 2.5V
 */
static esp_err_t bsp_enable_dsi_phy_power(void)
{
	static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
	if (phy_pwr_chan) {
		return ESP_OK;
	}
	esp_ldo_channel_config_t ldo_cfg = {
		.chan_id = CONFIG_BSP_JC4880P443C_DSI_PHY_LDO_CHANNEL,
		.voltage_mv = CONFIG_BSP_JC4880P443C_DSI_PHY_VOLTAGE_MV,
	};
	esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
	if (err == ESP_OK) {
		ESP_LOGI(TAG, "MIPI DSI PHY power enabled (LDO ch%d: %dmV)", ldo_cfg.chan_id, ldo_cfg.voltage_mv);
	} else {
		ESP_LOGW(TAG, "Failed to enable MIPI DSI PHY power: %s", esp_err_to_name(err));
	}
	return err;
}

/**
 * @brief Initialize backlight PWM control
 */
static esp_err_t bsp_display_brightness_init(void)
{
	const ledc_channel_config_t ch = {
		.gpio_num = CONFIG_BSP_JC4880P443C_LCD_BL_GPIO,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = CONFIG_BSP_JC4880P443C_BACKLIGHT_CHANNEL,
		.duty = 0,
		.timer_sel = CONFIG_BSP_JC4880P443C_BACKLIGHT_TIMER
	};
	const ledc_timer_config_t t = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = LEDC_TIMER_10_BIT,
		.timer_num = CONFIG_BSP_JC4880P443C_BACKLIGHT_TIMER,
		.freq_hz = CONFIG_BSP_JC4880P443C_BACKLIGHT_PWM_FREQ,
		.clk_cfg = LEDC_AUTO_CLK
	};
	ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");
	ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc ch");
	return ESP_OK;
}

esp_err_t bsp_display_backlight_on(void)
{
	ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "bl init");
	ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_JC4880P443C_BACKLIGHT_CHANNEL, 1023), TAG, "set duty");
	return ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_JC4880P443C_BACKLIGHT_CHANNEL);
}

esp_err_t bsp_display_brightness_set(uint8_t brightness_percent)
{
	if (brightness_percent > 100) {
		return ESP_ERR_INVALID_ARG;
	}

	ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "bl init");

	// Convert percentage to duty cycle (0-1023 for 10-bit resolution)
	uint32_t duty = (brightness_percent * 1023) / 100;
	ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_JC4880P443C_BACKLIGHT_CHANNEL, duty), TAG, "set duty");
	return ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_JC4880P443C_BACKLIGHT_CHANNEL);
}

void pc_vga_render(void *o);

/**
 * @brief Zero-copy Cache Synchronization for MIPI-DSI Framebuffer
 *
 * VGA renderer draws directly to globals.panel_fb.
 * MIPI-DSI DMA controller continuously scans this buffer from PSRAM.
 * We sync D-Cache to memory without performing any CPU memcpy.
 */
#if defined(USE_PPA_2D)
#include "driver/ppa.h"
static ppa_client_handle_t s_ppa_srm_client = NULL;
static ppa_client_handle_t s_ppa_fill_client = NULL;
static SemaphoreHandle_t s_ppa_sem = NULL;
static volatile bool s_ppa_in_flight = false;

static IRAM_ATTR bool ppa_srm_done_callback(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data)
{
	BaseType_t high_task_wakeup = pdFALSE;
	s_ppa_in_flight = false;
	if (s_ppa_sem) {
		xSemaphoreGiveFromISR(s_ppa_sem, &high_task_wakeup);
	}
	return high_task_wakeup == pdTRUE;
}

static esp_err_t bsp_ppa_init(void)
{
	s_ppa_sem = xSemaphoreCreateBinary();
	assert(s_ppa_sem);

	/* PPA SRM Client: 128-byte burst DMA with 2 pending transactions for double buffering */
	ppa_client_config_t srm_config = {
		.oper_type = PPA_OPERATION_SRM,
		.max_pending_trans_num = 2,
		.data_burst_length = PPA_DATA_BURST_LENGTH_128,
	};
	esp_err_t ret = ppa_register_client(&srm_config, &s_ppa_srm_client);
	if (ret == ESP_OK) {
		ppa_event_callbacks_t cbs = {
			.on_trans_done = ppa_srm_done_callback,
		};
		ppa_client_register_event_callbacks(s_ppa_srm_client, &cbs);
		ESP_LOGI(TAG, "ESP32-P4 PPA SRM 2D engine registered (128-byte Burst DMA, Async Mode)");
	} else {
		ESP_LOGW(TAG, "PPA SRM client registration failed: %s", esp_err_to_name(ret));
	}

	/* PPA Fill Client: 150 Megapixels/sec hardware fill accelerator */
	ppa_client_config_t fill_config = {
		.oper_type = PPA_OPERATION_FILL,
		.max_pending_trans_num = 1,
		.data_burst_length = PPA_DATA_BURST_LENGTH_128,
	};
	ret = ppa_register_client(&fill_config, &s_ppa_fill_client);
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "ESP32-P4 PPA Fill 2D engine registered (150 MP/s capability)");
	}

	return ESP_OK;
}

static esp_err_t ppa_blit_vga(const void *src_buf, int src_w, int src_h, void *dst_buf)
{
	if (!s_ppa_srm_client || !src_buf || !dst_buf) {
		return ESP_ERR_INVALID_STATE;
	}

	/* If previous 2D-DMA transaction is still processing, wait for completion */
	if (s_ppa_in_flight) {
		if (xSemaphoreTake(s_ppa_sem, pdMS_TO_TICKS(25)) != pdTRUE) {
			ESP_LOGW(TAG, "PPA SRM wait timeout");
		}
	}

	/* 1. D-Cache Synchronization: Commit all CPU-rendered pixel writes to PSRAM
	 * before PPA 2D-DMA engine reads them via AXI bus. Guarantees zero tearing. */
	esp_cache_msync((void *)src_buf, src_w * src_h * 2,
			ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

	/* 2. Configure PPA SRM transaction: 90° rotation with auto-scaling */
	ppa_srm_oper_config_t srm_cfg = {
		.in = {
			.buffer = src_buf,
			.pic_w = src_w,
			.pic_h = src_h,
			.block_w = src_w,
			.block_h = src_h,
			.block_offset_x = 0,
			.block_offset_y = 0,
			.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
		},
		.out = {
			.buffer = dst_buf,
			.buffer_size = CONFIG_BSP_JC4880P443C_LCD_H_RES * CONFIG_BSP_JC4880P443C_LCD_V_RES * 2,
			.pic_w = CONFIG_BSP_JC4880P443C_LCD_H_RES,
			.pic_h = CONFIG_BSP_JC4880P443C_LCD_V_RES,
			.block_offset_x = 0,
			.block_offset_y = 0,
			.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
		},
		.rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
		.scale_x = (float)CONFIG_BSP_JC4880P443C_LCD_V_RES / src_w,
		.scale_y = (float)CONFIG_BSP_JC4880P443C_LCD_H_RES / src_h,
		.mode = PPA_TRANS_MODE_NON_BLOCKING,
	};

	s_ppa_in_flight = true;
	esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa_srm_client, &srm_cfg);
	if (ret != ESP_OK) {
		s_ppa_in_flight = false;
		ESP_LOGW(TAG, "ppa_do_scale_rotate_mirror failed: %s", esp_err_to_name(ret));
	}
	return ret;
}

static esp_err_t ppa_fill_screen(void *dst_buf, uint16_t color_rgb565)
{
	if (!s_ppa_fill_client || !dst_buf) {
		return ESP_ERR_INVALID_STATE;
	}
	if (s_ppa_in_flight) {
		xSemaphoreTake(s_ppa_sem, pdMS_TO_TICKS(25));
	}

	uint8_t r = ((color_rgb565 >> 11) & 0x1F) * 255 / 31;
	uint8_t g = ((color_rgb565 >> 5) & 0x3F) * 255 / 63;
	uint8_t b = (color_rgb565 & 0x1F) * 255 / 31;

	ppa_fill_oper_config_t fill_cfg = {
		.out = {
			.buffer = dst_buf,
			.buffer_size = CONFIG_BSP_JC4880P443C_LCD_H_RES * CONFIG_BSP_JC4880P443C_LCD_V_RES * 2,
			.pic_w = CONFIG_BSP_JC4880P443C_LCD_H_RES,
			.pic_h = CONFIG_BSP_JC4880P443C_LCD_V_RES,
			.block_offset_x = 0,
			.block_offset_y = 0,
			.fill_cm = PPA_FILL_COLOR_MODE_RGB565,
		},
		.fill_block_w = CONFIG_BSP_JC4880P443C_LCD_H_RES,
		.fill_block_h = CONFIG_BSP_JC4880P443C_LCD_V_RES,
		.fill_argb_color = {
			.val = (0xFF << 24) | (r << 16) | (g << 8) | b,
		},
		.mode = PPA_TRANS_MODE_BLOCKING,
	};
	return ppa_do_fill(s_ppa_fill_client, &fill_cfg);
}
#endif

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
	(void)x_start; (void)y_start; (void)x_end; (void)y_end; (void)src;
	if (globals.panel_fb) {
		esp_cache_msync(globals.panel_fb, LCD_WIDTH * LCD_HEIGHT * 2,
				ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	}
}

void vga_task(void *arg)
{
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "vga runs on core %d\n", core_id);

	ESP_LOGI(TAG, "Init display");
#if defined(USE_PPA_2D)
	bsp_ppa_init();
#endif

	// DSI bus and DBI IO for ST7701
	esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
	esp_lcd_dsi_bus_config_t bus_config = {
		.bus_id = 0,
		.num_data_lanes = 2,
		.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
		.lane_bit_rate_mbps = CONFIG_BSP_JC4880P443C_DSI_LANE_BITRATE,
	};
	// Ensure MIPI DPHY is powered before creating the bus
	(void)bsp_enable_dsi_phy_power();
	esp_err_t dsi_err = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
	if (dsi_err != ESP_OK) {
		ESP_LOGW(TAG, "MIPI DSI bus init failed (%s) - running in simulator or without physical display", esp_err_to_name(dsi_err));
		globals.panel_fb = fbmalloc(CONFIG_BSP_JC4880P443C_LCD_H_RES * CONFIG_BSP_JC4880P443C_LCD_V_RES * 2);
		xEventGroupSetBits(global_event_group, BIT1);
		xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
		while (1) {
			int64_t frame_start = esp_timer_get_time();
			if (globals.pc && globals.vga_frame_ready) {
				globals.vga_frame_ready = 0;
				pc_vga_render(globals.pc);
			}
			if (globals.vga_dirty) {
				globals.vga_dirty = 0;
#if defined(USE_PPA_2D)
				if (globals.vga_fb && globals.panel_fb) {
					ppa_blit_vga(globals.vga_fb, LCD_WIDTH, LCD_HEIGHT, globals.panel_fb);
				}
#endif
			}
			int64_t elapsed = esp_timer_get_time() - frame_start;
			if (elapsed < 16666) {
				int delay_ms = (int)((16666 - elapsed) / 1000);
				if (delay_ms > 0) {
					vTaskDelay(pdMS_TO_TICKS(delay_ms));
				} else {
					taskYIELD();
				}
			} else {
				taskYIELD();
			}
		}
		return;
	}
	// Give PHY a moment to stabilize before DBI transfers
	vTaskDelay(pdMS_TO_TICKS(50));

	esp_lcd_panel_io_handle_t io;
	esp_lcd_dbi_io_config_t dbi_config = {
		.virtual_channel = 0,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
	};
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io));

	esp_lcd_panel_handle_t panel = NULL;
	// Explicit DPI timing for 480x800 panel at ~60Hz (vendor reference)
	esp_lcd_dpi_panel_config_t dpi_config = {
		.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
		.dpi_clock_freq_mhz = CONFIG_BSP_JC4880P443C_DPI_CLOCK_MHZ,
		.virtual_channel = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
		.in_color_format = LCD_COLOR_FMT_RGB565,
#else
		.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
#endif
		.num_fbs = CONFIG_BSP_JC4880P443C_NUM_FB,
		.video_timing = {
			.h_size = CONFIG_BSP_JC4880P443C_LCD_H_RES,
			.v_size = CONFIG_BSP_JC4880P443C_LCD_V_RES,
			.hsync_pulse_width = 12,
			.hsync_back_porch = 42,
			.hsync_front_porch = 42,
			.vsync_pulse_width = 2,
			.vsync_back_porch = 8,
			.vsync_front_porch = 166,
		},
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
		.flags = { .use_dma2d = true },
#endif
	};

	st7701_vendor_config_t vendor_config = {
		.mipi_config = {
			.dsi_bus = mipi_dsi_bus,
			.dpi_config = &dpi_config,
		},
		.init_cmds = st7701_lcd_cmds,
		.init_cmds_size = sizeof(st7701_lcd_cmds) / sizeof(st7701_lcd_cmds[0]),
		.flags = { .use_mipi_interface = 1 }
	};
	esp_lcd_panel_dev_config_t lcd_dev_config = {
		.bits_per_pixel = 16,
		.rgb_ele_order = BSP_LCD_COLOR_SPACE,
		.reset_gpio_num = BSP_LCD_RST,
		.vendor_config = &vendor_config,
	};

	ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &lcd_dev_config, &panel));
	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
	ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel));
#endif
	ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));

	// Ensure panel display is turned on
	ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

	bsp_display_brightness_init();
	bsp_display_brightness_set(30);

	/* ------ Get DMA frame buffer pointer from DPI panel ------ */
	void *fb_ptr = NULL;
	ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb_ptr));
	ESP_LOGI(TAG, "MIPI-DSI Panel FB @ %p (%d bytes)", fb_ptr, LCD_WIDTH * LCD_HEIGHT * 2);

#if defined(USE_PPA_2D)
	/* Clear panel DMA buffer to black using PPA hardware fill (2.5ms) */
	ppa_fill_screen(fb_ptr, 0x0000);
#endif

	globals.panel = panel;
	globals.panel_fb = fb_ptr;

	/* Signal i386_task that the LCD panel is ready */
	xEventGroupSetBits(global_event_group, BIT1);
	xEventGroupWaitBits(global_event_group,
			    BIT0,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);

	while (1) {
		int64_t frame_start = esp_timer_get_time();
		if (globals.pc && globals.vga_frame_ready) {
			globals.vga_frame_ready = 0;
			pc_vga_render(globals.pc);
		}
		if (globals.vga_dirty) {
			globals.vga_dirty = 0;
#if defined(USE_PPA_2D)
			if (globals.vga_fb && globals.panel_fb) {
				ppa_blit_vga(globals.vga_fb, LCD_WIDTH, LCD_HEIGHT, globals.panel_fb);
			} else {
				lcd_draw(0, 0, LCD_WIDTH, LCD_HEIGHT, NULL);
			}
#else
			lcd_draw(0, 0, LCD_WIDTH, LCD_HEIGHT, NULL);
#endif
		}
		/* Adaptive 60 FPS frame pacing: 16,666 µs per frame */
		int64_t elapsed = esp_timer_get_time() - frame_start;
		if (elapsed < 16666) {
			int delay_ms = (int)((16666 - elapsed) / 1000);
			if (delay_ms > 0) {
				vTaskDelay(pdMS_TO_TICKS(delay_ms));
			} else {
				taskYIELD();
			}
		} else {
			taskYIELD();
		}
	}
}
#endif
