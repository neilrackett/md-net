/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mailbox_test.c
 * Description: Host-side unit test for the RP mailbox module: ROM3
 *              command decode, TX assembly, RX publish/ack handshake and
 *              the ROM4 byte-swap staging.
 *
 * Build + run:
 *   cc -DMAILBOX_HOST_TEST -Irp/src/include -o /tmp/mailbox_test \
 *      tools/mailbox_test.c rp/src/mailbox.c && /tmp/mailbox_test
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "autoconf.h"
#include "mailbox.h"

extern uint8_t mailbox_test_rom[0x10000];

// m68k-eye view of the ROM4 window: byte at m68k offset k is RP byte k^1.
static uint8_t m68k_r8(uint32_t off) { return mailbox_test_rom[off ^ 1u]; }
static uint16_t m68k_r16(uint32_t off) {
  return (uint16_t)((m68k_r8(off) << 8) | m68k_r8(off + 1u));
}

// Compose a ROM3 sample the way the driver's dummy read does:
// chan in A9-A13, data in A1-A8.
static uint16_t rom3(uint8_t chan, uint8_t data) {
  return (uint16_t)(((uint16_t)chan << 9) | ((uint16_t)data << 1));
}

static void test_tx_roundtrip(void) {
  uint8_t frame[64];
  for (int i = 0; i < 64; i++) frame[i] = (uint8_t)(0x10 + i);

  mailbox_on_rom3_sample(rom3(MBC_TX_START, 64 & 0xFF));
  mailbox_on_rom3_sample(rom3(MBC_TX_LEN_HI, 64 >> 8));
  for (int i = 0; i < 64; i++) {
    mailbox_on_rom3_sample(rom3(MBC_TX_DATA, frame[i]));
  }
  mailbox_on_rom3_sample(rom3(MBC_TX_COMMIT, 1));
  assert(m68k_r16(MB_TX_ACK_OFF) == 1 && "TX ack published");
  printf("PASS: TX stream -> assembled frame + ack\n");
}

// The publish/ack handshake, exactly as the driver drives it. Also
// pins the resync behaviour: without it, a frame published before the
// driver ever loaded (which happens on every real boot -- the RP starts
// bridging LAN broadcasts as soon as WiFi is up) is never acked and
// blocks every later publish, killing RX for the whole session.
static void test_rx_publish_ack(void) {
  uint8_t f1[60], f2[60];
  uint16_t seq1, seq2;
  int i;

  memset(f1, 0xAA, sizeof(f1));
  memset(f2, 0xBB, sizeof(f2));
  f1[0] = 0x01; f1[59] = 0x99;
  f2[0] = 0x02;

  assert(mailbox_rx_enqueue(f1, sizeof(f1)));
  assert(mailbox_rx_enqueue(f2, sizeof(f2)));

  mailbox_publish_next();
  seq1 = m68k_r16(MB_RX_SEQ_OFF);
  assert(seq1 != 0 && "a frame was published");
  assert(m68k_r16(MB_RX_LEN_OFF) == sizeof(f1) && "length published");
  for (i = 0; i < (int)sizeof(f1); i++) {
    assert(m68k_r8(MB_RX_BUF_OFF + i) == f1[i] && "frame readable by m68k");
  }

  // Unacked: the window must not be overwritten while the ST may read it.
  mailbox_publish_next();
  assert(m68k_r16(MB_RX_SEQ_OFF) == seq1 && "no republish before ack");
  assert(m68k_r8(MB_RX_BUF_OFF) == f1[0] && "buffer untouched before ack");

  // Ack releases the window; the next frame appears.
  mailbox_on_rom3_sample(rom3(MBC_RX_ACK, (uint8_t)seq1));
  mailbox_publish_next();
  seq2 = m68k_r16(MB_RX_SEQ_OFF);
  assert(seq2 != seq1 && "sequence advanced after ack");
  assert(m68k_r8(MB_RX_BUF_OFF) == f2[0] && "second frame published");

  // Driver hello must resync a publication stranded before it loaded.
  assert(mailbox_rx_enqueue(f1, sizeof(f1)));
  mailbox_publish_next();  // blocked: seq2 never acked
  assert(m68k_r16(MB_RX_SEQ_OFF) == seq2 && "still blocked without ack");
  mailbox_on_rom3_sample(rom3(MBC_DRIVER_HELLO, 1));
  assert(mailbox_rx_enqueue(f1, sizeof(f1)));
  mailbox_publish_next();
  assert(m68k_r16(MB_RX_SEQ_OFF) != seq2 && "hello resyncs the handshake");
  printf("PASS: RX publish/ack handshake + driver-hello resync\n");
}

