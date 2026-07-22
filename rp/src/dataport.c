/**
 * File: dataport.c
 * Description: NE2000 remote-DMA data-port serve. See dataport.h and
 *              docs/ne2000-emulation.md.
 *
 * A read-only PIO tap (dataport.pio) autopushes every ROM4 read address.
 * dataport_service() drains that FIFO non-blockingly; for each read whose
 * register field is the data port ($10) it advances a pre-staged byte
 * stream into the data-port RAM slots, so the NEXT read served by romemul
 * returns the next byte. It is called from the mdnet Core-1 servicing loop
 * (which also drains the command bus and restages registers), so all
 * NE2000 bus servicing lives on one core with microsecond latency.
 *
 * dataport_arm() and dataport_service() are both called from Core 1, so
 * the stream state needs no cross-core synchronisation. The one exception
 * is the pre-arm of the MAC PROM, done on Core 0 before Core 1 is launched.
 */

#include "dataport.h"

#include "constants.h"
#include "dataport.pio.h"
#include "debug.h"
#include "hardware/pio.h"
#include "ne2000.h"

// Data-port serve slots: the high byte of the ROM4 words the driver reads
// for the data port ($FA2000/2/4/6). A byte read at an even ST address
// picks up RP RAM byte (offset ^ 1), so the served byte lives at offsets
// $2001/$2003/$2005/$2007 (the movep variants are covered by writing all
// four each advance).
#define DP_WORD_BASE 0x2000u
#define DP_SLOT0 (DP_WORD_BASE + 1u)  // $2001
#define DP_SLOT1 (DP_WORD_BASE + 3u)  // $2003
#define DP_SLOT2 (DP_WORD_BASE + 5u)  // $2005
#define DP_SLOT3 (DP_WORD_BASE + 7u)  // $2007

#define DP_REG 0x10u  // NE2000 data-port register number (A9-A13 == $10)

static PIO s_pio = pio1;
static int s_sm = -1;
static int s_crSm = -1;  // ROM3 command-register tap

static volatile uint32_t s_count = 0;
// Register-read visibility: the tap samples EVERY ROM4 read, so count the
// non-data-port ones too -- without this, a driver spinning on a register
// poll (e.g. waiting on an ISR bit) is indistinguishable from a dead
// driver, since romemul serves register reads with no RP involvement.
static volatile uint32_t s_regReads = 0;   // all non-data-port ROM4 reads
static volatile uint32_t s_reg7Reads = 0;  // reads of register 7 (ISR/CURR)

// First few data-port read ADDRESSES (low 9 bits carry the in-window
// offset): shows whether the driver reads one fixed slot or walks
// addresses -- required knowledge for any hardware-paced serve design.
static volatile uint16_t s_addrCap[8];
static volatile uint8_t s_addrN = 0;

static inline volatile uint8_t *rom4(void) {
  return (volatile uint8_t *)&__rom_in_ram_start__;
}

// Unified bus-event trace hook (lives in mdnet.c): reads are reported
// with the value STAGED at the moment of the read.
extern void mdnet_trace_evt(uint16_t evt);

static inline void dataport_note_reg_read(uint8_t reg) {
  volatile uint8_t *r = rom4();
  uint8_t v = r[((uint16_t)reg << 9) ^ 1u];  // staged value (cart byte-swap)
  mdnet_trace_evt((uint16_t)(0x8000u | ((uint16_t)reg << 8) | v));
}

static inline void dataport_note_addr(uint16_t addr) {
  if (s_addrN < 8u) {
    s_addrCap[s_addrN] = addr;
    s_addrN = (uint8_t)(s_addrN + 1u);
  }
}

uint8_t dataport_addrCapCount(void) { return s_addrN; }
uint16_t dataport_addrCap(uint8_t i) { return s_addrCap[i & 7u]; }

