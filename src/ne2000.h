#ifndef NE2000_H
#define NE2000_H

#include <stdint.h>

typedef struct NE2000State NE2000State;
void ne2000_ioport_write(void *opaque, uint32_t addr, uint32_t val);
uint32_t ne2000_ioport_read(void *opaque, uint32_t addr);
void ne2000_reset_ioport_write(void *opaque, uint32_t addr, uint32_t val);
uint32_t ne2000_reset_ioport_read(void *opaque, uint32_t addr);
void ne2000_asic_ioport_write(void *opaque, uint32_t addr, uint32_t val);
uint32_t ne2000_asic_ioport_read(void *opaque, uint32_t addr);

void ne2000_step(NE2000State *s);
NE2000State *isa_ne2000_init(int base, int irq,
                             void *pic,
                             void (*set_irq)(void *pic, int irq, int level));

#ifdef BUILD_ESP32
/* Hand an L2 frame received by the host NIC (WiFi or Ethernet) to the
 * emulated card.  Returns non-zero if the frame should also go to lwIP. */
int ne2000_l2_input(uint8_t *buf, int size);
#endif
// XXX
void ne2000_set_config_file(const char *file);
#endif /* NE2000_H */
