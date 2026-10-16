#ifdef USE_LCD_RGB
/*
 * RGB (RGB565 parallel) display backend for ESP32 boards.
 *
 * Drives either a plain RGB panel or an ADV7123 video DAC feeding a DB15 VGA
 * connector, using the esp_lcd RGB driver: the sink scans globals.panel_fb
 * directly via DMA, while the emulator renders into globals.vga_fb and the PPA
 * 2D engine scales/blits it into the panel framebuffer.
 *
 * GPIOs and timings come from board_<BOARD>.h (see board_wt0132p4_a1.h for the
 * 640x480 VGA wiring, board_s31_wroom3.h for a reference RGB panel).
 */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#include "common.h"

static const char *TAG = "lcd_rgb";

void pc_vga_render(void *o);

/* -------- Backlight (LEDC) -------- */
#if LCD_RGB_BL >= 0
static esp_err_t bsp_display_brightness_init(void)
{
	const ledc_channel_config_t ch = {
		.gpio_num = LCD_RGB_BL,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = LCD_BL_LEDC_CHANNEL,
		.duty = 0,
		.timer_sel = LCD_BL_LEDC_TIMER
	};
	const ledc_timer_config_t t = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = LEDC_TIMER_10_BIT,
		.timer_num = LCD_BL_LEDC_TIMER,
		.freq_hz = LCD_BL_LEDC_FREQ_HZ,
		.clk_cfg = LEDC_AUTO_CLK
	};
	ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc timer");
	ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc ch");
	return ESP_OK;
}

static esp_err_t bsp_display_brightness_set(uint8_t brightness_percent)
{
	if (brightness_percent > 100) {
		return ESP_ERR_INVALID_ARG;
	}
	ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "bl init");
	uint32_t duty = (brightness_percent * 1023) / 100;
	ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty), TAG, "set duty");
	return ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL);
}
#endif /* LCD_RGB_BL >= 0 */

/* -------- PPA 2D acceleration (same structure as lcd_st7701.c) -------- */
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
		ESP_LOGI(TAG, "PPA SRM 2D engine registered (128-byte Burst DMA, Async Mode)");
	} else {
		ESP_LOGW(TAG, "PPA SRM client registration failed: %s", esp_err_to_name(ret));
	}

	ppa_client_config_t fill_config = {
		.oper_type = PPA_OPERATION_FILL,
		.max_pending_trans_num = 1,
		.data_burst_length = PPA_DATA_BURST_LENGTH_128,
	};
	ret = ppa_register_client(&fill_config, &s_ppa_fill_client);
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "PPA Fill 2D engine registered");
	}

	return ESP_OK;
}

static esp_err_t ppa_blit_vga(const void *src_buf, int src_w, int src_h, void *dst_buf)
{
	if (!s_ppa_srm_client || !src_buf || !dst_buf) {
		return ESP_ERR_INVALID_STATE;
	}

	if (s_ppa_in_flight) {
		if (xSemaphoreTake(s_ppa_sem, pdMS_TO_TICKS(25)) != pdTRUE) {
			ESP_LOGW(TAG, "PPA SRM wait timeout");
		}
	}

	/* Commit CPU-rendered pixels to PSRAM before the PPA DMA engine reads them. */
	esp_cache_msync((void *)src_buf, src_w * src_h * 2,
			ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

	/* Scale the emulator framebuffer up to the panel, no rotation for RGB. */
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
			.buffer_size = LCD_WIDTH * LCD_HEIGHT * 2,
			.pic_w = LCD_WIDTH,
			.pic_h = LCD_HEIGHT,
			.block_offset_x = 0,
			.block_offset_y = 0,
			.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
		},
		.rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
		.scale_x = (float)LCD_WIDTH / src_w,
		.scale_y = (float)LCD_HEIGHT / src_h,
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
	if (!dst_buf) {
		return ESP_ERR_INVALID_STATE;
	}
	memset(dst_buf, 0, LCD_WIDTH * LCD_HEIGHT * 2);
	return ESP_OK;
}
#endif /* USE_PPA_2D */

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
	(void)x_start; (void)y_start; (void)x_end; (void)y_end; (void)src;
	if (globals.panel_fb) {
		esp_cache_msync(globals.panel_fb, LCD_WIDTH * LCD_HEIGHT * 2,
				ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	}
}

