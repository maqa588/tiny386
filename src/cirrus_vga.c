/*
 * Cirrus Logic CL-GD5446 VGA Emulator Implementation
 *
 * Tiny386 Emulation System
 * Hardware 2D Acceleration for Windows 95
 */

#include "cirrus_vga.h"
#include "cirrus_opencl.h"
#include "cirrus_ppa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ABS(x) ((x) < 0 ? -(x) : (x))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#include <stdarg.h>

#ifdef DEBUG_CIRRUS
static FILE *dbg_fp = NULL;
static void log_cirrus(const char *fmt, ...) {
    if (!dbg_fp) dbg_fp = fopen("/tmp/cirrus_debug.log", "w");
    if (!dbg_fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(dbg_fp, fmt, ap);
    va_end(ap);
    fflush(dbg_fp);
}
#else
static inline void log_cirrus(const char *fmt, ...) { (void)fmt; }
#endif

static void cirrus_bitblt_start(CirrusVGAState *s);
static void cirrus_bitblt_reset(CirrusVGAState *s);
static void cirrus_update_bank_ptr(CirrusVGAState *s, unsigned bank_index);
static int cirrus_bitblt_solidfill(CirrusVGAState *s, int blt_rop);
static int cirrus_bitblt_videotovideo_copy(CirrusVGAState *s);
static void cirrus_bitblt_cputovideo_next(CirrusVGAState *s);

static const uint8_t cirrus_gr_mask[16] = {
    0x0f, /* 0x00 */
    0x0f, /* 0x01 */
    0x0f, /* 0x02 */
    0x1f, /* 0x03 */
    0x03, /* 0x04 */
    0x7f, /* 0x05 */
    0x0f, /* 0x06 */
    0x0f, /* 0x07 */
    0xff, /* 0x08 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/* Mapping of 16 Cirrus ROPs to index 0..15 */
static uint8_t rop_to_index[256];
static bool s_rop_inited = false;

static void init_rop_table(void)
{
    if (s_rop_inited) return;
    memset(rop_to_index, 2, sizeof(rop_to_index)); /* Default to NOP */
    rop_to_index[CIRRUS_ROP_0] = 0;
    rop_to_index[CIRRUS_ROP_SRC_AND_DST] = 1;
    rop_to_index[CIRRUS_ROP_NOP] = 2;
    rop_to_index[CIRRUS_ROP_SRC_AND_NOTDST] = 3;
    rop_to_index[CIRRUS_ROP_NOTDST] = 4;
    rop_to_index[CIRRUS_ROP_SRC] = 5;
    rop_to_index[CIRRUS_ROP_1] = 6;
    rop_to_index[CIRRUS_ROP_NOTSRC_AND_DST] = 7;
    rop_to_index[CIRRUS_ROP_SRC_XOR_DST] = 8;
    rop_to_index[CIRRUS_ROP_SRC_OR_DST] = 9;
    rop_to_index[CIRRUS_ROP_NOTSRC_OR_NOTDST] = 10;
    rop_to_index[CIRRUS_ROP_SRC_NOTXOR_DST] = 11;
    rop_to_index[CIRRUS_ROP_SRC_OR_NOTDST] = 12;
    rop_to_index[CIRRUS_ROP_NOTSRC] = 13;
    rop_to_index[CIRRUS_ROP_NOTSRC_OR_DST] = 14;
    rop_to_index[CIRRUS_ROP_NOTSRC_AND_NOTDST] = 15;
    s_rop_inited = true;
}

static inline uint8_t apply_rop(int rop_idx, uint8_t d, uint8_t s)
{
    switch (rop_idx) {
    case 0: return 0;
    case 1: return s & d;
    case 2: return d;
    case 3: return s & ~d;
    case 4: return ~d;
    case 5: return s;
    case 6: return 0xFF;
    case 7: return ~s & d;
    case 8: return s ^ d;
    case 9: return s | d;
    case 10: return ~s | ~d;
    case 11: return ~(s ^ d);
    case 12: return s | ~d;
    case 13: return ~s;
    case 14: return ~s | d;
    case 15: return ~s & ~d;
    default: return s;
    }
}

static bool blit_region_is_unsafe(CirrusVGAState *s, int32_t pitch, int32_t addr, bool is_backward)
{
    if (!pitch) return true;
    pitch = ABS(pitch);
    if (is_backward) {
        int64_t min = addr - ((int64_t)s->cirrus_blt_height - 1) * pitch - s->cirrus_blt_width + 1;
        if (min < 0 || addr >= s->real_vram_size) return true;
    } else {
        int64_t max = addr + ((int64_t)s->cirrus_blt_height - 1) * pitch + s->cirrus_blt_width - 1;
        if (max >= s->real_vram_size) return true;
    }
    return false;
}

static bool blit_is_unsafe(CirrusVGAState *s, bool dst_only)
{
    if (s->cirrus_blt_width <= 0 || s->cirrus_blt_height <= 0) return true;
    if (s->cirrus_blt_width > CIRRUS_BLTBUFSIZE) return true;
    bool is_backward = !!(s->cirrus_blt_mode & CIRRUS_BLTMODE_BACKWARDS);
    if (blit_region_is_unsafe(s, s->cirrus_blt_dstpitch, s->cirrus_blt_dstaddr, is_backward)) return true;
    if (dst_only) return false;
    if (blit_region_is_unsafe(s, s->cirrus_blt_srcpitch, s->cirrus_blt_srcaddr, is_backward)) return true;
    return false;
}

static void cirrus_bitblt_reset(CirrusVGAState *s)
{
    s->vga->gr[0x31] &= ~(CIRRUS_BLT_START | CIRRUS_BLT_BUSY | CIRRUS_BLT_FIFOUSED);
    s->cirrus_srcptr = s->cirrus_bltbuf;
    s->cirrus_srcptr_end = s->cirrus_bltbuf;
    s->cirrus_srccounter = 0;
}

static void cirrus_update_bank_ptr(CirrusVGAState *s, unsigned bank_index)
{
    unsigned offset;
    unsigned limit;

    if (s->vga->gr[0x0B] & 0x01) {
        offset = s->vga->gr[0x09 + bank_index];
    } else {
        offset = s->vga->gr[0x09];
    }
    if (s->vga->gr[0x0B] & 0x20) {
        offset <<= 14;
    } else {
        offset <<= 12;
    }

    if ((unsigned)s->real_vram_size <= offset)
        limit = 0;
    else
        limit = s->real_vram_size - offset;

    if (((s->vga->gr[0x0B] & 0x01) == 0) && (bank_index != 0)) {
        if (limit > 0x8000) {
            offset += 0x8000;
            limit -= 0x8000;
        } else {
            limit = 0;
        }
    }

    if (limit > 0) {
        s->cirrus_bank_base[bank_index] = offset;
        s->cirrus_bank_limit[bank_index] = limit;
    } else {
        s->cirrus_bank_base[bank_index] = 0;
        s->cirrus_bank_limit[bank_index] = 0;
    }
}

static inline void cirrus_bitblt_fgcol(CirrusVGAState *s)
{
    uint32_t color = 0;
    switch (s->cirrus_blt_pixelwidth) {
    case 1:
        s->cirrus_blt_fgcol = s->cirrus_shadow_gr1;
        break;
    case 2:
        color = s->cirrus_shadow_gr1 | (s->vga->gr[0x11] << 8);
        s->cirrus_blt_fgcol = color;
        break;
    case 3:
        s->cirrus_blt_fgcol = s->cirrus_shadow_gr1 | (s->vga->gr[0x11] << 8) | (s->vga->gr[0x13] << 16);
        break;
    default:
    case 4:
        color = s->cirrus_shadow_gr1 | (s->vga->gr[0x11] << 8) | (s->vga->gr[0x13] << 16) | (s->vga->gr[0x15] << 24);
        s->cirrus_blt_fgcol = color;
        break;
    }
}

static inline void cirrus_bitblt_bgcol(CirrusVGAState *s)
{
    uint32_t color = 0;
    switch (s->cirrus_blt_pixelwidth) {
    case 1:
        s->cirrus_blt_bgcol = s->cirrus_shadow_gr0;
        break;
    case 2:
        color = s->cirrus_shadow_gr0 | (s->vga->gr[0x10] << 8);
        s->cirrus_blt_bgcol = color;
        break;
    case 3:
        s->cirrus_blt_bgcol = s->cirrus_shadow_gr0 | (s->vga->gr[0x10] << 8) | (s->vga->gr[0x12] << 16);
        break;
    default:
    case 4:
        color = s->cirrus_shadow_gr0 | (s->vga->gr[0x10] << 8) | (s->vga->gr[0x12] << 16) | (s->vga->gr[0x14] << 24);
        s->cirrus_blt_bgcol = color;
        break;
    }
}

/* Solid Fill operation */
static int cirrus_bitblt_solidfill(CirrusVGAState *s, int blt_rop)
{
    if (blit_is_unsafe(s, true)) return 0;

    int rop_idx = rop_to_index[blt_rop];

    /* 1. Try ESP32-P4 PPA hardware 2D accelerator */
    if (cirrus_ppa_available()) {
        if (cirrus_ppa_solid_fill(s, s->cirrus_blt_dstaddr, s->cirrus_blt_dstpitch,
                                  s->cirrus_blt_width, s->cirrus_blt_height,
                                  s->cirrus_blt_fgcol, s->cirrus_blt_pixelwidth, blt_rop)) {
            cirrus_bitblt_reset(s);
            return 1;
        }
    }

    /* 2. Try host OpenCL GPU/CPU accelerator */
    if (cirrus_opencl_available()) {
        if (cirrus_opencl_solid_fill(s, s->cirrus_blt_dstaddr, s->cirrus_blt_dstpitch,
                                     s->cirrus_blt_width, s->cirrus_blt_height,
                                     s->cirrus_blt_fgcol, s->cirrus_blt_pixelwidth, blt_rop)) {
            cirrus_bitblt_reset(s);
            return 1;
        }
    }

    /* 3. Fast CPU path */
    uint8_t *vram = s->vga->vga_ram;
    uint32_t dst = s->cirrus_blt_dstaddr;
    int dstpitch = s->cirrus_blt_dstpitch;
    int width = s->cirrus_blt_width;
    int height = s->cirrus_blt_height;
    int pw = s->cirrus_blt_pixelwidth;
    uint32_t fg = s->cirrus_blt_fgcol;

    for (int y = 0; y < height; y++) {
        uint8_t *d = vram + (dst + y * dstpitch);
        if (rop_idx == 5) { /* SRC plain copy */
            if (pw == 1) {
                memset(d, (uint8_t)fg, width);
            } else if (pw == 2) {
                uint16_t val16 = (uint16_t)fg;
                uint16_t *d16 = (uint16_t *)d;
                int count = width / 2;
                for (int x = 0; x < count; x++) d16[x] = val16;
            } else if (pw == 4) {
                uint32_t val32 = fg;
                uint32_t *d32 = (uint32_t *)d;
                int count = width / 4;
                for (int x = 0; x < count; x++) d32[x] = val32;
            } else {
                for (int x = 0; x < width; x += pw) {
                    for (int b = 0; b < pw; b++) d[x + b] = (fg >> (b * 8)) & 0xff;
                }
            }
        } else if (rop_idx == 0) { /* 0 */
            memset(d, 0, width);
        } else if (rop_idx == 6) { /* 1 */
            memset(d, 0xff, width);
        } else {
            /* General ROP */
            for (int x = 0; x < width; x++) {
                uint8_t src_b = (fg >> ((x % pw) * 8)) & 0xff;
                d[x] = apply_rop(rop_idx, d[x], src_b);
            }
        }
    }

    cirrus_bitblt_reset(s);
    return 1;
}

/* Video-to-Video Blit operation */
static int cirrus_bitblt_videotovideo_copy(CirrusVGAState *s)
{
    if (blit_is_unsafe(s, false)) return 0;

    int blt_rop = s->vga->gr[0x32];
    int rop_idx = rop_to_index[blt_rop];
    bool is_forward = !(s->cirrus_blt_mode & CIRRUS_BLTMODE_BACKWARDS);
    bool is_transp = (s->cirrus_blt_mode & CIRRUS_BLTMODE_TRANSPARENTCOMP);

    /* 1. Try ESP32-P4 PPA hardware accelerator for plain forward blit */
    if (is_forward && !is_transp && rop_idx == 5 && cirrus_ppa_available()) {
        if (cirrus_ppa_blit(s, s->cirrus_blt_dstaddr, s->cirrus_blt_srcaddr,
                            s->cirrus_blt_dstpitch, s->cirrus_blt_srcpitch,
                            s->cirrus_blt_width, s->cirrus_blt_height)) {
            cirrus_bitblt_reset(s);
            return 1;
        }
    }

    /* 2. Try host OpenCL GPU/CPU acceleration */
    if (cirrus_opencl_available()) {
        if (cirrus_opencl_copy(s, s->cirrus_blt_dstaddr, s->cirrus_blt_srcaddr,
                               s->cirrus_blt_dstpitch, s->cirrus_blt_srcpitch,
                               s->cirrus_blt_width, s->cirrus_blt_height,
                               is_forward, is_transp, rop_idx, s->cirrus_blt_pixelwidth)) {
            cirrus_bitblt_reset(s);
            return 1;
        }
    }

    /* 3. Fast CPU path */
    uint8_t *vram = s->vga->vga_ram;
    int width = s->cirrus_blt_width;
    int height = s->cirrus_blt_height;
    int dpitch = ABS(s->cirrus_blt_dstpitch);
    int spitch = ABS(s->cirrus_blt_srcpitch);
    uint32_t dst = s->cirrus_blt_dstaddr;
    uint32_t src = s->cirrus_blt_srcaddr;
    int pw = s->cirrus_blt_pixelwidth;
    uint32_t transp = s->vga->gr[0x34] | (s->vga->gr[0x35] << 8);

    if (is_forward) {
        if (!is_transp && rop_idx == 5) {
            for (int y = 0; y < height; y++) {
                uint32_t da = (dst + y * dpitch) & s->cirrus_addr_mask;
                uint32_t sa = (src + y * spitch) & s->cirrus_addr_mask;
                memmove(vram + da, vram + sa, width);
            }
        } else if (!is_transp) {
            for (int y = 0; y < height; y++) {
                uint32_t da = (dst + y * dpitch) & s->cirrus_addr_mask;
                uint32_t sa = (src + y * spitch) & s->cirrus_addr_mask;
                for (int x = 0; x < width; x++) {
                    uint32_t d_pixel = (da + x) & s->cirrus_addr_mask;
                    uint32_t s_pixel = (sa + x) & s->cirrus_addr_mask;
                    vram[d_pixel] = apply_rop(rop_idx, vram[d_pixel], vram[s_pixel]);
                }
            }
        } else {
            /* Forward Transparent Compare */
            for (int y = 0; y < height; y++) {
                uint32_t da = (dst + y * dpitch) & s->cirrus_addr_mask;
                uint32_t sa = (src + y * spitch) & s->cirrus_addr_mask;
                if (pw == 1) {
                    for (int x = 0; x < width; x++) {
                        uint32_t s_pixel = (sa + x) & s->cirrus_addr_mask;
                        uint8_t p = vram[s_pixel];
                        if (p != (uint8_t)transp) {
                            uint32_t d_pixel = (da + x) & s->cirrus_addr_mask;
                            vram[d_pixel] = apply_rop(rop_idx, vram[d_pixel], p);
                        }
                    }
                } else if (pw == 2) {
                    int count = width / 2;
                    for (int x = 0; x < count; x++) {
                        uint32_t sa2 = (sa + x * 2) & s->cirrus_addr_mask & ~1;
                        uint32_t da2 = (da + x * 2) & s->cirrus_addr_mask & ~1;
                        uint16_t p = *(uint16_t *)(vram + sa2);
                        if (p != (uint16_t)transp) {
                            uint16_t d16 = *(uint16_t *)(vram + da2);
                            uint16_t res = (apply_rop(rop_idx, d16 & 0xff, p & 0xff)) |
                                           (apply_rop(rop_idx, d16 >> 8, p >> 8) << 8);
                            *(uint16_t *)(vram + da2) = res;
                        }
                    }
                }
            }
        }
    } else {
        /* Backward copy:
         * In hardware backward blit, dst and src point to the LAST BYTE (bottom right)
         * of the bounding box. Lines are processed from bottom to top (y = 0 to height-1).
         * Within each scanline, pixels are processed from right to left.
         */
        if (!is_transp && rop_idx == 5) {
            for (int y = 0; y < height; y++) {
                uint32_t da = (dst - y * dpitch - width + 1) & s->cirrus_addr_mask;
                uint32_t sa = (src - y * spitch - width + 1) & s->cirrus_addr_mask;
                memmove(vram + da, vram + sa, width);
            }
        } else if (!is_transp) {
            for (int y = 0; y < height; y++) {
                uint32_t da = (dst - y * dpitch) & s->cirrus_addr_mask;
                uint32_t sa = (src - y * spitch) & s->cirrus_addr_mask;
                for (int x = 0; x < width; x++) {
                    uint32_t d_pixel = (da - x) & s->cirrus_addr_mask;
                    uint32_t s_pixel = (sa - x) & s->cirrus_addr_mask;
                    vram[d_pixel] = apply_rop(rop_idx, vram[d_pixel], vram[s_pixel]);
                }
            }
        } else {
            /* Backward Transparent Compare */
            for (int y = 0; y < height; y++) {
                uint32_t da = (dst - y * dpitch) & s->cirrus_addr_mask;
                uint32_t sa = (src - y * spitch) & s->cirrus_addr_mask;
                if (pw == 1) {
                    for (int x = 0; x < width; x++) {
                        uint32_t d_pixel = (da - x) & s->cirrus_addr_mask;
                        uint32_t s_pixel = (sa - x) & s->cirrus_addr_mask;
                        uint8_t p = vram[s_pixel];
                        if (p != (uint8_t)transp) {
                            vram[d_pixel] = apply_rop(rop_idx, vram[d_pixel], p);
                        }
                    }
                } else if (pw == 2) {
                    int count = width / 2;
                    for (int x = 0; x < count; x++) {
                        uint32_t sa2 = (sa - x * 2 - 1) & s->cirrus_addr_mask & ~1;
                        uint32_t da2 = (da - x * 2 - 1) & s->cirrus_addr_mask & ~1;
                        uint16_t p = *(uint16_t *)(vram + sa2);
                        if (p != (uint16_t)transp) {
                            uint16_t d16 = *(uint16_t *)(vram + da2);
                            uint16_t res = (apply_rop(rop_idx, d16 & 0xff, p & 0xff)) |
                                           (apply_rop(rop_idx, d16 >> 8, p >> 8) << 8);
                            *(uint16_t *)(vram + da2) = res;
                        }
                    }
                }
            }
        }
    }

    cirrus_bitblt_reset(s);
    return 1;
}

/* Pattern Copy operation (8x8 pattern) - supports Video-to-Video and CPU-to-Video */
static int cirrus_bitblt_common_patterncopy(CirrusVGAState *s, bool is_cputovideo)
{
    if (blit_is_unsafe(s, true)) return 0;

    int blt_rop = s->vga->gr[0x32];
    int rop_idx = rop_to_index[blt_rop];
    uint8_t *vram = s->vga->vga_ram;
    uint32_t dst = s->cirrus_blt_dstaddr;
    uint32_t src = s->cirrus_blt_srcaddr;
    int dstpitch = s->cirrus_blt_dstpitch;
    int width = s->cirrus_blt_width;
    int height = s->cirrus_blt_height;
    int pw = s->cirrus_blt_pixelwidth;
    bool transp = (s->cirrus_blt_mode & CIRRUS_BLTMODE_TRANSPARENTCOMP);
    int srcskipleft = s->vga->gr[0x2f] & 0x07;
    int dstskipleft = srcskipleft * pw;

    if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND) {
        /* Monochrome 8x8 pattern (8 bytes at src or bltbuf) */
        cirrus_bitblt_fgcol(s);
        cirrus_bitblt_bgcol(s);
        unsigned int bits_xor = 0;
        uint32_t col = s->cirrus_blt_fgcol;
        uint32_t colors[2];

        if (s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_COLOREXPINV) {
            bits_xor = 0xff;
            col = s->cirrus_blt_bgcol;
        } else {
            bits_xor = 0x00;
            col = s->cirrus_blt_fgcol;
        }
        colors[0] = s->cirrus_blt_bgcol;
        colors[1] = s->cirrus_blt_fgcol;

        int pattern_y = s->cirrus_blt_srcaddr & 7;
        uint32_t pat_base = s->cirrus_blt_srcaddr & ~7;

        for (int y = 0; y < height; y++) {
            uint8_t bits = (is_cputovideo ?
                s->cirrus_bltbuf[pattern_y & (CIRRUS_BLTBUFSIZE - 1)] :
                vram[(pat_base + pattern_y) & s->cirrus_addr_mask]) ^ bits_xor;
            int bitpos = 7 - srcskipleft;
            uint32_t da = (dst + dstskipleft) & s->cirrus_addr_mask;

            for (int x = dstskipleft; x < width; x += pw) {
                int bit = (bits >> bitpos) & 1;
                if (transp) {
                    if (bit) {
                        for (int b = 0; b < pw; b++) {
                            uint32_t a = (da + b) & s->cirrus_addr_mask;
                            vram[a] = apply_rop(rop_idx, vram[a], (col >> (b * 8)) & 0xff);
                        }
                    }
                } else {
                    uint32_t c = colors[bit];
                    for (int b = 0; b < pw; b++) {
                        uint32_t a = (da + b) & s->cirrus_addr_mask;
                        vram[a] = apply_rop(rop_idx, vram[a], (c >> (b * 8)) & 0xff);
                    }
                }
                da = (da + pw) & s->cirrus_addr_mask;
                bitpos = (bitpos - 1) & 7;
            }
            pattern_y = (pattern_y + 1) & 7;
            dst = (dst + dstpitch) & s->cirrus_addr_mask;
        }
    } else {
        /* Color 8x8 pattern (64 pixels = 64 * pw bytes) */
        int pattern_pitch = 8 * pw;
        int pattern_size = 64 * pw;
        uint32_t pat_base = src & ~(pattern_size - 1);
        int pattern_y = (src / pattern_pitch) & 7;

        for (int y = 0; y < height; y++) {
            uint32_t src_line = pat_base + pattern_y * pattern_pitch;
            int pattern_x = dstskipleft % pattern_pitch;
            uint32_t da = (dst + dstskipleft) & s->cirrus_addr_mask;

            for (int x = dstskipleft; x < width; x += pw) {
                for (int b = 0; b < pw; b++) {
                    uint32_t sa = (src_line + pattern_x + b) & s->cirrus_addr_mask;
                    uint32_t a = (da + b) & s->cirrus_addr_mask;
                    uint8_t sp = is_cputovideo ?
                        s->cirrus_bltbuf[(sa - pat_base) & (CIRRUS_BLTBUFSIZE - 1)] :
                        vram[sa];
                    vram[a] = apply_rop(rop_idx, vram[a], sp);
                }
                pattern_x = (pattern_x + pw) & (pattern_pitch - 1);
                da = (da + pw) & s->cirrus_addr_mask;
            }
            pattern_y = (pattern_y + 1) & 7;
            dst = (dst + dstpitch) & s->cirrus_addr_mask;
        }
    }

    if (!is_cputovideo) {
        cirrus_bitblt_reset(s);
    }
    return 1;
}

static void cirrus_do_colorexpand(CirrusVGAState *s, uint32_t dstaddr, uint32_t srcaddr, bool is_cputovideo)
{
    static int col_cnt = 0;
    if (++col_cnt <= 200) {
        log_cirrus("[COLOREXP] dst=%u (x=%u y=%u) w=%d h=%d pw=%d skip=%d pitch=%d rop=%02x transp=%d cpu=%d fg=%04x bg=%04x\n",
                dstaddr, (dstaddr % 1280)/2, dstaddr / 1280, s->cirrus_blt_width,
                is_cputovideo ? 1 : s->cirrus_blt_height, s->cirrus_blt_pixelwidth,
                s->vga->gr[0x2f] & 7, s->cirrus_blt_dstpitch, s->vga->gr[0x32],
                !!(s->cirrus_blt_mode & CIRRUS_BLTMODE_TRANSPARENTCOMP), is_cputovideo,
                s->cirrus_blt_fgcol, s->cirrus_blt_bgcol);
    }
    int pw = s->cirrus_blt_pixelwidth;
    int bltwidth = s->cirrus_blt_width;
    int bltheight = is_cputovideo ? 1 : s->cirrus_blt_height;
    int dstpitch = s->cirrus_blt_dstpitch;
    int blt_rop = s->vga->gr[0x32];
    int rop_idx = rop_to_index[blt_rop];
    bool transp = (s->cirrus_blt_mode & CIRRUS_BLTMODE_TRANSPARENTCOMP);
    uint8_t *vram = s->vga->vga_ram;

    cirrus_bitblt_fgcol(s);
    cirrus_bitblt_bgcol(s);

    int srcskipleft = s->vga->gr[0x2f] & 0x07;
    int dstskipleft = srcskipleft * pw;

    unsigned bits_xor;
    uint32_t col;
    uint32_t colors[2];

    if (s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_COLOREXPINV) {
        bits_xor = 0xff;
        col = s->cirrus_blt_bgcol;
        colors[0] = s->cirrus_blt_fgcol;
        colors[1] = s->cirrus_blt_bgcol;
    } else {
        bits_xor = 0x00;
        col = s->cirrus_blt_fgcol;
        colors[0] = s->cirrus_blt_bgcol;
        colors[1] = s->cirrus_blt_fgcol;
    }

    for (int y = 0; y < bltheight; y++) {
        unsigned bitmask = 0x80 >> srcskipleft;
        uint8_t raw_byte;
        if (is_cputovideo) {
            raw_byte = s->cirrus_bltbuf[srcaddr++ & (CIRRUS_BLTBUFSIZE - 1)];
        } else {
            raw_byte = vram[srcaddr++ & s->cirrus_addr_mask];
        }
        unsigned bits = raw_byte ^ bits_xor;
        uint32_t addr = (dstaddr + dstskipleft) & s->cirrus_addr_mask;

        for (int x = dstskipleft; x < bltwidth; x += pw) {
            if ((bitmask & 0xff) == 0) {
                bitmask = 0x80;
                if (is_cputovideo) {
                    raw_byte = s->cirrus_bltbuf[srcaddr++ & (CIRRUS_BLTBUFSIZE - 1)];
                } else {
                    raw_byte = vram[srcaddr++ & s->cirrus_addr_mask];
                }
                bits = raw_byte ^ bits_xor;
            }
            int index = (bits & bitmask);
            if (transp) {
                if (index) {
                    for (int b = 0; b < pw; b++) {
                        uint32_t a = (addr + b) & s->cirrus_addr_mask;
                        vram[a] = apply_rop(rop_idx, vram[a], (col >> (b * 8)) & 0xff);
                    }
                }
            } else {
                uint32_t c = colors[!!index];
                for (int b = 0; b < pw; b++) {
                    uint32_t a = (addr + b) & s->cirrus_addr_mask;
                    vram[a] = apply_rop(rop_idx, vram[a], (c >> (b * 8)) & 0xff);
                }
            }
            addr = (addr + pw) & s->cirrus_addr_mask;
            bitmask >>= 1;
        }
        dstaddr = (dstaddr + dstpitch) & s->cirrus_addr_mask;
    }
}

/* CPU-to-Video transfer completion step */
static void cirrus_bitblt_cputovideo_next(CirrusVGAState *s)
{
    if (s->cirrus_srccounter <= 0) {
        cirrus_bitblt_reset(s);
        return;
    }

    if (s->cirrus_blt_mode & CIRRUS_BLTMODE_PATTERNCOPY) {
        cirrus_bitblt_common_patterncopy(s, true);
        s->cirrus_srccounter = 0;
        cirrus_bitblt_reset(s);
        return;
    }

    if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND) {
        cirrus_do_colorexpand(s, s->cirrus_blt_dstaddr, 0, true);
    } else {
        /* Direct raw pixel transfer from host */
        int blt_rop = s->vga->gr[0x32];
        int rop_idx = rop_to_index[blt_rop];
        uint8_t *vram = s->vga->vga_ram;
        uint32_t dst = s->cirrus_blt_dstaddr;
        for (int x = 0; x < s->cirrus_blt_width; x++) {
            uint32_t a = (dst + x) & s->cirrus_addr_mask;
            vram[a] = apply_rop(rop_idx, vram[a], s->cirrus_bltbuf[x]);
        }
    }

    s->cirrus_blt_dstaddr += s->cirrus_blt_dstpitch;
    s->cirrus_srccounter -= s->cirrus_blt_srcpitch;

    if (s->cirrus_srccounter <= 0) {
        cirrus_bitblt_reset(s);
    } else {
        /* Ready for next scanline from host */
        s->cirrus_srcptr = s->cirrus_bltbuf;
        s->cirrus_srcptr_end = s->cirrus_bltbuf + s->cirrus_blt_srcpitch;
    }
}

