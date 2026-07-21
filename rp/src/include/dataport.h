/**
 * File: dataport.h
 * Description: NE2000 remote-DMA data-port serve. The data port ($FA2000,
 *              register $10) is read repeatedly at the same address, and
 *              the real chip returns the next ring/PROM byte each time via
 *              an internal auto-increment. Static ROM4 RAM can't do that,
 *              so this module dedicates Core 1 to it: a read-only PIO tap
 *              (dataport.pio) reports every ROM4 read, and Core 1 advances
 *              a pre-staged byte stream into the data-port RAM slots one
 *              byte per data-port read.
 *
 * A single stream pointer, advanced one byte per read, serves BOTH access
 * patterns the driver uses -- the probe's repeated byte reads at $FA2000
 * and packet DMA's movep stepping across $FA2000/2/4/6 -- because each
 * successive read just wants the next byte. See docs/ne2000-emulation.md.
 */

#ifndef DATAPORT_H
#define DATAPORT_H

#include <stdint.h>

// Claim a PIO SM for the read-only ROM4 tap. Call once, after romemul is
// up. Does not launch Core 1 -- the mdnet servicing loop drives
// dataport_service() from Core 1.
void dataport_init(void);

// Set the byte the data port currently serves (written to all four
// data-port RAM slots). Call each Core-1 iteration with the chip's current
// remote-DMA byte so RSAR changes are reflected before the ST reads.
void dataport_set_byte(uint8_t b);

// Drain the ROM4 tap FIFO; for each data-port read that occurred, call
// next_byte() and serve its result (so the ST's following read gets the
// next byte). Call from the Core-1 servicing loop.
void dataport_service(uint8_t (*next_byte)(void));

// Non-blocking read of the low-latency ROM3 command-register tap. Returns 1
// and stores the 16-bit ROM3 address (of a register/data write) in *addr,
// or 0 if the FIFO is empty. Used to track the driver's selected page with
// far lower latency than the commemul DMA ring.
int dataport_crtap_get(uint16_t *addr);

// Diagnostics: total data-port reads Core 1 has observed since boot (used
// to confirm the tap is detecting reads on hardware).
uint32_t dataport_readCount(void);
// Non-data-port ROM4 read counters (all registers / register 7), so a
// driver spinning on a register poll is visible on the UART.
uint32_t dataport_regReadCount(void);
uint32_t dataport_reg7ReadCount(void);

#endif  // DATAPORT_H