// Ring mode, as a version-2 driver drives it: hello with version 2,
// then consume frames slot by slot and ack cumulatively.
static void test_rx_ring(void) {
  uint8_t f[MB_RXR_SLOTS + 4][60];
  uint16_t base, seq;
  unsigned i, j;

  mailbox_on_rom3_sample(rom3(MBC_DRIVER_HELLO, 2));
  base = m68k_r16(MB_RXR_SEQ_OFF);

  for (i = 0; i < MB_RXR_SLOTS + 4; i++) {
    memset(f[i], (int)(0x30 + i), sizeof(f[i]));
    f[i][59] = (uint8_t)i;
    assert(mailbox_rx_enqueue(f[i], sizeof(f[i])));
  }

  // Every free slot fills at once; the rest wait for acks.
  mailbox_publish_next();
  seq = m68k_r16(MB_RXR_SEQ_OFF);
  assert((uint16_t)(seq - base) == MB_RXR_SLOTS && "ring filled, no more");
  for (i = 0; i < MB_RXR_SLOTS; i++) {
    uint16_t s = (uint16_t)(base + 1u + i);
    uint32_t slot = s % MB_RXR_SLOTS;
    assert(m68k_r16(MB_RXR_LEN_OFF + slot * 2u) == 60 && "slot length");
    for (j = 0; j < 60; j++) {
      assert(m68k_r8(MB_RXR_BUF_OFF + slot * MB_RXR_STRIDE + j) == f[i][j] &&
             "slot holds its frame, m68k byte order");
    }
  }
  mailbox_publish_next();
  assert(m68k_r16(MB_RXR_SEQ_OFF) == seq && "nothing published without acks");

  // A bogus ack (further ahead than anything outstanding) is ignored.
  mailbox_on_rom3_sample(rom3(MBC_RX_ACK, (uint8_t)(base + 100u)));
  mailbox_publish_next();
  assert(m68k_r16(MB_RXR_SEQ_OFF) == seq && "bogus ack ignored");

  // Consuming three frees three slots; three more appear, in order,
  // and the slot the ST is still reading is left alone.
  mailbox_on_rom3_sample(rom3(MBC_RX_ACK, (uint8_t)(base + 3u)));
  mailbox_publish_next();
  assert((uint16_t)(m68k_r16(MB_RXR_SEQ_OFF) - base) == MB_RXR_SLOTS + 3u);
  {
    uint32_t slot4 = (uint32_t)(base + 4u) % MB_RXR_SLOTS;
    assert(m68k_r8(MB_RXR_BUF_OFF + slot4 * MB_RXR_STRIDE + 59) == 3 &&
           "unacked slot untouched");
    uint32_t slot9 = (uint32_t)(base + MB_RXR_SLOTS + 1u) % MB_RXR_SLOTS;
    assert(m68k_r8(MB_RXR_BUF_OFF + slot9 * MB_RXR_STRIDE + 59) ==
               MB_RXR_SLOTS && "freed slot reused for the next frame");
  }

  // Ack everything: the last queued frame goes out too.
  mailbox_on_rom3_sample(
      rom3(MBC_RX_ACK, (uint8_t)(base + MB_RXR_SLOTS + 3u)));
  mailbox_publish_next();
  assert((uint16_t)(m68k_r16(MB_RXR_SEQ_OFF) - base) == MB_RXR_SLOTS + 4u);
  mailbox_on_rom3_sample(
      rom3(MBC_RX_ACK, (uint8_t)(base + MB_RXR_SLOTS + 4u)));

  // The legacy window was never written in ring mode.
  assert(m68k_r16(MB_RX_SEQ_OFF) == 0 ||
         m68k_r8(MB_RX_BUF_OFF) != f[0][0]);
  printf("PASS: RX ring fill, cumulative ack, slot reuse, bogus ack\n");
}