static void cirrus_bitblt_start(CirrusVGAState *s)
{
    init_rop_table();
    if (!s->enable_blitter) {
        cirrus_bitblt_reset(s);
        return;
    }

    s->vga->gr[0x31] |= CIRRUS_BLT_BUSY;

    s->cirrus_blt_width = (s->vga->gr[0x20] | (s->vga->gr[0x21] << 8)) + 1;
    s->cirrus_blt_height = (s->vga->gr[0x22] | (s->vga->gr[0x23] << 8)) + 1;
    s->cirrus_blt_dstpitch = (int16_t)(s->vga->gr[0x24] | (s->vga->gr[0x25] << 8));
    s->cirrus_blt_srcpitch = (int16_t)(s->vga->gr[0x26] | (s->vga->gr[0x27] << 8));
    s->cirrus_blt_dstaddr = (s->vga->gr[0x28] | (s->vga->gr[0x29] << 8) | (s->vga->gr[0x2a] << 16)) & s->cirrus_addr_mask;
    s->cirrus_blt_srcaddr = (s->vga->gr[0x2c] | (s->vga->gr[0x2d] << 8) | (s->vga->gr[0x2e] << 16)) & s->cirrus_addr_mask;
    s->cirrus_blt_mode = s->vga->gr[0x30];
    s->cirrus_blt_modeext = s->vga->gr[0x33];

    switch (s->cirrus_blt_mode & CIRRUS_BLTMODE_PIXELWIDTHMASK) {
    case CIRRUS_BLTMODE_PIXELWIDTH8:  s->cirrus_blt_pixelwidth = 1; break;
    case CIRRUS_BLTMODE_PIXELWIDTH16: s->cirrus_blt_pixelwidth = 2; break;
    case CIRRUS_BLTMODE_PIXELWIDTH24: s->cirrus_blt_pixelwidth = 3; break;
    case CIRRUS_BLTMODE_PIXELWIDTH32: s->cirrus_blt_pixelwidth = 4; break;
    default: s->cirrus_blt_pixelwidth = 1; break;
    }

    uint8_t blt_rop = s->vga->gr[0x32];

    static int blt_cnt = 0;
    if (++blt_cnt <= 200) {
        log_cirrus("[BLT_START] mode=%02x mext=%02x rop=%02x w=%d h=%d dst=%08x (x=%d y=%d) src=%08x dpitch=%d spitch=%d mask=%02x\n",
            s->cirrus_blt_mode, s->cirrus_blt_modeext, blt_rop, s->cirrus_blt_width, s->cirrus_blt_height,
            s->cirrus_blt_dstaddr, (s->cirrus_blt_dstaddr % 1280)/2, s->cirrus_blt_dstaddr / 1280,
            s->cirrus_blt_srcaddr, s->cirrus_blt_dstpitch, s->cirrus_blt_srcpitch, s->vga->gr[0x2f]);
    }

    /* Solid Fill */
    if ((s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_SOLIDFILL) &&
        (s->cirrus_blt_mode & (CIRRUS_BLTMODE_PATTERNCOPY | CIRRUS_BLTMODE_COLOREXPAND)) ==
        (CIRRUS_BLTMODE_PATTERNCOPY | CIRRUS_BLTMODE_COLOREXPAND)) {
        cirrus_bitblt_fgcol(s);
        cirrus_bitblt_solidfill(s, blt_rop);
        return;
    }

    /* CPU-to-Video Blit */
    if (s->cirrus_blt_mode & CIRRUS_BLTMODE_MEMSYSSRC) {
        s->cirrus_srcptr = s->cirrus_bltbuf;
        if (s->cirrus_blt_mode & CIRRUS_BLTMODE_PATTERNCOPY) {
            if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND)
                s->cirrus_blt_srcpitch = 8;
            else
                s->cirrus_blt_srcpitch = 8 * 8 * s->cirrus_blt_pixelwidth;
            s->cirrus_srccounter = s->cirrus_blt_srcpitch;
        } else if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND) {
            int w = (s->cirrus_blt_width / s->cirrus_blt_pixelwidth) + (s->vga->gr[0x2f] & 7);
            if (s->cirrus_blt_modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY)
                s->cirrus_blt_srcpitch = ((w + 31) >> 5) * 4;
            else
                s->cirrus_blt_srcpitch = ((w + 7) >> 3);
            s->cirrus_srccounter = s->cirrus_blt_srcpitch * s->cirrus_blt_height;
        } else {
            s->cirrus_blt_srcpitch = (s->cirrus_blt_width + 3) & ~3;
            s->cirrus_srccounter = s->cirrus_blt_srcpitch * s->cirrus_blt_height;
        }
        if (s->cirrus_blt_srcpitch > CIRRUS_BLTBUFSIZE) {
            cirrus_bitblt_reset(s);
            return;
        }
        s->cirrus_srcptr_end = s->cirrus_bltbuf + s->cirrus_blt_srcpitch;
        return;
    }

    /* Video-to-Video Blit */
    if (s->cirrus_blt_mode & CIRRUS_BLTMODE_PATTERNCOPY) {
        cirrus_bitblt_common_patterncopy(s, false);
    } else if (s->cirrus_blt_mode & CIRRUS_BLTMODE_COLOREXPAND) {
        cirrus_do_colorexpand(s, s->cirrus_blt_dstaddr, s->cirrus_blt_srcaddr, false);
        cirrus_bitblt_reset(s);
    } else {
        cirrus_bitblt_videotovideo_copy(s);
    }
}

