/**
 * File: ne2000_test.c
 * Description: Host-side unit test for the hardware-independent NE2000
 *              model (rp/src/ne2000.c). Drives the exact EtherNEC-driver
 *              register sequences (probe / remote-DMA read / transmit) and
 *              a deliver-RX round trip, asserting the model behaves the
 *              way STinG's driver expects.
 *
 * Build + run:
 *   cc -I rp/src/include -o /tmp/ne2000_test tools/ne2000_test.c \
 *      rp/src/ne2000.c && /tmp/ne2000_test
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mdnet.h"
#include "ne2000.h"

// 8390 register offsets / command bits, mirrored from the driver.
enum {
  R_CR = 0x00,
  R_STARTPG = 0x01,
  R_STOPPG = 0x02,
  R_BNRY = 0x03,
  R_TPSR = 0x04,
  R_TCNTLO = 0x05,
  R_TCNTHI = 0x06,
  R_ISR = 0x07,
  R_RSARLO = 0x08,
  R_RSARHI = 0x09,
  R_RCNTLO = 0x0A,
  R_RCNTHI = 0x0B,
  R_RXCR = 0x0C,
  R_TXCR = 0x0D,
  R_DCFG = 0x0E,
  R_IMR = 0x0F,
  R_DATA = 0x10,
  R_RESET = 0x1F,
  R_PAR0 = 0x01,
  R_CURR = 0x07,
};
enum {
  CR_STOP = 0x01,
  CR_START = 0x02,
  CR_TRANS = 0x04,
  CR_RREAD = 0x08,
  CR_RWRITE = 0x10,
  CR_NODMA = 0x20,
  CR_PAGE0 = 0x00,
  CR_PAGE1 = 0x40,
};

static void wr(ne2000_t *c, uint8_t reg, uint8_t v) {
  ne2000_reg_write(c, reg, v);
}
static uint8_t rd(ne2000_t *c, uint8_t reg) { return ne2000_reg_read(c, reg); }

static void test_probe_prom(void) {
  const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
  ne2000_t chip;
  ne2000_reset(&chip, mac);

  // ei_probe1: soft reset then require the reset bit.
  (void)rd(&chip, R_RESET);
  wr(&chip, R_RESET, 0);
  assert((rd(&chip, R_ISR) & NE2000_ISR_RESET) && "reset bit must be set");

  // Ack, monitor+loopback, remote-DMA read 32 bytes of PROM from addr 0.
  wr(&chip, R_ISR, 0xFF);
  wr(&chip, R_CR, CR_STOP | CR_NODMA | CR_PAGE0);
  wr(&chip, R_DCFG, 0x48);
  wr(&chip, R_RCNTLO, 0);
  wr(&chip, R_RCNTHI, 0);
  wr(&chip, R_IMR, 0);
  wr(&chip, R_ISR, 0xFF);
  wr(&chip, R_RXCR, 0x20);
  wr(&chip, R_TXCR, 0x02);
  wr(&chip, R_RCNTLO, NE2000_PROM_SIZE);
  wr(&chip, R_RCNTHI, 0);
  wr(&chip, R_RSARLO, 0);
  wr(&chip, R_RSARHI, 0);
  wr(&chip, R_CR, CR_RREAD | CR_START);

  uint8_t prom[NE2000_PROM_SIZE];
  for (int i = 0; i < (int)NE2000_PROM_SIZE; i++) {
    prom[i] = rd(&chip, R_DATA);
  }

  // Each MAC byte doubled; NE2000 signature $57 at 14/15.
  for (int i = 0; i < 6; i++) {
    assert(prom[2 * i] == mac[i] && prom[2 * i + 1] == mac[i] &&
           "MAC byte must appear doubled in PROM");
  }
  assert(prom[14] == 0x57 && prom[15] == 0x57 && "NE2000 signature");
  assert((rd(&chip, R_ISR) & NE2000_ISR_RDC) && "RDC after count exhausted");
  printf("PASS: probe PROM read (doubled MAC + $57 signature)\n");
}

// Standard NS8390_init register setup, leaving the chip started.
static void init_chip(ne2000_t *c, const uint8_t mac[6]) {
  wr(c, R_CR, CR_STOP | CR_NODMA | CR_PAGE0);
  wr(c, R_DCFG, 0x48);
  wr(c, R_RCNTLO, 0);
  wr(c, R_RCNTHI, 0);
  wr(c, R_RXCR, 0x20);
  wr(c, R_TXCR, 0x02);
  wr(c, R_TPSR, NE2000_TX_START_PAGE);
  wr(c, R_BNRY, NE2000_RX_START_PAGE);
  wr(c, R_STARTPG, NE2000_RX_START_PAGE);
  wr(c, R_STOPPG, NE2000_STOP_PAGE);
  wr(c, R_ISR, 0xFF);
  wr(c, R_IMR, 0);
  wr(c, R_CR, CR_STOP | CR_NODMA | CR_PAGE1);
  for (int i = 0; i < 6; i++) wr(c, R_PAR0 + i, mac[i]);
  for (int i = 0; i < 8; i++) wr(c, 0x08 + i, 0xFF);
  wr(c, R_CURR, NE2000_RX_START_PAGE + 1);
  wr(c, R_CR, CR_STOP | CR_NODMA | CR_PAGE0);
  wr(c, R_CR, CR_START | CR_NODMA | CR_PAGE0);
  wr(c, R_TXCR, 0x00);
  wr(c, R_RXCR, 0x04);
}

static void test_rx_roundtrip(void) {
  const uint8_t mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
  ne2000_t chip;
  ne2000_reset(&chip, mac);
  init_chip(&chip, mac);

  // A 100-byte frame: dst, src, type, payload.
  uint8_t frame[100];
  for (int i = 0; i < 100; i++) frame[i] = (uint8_t)(0x40 + i);
  memcpy(frame, mac, 6);  // dst = us

  uint8_t curr_before = chip.curr;
  assert(ne2000_deliver_rx(&chip, frame, sizeof(frame)) && "deliver ok");
  assert(chip.isr & NE2000_ISR_RX);
  assert(chip.curr != curr_before && "CURR advanced");

  // ei_receive: read CURR (page 1), then the 4-byte header via remote DMA
  // from the read page (== rx_start_page after init).
  wr(&chip, R_CR, CR_START | CR_NODMA | CR_PAGE1);
  uint8_t cur = rd(&chip, R_CURR);
  wr(&chip, R_CR, CR_START | CR_NODMA | CR_PAGE0);
  assert(cur == chip.curr);

  // The driver's initial read page == CURR before the packet arrived
  // (NS8390_init sets CURR = rx_start_page + 1, and the first packet lands
  // there).
  uint8_t read_page = curr_before;
  wr(&chip, R_RCNTLO, 4);
  wr(&chip, R_RCNTHI, 0);
  wr(&chip, R_RSARLO, 0);
  wr(&chip, R_RSARHI, read_page);
  wr(&chip, R_CR, CR_RREAD | CR_START);
  uint8_t status = rd(&chip, R_DATA);
  uint8_t next = rd(&chip, R_DATA);
  uint8_t cnt_lo = rd(&chip, R_DATA);
  uint8_t cnt_hi = rd(&chip, R_DATA);
  uint16_t count = (uint16_t)(cnt_lo | (cnt_hi << 8));

  assert(status == 0x01 && "RSR = ENRSR_RXOK");
  assert(next == chip.curr && "header next-page == CURR");
  // count = frame + CRC, excluding the 4-byte header (what the driver
  // reads as the body length) = max(len,60) + 4 = 100 + 4 = 104.
  assert(count == 104 && "header byte count");
  assert(next >= NE2000_RX_START_PAGE && next <= NE2000_STOP_PAGE);

  // Read the frame body and check the first bytes match.
  wr(&chip, R_RCNTLO, (uint8_t)(count & 0xFF));
  wr(&chip, R_RCNTHI, (uint8_t)(count >> 8));
  wr(&chip, R_RSARLO, 4);  // skip the 4-byte header
  wr(&chip, R_RSARHI, read_page);
  wr(&chip, R_CR, CR_RREAD | CR_START);
  for (int i = 0; i < 12; i++) {
    assert(rd(&chip, R_DATA) == frame[i] && "frame body matches");
  }
  printf("PASS: RX deliver -> ring header + body read back\n");
}

static void test_tx_roundtrip(void) {
  const uint8_t mac[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
  ne2000_t chip;
  ne2000_reset(&chip, mac);
  init_chip(&chip, mac);

  uint8_t frame[64];
  for (int i = 0; i < 64; i++) frame[i] = (uint8_t)(0x80 + i);

  // ei_start_xmit: remote-DMA write to the tx page, then transmit. The
  // real EtherNEC driver arms the write with a HALVED (word-style) RSAR --
  // $2000 for the tx page at byte $4000 -- while reads stay byte-addressed
  // (observed on hardware). take_tx must therefore serve the positionally
  // staged bytes, not a mem[] region derived from the write address.
  wr(&chip, R_RCNTLO, sizeof(frame));
  wr(&chip, R_RCNTHI, 0);
  wr(&chip, R_RSARLO, 0);
  wr(&chip, R_RSARHI, NE2000_TX_START_PAGE / 2);  // halved: $20 -> $2000
  wr(&chip, R_CR, CR_RWRITE | CR_START);
  for (int i = 0; i < (int)sizeof(frame); i++) wr(&chip, R_DATA, frame[i]);
  wr(&chip, R_CR, CR_NODMA | CR_START);
  wr(&chip, R_ISR, NE2000_ISR_RDC);
  wr(&chip, R_TCNTLO, sizeof(frame));
  wr(&chip, R_TCNTHI, 0);
  wr(&chip, R_CR, CR_TRANS | CR_START);

  uint8_t out[NE2000_MTU];
  uint16_t n = ne2000_take_tx(&chip, out);
  assert(n == sizeof(frame) && "tx length");
  assert(memcmp(out, frame, sizeof(frame)) == 0 && "tx frame matches");
  assert(chip.isr & NE2000_ISR_TX && "TX ISR set");
  assert(ne2000_take_tx(&chip, out) == 0 && "tx handed out only once");
  printf("PASS: TX stage -> take_tx round trip\n");
}

// The EtherNEC ROM3 write encoding must round-trip through the commemul
// sample decode: offset = (reg<<9)|(data<<1) -> reg, data.
static void test_rom3_decode(void) {
  for (uint8_t reg = 0; reg < 0x20; reg++) {
    for (int d = 0; d < 256; d++) {
      uint16_t sample = (uint16_t)(((uint16_t)reg << 9) | ((uint16_t)d << 1));
      assert(mdnet_sample_reg(sample) == reg);
      assert(mdnet_sample_data(sample) == (uint8_t)d);
    }
  }
  // Register-read staging offset: reg N read is served from ROM4 byte
  // (N<<9)^1.
  assert(MDNET_REG_READ_OFFSET(0x07) == ((0x07u << 9) ^ 1u));
  assert(MDNET_REG_READ_OFFSET(0x00) == 1u);
  printf("PASS: ROM3 sample decode + ROM4 read-offset\n");
}

int main(void) {
  test_probe_prom();
  test_rx_roundtrip();
  test_tx_roundtrip();
  test_rom3_decode();
  printf("all NE2000 core tests pass\n");
  return 0;
}
