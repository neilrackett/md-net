/**
 * File: dataport.c
 * Description: NE2000 remote-DMA data-port serve on Core 1. See dataport.h
 *              and docs/ne2000-emulation.md.
 *
 * A read-only PIO tap (dataport.pio) autopushes every ROM4 read address.
 * Core 1 blocks on that FIFO; for each read whose register field is the
 * data port ($10) it advances a pre-staged byte stream into the data-port
 * RAM slots, so the NEXT read served by romemul returns the next byte.
 * Core 1 touches only RAM + PIO (never flash), so it is safe alongside
 * Core 0's flash-owning role.
 */

#include "dataport.h"

#include "constants.h"
#include "dataport.pio.h"
#include "debug.h"
#include "hardware/pio.h"
#include "ne2000.h"
#include "pico/multicore.h"
#include "pico/platform.h"

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

// Shared serve state. Core 0 writes on arm; Core 1 reads/advances.
static uint8_t s_stream[NE2000_MTU + 32];
static volatile uint16_t s_len = 0;
static volatile uint16_t s_idx = 0;
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

// Core 1: block on the ROM4 tap and advance the data-port stream one byte
// per data-port read.
static void __not_in_flash_func(dataport_core1_loop)(void) {
  for (;;) {
    uint32_t word = pio_sm_get_blocking(s_pio, (uint)s_sm);
    uint16_t addr = (uint16_t)(word >> 16);
    if (((addr >> 9) & 0x1Fu) != DP_REG) {
      continue;
    }
    s_count++;
    uint16_t i = s_idx;
    if (i < s_len) {
      write_slots(s_stream[i]);
      s_idx = (uint16_t)(i + 1u);
    }
  }
}

void dataport_arm(const uint8_t *stream, uint16_t len) {
  if (len > sizeof(s_stream)) {
    len = sizeof(s_stream);
  }
  s_len = 0;  // freeze the advance while we restage
  __sync_synchronize();
  for (uint16_t i = 0; i < len; i++) {
    s_stream[i] = stream[i];
  }
  write_slots(len > 0 ? stream[0] : 0u);  // preload the first byte
  s_idx = 1u;
  __sync_synchronize();
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

  multicore_launch_core1(dataport_core1_loop);
  DPRINTF("dataport: ROM4 tap on pio1/sm%d, Core 1 servicing data port\n",
          s_sm);
}

uint32_t dataport_readCount(void) { return s_count; }