bool cirrus_is_bitblt_cputovideo(CirrusVGAState *s)
{
    return s && (s->cirrus_srcptr < s->cirrus_srcptr_end);
}

void cirrus_bitblt_cputovideo_write(CirrusVGAState *s, uint8_t val)
{
    if (!s || s->cirrus_srcptr >= s->cirrus_srcptr_end) return;
    *s->cirrus_srcptr++ = val;
    if (s->cirrus_srcptr >= s->cirrus_srcptr_end) {
        cirrus_bitblt_cputovideo_next(s);
    }
}

void cirrus_bitblt_cputovideo_write_buf(CirrusVGAState *s, const uint8_t *buf, int len)
{
    while (len > 0 && cirrus_is_bitblt_cputovideo(s)) {
        int avail = s->cirrus_srcptr_end - s->cirrus_srcptr;
        int chunk = MIN(avail, len);
        memcpy(s->cirrus_srcptr, buf, chunk);
        s->cirrus_srcptr += chunk;
        buf += chunk;
        len -= chunk;
        if (s->cirrus_srcptr >= s->cirrus_srcptr_end) {
            cirrus_bitblt_cputovideo_next(s);
        }
    }
}

static uint8_t cirrus_read_hidden_dac(CirrusVGAState *s)
{
    if (++s->cirrus_hidden_dac_lockindex == 5) {
        s->cirrus_hidden_dac_lockindex = 0;
        return s->cirrus_hidden_dac_data;
    }
    return 0xff;
}

