/**
 * File: ne2000.h
 * Description: Hardware-independent NE2000 (NatSemi DP8390 + 16 KB ring)
 *              register-file model for MD/Net. This is the emulated chip
 *              state machine only -- it has NO RP2040 or cartridge-bus
 *              dependencies. The bus glue (commemul ROM3 write decode,
 *              ROM4 read staging, the data-port PIO path) drives this
 *              core from emul.c; the WiFi bridge feeds/drains it via
 *              ne2000_deliver_rx() / ne2000_take_tx().
 *
 * Register/command semantics follow the EtherNEC driver (NE.S, 8390.I)
 * that STinG loads for the NetUSBee, so the model is exactly what that
 * driver expects to probe and drive. See docs/ne2000-emulation.md.
 */

#ifndef NE2000_H
#define NE2000_H

#include <stdbool.h>
#include <stdint.h>

// Ring geometry for the 16 KB NE2000 clone (NESM_* in 8390.I). Pages are
// 256 bytes. The 8390 addresses its 16 KB of buffer RAM as pages
// $40..$7F; page $40 maps to ring byte 0.
#define NE2000_PAGE_SIZE 256u
#define NE2000_TX_START_PAGE 0x40u  // NESM_START_PG
#define NE2000_RX_START_PAGE 0x46u  // tx start + TX_PAGES (6)
#define NE2000_STOP_PAGE 0x80u      // NESM_STOP_PG
#define NE2000_RING_FIRST_PAGE NE2000_TX_START_PAGE
#define NE2000_RING_PAGES (NE2000_STOP_PAGE - NE2000_RING_FIRST_PAGE)  // 64
#define NE2000_RING_BYTES (NE2000_RING_PAGES * NE2000_PAGE_SIZE)       // 16 KB

// The MAC PROM the driver reads during probe: 32 bytes, each of the 6 MAC
// bytes doubled, with the NE2000 signature $57 at PROM[14] and PROM[15].
#define NE2000_PROM_SIZE 32u

#define NE2000_MTU 1518u  // 6+6+2+1500+4(CRC); driver rejects longer frames

// RX pacing: hold off delivering once the ring already carries this many
// unread pages, so CURR stays still long enough for the driver's re-read
// to agree and it can drain without wandering below the ring. ~8 pages =
// one max-size frame or several small ones -- enough not to stall a
// draining driver, tight enough to keep CURR from churning.
#define NE2000_RX_PACE_PAGES 8

// Interrupt status/mask bits (EN0_ISR / EN0_IMR) used by the model.
#define NE2000_ISR_RX 0x01u     // packet received, no error (ENISR_RX)
#define NE2000_ISR_TX 0x02u     // packet transmitted (ENISR_TX)
#define NE2000_ISR_RX_ERR 0x04u
#define NE2000_ISR_TX_ERR 0x08u
#define NE2000_ISR_OVER 0x10u   // receiver overwrote the ring (ENISR_OVER)
#define NE2000_ISR_RDC 0x40u    // remote DMA complete (ENISR_RDC)
#define NE2000_ISR_RESET 0x80u  // reset completed (ENISR_RESET)

typedef struct {
  // Register file, page 0/1/2 (32 regs per page; we key on offset + the
  // page selected by CR bits 6-7). Only the registers the driver touches
  // are modelled with side effects; the rest read/write straight.
  uint8_t cr;         // command register (all pages)
  uint8_t isr;        // interrupt status
  uint8_t imr;        // interrupt mask
  uint8_t dcr;        // data config
  uint8_t rcr;        // rx config
  uint8_t tcr;        // tx config
  uint8_t tsr;        // tx status
  uint8_t tpsr;       // tx start page
  uint8_t pstart;     // rx ring start page
  uint8_t pstop;      // rx ring stop page
  uint8_t bnry;       // boundary page
  uint8_t curr;       // current page (page 1)
  uint8_t par[6];     // physical address (page 1)
  uint8_t mar[8];     // multicast (page 1)
  uint16_t rsar;      // remote DMA start address
  uint16_t rcnt;      // remote DMA byte count (remaining)
  uint8_t tcnt_lo;    // transmit byte count low
  uint8_t tcnt_hi;    // transmit byte count high

  uint8_t prom[NE2000_PROM_SIZE];    // MAC PROM (doubled bytes + $57 sig)
  uint8_t mem[NE2000_RING_BYTES];    // 16 KB buffer RAM (ring + tx page)

  bool started;       // CR START seen since last STOP
} ne2000_t;

// Reset the chip to power-on state and load the station MAC (also builds
// the doubled PROM image). Safe to call repeatedly.
void ne2000_reset(ne2000_t *chip, const uint8_t mac[6]);

// A decoded EtherNEC register write (ROM3 sample: reg in A9-A13, data in
// A1-A8). reg is 0..31.
void ne2000_reg_write(ne2000_t *chip, uint8_t reg, uint8_t data);

// A register read (ROM4). Returns the value the bus should present for
// register `reg` (0..31). For the data port ($10) this returns the next
// remote-DMA byte and advances the internal pointer.
uint8_t ne2000_reg_read(ne2000_t *chip, uint8_t reg);

// Deliver a received Ethernet frame into the rx ring (adds the 4-byte
// 8390 header, advances CURR, sets ISR_RX). Returns false if the ring
// can't fit it (sets ISR_OVER). `len` excludes CRC; a 4-byte CRC slot is
// accounted for in the header length the way the driver expects.
bool ne2000_deliver_rx(ne2000_t *chip, const uint8_t *frame, uint16_t len);

// If the driver has issued a transmit command since the last call, copy
// the staged frame into `out` (capacity NE2000_MTU) and return its
// length; otherwise return 0. Clears the pending-tx latch.
uint16_t ne2000_take_tx(ne2000_t *chip, uint8_t *out);

// Data-port serve cursor: the byte at the current remote-DMA address
// (RSAR), and a step to the next byte (advances RSAR with rx-ring wrap,
// decrements RCNT, sets ISR_RDC at count 0). Used to serve the data port
// directly from live chip state, so RSAR set-up by the driver (via the
// register path) is honoured without a separately-armed stream.
uint8_t ne2000_dma_current(const ne2000_t *chip);
void ne2000_dma_advance(ne2000_t *chip);

#endif  // NE2000_H
