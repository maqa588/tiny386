/*
 * Cirrus CL-GD5446 2D Hardware Acceleration - ESP32-P4 PPA Backend
 *
 * Tiny386 Emulation System
 * Leverages the ESP32-P4 hardware Pixel Processing Accelerator (PPA)
 * for blitting and high-speed rectangle fills up to 150 MP/s.
 */

#include "cirrus_ppa.h"
#include "cirrus_vga.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM) || defined(BUILD_ESP32)

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "cirrus_ppa";
static ppa_client_handle_t s_ppa_fill_client = NULL;
static ppa_client_handle_t s_ppa_srm_client = NULL;
static bool s_ppa_ready = false;

bool cirrus_ppa_init(void)
{
    if (s_ppa_ready) return true;

    /* 1. Register PPA Fill Client (up to 150 Megapixels/sec) */
    ppa_client_config_t fill_config = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ret = ppa_register_client(&fill_config, &s_ppa_fill_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register PPA fill client: %s", esp_err_to_name(ret));
        return false;
    }

    /* 2. Register PPA SRM Client (2D-DMA BitBlt engine) */
    ppa_client_config_t srm_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ret = ppa_register_client(&srm_config, &s_ppa_srm_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register PPA SRM client: %s", esp_err_to_name(ret));
        ppa_unregister_client(s_ppa_fill_client);
        s_ppa_fill_client = NULL;
        return false;
    }

    s_ppa_ready = true;
    ESP_LOGI(TAG, "ESP32-P4 PPA 2D Engine registered successfully for Cirrus CL-GD5446 acceleration");
    return true;
}

bool cirrus_ppa_available(void)
{
    return s_ppa_ready;
}

bool cirrus_ppa_solid_fill(CirrusVGAState *s,
                           uint32_t dstaddr, int dstpitch,
                           int width, int height,
                           uint32_t color, int pixelwidth, int rop)
{
    if (!s_ppa_ready || !s || !s->vga || !s->vga->vga_ram) return false;
    if (rop != CIRRUS_ROP_SRC && rop != CIRRUS_ROP_0 && rop != CIRRUS_ROP_1) return false;

    int pixels_w = width / (pixelwidth > 0 ? pixelwidth : 1);
    if (pixels_w <= 0 || height <= 0 || dstpitch <= 0) return false;

    size_t span = (size_t)dstpitch * height;
    if (dstaddr + span > (size_t)s->real_vram_size) return false;

    uint32_t fill_val = (rop == CIRRUS_ROP_0) ? 0 : ((rop == CIRRUS_ROP_1) ? 0xFFFFFFFF : color);

    /* Map color format to PPA fill structure */
    uint8_t a = 0xFF, r = 0, g = 0, b = 0;
    ppa_srm_color_mode_t cmode = PPA_SRM_COLOR_MODE_RGB565;

    if (pixelwidth == 2) {
        /* RGB565 / RGB555 */
        uint16_t c16 = (uint16_t)fill_val;
        r = ((c16 >> 11) & 0x1F) * 255 / 31;
        g = ((c16 >> 5) & 0x3F) * 255 / 63;
        b = (c16 & 0x1F) * 255 / 31;
        cmode = PPA_SRM_COLOR_MODE_RGB565;
    } else if (pixelwidth == 3 || pixelwidth == 4) {
        /* ARGB8888 */
        r = (fill_val >> 16) & 0xFF;
        g = (fill_val >> 8) & 0xFF;
        b = fill_val & 0xFF;
        cmode = PPA_SRM_COLOR_MODE_ARGB8888;
    } else {
        /* 8bpp color index: PPA fill doesn't support 8bpp directly, fallback to CPU */
        return false;
    }

    ppa_fill_oper_config_t fill_cfg = {
        .out = {
            .buffer = s->vga->vga_ram + dstaddr,
            .buffer_size = span,
            .pic_w = dstpitch / (pixelwidth > 0 ? pixelwidth : 1),
            .pic_h = height,
            .block_w = pixels_w,
            .block_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_color_mode = cmode,
        },
        .fill_argb_color = {
            .val = (a << 24) | (r << 16) | (g << 8) | b,
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_fill(s_ppa_fill_client, &fill_cfg);
    if (ret == ESP_OK) {
        esp_cache_msync(s->vga->vga_ram + dstaddr, span,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        return true;
    }
    return false;
}

bool cirrus_ppa_blit(CirrusVGAState *s,
                     uint32_t dstaddr, uint32_t srcaddr,
                     int dstpitch, int srcpitch,
                     int width, int height)
{
    if (!s_ppa_ready || !s || !s->vga || !s->vga->vga_ram) return false;
    if (width <= 0 || height <= 0 || dstpitch <= 0 || srcpitch <= 0) return false;

    size_t src_span = (size_t)srcpitch * height;
    size_t dst_span = (size_t)dstpitch * height;
    if (srcaddr + src_span > (size_t)s->real_vram_size ||
        dstaddr + dst_span > (size_t)s->real_vram_size) {
        return false;
    }

    /* Commit CPU cache writes for source before PPA DMA read */
    esp_cache_msync(s->vga->vga_ram + srcaddr, src_span,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    ppa_srm_oper_config_t srm_cfg = {
        .in = {
            .buffer = s->vga->vga_ram + srcaddr,
            .pic_w = srcpitch,
            .pic_h = height,
            .block_w = width,
            .block_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_color_mode = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s->vga->vga_ram + dstaddr,
            .buffer_size = dst_span,
            .pic_w = dstpitch,
            .pic_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_color_mode = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .rgb_swap = false,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa_srm_client, &srm_cfg);
    if (ret == ESP_OK) {
        esp_cache_msync(s->vga->vga_ram + dstaddr, dst_span,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        return true;
    }
    return false;
}

#else

/* Non-ESP32 Stub Implementations */
bool cirrus_ppa_init(void) { return false; }
bool cirrus_ppa_available(void) { return false; }
bool cirrus_ppa_solid_fill(CirrusVGAState *s, uint32_t dstaddr, int dstpitch, int width, int height, uint32_t color, int pixelwidth, int rop)
{
    (void)s; (void)dstaddr; (void)dstpitch; (void)width; (void)height; (void)color; (void)pixelwidth; (void)rop;
    return false;
}
bool cirrus_ppa_blit(CirrusVGAState *s, uint32_t dstaddr, uint32_t srcaddr, int dstpitch, int srcpitch, int width, int height)
{
    (void)s; (void)dstaddr; (void)srcaddr; (void)dstpitch; (void)srcpitch; (void)width; (void)height;
    return false;
}

#endif