static void cirrus_write_hidden_dac(CirrusVGAState *s, uint8_t val)
{
    if (s->cirrus_hidden_dac_lockindex == 4) {
        s->cirrus_hidden_dac_data = val;
    }
    s->cirrus_hidden_dac_lockindex = 0;
}

/* MMIO Register access (offset >= 0x100 in BAR1) */
static uint8_t cirrus_mmio_blt_read(CirrusVGAState *s, unsigned reg)
{
    switch (reg) {
    case CIRRUS_MMIO_BLTBGCOLOR + 0: return s->cirrus_shadow_gr0;
    case CIRRUS_MMIO_BLTBGCOLOR + 1: return s->vga->gr[0x10];
    case CIRRUS_MMIO_BLTBGCOLOR + 2: return s->vga->gr[0x12];
    case CIRRUS_MMIO_BLTBGCOLOR + 3: return s->vga->gr[0x14];

    case CIRRUS_MMIO_BLTFGCOLOR + 0: return s->cirrus_shadow_gr1;
    case CIRRUS_MMIO_BLTFGCOLOR + 1: return s->vga->gr[0x11];
    case CIRRUS_MMIO_BLTFGCOLOR + 2: return s->vga->gr[0x13];
    case CIRRUS_MMIO_BLTFGCOLOR + 3: return s->vga->gr[0x15];

    case CIRRUS_MMIO_BLTWIDTH + 0: return s->vga->gr[0x20];
    case CIRRUS_MMIO_BLTWIDTH + 1: return s->vga->gr[0x21];
    case CIRRUS_MMIO_BLTHEIGHT + 0: return s->vga->gr[0x22];
    case CIRRUS_MMIO_BLTHEIGHT + 1: return s->vga->gr[0x23];

    case CIRRUS_MMIO_BLTDESTPITCH + 0: return s->vga->gr[0x24];
    case CIRRUS_MMIO_BLTDESTPITCH + 1: return s->vga->gr[0x25];
    case CIRRUS_MMIO_BLTSRCPITCH + 0: return s->vga->gr[0x26];
    case CIRRUS_MMIO_BLTSRCPITCH + 1: return s->vga->gr[0x27];

    case CIRRUS_MMIO_BLTDESTADDR + 0: return s->vga->gr[0x28];
    case CIRRUS_MMIO_BLTDESTADDR + 1: return s->vga->gr[0x29];
    case CIRRUS_MMIO_BLTDESTADDR + 2: return s->vga->gr[0x2a];

    case CIRRUS_MMIO_BLTSRCADDR + 0: return s->vga->gr[0x2c];
    case CIRRUS_MMIO_BLTSRCADDR + 1: return s->vga->gr[0x2d];
    case CIRRUS_MMIO_BLTSRCADDR + 2: return s->vga->gr[0x2e];

    case CIRRUS_MMIO_BLTWRITEMASK: return s->vga->gr[0x2f];
    case CIRRUS_MMIO_BLTMODE: return s->vga->gr[0x30];
    case CIRRUS_MMIO_BLTROP: return s->vga->gr[0x32];
    case CIRRUS_MMIO_BLTMODEEXT: return s->vga->gr[0x33];

    case CIRRUS_MMIO_BLTTRANSPARENTCOLOR + 0: return s->vga->gr[0x34];
    case CIRRUS_MMIO_BLTTRANSPARENTCOLOR + 1: return s->vga->gr[0x35];
    case CIRRUS_MMIO_BLTTRANSPARENTCOLORMASK + 0: return s->vga->gr[0x38];
    case CIRRUS_MMIO_BLTTRANSPARENTCOLORMASK + 1: return s->vga->gr[0x39];

    case CIRRUS_MMIO_BLTSTATUS: return s->vga->gr[0x31];
    default: return 0xff;
    }
}