static void vga_loop(void)
{
	while (1) {
		int64_t frame_start = esp_timer_get_time();
		/* Core 0 owns the VGA timing state and flags each frame boundary;
		 * here on Core 1 we only rasterise the pending frame and push it
		 * out, keeping Core 0 free for the x86 interpreter/JIT. */
		if (globals.pc && globals.vga_frame_ready) {
			globals.vga_frame_ready = 0;
			pc_vga_render(globals.pc);
			/* Composite the on-screen control panel over the guest frame
			 * (RGB565, hence the 2-byte pitch).  Its visibility can change
			 * independently of guest drawing, so force a refresh when shown. */
			if (globals.vga_fb &&
			    esp_osd_render(globals.vga_fb, LCD_WIDTH, LCD_HEIGHT, LCD_WIDTH * 2))
				globals.vga_dirty = 1;
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
		/* Adaptive 60 FPS frame pacing: 16,666 us per frame */
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

void vga_task(void *arg)
{
	(void)arg;
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "vga runs on core %d\n", core_id);

	ESP_LOGI(TAG, "Init RGB display");
#if defined(USE_PPA_2D)
	bsp_ppa_init();
#endif

	esp_lcd_panel_handle_t panel = NULL;
	esp_lcd_rgb_panel_config_t rgb_panel_config = {
		.clk_src = LCD_CLK_SRC_DEFAULT,
		.timings = {
			.pclk_hz = LCD_PCLK_HZ,
			.h_res = LCD_WIDTH,
			.v_res = LCD_HEIGHT,
			.hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH,
			.hsync_back_porch = LCD_HSYNC_BACK_PORCH,
			.hsync_front_porch = LCD_HSYNC_FRONT_PORCH,
			.vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH,
			.vsync_back_porch = LCD_VSYNC_BACK_PORCH,
			.vsync_front_porch = LCD_VSYNC_FRONT_PORCH,
#if defined(LCD_RGB_HSYNC_IDLE_LOW)
			/* VGA (ADV7123 -> DB15) uses active-low sync pulses; a DPI panel
			 * normally uses active-high, hence this is board-configurable. */
			.flags = {
				.hsync_idle_low = LCD_RGB_HSYNC_IDLE_LOW,
				.vsync_idle_low = LCD_RGB_VSYNC_IDLE_LOW,
			},
#endif
		},
		.data_width = 16,
		.in_color_format = LCD_COLOR_FMT_RGB565,
		.out_color_format = LCD_COLOR_FMT_RGB565,
		.num_fbs = 1,
		.bounce_buffer_size_px = LCD_WIDTH * 8,
		.hsync_gpio_num = LCD_RGB_HSYNC,
		.vsync_gpio_num = LCD_RGB_VSYNC,
		.de_gpio_num = LCD_RGB_DE,
		.pclk_gpio_num = LCD_RGB_PCLK,
		.disp_gpio_num = LCD_RGB_DISP,
		.data_gpio_nums = {
			LCD_RGB_D0,  LCD_RGB_D1,  LCD_RGB_D2,  LCD_RGB_D3,
			LCD_RGB_D4,  LCD_RGB_D5,  LCD_RGB_D6,  LCD_RGB_D7,
			LCD_RGB_D8,  LCD_RGB_D9,  LCD_RGB_D10, LCD_RGB_D11,
			LCD_RGB_D12, LCD_RGB_D13, LCD_RGB_D14, LCD_RGB_D15,
		},
		.flags = {
			.fb_in_psram = 1,
		},
	};

	esp_err_t err = esp_lcd_new_rgb_panel(&rgb_panel_config, &panel);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "RGB panel init failed (%s) - running in simulator or without physical display",
			 esp_err_to_name(err));
		globals.panel_fb = fbmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
		xEventGroupSetBits(global_event_group, BIT1);
		xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);
		vga_loop();
		return;
	}

	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
	/* Ensure the panel is turned on (asserts the DISP line if wired). */
	esp_err_t on_err = esp_lcd_panel_disp_on_off(panel, true);
	if (on_err != ESP_OK && on_err != ESP_ERR_NOT_SUPPORTED) {
		ESP_ERROR_CHECK(on_err);
	}

	#if LCD_RGB_BL >= 0
	bsp_display_brightness_init();
	bsp_display_brightness_set(30);
#endif

	void *fb_ptr = NULL;
	ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb_ptr));
	ESP_LOGI(TAG, "RGB Panel FB @ %p (%d bytes)", fb_ptr, LCD_WIDTH * LCD_HEIGHT * 2);

#if defined(USE_PPA_2D)
	ppa_fill_screen(fb_ptr, 0x0000);
#endif

	globals.panel = panel;
	globals.panel_fb = fb_ptr;

	/* Signal i386_task that the LCD panel (and panel_fb) is ready. */
	xEventGroupSetBits(global_event_group, BIT1);
	xEventGroupWaitBits(global_event_group, BIT0, pdFALSE, pdFALSE, portMAX_DELAY);

	vga_loop();
}
#endif /* USE_LCD_RGB */