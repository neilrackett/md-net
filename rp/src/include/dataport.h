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

// Start the data-port serve: claim a PIO SM for the ROM4 tap and launch
// Core 1 to service it. Call once, after romemul is up.
void dataport_init(void);

// Arm a remote-DMA read: `stream[0..len)` is the byte sequence the ST will
// read out of the data port next. Copies the stream, preloads the first
// byte into the served RAM slots, and resets the advance pointer. Safe to
// call from Core 0 between transfers.
void dataport_arm(const uint8_t *stream, uint16_t len);

// Diagnostics: total data-port reads Core 1 has observed since boot (used
// to confirm the tap is detecting reads on hardware).
uint32_t dataport_readCount(void);

#endif  // DATAPORT_H
