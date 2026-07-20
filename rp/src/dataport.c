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

// Stream state. Touched only by Core 1 (dataport_service + dataport_arm),
// except the pre-launch PROM pre-arm on Core 0.
static uint8_t s_stream[NE2000_MTU + 32];
static uint16_t s_len = 0;
static uint16_t s_idx = 0;
static volatile uint32_t s_count = 0;

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

void dataport_service(void) {
  while (!pio_sm_is_rx_fifo_empty(s_pio, (uint)s_sm)) {
    uint32_t word = pio_sm_get(s_pio, (uint)s_sm);
    uint16_t addr = (uint16_t)(word >> 16);
    if (((addr >> 9) & 0x1Fu) != DP_REG) {
      continue;
    }
    s_count++;
    if (s_idx < s_len) {
      write_slots(s_stream[s_idx]);
      s_idx = (uint16_t)(s_idx + 1u);
    }
  }
}

void dataport_arm(const uint8_t *stream, uint16_t len) {
  if (len > sizeof(s_stream)) {
    len = sizeof(s_stream);
  }
  for (uint16_t i = 0; i < len; i++) {
    s_stream[i] = stream[i];
  }
  write_slots(len > 0 ? stream[0] : 0u);  // preload the first byte
  s_idx = 1u;
  s_len = len;
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
  DPRINTF("dataport: ROM4 tap on pio1/sm%d\n", s_sm);
}

uint32_t dataport_readCount(void) { return s_count; }