static void cirrus_mmio_blt_write(CirrusVGAState *s, unsigned reg, uint8_t val)
{
    switch (reg) {
    case CIRRUS_MMIO_BLTBGCOLOR + 0: s->cirrus_shadow_gr0 = val; s->vga->gr[0x00] = val; break;
    case CIRRUS_MMIO_BLTBGCOLOR + 1: s->vga->gr[0x10] = val; break;
    case CIRRUS_MMIO_BLTBGCOLOR + 2: s->vga->gr[0x12] = val; break;
    case CIRRUS_MMIO_BLTBGCOLOR + 3: s->vga->gr[0x14] = val; break;

    case CIRRUS_MMIO_BLTFGCOLOR + 0: s->cirrus_shadow_gr1 = val; s->vga->gr[0x01] = val; break;
    case CIRRUS_MMIO_BLTFGCOLOR + 1: s->vga->gr[0x11] = val; break;
    case CIRRUS_MMIO_BLTFGCOLOR + 2: s->vga->gr[0x13] = val; break;
    case CIRRUS_MMIO_BLTFGCOLOR + 3: s->vga->gr[0x15] = val; break;

    case CIRRUS_MMIO_BLTWIDTH + 0: s->vga->gr[0x20] = val; break;
    case CIRRUS_MMIO_BLTWIDTH + 1: s->vga->gr[0x21] = val & 0x1f; break;
    case CIRRUS_MMIO_BLTHEIGHT + 0: s->vga->gr[0x22] = val; break;
    case CIRRUS_MMIO_BLTHEIGHT + 1: s->vga->gr[0x23] = val & 0x1f; break;

    case CIRRUS_MMIO_BLTDESTPITCH + 0: s->vga->gr[0x24] = val; break;
    case CIRRUS_MMIO_BLTDESTPITCH + 1: s->vga->gr[0x25] = val & 0x1f; break;
    case CIRRUS_MMIO_BLTSRCPITCH + 0: s->vga->gr[0x26] = val; break;
    case CIRRUS_MMIO_BLTSRCPITCH + 1: s->vga->gr[0x27] = val & 0x1f; break;

    case CIRRUS_MMIO_BLTDESTADDR + 0: s->vga->gr[0x28] = val; break;
    case CIRRUS_MMIO_BLTDESTADDR + 1: s->vga->gr[0x29] = val; break;
    case CIRRUS_MMIO_BLTDESTADDR + 2:
        s->vga->gr[0x2a] = val & 0x3f;
        if (s->vga->gr[0x31] & CIRRUS_BLT_AUTOSTART) {
            cirrus_bitblt_start(s);
        }
        break;

    case CIRRUS_MMIO_BLTSRCADDR + 0: s->vga->gr[0x2c] = val; break;
    case CIRRUS_MMIO_BLTSRCADDR + 1: s->vga->gr[0x2d] = val; break;
    case CIRRUS_MMIO_BLTSRCADDR + 2: s->vga->gr[0x2e] = val & 0x3f; break;

    case CIRRUS_MMIO_BLTWRITEMASK: s->vga->gr[0x2f] = val; break;
    case CIRRUS_MMIO_BLTMODE: s->vga->gr[0x30] = val; break;
    case CIRRUS_MMIO_BLTROP: s->vga->gr[0x32] = val; break;
    case CIRRUS_MMIO_BLTMODEEXT: s->vga->gr[0x33] = val; break;

    case CIRRUS_MMIO_BLTTRANSPARENTCOLOR + 0: s->vga->gr[0x34] = val; break;
    case CIRRUS_MMIO_BLTTRANSPARENTCOLOR + 1: s->vga->gr[0x35] = val; break;
    case CIRRUS_MMIO_BLTTRANSPARENTCOLORMASK + 0: s->vga->gr[0x38] = val; break;
    case CIRRUS_MMIO_BLTTRANSPARENTCOLORMASK + 1: s->vga->gr[0x39] = val; break;

    case CIRRUS_MMIO_BLTSTATUS:
        s->vga->gr[0x31] = val;
        if (val & CIRRUS_BLT_RESET) {
            cirrus_bitblt_reset(s);
        } else if (val & CIRRUS_BLT_START) {
            cirrus_bitblt_start(s);
        }
        break;
    }
}

