#ifndef PC_H
#define PC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "i386.h"
#include "i8259.h"
#include "i8254.h"
#include "ide.h"
#include "vga.h"
#include "i8042.h"
#include "misc.h"
#include "adlib.h"
#include "ne2000.h"
#include "i8257.h"
#include "sb16.h"
#include "pcspk.h"
#include "pci.h"
#include "ini.h"

/// Platform HAL
uint32_t get_uticks();
void *pcmalloc(long size);
void *bigmalloc(size_t size);
int load_rom(void *phys_mem, const char *file, uword addr, int backward);

/// PC
#if defined(USE_CPUABS)
#include "kvm.h"
typedef struct CPUABS CPU;
#else
typedef CPUI386 CPU;
#endif

typedef struct {
	CPU *cpu;
	PicState2 *pic;
	PITState *pit;
	U8250 *serial;
	CMOS *cmos;
	IDEIFState *ide, *ide2;
	VGAState *vga;
	char *phys_mem;
	long phys_mem_size;
	char *vga_mem;
	int vga_mem_size;
	int64_t boot_start_time;

	SimpleFBDrawFunc *redraw;
	void *redraw_data;

	KBDState *i8042;
	PS2KbdState *kbd;
	PS2MouseState *mouse;
	AdlibState *adlib;
	NE2000State *ne2000;
	I8257State *isa_dma, *isa_hdma;
	SB16State *sb16;
	PCSpkState *pcspk;

	I440FXState *i440fx;
	PCIBus *pcibus;
	PCIDevice *pci_ide;
	PCIDevice *pci_vga;
	struct CirrusVGAState *cirrus;
	uword pci_vga_ram_addr;
	uword pci_vga_mmio_addr;

	EMULINK *emulink;

	// non-owning strings
	const char *bios;
	const char *vga_bios;

	u8 port92;
	int shutdown_state;
	u8 apm_prev;	/* last byte written to the APM BIOS port (0x8900) */
	/* Guest-initiated APM low-power request: 0 = none, 1 = standby,
	   2 = suspend.  SeaBIOS reports these by writing the bare ASCII
	   "Standby"/"Suspend" message to port 0x8900; a front-end that supports
	   it parks the machine and clears this field once the guest is resumed. */
	int apm_suspend;
	int reset_request;
	int pm_established;
	uint32_t pm_since_us;
	uint32_t disk_io_us;
	uint32_t halt_poll_us;
	uint32_t halt_since_us;
	uint32_t halt_cs, halt_ip;

	// non-owning strings
	const char *linuxstart;
	const char *kernel;
	const char *initrd;
	const char *cmdline;
	int enable_serial;
	int full_update;

	/* Drive paths and sizes for OSD display and media swapping */
	char hda_path[256];
	int64_t hda_size;
	char fda_path[256];
	int64_t fda_size;
	char cdb_path[256];
	int64_t cdb_size;
	/* Secondary-master virtual CD-ROM backed by a host shared folder.
	   cdc_size is 0 when the medium is folder-backed (size is only known
	   to the ISO synthesizer). */
	char cdc_path[256];
	int64_t cdc_size;
} PC;

// if filled by ini_parse(), all strings are malloc'd
typedef struct {
	const char *linuxstart;
	const char *kernel;
	const char *initrd;
	const char *cmdline;
	const char *bios;
	const char *vga_bios;
	const char *vga_card;
	long mem_size;
	long vga_mem_size;
	const char *disks[4];
	int iscd[4];
	const char *fdd[2];
	int fill_cmos;
	const char *boot;
	int width;
	int height;
	int cpu_gen;
	int fpu;
	int enable_serial;
	int vga_force_8dm;
	int ne2000_irq;
} PCConfig;

PC *pc_new(SimpleFBDrawFunc *redraw, void *redraw_data,
	   u8 *fb, PCConfig *conf);

void pc_vga_step(void *o);
/* Split VGA stepping for dual-core ESP32-P4 front-ends: pc_vga_tick() only
   advances the timing state (Core 0), pc_vga_render() rasterises into the
   framebuffer (Core 1).  Desktop front-ends keep using pc_vga_step(). */
int pc_vga_tick(void *o);
void pc_vga_render(void *o);
void pc_step(PC *pc);

/* True once the guest has settled into the halted real-mode idle loop it runs
   after a shutdown it cannot complete (e.g. Windows 95's "It's now safe to
   turn off your computer" screen).  The front-end can then exit. */
bool pc_guest_powered_off(PC *pc);

void mixer_callback(void *opaque, uint8_t *stream, int free);

int parse_conf_ini(void* user, const char* section,
		   const char* name, const char* value);
void load_bios_and_reset(PC *pc);

#endif /* PC_H */
