/*
 * Cirrus Logic CL-GD5446 VGA Emulator Header
 *
 * Tiny386 Emulation System
 */

#ifndef CIRRUS_VGA_H
#define CIRRUS_VGA_H

#include <stdint.h>
#include <stdbool.h>
#include "pci.h"
#include "vga_internal.h"

#define CIRRUS_ID_CLGD5446          (0x2E << 2) // 0xB8

// Sequencer 0x07 (Extended Sequencer Mode)
#define CIRRUS_SR7_BPP_VGA          0x00
#define CIRRUS_SR7_BPP_SVGA         0x01
#define CIRRUS_SR7_BPP_MASK         0x0e
#define CIRRUS_SR7_BPP_8            0x00
#define CIRRUS_SR7_BPP_16_DOUBLEVCLK 0x02
#define CIRRUS_SR7_BPP_24           0x04
#define CIRRUS_SR7_BPP_16           0x06
#define CIRRUS_SR7_BPP_32           0x08
#define CIRRUS_SR7_ISAADDR_MASK     0xe0

// Sequencer 0x0F (DRAM Control)
#define CIRRUS_MEMSIZE_512k         0x08
#define CIRRUS_MEMSIZE_1M           0x10
#define CIRRUS_MEMSIZE_2M           0x18
#define CIRRUS_MEMFLAGS_BANKSWITCH  0x80

// Sequencer 0x12 (Hardware Cursor)
#define CIRRUS_CURSOR_SHOW          0x01
#define CIRRUS_CURSOR_HIDDENPEL     0x02
#define CIRRUS_CURSOR_LARGE         0x04 // 64x64 if set, 32x32 if clear

// Sequencer 0x17 (Bus Configuration)
#define CIRRUS_BUSTYPE_PCI          0x20
#define CIRRUS_MMIO_ENABLE          0x04
#define CIRRUS_MMIO_USE_PCIADDR     0x40

// Graphics 0x0B (Banking)
#define CIRRUS_BANKING_DUAL         0x01
#define CIRRUS_BANKING_GRANULARITY_16K 0x20

// Graphics 0x30 (BitBlt Mode)
#define CIRRUS_BLTMODE_BACKWARDS    0x01
#define CIRRUS_BLTMODE_MEMSYSDEST   0x02
#define CIRRUS_BLTMODE_MEMSYSSRC    0x04
#define CIRRUS_BLTMODE_TRANSPARENTCOMP 0x08
#define CIRRUS_BLTMODE_PATTERNCOPY  0x40
#define CIRRUS_BLTMODE_COLOREXPAND  0x80
#define CIRRUS_BLTMODE_PIXELWIDTHMASK 0x30
#define CIRRUS_BLTMODE_PIXELWIDTH8  0x00
#define CIRRUS_BLTMODE_PIXELWIDTH16 0x10
#define CIRRUS_BLTMODE_PIXELWIDTH24 0x20
#define CIRRUS_BLTMODE_PIXELWIDTH32 0x30

// Graphics 0x31 (BitBlt Status / Control)
#define CIRRUS_BLT_BUSY             0x01
#define CIRRUS_BLT_START            0x02
#define CIRRUS_BLT_RESET            0x04
#define CIRRUS_BLT_FIFOUSED         0x10
#define CIRRUS_BLT_AUTOSTART        0x80

// Graphics 0x32 (Raster Operations - ROPs)
#define CIRRUS_ROP_0                0x00
#define CIRRUS_ROP_SRC_AND_DST      0x05
#define CIRRUS_ROP_NOP              0x06
#define CIRRUS_ROP_SRC_AND_NOTDST   0x09
#define CIRRUS_ROP_NOTDST           0x0b
#define CIRRUS_ROP_SRC              0x0d
#define CIRRUS_ROP_1                0x0e
#define CIRRUS_ROP_NOTSRC_AND_DST   0x50
#define CIRRUS_ROP_SRC_XOR_DST      0x59
#define CIRRUS_ROP_SRC_OR_DST       0x6d
#define CIRRUS_ROP_NOTSRC_OR_NOTDST 0x90
#define CIRRUS_ROP_SRC_NOTXOR_DST   0x95
#define CIRRUS_ROP_SRC_OR_NOTDST    0xad
#define CIRRUS_ROP_NOTSRC           0xd0
#define CIRRUS_ROP_NOTSRC_OR_DST    0xd6
#define CIRRUS_ROP_NOTSRC_AND_NOTDST 0xda

// Graphics 0x33 (BitBlt Mode Extension)
#define CIRRUS_BLTMODEEXT_DWORDGRANULARITY 0x01
#define CIRRUS_BLTMODEEXT_COLOREXPINV      0x02
#define CIRRUS_BLTMODEEXT_SOLIDFILL        0x04

