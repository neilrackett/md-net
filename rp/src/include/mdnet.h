/**
 * File: mdnet.h
 * Description: Cartridge-bus integration for the NE2000 model. Ties the
 *              hardware-independent chip (ne2000.c) to the SidecarTridge
 *              bus emulation:
 *                - register WRITES arrive as ROM3 "dummy reads" captured
 *                  by commemul (decoded here),
 *                - register READS are served from the ROM4 RAM mirror
 *                  (staged here, byte-swapped for the cart bus),
 *                - the remote-DMA data port needs a dedicated
 *                  auto-incrementing serve path (see docs; not yet wired).
 *
 * The pure ROM3-sample decode is exposed as dependency-free inlines so it
 * can be unit-tested on the host (tools/ne2000_test.c).
 *
 * See docs/ne2000-emulation.md.
 */

#ifndef MDNET_H
#define MDNET_H

#include <stdint.h>

// EtherNEC ROM3 write encoding: address offset = (reg << 9) | (data << 1).
// A commemul sample is that 16-bit ROM3 offset (the low byte discriminator
// pattern IKBD relied on). Register = address bits A9-A13, data = A1-A8.
#define MDNET_REG_SHIFT 9
#define MDNET_REG_MASK 0x1Fu
#define MDNET_DATA_SHIFT 1
#define MDNET_DATA_MASK 0xFFu

// Register reads live in the ROM4 window at offset (reg << 9). Byte reads
// pick up RP RAM byte (offset ^ 1) across the cart-bus within-word swap
// (same rule cart_writeM68kString relies on, validated on hardware).
#define MDNET_REG_READ_OFFSET(reg) ((((uint32_t)(reg)) << 9) ^ 1u)

static inline uint8_t mdnet_sample_reg(uint16_t sample) {
  return (uint8_t)((sample >> MDNET_REG_SHIFT) & MDNET_REG_MASK);
}
static inline uint8_t mdnet_sample_data(uint16_t sample) {
  return (uint8_t)((sample >> MDNET_DATA_SHIFT) & MDNET_DATA_MASK);
}

// Bring up the emulated NE2000: reset the chip with the Pico W's STA MAC
// and stage its initial register file into the ROM4 mirror. Call after
// WiFi init (the MAC must be available). Does NOT repaint the ROM4 mirror
// yet -- see mdnet_activate() for the boot-cartridge -> register-map
// mode switch.
void mdnet_init(void);

// Repaint the ROM4 RAM mirror from the cartridge boot image to the NE2000
// register-read staging map. Call once the ST has finished the cartridge
// init (message shown, returned to GEM) so the boot magic is no longer
// being read. After this the cartridge magic is gone until the next cold
// boot -- see the mode-switch discussion in docs/ne2000-emulation.md.
void mdnet_activate(void);

// Main-loop service: drain the commemul ROM3 ring into the chip (register
// + data-port writes), run the WiFi packet bridge (TX out, RX into the
// ring), and re-stage the register-read map. Cheap to call every loop.
void mdnet_poll(void);

#endif  // MDNET_H