uint8_t cirrus_mmio_read(CirrusVGAState *s, uint32_t addr)
{
    if (addr >= 0x100) {
        return cirrus_mmio_blt_read(s, addr - 0x100);
    } else {
        return cirrus_ioport_read(s, 0x3c0 + addr);
    }
}

void cirrus_mmio_write(CirrusVGAState *s, uint32_t addr, uint8_t val)
{
    if (addr >= 0x100) {
        cirrus_mmio_blt_write(s, addr - 0x100, val);
    } else {
        cirrus_ioport_write(s, 0x3c0 + addr, val);
    }
}

/* Standard I/O port read handler */
uint32_t cirrus_ioport_read(CirrusVGAState *s, uint32_t addr)
{
    switch (addr) {
    case 0x3c4:
        return s->vga->sr_index;
    case 0x3c5:
        switch (s->vga->sr_index) {
        case 0x06: return s->vga->sr[0x06];
        case 0x10: case 0x30: case 0x50: case 0x70:
        case 0x90: case 0xb0: case 0xd0: case 0xf0:
            return s->vga->sr[0x10];
        case 0x11: case 0x31: case 0x51: case 0x71:
        case 0x91: case 0xb1: case 0xd1: case 0xf1:
            return s->vga->sr[0x11];
        default:
            if (s->vga->sr_index < 64)
                return s->vga->sr[s->vga->sr_index];
            return 0xff;
        }
    case 0x3c6:
        return cirrus_read_hidden_dac(s);
    case 0x3c8:
        s->cirrus_hidden_dac_lockindex = 0;
        return s->vga->dac_write_index;
    case 0x3c9:
        if (s->vga->sr[0x12] & CIRRUS_CURSOR_HIDDENPEL) {
            uint8_t val = s->cirrus_hidden_palette[(s->vga->dac_read_index & 0x0f) * 3 + s->vga->dac_sub_index];
            if (++s->vga->dac_sub_index == 3) {
                s->vga->dac_sub_index = 0;
                s->vga->dac_read_index++;
            }
            return val;
        }
        return vga_ioport_read(s->vga, addr);
    case 0x3ce:
        return s->vga->gr_index;
    case 0x3cf:
        switch (s->vga->gr_index) {
        case 0x00: return s->cirrus_shadow_gr0;
        case 0x01: return s->cirrus_shadow_gr1;
        default:
            if (s->vga->gr_index < 64)
                return s->vga->gr[s->vga->gr_index];
            return 0xff;
        }
    case 0x3b4: case 0x3d4:
        return s->vga->cr_index;
    case 0x3b5: case 0x3d5:
        switch (s->vga->cr_index) {
        case 0x24: return s->vga->ar_flip_flop << 7;
        case 0x26: return s->vga->ar_index & 0x3f;
        case 0x27: return s->device_id;
        default:
            return vga_ioport_read(s->vga, addr);
        }
    default:
        return vga_ioport_read(s->vga, addr);
    }
}