// Run the ring through both wraps: the 8-bit ack byte (every 256) and
// the 16-bit sequence (once), one frame at a time and in bursts.
static void test_rx_ring_wrap(void) {
  uint8_t f[60];
  uint32_t n;
  uint16_t seq = m68k_r16(MB_RXR_SEQ_OFF);
  memset(f, 0x5A, sizeof(f));
  for (n = 0; n < 70000u; n++) {
    unsigned burst = 1u + (unsigned)(n % 5u), k;
    for (k = 0; k < burst; k++) assert(mailbox_rx_enqueue(f, sizeof(f)));
    mailbox_publish_next();
    assert((uint16_t)(m68k_r16(MB_RXR_SEQ_OFF) - seq) == burst &&
           "burst published in full");
    seq = m68k_r16(MB_RXR_SEQ_OFF);
    mailbox_on_rom3_sample(rom3(MBC_RX_ACK, (uint8_t)seq));
  }
  printf("PASS: RX ring across the 8-bit ack and 16-bit sequence wraps\n");
}

// A partial ack that straddles the 8-bit boundary: drive the sequence
// to 253, publish five, ack three (through 256), then the rest.
static void test_rx_ring_partial_ack_at_wrap(void) {
  uint8_t f[60];
  uint16_t seq;
  unsigned i;
  memset(f, 0x3C, sizeof(f));
  seq = m68k_r16(MB_RXR_SEQ_OFF);
  while ((uint8_t)seq != 253) {
    assert(mailbox_rx_enqueue(f, sizeof(f)));
    mailbox_publish_next();
    seq = m68k_r16(MB_RXR_SEQ_OFF);
    mailbox_on_rom3_sample(rom3(MBC_RX_ACK, (uint8_t)seq));
  }
  for (i = 0; i < 8; i++) assert(mailbox_rx_enqueue(f, sizeof(f)));
  mailbox_publish_next();
  assert((uint8_t)m68k_r16(MB_RXR_SEQ_OFF) == 5 && "ring full: 254..5");
  mailbox_on_rom3_sample(rom3(MBC_RX_ACK, 0));  // 254, 255, 256(=0) consumed
  for (i = 0; i < 6; i++) assert(mailbox_rx_enqueue(f, sizeof(f)));
  mailbox_publish_next();
  assert((uint8_t)m68k_r16(MB_RXR_SEQ_OFF) == 8 &&
         "three slots freed across the wrap, no more");
  mailbox_on_rom3_sample(rom3(MBC_RX_ACK, 8));
  mailbox_publish_next();
  assert((uint8_t)m68k_r16(MB_RXR_SEQ_OFF) == 11 && "remaining three");
  mailbox_on_rom3_sample(rom3(MBC_RX_ACK, 11));
  printf("PASS: partial cumulative ack across the 8-bit wrap\n");
}

// A version-1 driver arriving later gets the single window back.
static void test_mode_switch_back(void) {
  uint8_t f[60];
  uint16_t rseq, seq;
  memset(f, 0x77, sizeof(f));
  mailbox_on_rom3_sample(rom3(MBC_DRIVER_BYE, 0));
  rseq = m68k_r16(MB_RXR_SEQ_OFF);
  assert(mailbox_rx_enqueue(f, sizeof(f)));
  mailbox_publish_next();
  assert(m68k_r16(MB_RXR_SEQ_OFF) == rseq && "ring idle after bye");
  seq = m68k_r16(MB_RX_SEQ_OFF);
  assert(seq != 0 && m68k_r8(MB_RX_BUF_OFF) == 0x77 && "window served");
  mailbox_on_rom3_sample(rom3(MBC_RX_ACK, (uint8_t)seq));
  mailbox_on_rom3_sample(rom3(MBC_DRIVER_HELLO, 1));
  assert(mailbox_rx_enqueue(f, sizeof(f)));
  mailbox_publish_next();
  assert(m68k_r16(MB_RX_SEQ_OFF) != seq && "v1 hello: window mode");
  assert(m68k_r16(MB_RXR_SEQ_OFF) == rseq && "ring untouched by a v1 driver");
  printf("PASS: bye/hello(1) return to the single window\n");
}