// GROUND TRUTH capture: the slot content AT EVENT TIME (the cycle has
// just ended, nothing has been restaged) is the byte the m68k actually
// latched. Every prior serve diagnostic recorded what we intended to
// serve; this records what the bus delivered.
static volatile uint8_t s_gotCap[6];
static volatile uint8_t s_gotN = 0xFFu;

void dataport_got_reset(void) { s_gotN = 0; }
uint8_t dataport_got(uint8_t i) { return s_gotCap[i > 5u ? 5u : i]; }

// Stage a 4-byte WINDOW of the stream across the four slots: byte base+k
// at slot k. A MOVEP.L body copy reads the four ascending addresses in
// back-to-back ~250 ns bus cycles -- no per-event restage can feed those,
// but a pre-staged window needs none: the burst naturally collects
// b0..b3. Single move.b readers always hit slot 0 and consume b0.
void __not_in_flash_func(dataport_stage4)(uint8_t b0, uint8_t b1, uint8_t b2,
                                          uint8_t b3) {
  volatile uint8_t *r = rom4();
  r[DP_SLOT0] = b0;
  r[DP_SLOT1] = b1;
  r[DP_SLOT2] = b2;
  r[DP_SLOT3] = b3;
}

// Rewrite only slots 0..k with the next window's first k+1 bytes. A slot
// that was just consumed is always safe to rewrite immediately -- an
// in-flight movep burst only reads ASCENDING slots -- so this gives
// pollers a ~200 ns slot-0 refresh (their reads come ~1 us apart, faster
// than any deferral) and refreshes the whole window instantly when a
// movep.l completes (k=3), with zero risk to a burst in progress.
void __not_in_flash_func(dataport_stage_upto)(uint8_t k, uint8_t b0,
                                              uint8_t b1, uint8_t b2,
                                              uint8_t b3) {
  volatile uint8_t *r = rom4();
  r[DP_SLOT0] = b0;
  if (k >= 1u) {
    r[DP_SLOT1] = b1;
  }
  if (k >= 2u) {
    r[DP_SLOT2] = b2;
  }
  if (k >= 3u) {
    r[DP_SLOT3] = b3;
  }
}

// Tight serve for an armed remote read: poll only the ROM4 tap so each
// data-port read is re-staged within ~200 ns -- the full Core-1 loop's
// ~1.5 us latency loses against the m68k's back-to-back read pace, which
// duplicated served bytes (observed: the PROM MAC read back as
// 28:28:28:cd:c1:c1). Exits on command-register activity (the driver
// moved on) or a sustained quiet period.
// Restage hook (mdnet.c): rebuilds the 4-byte window from the live RSAR.
extern void mdnet_dp_restage(void);

void __not_in_flash_func(dataport_serve_burst)(void (*consumed)(uint8_t slot)) {
  uint32_t quiet = 0;
  bool dirty = false;
  while (quiet < 20000u) {  // ~200 us of bus silence ends the burst
    // TRUE BUS ORDER: a pending ROM3 write PREEMPTS read serving. The
    // bus serializes everything; a write sitting in the crtap FIFO
    // precedes any read event that arrives after it, and applying reads
    // past it serves the wrong stream when the driver chains arms
    // back-to-back (the wrong-stream headers seen while ring-walking).
    // Exit so crtap_service applies the write -- which first drains the
    // reads that preceded it (see the barrier there).
    if (!pio_sm_is_rx_fifo_empty(s_pio, (uint)s_crSm)) {
      break;
    }
    // Deferred restage: only rewrite the window once the read burst has
    // paused -- a movep.l burst consumes the pre-staged window
    // untouched; pollers read >= 1.5 us apart so the deferral is
    // invisible to them.
    if (dirty && quiet > 16u) {
      mdnet_dp_restage();
      dirty = false;
    }
    if (!pio_sm_is_rx_fifo_empty(s_pio, (uint)s_sm)) {
      uint32_t word = pio_sm_get(s_pio, (uint)s_sm);
      uint16_t addr = (uint16_t)(word >> 16);
      uint8_t reg = (uint8_t)((addr >> 9) & 0x1Fu);
      if (reg == DP_REG) {
        s_count++;
        dataport_note_addr(addr);
        uint8_t k = (uint8_t)((addr >> 1) & 3u);
        if (s_gotN < 6u) {  // ground truth: what this cycle delivered
          s_gotCap[s_gotN++] = rom4()[DP_SLOT0 + 2u * k];
        }
        consumed(k);  // slot = which window byte
        dirty = true;
        quiet = 0;
        continue;
      }
      s_regReads++;
      if (reg == 0x07u) {
        s_reg7Reads++;
      }
      dataport_note_reg_read(reg);
    }
    quiet++;
  }
  if (dirty) {
    mdnet_dp_restage();  // burst over: leave the window current
  }
}