/* Standard I/O port write handler */
void cirrus_ioport_write(CirrusVGAState *s, uint32_t addr, uint32_t val)
{
    val &= 0xff;
    switch (addr) {
    case 0x3c4:
        s->vga->sr_index = val;
        break;
    case 0x3c5:
        switch (s->vga->sr_index) {
        case 0x06: /* Unlock extensions: 0x12 unlocks, any other locks */
            if ((val & 0x17) == 0x12) {
                s->vga->sr[0x06] = 0x12;
            } else {
                s->vga->sr[0x06] = 0x0f;
            }
            break;
        case 0x10: case 0x30: case 0x50: case 0x70:
        case 0x90: case 0xb0: case 0xd0: case 0xf0:
            s->vga->sr[0x10] = val;
            s->hw_cursor_x = (val << 3) | (s->vga->sr_index >> 5);
            break;
        case 0x11: case 0x31: case 0x51: case 0x71:
        case 0x91: case 0xb1: case 0xd1: case 0xf1:
            s->vga->sr[0x11] = val;
            s->hw_cursor_y = (val << 3) | (s->vga->sr_index >> 5);
            break;
        case 0x12:
            s->vga->sr[0x12] = val;
            break;
        case 0x17:
            s->vga->sr[0x17] = (s->vga->sr[0x17] & 0x38) | (val & 0xc7);
            break;
        default:
            if (s->vga->sr_index < 64) {
                s->vga->sr[s->vga->sr_index] = val;
            }
            if (s->vga->sr_index < 8) {
                vga_ioport_write(s->vga, addr, val);
            }
            break;
        }
        break;
    case 0x3c6:
        cirrus_write_hidden_dac(s, val);
        break;
    case 0x3c8:
        s->cirrus_hidden_dac_lockindex = 0;
        vga_ioport_write(s->vga, addr, val);
        break;
    case 0x3c9:
        if (s->vga->sr[0x12] & CIRRUS_CURSOR_HIDDENPEL) {
            s->cirrus_hidden_palette[(s->vga->dac_write_index & 0x0f) * 3 + s->vga->dac_sub_index] = val;
            if (++s->vga->dac_sub_index == 3) {
                s->vga->dac_sub_index = 0;
                s->vga->dac_write_index++;
            }
            return;
        }
        vga_ioport_write(s->vga, addr, val);
        break;
    case 0x3ce:
        s->vga->gr_index = val;
        break;
    case 0x3cf:
        switch (s->vga->gr_index) {
        case 0x00:
            s->vga->gr[0x00] = val & 0x0f;
            s->cirrus_shadow_gr0 = val;
            break;
        case 0x01:
            s->vga->gr[0x01] = val & 0x0f;
            s->cirrus_shadow_gr1 = val;
            break;
        case 0x02: case 0x03: case 0x04:
        case 0x06: case 0x07: case 0x08:
            s->vga->gr[s->vga->gr_index] = val & cirrus_gr_mask[s->vga->gr_index];
            break;
        case 0x05:
            s->vga->gr[0x05] = val & 0x7f;
            break;
        case 0x09: case 0x0a: case 0x0b:
            s->vga->gr[s->vga->gr_index] = val;
            cirrus_update_bank_ptr(s, 0);
            cirrus_update_bank_ptr(s, 1);
            break;
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
            s->vga->gr[s->vga->gr_index] = val;
            break;
        case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x29:
        case 0x2c: case 0x2d: case 0x2f: case 0x30: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x38: case 0x39:
            s->vga->gr[s->vga->gr_index] = val;
            break;
        case 0x21: case 0x23: case 0x25: case 0x27:
            s->vga->gr[s->vga->gr_index] = val & 0x1f;
            break;
        case 0x2a:
            s->vga->gr[0x2a] = val & 0x3f;
            if (s->vga->gr[0x31] & CIRRUS_BLT_AUTOSTART) {
                cirrus_bitblt_start(s);
            }
            break;
        case 0x2e:
            s->vga->gr[0x2e] = val & 0x3f;
            break;
        case 0x31:
            s->vga->gr[0x31] = val;
            if (val & CIRRUS_BLT_RESET) {
                cirrus_bitblt_reset(s);
            } else if (val & CIRRUS_BLT_START) {
                cirrus_bitblt_start(s);
            }
            break;
        default:
            if (s->vga->gr_index < 64) {
                s->vga->gr[s->vga->gr_index] = val;
            }
            break;
        }
        break;
    case 0x3b4: case 0x3d4:
        s->vga->cr_index = val;
        break;
    case 0x3b5: case 0x3d5:
        if (s->vga->cr_index < 256) {
            s->vga->cr[s->vga->cr_index] = val;
        }
        vga_ioport_write(s->vga, addr, val);
        break;
    default:
        vga_ioport_write(s->vga, addr, val);
        break;
    }
}

/* Banked legacy memory access (0xA0000 - 0xBFFFF) */
uint8_t cirrus_vga_mem_read(CirrusVGAState *s, uint32_t addr)
{
    if ((s->vga->sr[0x07] & 0x01) == 0) {
        return vga_mem_read(s->vga, addr);
    }
    if (addr < 0x10000) {
        unsigned bank_idx = addr >> 15;
        unsigned bank_off = addr & 0x7fff;
        if (bank_off < s->cirrus_bank_limit[bank_idx]) {
            bank_off += s->cirrus_bank_base[bank_idx];
            if ((s->vga->gr[0x0B] & 0x14) == 0x14) bank_off <<= 4;
            else if (s->vga->gr[0x0B] & 0x02) bank_off <<= 3;
            return s->vga->vga_ram[bank_off & s->cirrus_addr_mask];
        }
        return 0xff;
    } else if (addr >= 0x18000 && addr < 0x18100) {
        if ((s->vga->sr[0x17] & 0x44) == 0x04) {
            return cirrus_mmio_blt_read(s, addr & 0xff);
        }
    }
    return 0xff;
}

static void cirrus_mem_writeb_mode4and5_8bpp(CirrusVGAState *s,
                                             unsigned mode,
                                             unsigned offset,
                                             uint32_t mem_value)
{
    int x;
    unsigned val = mem_value;
    uint8_t *dst;

    for (x = 0; x < 8; x++) {
        dst = s->vga->vga_ram + ((offset + x) & s->cirrus_addr_mask);
        if (val & 0x80) {
            *dst = s->cirrus_shadow_gr1;
        } else if (mode == 5) {
            *dst = s->cirrus_shadow_gr0;
        }
        val <<= 1;
    }
}

static void cirrus_mem_writeb_mode4and5_16bpp(CirrusVGAState *s,
                                              unsigned mode,
                                              unsigned offset,
                                              uint32_t mem_value)
{
    static int m45_cnt = 0;
    if (++m45_cnt <= 100) {
        log_cirrus("[MODE45_16] mode=%d off=%08x (x=%d y=%d) val=%02x fg=%02x%02x bg=%02x%02x\n",
            mode, offset, (offset % 1280)/2, offset / 1280, mem_value,
            s->vga->gr[0x11], s->cirrus_shadow_gr1, s->vga->gr[0x10], s->cirrus_shadow_gr0);
    }
    int x;
    unsigned val = mem_value;
    uint8_t *dst;

    for (x = 0; x < 8; x++) {
        dst = s->vga->vga_ram + ((offset + 2 * x) & s->cirrus_addr_mask & ~1);
        if (val & 0x80) {
            *dst = s->cirrus_shadow_gr1;
            *(dst + 1) = s->vga->gr[0x11];
        } else if (mode == 5) {
            *dst = s->cirrus_shadow_gr0;
            *(dst + 1) = s->vga->gr[0x10];
        }
        val <<= 1;
    }
}

void cirrus_vga_mem_write(CirrusVGAState *s, uint32_t addr, uint8_t val)
{
    if (s->cirrus_srcptr < s->cirrus_srcptr_end) {
        cirrus_bitblt_cputovideo_write(s, val);
        return;
    }
    if ((s->vga->sr[0x07] & 0x01) == 0) {
        vga_mem_write(s->vga, addr, val);
        return;
    }
    if (addr < 0x10000) {
        unsigned bank_idx = addr >> 15;
        unsigned bank_off = addr & 0x7fff;
        if (bank_off < s->cirrus_bank_limit[bank_idx]) {
            bank_off += s->cirrus_bank_base[bank_idx];
            if ((s->vga->gr[0x0B] & 0x14) == 0x14) {
                bank_off <<= 4;
            } else if (s->vga->gr[0x0B] & 0x02) {
                bank_off <<= 3;
            }
            bank_off &= s->cirrus_addr_mask;
            unsigned mode = s->vga->gr[0x05] & 0x7;
            if (mode < 4 || mode > 5 || ((s->vga->gr[0x0B] & 0x4) == 0)) {
                s->vga->vga_ram[bank_off] = val;
            } else {
                if ((s->vga->gr[0x0B] & 0x14) != 0x14) {
                    cirrus_mem_writeb_mode4and5_8bpp(s, mode, bank_off, val);
                } else {
                    cirrus_mem_writeb_mode4and5_16bpp(s, mode, bank_off, val);
                }
            }
        }
    } else if (addr >= 0x18000 && addr < 0x18100) {
        if ((s->vga->sr[0x17] & 0x44) == 0x04) {
            cirrus_mmio_blt_write(s, addr & 0xff, val);
        }
    }
}

uint8_t cirrus_linear_read8(CirrusVGAState *s, uint32_t addr)
{
    addr &= s->cirrus_addr_mask;

    if (((s->vga->sr[0x17] & 0x44) == 0x44) &&
        ((addr & s->linear_mmio_mask) == s->linear_mmio_mask)) {
        return cirrus_mmio_blt_read(s, addr & 0xff);
    }

    if ((s->vga->gr[0x0B] & 0x14) == 0x14) {
        addr <<= 4;
    } else if (s->vga->gr[0x0B] & 0x02) {
        addr <<= 3;
    }
    addr &= s->cirrus_addr_mask;
    return s->vga->vga_ram[addr];
}