// The RP-side filter: only what the ST could want costs it a slot.
static void test_rx_filter(void) {
  uint8_t f[64];
  const uint32_t own = 0xC0A801F1u;  // 192.168.1.241
  memset(f, 0, sizeof(f));
  f[12] = 0x08; f[13] = 0x06;
  assert(mailbox_rx_wanted(f, 60, own) && "ARP wanted");
  f[12] = 0x86; f[13] = 0xDD;
  assert(!mailbox_rx_wanted(f, 60, own) && "IPv6 dropped");
  f[12] = 0x08; f[13] = 0x00;
  f[30] = 0xC0; f[31] = 0xA8; f[32] = 0x01; f[33] = 0xF2;
  assert(mailbox_rx_wanted(f, 60, own) && "IP to the ST wanted");
  assert(!mailbox_rx_wanted(f, 20, own) && "truncated IP dropped");
  f[33] = 0xFF;
  assert(mailbox_rx_wanted(f, 60, own) && "subnet broadcast wanted");
  f[30] = f[31] = f[32] = f[33] = 0xFF;
  assert(mailbox_rx_wanted(f, 60, own) && "limited broadcast wanted");
  f[30] = 0xC0; f[31] = 0xA8; f[32] = 0x01; f[33] = 0xF1;
  assert(!mailbox_rx_wanted(f, 60, own) && "Pico's own unicast dropped");
  f[30] = 0xE0; f[31] = 0x00; f[32] = 0x00; f[33] = 0xFB;
  assert(!mailbox_rx_wanted(f, 60, own) && "multicast dropped");
  assert(!mailbox_rx_wanted(f, 10, own) && "runt dropped");
  printf("PASS: RX filter (ARP, IP for the ST, broadcasts; not own/multicast/IPv6)\n");
}

static void test_decode_matches_driver_encoding(void) {
  // The driver issues: (void)*(volatile uint8*)(0xFB0000 + (chan<<9) + (data<<1))
  // The PIO sample is the low 16 address bits. Verify decode for all values.
  for (uint8_t chan = 0; chan < 8; chan++) {
    for (int d = 0; d < 256; d++) {
      uint16_t s = rom3(chan, (uint8_t)d);
      assert(((s >> 9) & 0x1F) == chan);
      assert(((s >> 1) & 0xFF) == d);
    }
  }
  printf("PASS: ROM3 channel/data encode-decode round trip\n");
}

// Address selection for the ST: it must never offer the Pico's own
// address, the network address or the broadcast address, and it should
// start just above the Pico so a lease of .241 offers the ST .242.
static void test_autoconf_candidates(void) {
  uint32_t ip = 0xC0A801F1u;   // 192.168.1.241
  uint32_t mask = 0xFFFFFF00u; // /24
  uint32_t seen[8];
  int i, j;

  for (i = 0; i < 8; i++) {
    seen[i] = autoconf_candidate(ip, mask, (uint8_t)i);
    assert(seen[i] != 0 && "a /24 has plenty of room");
    assert((seen[i] & mask) == (ip & mask) && "stays in our subnet");
    assert(seen[i] != ip && "never our own address");
    assert((seen[i] & ~mask) != 0 && "never the network address");
    assert((seen[i] & ~mask) != ~mask && "never the broadcast address");
    for (j = 0; j < i; j++) assert(seen[j] != seen[i] && "no repeats");
  }
  assert(seen[0] == 0xC0A801F2u && ".241 offers the ST .242");
  assert(seen[1] == 0xC0A801F3u && "then .243");

  // Wrapping past .254 must skip .255 and .0 and continue at .1.
  assert(autoconf_candidate(0xC0A801FDu, mask, 0) == 0xC0A801FEu);
  assert(autoconf_candidate(0xC0A801FDu, mask, 1) == 0xC0A80101u);

  // A subnet with no room for a second host must refuse.
  assert(autoconf_candidate(ip, 0xFFFFFFFFu, 0) == 0 && "/32 has no room");
  assert(autoconf_candidate(ip, 0xFFFFFFFEu, 0) == 0 && "/31 has no room");
  printf("PASS: ST address selection (subnet, skips, wrap, exhaustion)\n");
}

int main(void) {
  test_decode_matches_driver_encoding();
  test_tx_roundtrip();
  test_rx_publish_ack();
  test_rx_ring();
  test_rx_ring_wrap();
  test_rx_ring_partial_ack_at_wrap();
  test_mode_switch_back();
  test_rx_filter();
  test_autoconf_candidates();
  printf("all mailbox tests pass\n");
  return 0;
}
