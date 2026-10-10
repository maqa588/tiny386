#ifndef COMMON_H
#define COMMON_H

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
};

extern EventGroupHandle_t global_event_group;
extern struct Globals globals;

void *fbmalloc(long size);
void *psmalloc(long size);

#endif /* COMMON_H */
