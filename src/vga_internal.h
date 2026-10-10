/*
 * Internal VGA and Framebuffer definitions for Tiny386
 */

#ifndef VGA_INTERNAL_H
#define VGA_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "vga.h"

#define MSR_COLOR_EMULATION 0x01
#define MSR_PAGE_SELECT     0x20

#define ST01_V_RETRACE      0x08
#define ST01_DISP_ENABLE    0x01

#define VBE_DISPI_INDEX_ID              0x0
#define VBE_DISPI_INDEX_XRES            0x1
#define VBE_DISPI_INDEX_YRES            0x2
#define VBE_DISPI_INDEX_BPP             0x3
#define VBE_DISPI_INDEX_ENABLE          0x4
#define VBE_DISPI_INDEX_BANK            0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH      0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     0x7
#define VBE_DISPI_INDEX_X_OFFSET        0x8
#define VBE_DISPI_INDEX_Y_OFFSET        0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xa
#define VBE_DISPI_INDEX_NB              0xb

#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID1                   0xB0C1
#define VBE_DISPI_ID2                   0xB0C2
#define VBE_DISPI_ID3                   0xB0C3
#define VBE_DISPI_ID4                   0xB0C4
#define VBE_DISPI_ID5                   0xB0C5

#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80

#define FB_ALLOC_ALIGN (1 << 20)
#define MAX_TEXT_WIDTH 132
#define MAX_TEXT_HEIGHT 60

struct FBDevice {
    int width;
    int height;
    int stride; /* current stride in bytes */
    uint8_t *fb_data; /* current pointer to the pixel data */
};

struct CirrusVGAState;

struct VGAState {
    FBDevice *fb_dev;
    int graphic_mode;
    void (*frame_begin)(void *opaque);
    void (*frame_end)(void *opaque);
    void *frame_opaque;
    uint32_t cursor_blink_time;
    int cursor_visible_phase;
    uint32_t retrace_time;
    int retrace_phase;
    int force_8dm;

    uint8_t *vga_ram;
    int vga_ram_size;
    
    uint8_t sr_index;
    uint8_t sr[64];
    uint8_t gr_index;
    uint8_t gr[64];
    uint8_t ar_index;
    uint8_t ar[32];
    int ar_flip_flop;
    uint8_t cr_index;
    uint8_t cr[256]; /* CRT registers */
    uint8_t msr; /* Misc Output Register */
    uint8_t fcr; /* Feature Control Register */
    uint8_t st00; /* status 0 */
    uint8_t st01; /* status 1 */
    uint8_t dac_state;
    uint8_t dac_sub_index;
    uint8_t dac_read_index;
    uint8_t dac_write_index;
    uint8_t dac_8bit;
    uint8_t dac_cache[3]; /* used when writing */
    uint8_t palette[768];
    int32_t bank_offset;

    uint32_t latch;

    int comp_ntsc;
    int term_refresh;
    VGATextOps *vga_text_ops;
    void *vga_text_obj;
    
    /* text mode state */
    uint32_t last_palette[16];
#ifndef FULL_UPDATE
    uint16_t last_ch_attr[MAX_TEXT_WIDTH * MAX_TEXT_HEIGHT];
#endif
    uint32_t last_width;
    uint32_t last_height;
    uint16_t last_line_offset;
    uint16_t last_start_addr;
    uint16_t last_cursor_offset;
    uint8_t last_cursor_start;
    uint8_t last_cursor_end;

    /* VBE extension */
    uint16_t vbe_index;
    uint16_t vbe_regs[VBE_DISPI_INDEX_NB];
    uint32_t vbe_start_addr;
    uint32_t vbe_line_offset;

    struct CirrusVGAState *cirrus;

#if defined(SCALE_3_2) || defined(SCALE_2_1) || defined(SWAPXY)
#ifndef LCD_WIDTH
#define LCD_WIDTH 2048
#endif
    uint8_t tmpbuf[(LCD_WIDTH > 720 ? LCD_WIDTH : 720) * 3 * 2];
#endif
};

#endif /* VGA_INTERNAL_H */
