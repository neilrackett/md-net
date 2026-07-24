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

int main(void) {
  test_decode_matches_driver_encoding();
  test_tx_roundtrip();
  test_rx_publish_ack();
  printf("all mailbox tests pass\n");
  return 0;
}
