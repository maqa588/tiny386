/*
 * Cirrus CL-GD5446 2D Hardware Acceleration - OpenCL Backend Header
 *
 * Tiny386 Emulation System
 */

#ifndef CIRRUS_OPENCL_H
#define CIRRUS_OPENCL_H

#include <stdint.h>
#include <stdbool.h>

struct CirrusVGAState;

bool cirrus_opencl_init(void);
bool cirrus_opencl_available(void);
void cirrus_opencl_cleanup(void);

bool cirrus_opencl_solid_fill(struct CirrusVGAState *s,
                              uint32_t dstaddr, int dstpitch,
                              int width, int height,
                              uint32_t color, int pixelwidth, int rop);

bool cirrus_opencl_copy(struct CirrusVGAState *s,
                        uint32_t dstaddr, uint32_t srcaddr,
                        int dstpitch, int srcpitch,
                        int width, int height,
                        bool is_forward, bool is_transp,
                        int rop, int pixelwidth);

#endif /* CIRRUS_OPENCL_H */
