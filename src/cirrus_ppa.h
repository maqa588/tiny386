/*
 * Cirrus CL-GD5446 2D Hardware Acceleration - ESP32-P4 PPA Backend Header
 *
 * Tiny386 Emulation System
 */

#ifndef CIRRUS_PPA_H
#define CIRRUS_PPA_H

#include <stdint.h>
#include <stdbool.h>

struct CirrusVGAState;

bool cirrus_ppa_init(void);
bool cirrus_ppa_available(void);

bool cirrus_ppa_solid_fill(struct CirrusVGAState *s,
                           uint32_t dstaddr, int dstpitch,
                           int width, int height,
                           uint32_t color, int pixelwidth, int rop);

bool cirrus_ppa_blit(struct CirrusVGAState *s,
                     uint32_t dstaddr, uint32_t srcaddr,
                     int dstpitch, int srcpitch,
                     int width, int height);

#endif /* CIRRUS_PPA_H */