// Memory-Mapped I/O Register Offsets (BAR1 offset >= 0x100)
#define CIRRUS_MMIO_BLTBGCOLOR        0x00
#define CIRRUS_MMIO_BLTFGCOLOR        0x04
#define CIRRUS_MMIO_BLTWIDTH          0x08
#define CIRRUS_MMIO_BLTHEIGHT         0x0a
#define CIRRUS_MMIO_BLTDESTPITCH      0x0c
#define CIRRUS_MMIO_BLTSRCPITCH       0x0e
#define CIRRUS_MMIO_BLTDESTADDR       0x10
#define CIRRUS_MMIO_BLTSRCADDR        0x14
#define CIRRUS_MMIO_BLTWRITEMASK      0x17
#define CIRRUS_MMIO_BLTMODE           0x18
#define CIRRUS_MMIO_BLTROP            0x1a
#define CIRRUS_MMIO_BLTMODEEXT        0x1b
#define CIRRUS_MMIO_BLTTRANSPARENTCOLOR 0x1c
#define CIRRUS_MMIO_BLTTRANSPARENTCOLORMASK 0x20
#define CIRRUS_MMIO_BLTSTATUS         0x40

#define CIRRUS_BLTBUFSIZE             (2048 * 4)
#define CIRRUS_PNPMMIO_SIZE           0x1000

typedef struct CirrusVGAState CirrusVGAState;
typedef void (*cirrus_bitblt_rop_t)(CirrusVGAState *s,
                                    uint32_t dstaddr, uint32_t srcaddr,
                                    int dstpitch, int srcpitch,
                                    int bltwidth, int bltheight);

struct CirrusVGAState {
    VGAState *vga;
    PCIDevice *pci_dev;
    int device_id;
    int real_vram_size;
    uint32_t cirrus_addr_mask;
    uint32_t linear_mmio_mask;

    uint8_t cirrus_shadow_gr0;
    uint8_t cirrus_shadow_gr1;
    uint8_t cirrus_hidden_dac_lockindex;
    uint8_t cirrus_hidden_dac_data;

    uint32_t cirrus_bank_base[2];
    uint32_t cirrus_bank_limit[2];
    uint8_t cirrus_hidden_palette[48];

    bool enable_blitter;
    int cirrus_blt_pixelwidth;
    int cirrus_blt_width;
    int cirrus_blt_height;
    int cirrus_blt_dstpitch;
    int cirrus_blt_srcpitch;
    uint32_t cirrus_blt_fgcol;
    uint32_t cirrus_blt_bgcol;
    uint32_t cirrus_blt_dstaddr;
    uint32_t cirrus_blt_srcaddr;
    uint8_t cirrus_blt_mode;
    uint8_t cirrus_blt_modeext;
    cirrus_bitblt_rop_t cirrus_rop;

    uint8_t cirrus_bltbuf[CIRRUS_BLTBUFSIZE];
    uint8_t *cirrus_srcptr;
    uint8_t *cirrus_srcptr_end;
    int32_t cirrus_srccounter;

    int hw_cursor_x;
    int hw_cursor_y;

    uint32_t lfb_addr;
    bool lfb_enabled;
    uint32_t mmio_addr;
    bool mmio_enabled;
};

CirrusVGAState *cirrus_vga_init(VGAState *vga, PCIBus *bus, void *pc_opaque, PCIBarSetFunc *bar_set);
void cirrus_reset(CirrusVGAState *s);

uint32_t cirrus_ioport_read(CirrusVGAState *s, uint32_t addr);
void cirrus_ioport_write(CirrusVGAState *s, uint32_t addr, uint32_t val);

uint8_t cirrus_mmio_read(CirrusVGAState *s, uint32_t addr);
void cirrus_mmio_write(CirrusVGAState *s, uint32_t addr, uint8_t val);

uint8_t cirrus_vga_mem_read(CirrusVGAState *s, uint32_t addr);
void cirrus_vga_mem_write(CirrusVGAState *s, uint32_t addr, uint8_t val);
uint8_t cirrus_linear_read8(CirrusVGAState *s, uint32_t addr);
void cirrus_linear_write8(CirrusVGAState *s, uint32_t addr, uint8_t val);

bool cirrus_is_bitblt_cputovideo(CirrusVGAState *s);
void cirrus_bitblt_cputovideo_write(CirrusVGAState *s, uint8_t val);
void cirrus_bitblt_cputovideo_write_buf(CirrusVGAState *s, const uint8_t *buf, int len);

int cirrus_get_bpp16(CirrusVGAState *s);
void cirrus_cursor_draw(CirrusVGAState *s, uint8_t *fb_data, int width, int height, int stride, int x_offset, int y_offset);

#endif /* CIRRUS_VGA_H */
