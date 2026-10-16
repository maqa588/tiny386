#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

struct Globals {
	void *pc;
	void *kbd;
	void *mouse;
	void *panel;
	void *panel_fb;   /* RGB/DPI panel DMA frame buffer (NULL if not used) */
	void *vga_fb;     /* Linear source framebuffer rendered by emulator (used with PPA 2D) */
	volatile uint32_t vga_dirty; /* Non-blocking dirty flag: Core 0 writes, Core 1 clears */
	volatile uint32_t vga_frame_ready; /* Set by Core 0 at a VGA frame boundary; Core 1 renders it */
};

extern EventGroupHandle_t global_event_group;
extern struct Globals globals;

void *fbmalloc(long size);
void *psmalloc(long size);

/* -------- OSD (on-screen control panel) front-end glue (esp_main.c) --------
 * Media can be mounted/ejected from the panel (including a host shared folder
 * synthesised as a virtual CD), driven from the USB HID input path and
 * composited onto the emulator framebuffer by the display task.  These entry
 * points may be called from any core/task; they serialise internally. */
void esp_osd_init(void *pc);
int  esp_osd_key(int scancode, int down);        /* returns 1 if the OSD handled it */
int  esp_osd_mouse(int dx, int dy, int buttons); /* returns 1 if the OSD handled it */
int  esp_osd_render(unsigned char *pixels, int w, int h, int pitch); /* 1 if drawn */

#endif /* COMMON_H */
