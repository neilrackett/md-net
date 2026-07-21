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

static inline void dataport_note_addr(uint16_t addr) {
  if (s_addrN < 8u) {
    s_addrCap[s_addrN] = addr;
    s_addrN = (uint8_t)(s_addrN + 1u);
  }
}

uint8_t dataport_addrCapCount(void) { return s_addrN; }
uint16_t dataport_addrCap(uint8_t i) { return s_addrCap[i & 7u]; }

static inline volatile uint8_t *rom4(void) {
  return (volatile uint8_t *)&__rom_in_ram_start__;
}

static inline void write_slots(uint8_t b) {
  volatile uint8_t *r = rom4();
  r[DP_SLOT0] = b;
  r[DP_SLOT1] = b;
  r[DP_SLOT2] = b;
  r[DP_SLOT3] = b;
}

void __not_in_flash_func(dataport_set_byte)(uint8_t b) { write_slots(b); }

// Tight serve for an armed remote read: poll only the ROM4 tap so each
// data-port read is re-staged within ~200 ns -- the full Core-1 loop's
// ~1.5 us latency loses against the m68k's back-to-back read pace, which
// duplicated served bytes (observed: the PROM MAC read back as
// 28:28:28:cd:c1:c1). Exits on command-register activity (the driver
// moved on) or a sustained quiet period.
void __not_in_flash_func(dataport_serve_burst)(uint8_t (*next_byte)(void)) {
  uint32_t quiet = 0;
  while (quiet < 20000u) {  // ~200 us of bus silence ends the burst
    if (!pio_sm_is_rx_fifo_empty(s_pio, (uint)s_sm)) {
      uint32_t word = pio_sm_get(s_pio, (uint)s_sm);
      uint16_t addr = (uint16_t)(word >> 16);
      uint8_t reg = (uint8_t)((addr >> 9) & 0x1Fu);
      if (reg == DP_REG) {
        s_count++;
        dataport_note_addr(addr);
        write_slots(next_byte());
        quiet = 0;
        continue;
      }
      s_regReads++;
      if (reg == 0x07u) {
        s_reg7Reads++;
      }
    }
    if (!pio_sm_is_rx_fifo_empty(s_pio, (uint)s_crSm)) {
      break;  // command-register activity: return to the full loop
    }
    quiet++;
  }
}

void __not_in_flash_func(dataport_service)(uint8_t (*next_byte)(void)) {
  while (!pio_sm_is_rx_fifo_empty(s_pio, (uint)s_sm)) {
    uint32_t word = pio_sm_get(s_pio, (uint)s_sm);
    uint16_t addr = (uint16_t)(word >> 16);
    uint8_t reg = (uint8_t)((addr >> 9) & 0x1Fu);
    if (reg != DP_REG) {
      s_regReads++;
      if (reg == 0x07u) {
        s_reg7Reads++;
      }
      continue;
    }
    s_count++;
    dataport_note_addr(addr);
    // A data-port read just consumed the served byte; stage the next one so
    // the ST's next read gets it.
    write_slots(next_byte());
  }
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