void cirrus_linear_write8(CirrusVGAState *s, uint32_t addr, uint8_t val)
{
    addr &= s->cirrus_addr_mask;

    if (((s->vga->sr[0x17] & 0x44) == 0x44) &&
        ((addr & s->linear_mmio_mask) == s->linear_mmio_mask)) {
        cirrus_mmio_blt_write(s, addr & 0xff, val);
        return;
    }

    if (s->cirrus_srcptr < s->cirrus_srcptr_end) {
        cirrus_bitblt_cputovideo_write(s, val);
        return;
    }

    if ((s->vga->gr[0x0B] & 0x14) == 0x14) {
        addr <<= 4;
    } else if (s->vga->gr[0x0B] & 0x02) {
        addr <<= 3;
    }
    addr &= s->cirrus_addr_mask;

    unsigned mode = s->vga->gr[0x05] & 0x7;
    if (mode < 4 || mode > 5 || ((s->vga->gr[0x0B] & 0x4) == 0)) {
        s->vga->vga_ram[addr] = val;
    } else {
        if ((s->vga->gr[0x0B] & 0x14) != 0x14) {
            cirrus_mem_writeb_mode4and5_8bpp(s, mode, addr, val);
        } else {
            cirrus_mem_writeb_mode4and5_16bpp(s, mode, addr, val);
        }
    }
}

int cirrus_get_bpp16(CirrusVGAState *s)
{
    return (s->cirrus_hidden_dac_data & 0x01) ? 16 : 15;
}

/* Hardware cursor rendering onto 32-bit RGBA display buffer */
void cirrus_cursor_draw(CirrusVGAState *s, uint8_t *fb_data, int width, int height, int stride, int x_offset, int y_offset)
{
    if (!s || !(s->vga->sr[0x12] & CIRRUS_CURSOR_SHOW)) return;

    int cur_w, cur_h, poffset;
    const uint8_t *src = s->vga->vga_ram + s->real_vram_size - 16384;

    if (s->vga->sr[0x12] & CIRRUS_CURSOR_LARGE) {
        cur_w = 64; cur_h = 64;
        src += (s->vga->sr[0x13] & 0x3c) * 256;
        poffset = 8;
    } else {
        cur_w = 32; cur_h = 32;
        src += (s->vga->sr[0x13] & 0x3f) * 256;
        poffset = 128;
    }

    int cx = s->hw_cursor_x + x_offset;
    int cy = s->hw_cursor_y + y_offset;

    uint32_t col0 = 0x00000000; /* Black */
    uint32_t col1 = 0x00FFFFFF; /* White */

    if (s->cirrus_hidden_palette[0] || s->cirrus_hidden_palette[1] || s->cirrus_hidden_palette[2] ||
        s->cirrus_hidden_palette[3] || s->cirrus_hidden_palette[4] || s->cirrus_hidden_palette[5]) {
        uint8_t r0 = s->cirrus_hidden_palette[0] << 2;
        uint8_t g0 = s->cirrus_hidden_palette[1] << 2;
        uint8_t b0 = s->cirrus_hidden_palette[2] << 2;
        uint8_t r1 = s->cirrus_hidden_palette[3] << 2;
        uint8_t g1 = s->cirrus_hidden_palette[4] << 2;
        uint8_t b1 = s->cirrus_hidden_palette[5] << 2;
        col0 = b0 | (g0 << 8) | (r0 << 16);
        col1 = b1 | (g1 << 8) | (r1 << 16);
    }

    for (int y = 0; y < cur_h; y++) {
        int scr_y = cy + y;
        if (scr_y < 0 || scr_y >= height) continue;

        const uint8_t *plane0, *plane1;
        if (cur_h == 64) {
            plane0 = src + y * 16;
            plane1 = plane0 + poffset;
        } else {
            plane0 = src + y * 4;
            plane1 = plane0 + poffset;
        }

#if BPP == 16
        uint16_t col0_16 = ((col0 >> 3) & 0x1f) | (((col0 >> 10) & 0x3f) << 5) | (((col0 >> 19) & 0x1f) << 11);
        uint16_t col1_16 = ((col1 >> 3) & 0x1f) | (((col1 >> 10) & 0x3f) << 5) | (((col1 >> 19) & 0x1f) << 11);
        uint16_t *dst_line = (uint16_t *)(fb_data + scr_y * stride);

        for (int x = 0; x < cur_w; x++) {
            int scr_x = cx + x;
            if (scr_x < 0 || scr_x >= width) continue;

            int b0 = (plane0[x >> 3] >> (7 - (x & 7))) & 1;
            int b1 = (plane1[x >> 3] >> (7 - (x & 7))) & 1;

            switch (b0 | (b1 << 1)) {
            case 0: /* Transparent */ break;
            case 1: /* Invert */ dst_line[scr_x] ^= 0xFFFF; break;
            case 2: /* Color 0 */ dst_line[scr_x] = col0_16; break;
            case 3: /* Color 1 */ dst_line[scr_x] = col1_16; break;
            }
        }
#else
        uint32_t *dst_line = (uint32_t *)(fb_data + scr_y * stride);

        for (int x = 0; x < cur_w; x++) {
            int scr_x = cx + x;
            if (scr_x < 0 || scr_x >= width) continue;

            int b0 = (plane0[x >> 3] >> (7 - (x & 7))) & 1;
            int b1 = (plane1[x >> 3] >> (7 - (x & 7))) & 1;

            switch (b0 | (b1 << 1)) {
            case 0: /* Transparent */ break;
            case 1: /* Invert */ dst_line[scr_x] ^= 0x00FFFFFF; break;
            case 2: /* Color 0 */ dst_line[scr_x] = col0; break;
            case 3: /* Color 1 */ dst_line[scr_x] = col1; break;
            }
        }
#endif
    }
}

void cirrus_reset(CirrusVGAState *s)
{
    init_rop_table();
    s->vga->sr[0x06] = 0x0f;
    s->vga->sr[0x1F] = 0x2d;
    s->vga->gr[0x18] = 0x0f;
    if (s->real_vram_size <= 2 * 1024 * 1024) {
        s->vga->sr[0x0f] = 0x18; // 2MB DRAM flag (SeaBIOS: ((v>>3)&3)==3, not 0x80 -> 2048K)
        s->vga->sr[0x15] = 0x03; // 2MB memory size
    } else {
        s->vga->sr[0x0f] = 0x98; // 4MB DRAM flag
        s->vga->sr[0x15] = 0x04; // 4MB memory size
    }
    s->vga->sr[0x17] = 0x20; // PCI bus
    s->vga->cr[0x27] = s->device_id;
    s->cirrus_hidden_dac_lockindex = 5;
    s->cirrus_hidden_dac_data = 0;
    cirrus_bitblt_reset(s);
    cirrus_update_bank_ptr(s, 0);
    cirrus_update_bank_ptr(s, 1);
}

CirrusVGAState *cirrus_vga_init(VGAState *vga, PCIBus *bus, void *pc_opaque, PCIBarSetFunc *bar_set)
{
    CirrusVGAState *s = (CirrusVGAState *)calloc(1, sizeof(CirrusVGAState));
    if (!s) return NULL;

    s->vga = vga;
    vga->cirrus = s;
    s->device_id = CIRRUS_ID_CLGD5446;
    s->real_vram_size = vga->vga_ram_size;
    s->cirrus_addr_mask = s->real_vram_size - 1;
    s->linear_mmio_mask = s->real_vram_size - 256;
    s->enable_blitter = true;

    cirrus_reset(s);

    /* Register PCI Device: 1013:00b8 (Cirrus Logic CL-GD5446 VGA) */
    s->pci_dev = pci_register_device(bus, "Cirrus CLGD5446", -1, 0x1013, 0x00b8, 0x00, 0x0300);

    /* BAR 0: 4MB Linear Frame Buffer (LFB) */
    uint32_t lfb_bar_size = 1;
    while (lfb_bar_size < (uint32_t)s->real_vram_size) lfb_bar_size <<= 1;
    pci_register_bar(s->pci_dev, 0, lfb_bar_size, PCI_ADDRESS_SPACE_MEM_PREFETCH, pc_opaque, bar_set);

    /* BAR 1: 4KB Memory-Mapped I/O (MMIO) */
    pci_register_bar(s->pci_dev, 1, CIRRUS_PNPMMIO_SIZE, PCI_ADDRESS_SPACE_MEM, pc_opaque, bar_set);

    /* Initialize OpenCL (host) and PPA (ESP32-P4) 2D engines */
    cirrus_opencl_init();
    cirrus_ppa_init();

    return s;
}