void __not_in_flash_func(dataport_service)(void (*consumed)(uint8_t slot)) {
  while (!pio_sm_is_rx_fifo_empty(s_pio, (uint)s_sm)) {
    uint32_t word = pio_sm_get(s_pio, (uint)s_sm);
    uint16_t addr = (uint16_t)(word >> 16);
    uint8_t reg = (uint8_t)((addr >> 9) & 0x1Fu);
    if (reg != DP_REG) {
      s_regReads++;
      if (reg == 0x07u) {
        s_reg7Reads++;
      }
      dataport_note_reg_read(reg);
      continue;
    }
    s_count++;
    dataport_note_addr(addr);
    uint8_t k = (uint8_t)((addr >> 1) & 3u);
    if (s_gotN < 6u) {  // ground truth: what this cycle delivered
      s_gotCap[s_gotN++] = rom4()[DP_SLOT0 + 2u * k];
    }
    consumed(k);
  }
  // No restage here: tail events drained outside a burst belong to a
  // read stream the driver has already left (the burst exits on its
  // CR activity); the next stream is prestaged at its arm, and the cold
  // lap refreshes the window in between. An immediate restage here could
  // land mid-movep when this runs from the yield during a delivery.
}

void dataport_init(void) {
  int offset = pio_add_program(s_pio, &dataport_read_program);
  if (offset < 0) {
    DPRINTF("dataport_init: pio_add_program failed (%d)\n", offset);
    return;
  }
  s_sm = pio_claim_unused_sm(s_pio, true);
  dataport_read_program_init(s_pio, (uint)s_sm, (uint)offset,
                             READ_ADDR_GPIO_BASE, READ_ADDR_PIN_COUNT,
                             SAMPLE_DIV_FREQ);
  pio_sm_set_enabled(s_pio, (uint)s_sm, true);

  // Low-latency ROM3 tap for command-register writes (direct FIFO).
  int crOffset = pio_add_program(s_pio, &crtap_read_program);
  if (crOffset < 0) {
    DPRINTF("dataport_init: crtap pio_add_program failed (%d)\n", crOffset);
    return;
  }
  s_crSm = pio_claim_unused_sm(s_pio, true);
  crtap_read_program_init(s_pio, (uint)s_crSm, (uint)crOffset,
                          READ_ADDR_GPIO_BASE, READ_ADDR_PIN_COUNT,
                          SAMPLE_DIV_FREQ);
  pio_sm_set_enabled(s_pio, (uint)s_crSm, true);

  DPRINTF("dataport: ROM4 tap pio1/sm%d, ROM3 CR tap pio1/sm%d\n", s_sm,
          s_crSm);
}

int __not_in_flash_func(dataport_crtap_get)(uint16_t *addr) {
  if (s_crSm < 0 || pio_sm_is_rx_fifo_empty(s_pio, (uint)s_crSm)) {
    return 0;
  }
  uint32_t word = pio_sm_get(s_pio, (uint)s_crSm);
  *addr = (uint16_t)(word >> 16);
  return 1;
}

uint32_t dataport_readCount(void) { return s_count; }
uint32_t dataport_regReadCount(void) { return s_regReads; }
uint32_t dataport_reg7ReadCount(void) { return s_reg7Reads; }
