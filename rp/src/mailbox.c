/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mailbox.c
 * Description: RP side of the MD/Net cart-bus mailbox. See mailbox.h and
 *              docs/mailbox-protocol.md.
 *
 * Two RX contracts coexist: the single window a version-1 driver
 * expects, and the slot ring a version-2 driver drains several frames
 * at a time. The driver's HELLO picks one; until then, and after BYE,
 * the single window is served, so a cartridge reflashed under an old
 * driver behaves exactly as before.
 *
 * Everything runs on Core 0: the commemul DMA ring absorbs ROM3 bursts
 * (a full 1500-byte TX stream is ~6 KB of the 32 KB ring), so
 * millisecond-scale poll latency is fine -- there is no timing-critical
 * serving anywhere in this design. ROM4 reads are served by romemul
 * from RAM whose contents only change between sequence-numbered
 * handshakes.
 */

#include "mailbox.h"

#include <string.h>

#ifdef MAILBOX_HOST_TEST
#include <stdio.h>
#define DPRINTF(...) printf(__VA_ARGS__)
#else
#include "autoconf.h"
#include "cart_shared.h"
#include "commemul.h"
#include "debug.h"
#include "lwip/pbuf.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#endif

// ---- ROM4 mirror access (cart bus swaps bytes within 16-bit words) ----

#ifdef MAILBOX_HOST_TEST
uint8_t mailbox_test_rom[0x10000];
static inline volatile uint8_t *rom4(void) { return mailbox_test_rom; }
#else
static inline volatile uint8_t *rom4(void) {
  return (volatile uint8_t *)&__rom_in_ram_start__;
}
#endif

static inline void mb_w8(uint32_t off, uint8_t v) { rom4()[off ^ 1u] = v; }

// 16-bit publish, ATOMICALLY. A little-endian store at the (even) RP
// offset lands the low byte at off and the high byte at off+1, which
// the cart bus swap presents to the m68k as a correct big-endian word --
// and it is a single store, so the ST can never observe a half-updated
// value. Two byte stores would tear: the RX sequence crossing 255->256
// could be read as 511, whose low byte then acks a sequence the RP
// never published, wedging the publish handshake for good.
static inline void mb_w16(uint32_t off, uint16_t v) {
  *(volatile uint16_t *)&rom4()[off] = v;  // off must be even
}
static inline void mb_w32(uint32_t off, uint32_t v) {
  mb_w16(off, (uint16_t)(v >> 16));
  mb_w16(off + 2u, (uint16_t)v);
}

// ---- RX publish queue (WiFi tap -> mailbox window) ----

// Set to 1 to log every bridged frame (see mailbox_tx_complete).
#ifndef MAILBOX_TRACE_FRAMES
#define MAILBOX_TRACE_FRAMES 0
#endif

// Deep enough to absorb a sender's burst: with STinG's 10000-byte
// receive window and 536-byte segments, 18 frames can be in flight.
#define RXQ_SLOTS 16u
static struct {
  volatile uint16_t head, tail;
  uint16_t len[RXQ_SLOTS];
  uint8_t data[RXQ_SLOTS][MB_FRAME_MAX];
} s_rxq;

static uint16_t s_rxSeq = 0;       // last published sequence number
static uint8_t s_rxAckLow = 0;     // low byte of the last MBC_RX_ACK
static bool s_rxOutstanding = false;  // a published frame awaits its ack

// Ring mode (driver version >= 2). s_rxrSeq is the newest published
// sequence, s_rxrAcked the newest the ST has consumed; the difference
// is the number of slots in use. Both wrap freely through 0.
static bool s_ringMode = false;
static uint16_t s_rxrSeq = 0;
static uint16_t s_rxrAcked = 0;

// ---- TX assembly (ROM3 stream -> WiFi) ----

static uint8_t s_txBuf[MB_FRAME_MAX];
static uint16_t s_txLen = 0;    // announced length (TX_START/LEN_HI)
static uint16_t s_txFill = 0;   // bytes received so far
static bool s_txActive = false;

// ---- Diagnostics ----
static uint32_t s_rxPublished = 0, s_rxDropped = 0;
static uint32_t s_txFrames = 0, s_txErrors = 0;
static bool s_driverHello = false;

bool mailbox_rx_enqueue(const uint8_t *frame, uint16_t len) {
  uint16_t h = s_rxq.head;
  uint16_t nh = (uint16_t)((h + 1u) % RXQ_SLOTS);
  if (nh == s_rxq.tail || len > MB_FRAME_MAX) {
    s_rxDropped++;
    return false;
  }
  memcpy(s_rxq.data[h], frame, len);
  s_rxq.len[h] = len;
  __sync_synchronize();
  s_rxq.head = nh;
  return true;
}

static uint16_t rxq_depth(void) {
  return (uint16_t)((s_rxq.head + RXQ_SLOTS - s_rxq.tail) % RXQ_SLOTS);
}

// Stage a frame into the ROM4 window at m68k offset off.
static void mb_wframe(uint32_t off, const uint8_t *f, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    mb_w8(off + i, f[i]);
  }
}

// Ring mode: fill every free slot. A slot is free once the ST's
// cumulative ack has passed the frame that was in it, so nothing here
// is ever rewritten while the ST may still be reading it -- the same
// invariant as the single window, held per slot.
static void mailbox_publish_ring(void) {
  while (s_rxq.tail != s_rxq.head &&
         (uint16_t)(s_rxrSeq - s_rxrAcked) < MB_RXR_SLOTS) {
    uint16_t t = s_rxq.tail;
    uint16_t seq = (uint16_t)(s_rxrSeq + 1u);
    uint32_t slot = seq % MB_RXR_SLOTS;
    mb_wframe(MB_RXR_BUF_OFF + slot * MB_RXR_STRIDE, s_rxq.data[t],
              s_rxq.len[t]);
    mb_w16(MB_RXR_LEN_OFF + slot * 2u, s_rxq.len[t]);
    s_rxq.tail = (uint16_t)((t + 1u) % RXQ_SLOTS);
    __sync_synchronize();
    s_rxrSeq = seq;
    mb_w16(MB_RXR_SEQ_OFF, seq);
    s_rxPublished++;
  }
  mb_w16(MB_RX_CREDITS_OFF, rxq_depth());
}

// Publish the next queued frame into the window. Only called when the
// previous publication has been acked, so the buffer is never rewritten
// while the ST may still be reading it.
#ifdef MAILBOX_HOST_TEST
void mailbox_publish_next(void);  // host test drives this directly
#else
static
#endif
void mailbox_publish_next(void) {
  uint16_t t;
  if (s_ringMode) {
    mailbox_publish_ring();
    return;
  }
  // THE protocol invariant, enforced here rather than at the call site:
  // the window is never rewritten while a publication is unacked, so
  // the ST can copy it at its leisure with no timing constraint at all.
  if (s_rxOutstanding) {
    return;
  }
  t = s_rxq.tail;
  if (t == s_rxq.head) {
    return;  // nothing queued
  }
  uint16_t len = s_rxq.len[t];
  mb_wframe(MB_RX_BUF_OFF, s_rxq.data[t], len);
  s_rxq.tail = (uint16_t)((t + 1u) % RXQ_SLOTS);
  mb_w16(MB_RX_LEN_OFF, len);
  mb_w16(MB_RX_CREDITS_OFF, rxq_depth());
  __sync_synchronize();
  s_rxSeq++;
  if (s_rxSeq == 0u) {
    s_rxSeq = 1u;  // 0 means "nothing published yet"
  }
  mb_w16(MB_RX_SEQ_OFF, s_rxSeq);
  s_rxOutstanding = true;
  s_rxPublished++;
}

// Forget everything queued or outstanding, in both modes. Used when a
// driver arrives or leaves: what was published before has no consumer.
static void mailbox_rx_reset(void) {
  s_rxOutstanding = false;
  s_rxrAcked = s_rxrSeq;
  s_rxq.tail = s_rxq.head;
}

// Hand a completed TX frame to the WiFi.
static void mailbox_tx_complete(uint8_t seqByte) {
  if (s_txFill != s_txLen || s_txLen < 14u) {
    DPRINTF("mailbox: TX commit mismatch fill=%u len=%u\n", (unsigned)s_txFill,
            (unsigned)s_txLen);
    s_txErrors++;
  } else {
#ifndef MAILBOX_HOST_TEST
    int err = cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, s_txLen, s_txBuf,
                                  false);
    if (err != 0) {
      DPRINTF("mailbox: TX %u bytes failed: %d\n", (unsigned)s_txLen, err);
      s_txErrors++;
    } else {
      s_txFrames++;
    }
#if MAILBOX_TRACE_FRAMES
    // Per-frame tracing is OFF by default: one line is ~110 chars, and a
    // blocking UART write at 115200 stalls Core 0 for ~10 ms -- inside
    // the very loop that publishes RX and drains TX, so it inflates the
    // latency it is meant to measure. Turn on only to inspect frames.
    DPRINTF("mailbox: TX len=%u [%02x %02x %02x %02x %02x %02x | %02x %02x "
            "%02x %02x %02x %02x | %02x %02x]\n",
            (unsigned)s_txLen, s_txBuf[0], s_txBuf[1], s_txBuf[2], s_txBuf[3],
            s_txBuf[4], s_txBuf[5], s_txBuf[6], s_txBuf[7], s_txBuf[8],
            s_txBuf[9], s_txBuf[10], s_txBuf[11], s_txBuf[12], s_txBuf[13]);
#endif
#else
    s_txFrames++;
#endif
  }
  mb_w16(MB_TX_ACK_OFF, seqByte);
  s_txActive = false;
  s_txLen = 0;
  s_txFill = 0;
}

void mailbox_on_rom3_sample(uint16_t sample) {
  uint8_t chan = (uint8_t)((sample >> 9) & 0x1Fu);
  uint8_t data = (uint8_t)((sample >> 1) & 0xFFu);
  switch (chan) {
    case MBC_TX_DATA:  // hottest channel first
      if (s_txActive && s_txFill < s_txLen && s_txFill < MB_FRAME_MAX) {
        s_txBuf[s_txFill++] = data;
      }
      break;
    case MBC_RX_ACK:
      s_rxAckLow = data;
      if (s_ringMode) {
        // Cumulative: the ST consumed everything up to this sequence.
        // At most MB_RXR_SLOTS can be outstanding, so the byte delta is
        // unambiguous; anything larger is noise and is ignored.
        uint8_t delta = (uint8_t)(data - (uint8_t)s_rxrAcked);
        if (delta <= (uint16_t)(s_rxrSeq - s_rxrAcked)) {
          s_rxrAcked = (uint16_t)(s_rxrAcked + delta);
        }
      } else if (s_rxOutstanding && data == (uint8_t)s_rxSeq) {
        s_rxOutstanding = false;  // window free; next publish in poll
      }
      break;
    case MBC_TX_START:
      s_txActive = true;
      s_txLen = data;  // low byte; LEN_HI follows
      s_txFill = 0;
      break;
    case MBC_TX_LEN_HI:
      if (s_txActive) {
        s_txLen |= (uint16_t)((uint16_t)data << 8);
        if (s_txLen > MB_FRAME_MAX) {
          DPRINTF("mailbox: TX length %u too big\n", (unsigned)s_txLen);
          s_txActive = false;
          s_txErrors++;
        }
      }
      break;
    case MBC_TX_COMMIT:
      if (s_txActive) {
        mailbox_tx_complete(data);
      }
      break;
    case MBC_DRIVER_HELLO:
      // The driver just came up (first load, reload, or port
      // re-activation). Resync: anything published before it existed
      // was never going to be acked, and leaving that publication
      // outstanding would block every future publish -- the RP starts
      // publishing LAN broadcast traffic as soon as WiFi is up, long
      // before the user reaches STinG, so without this reset RX is dead
      // on arrival. Queued pre-driver frames are stale, so drop them
      // and start clean. s_rxSeq stays monotonic: the driver latches
      // the current value at set_state, so the next publish always
      // differs from it.
      //
      // The version byte also picks the RX mode: a version-2 driver
      // reads the ring, a version-1 driver the single window, and it
      // must be the driver's choice because it is the side that cannot
      // be updated in the field -- a cartridge can be reflashed without
      // anyone touching the ST.
      mailbox_rx_reset();
      s_ringMode = data >= 2u;
      s_txActive = false;
      s_txLen = 0;
      s_txFill = 0;
      s_driverHello = true;
      DPRINTF("mailbox: driver hello, version %u (%s, rx resync at seq %u)\n",
              (unsigned)data, s_ringMode ? "ring" : "single window",
              (unsigned)(s_ringMode ? s_rxrSeq : s_rxSeq));
      break;
    case MBC_DRIVER_BYE:
      DPRINTF("mailbox: driver bye\n");
      s_driverHello = false;
      mailbox_rx_reset();  // no consumer left; do not block publishes
      s_ringMode = false;  // whatever loads next starts from the v1 contract
      break;
    default:
      break;  // MBC_NOP + unassigned channels: ignore
  }
}

bool mailbox_rx_wanted(const uint8_t *f, uint16_t len, uint32_t own_ip) {
  uint16_t type;
  uint32_t dst;
  if (len < 14u) {
    return false;
  }
  type = (uint16_t)(((uint16_t)f[12] << 8) | f[13]);
  if (type == 0x0806u) {
    return true;  // ARP: the driver keeps its own cache
  }
  if (type != 0x0800u || len < 34u) {
    return false;  // IPv6, LLDP, and friends: nothing on the ST wants them
  }
  dst = ((uint32_t)f[30] << 24) | ((uint32_t)f[31] << 16) |
        ((uint32_t)f[32] << 8) | (uint32_t)f[33];
  if (dst == own_ip) {
    return false;  // the Pico's own traffic (DHCP renewals, probes)
  }
  if ((dst >> 28) == 0xEu) {
    return false;  // multicast
  }
  return true;
}

#ifndef MAILBOX_HOST_TEST

// RX tap: every frame the CYW43 receives passes through the STA netif
// input. Queue a copy for the mailbox, then chain to lwIP so the Pico's
// own stack keeps working (shared-MAC design).
#include "lwip/netif.h"

static netif_input_fn s_orig_input = NULL;

static err_t mailbox_netif_input(struct pbuf *p, struct netif *inp) {
  uint16_t len = p->tot_len;
  if (len >= 14u && len <= MB_FRAME_MAX) {
    static uint8_t rxbuf[MB_FRAME_MAX];
    pbuf_copy_partial(p, rxbuf, len, 0);
    autoconf_observe(rxbuf, len);  // watch for defenders of our candidate
    // Every frame handed over costs the ST a service slot whether it
    // wants the frame or not, so filter here rather than in the driver.
    if (mailbox_rx_wanted(rxbuf, len,
                          lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(inp))))) {
      mailbox_rx_enqueue(rxbuf, len);
    }
  }
  if (s_orig_input != NULL) {
    return s_orig_input(p, inp);
  }
  pbuf_free(p);
  return ERR_OK;
}

static void install_rx_tap(void) {
  struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
  if (n->input != mailbox_netif_input) {
    s_orig_input = n->input;
    n->input = mailbox_netif_input;
    DPRINTF("mailbox: RX tap installed on STA netif\n");
  }
}

void mailbox_publish_config(uint32_t ip, uint32_t mask, uint32_t gw,
                            uint32_t dns) {
  static uint16_t s_cfgSeq = 1;
  mb_w32(MB_CFG_IP_OFF, ip);
  mb_w32(MB_CFG_MASK_OFF, mask);
  mb_w32(MB_CFG_GW_OFF, gw);
  mb_w32(MB_CFG_DNS_OFF, dns);
  __sync_synchronize();  // config complete before the sequence advertises it
  mb_w16(MB_CFG_SEQ_OFF, ++s_cfgSeq);
}

void mailbox_init(void) {
  uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};  // fallback LAA
  if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) != 0) {
    DPRINTF("mailbox: cyw43 MAC unavailable, using fallback\n");
  }
  // Capabilities go in before the magic: the driver treats the magic
  // as "the mailbox is staged" and reads the capabilities on its
  // strength, so they must never be observable as zero after it.
  mb_w16(MB_CAPS_OFF, MB_CAP_RX_RING);
  mb_w16(MB_RXR_SEQ_OFF, 0);
  for (uint32_t i = 0; i < MB_RXR_SLOTS; i++) {
    mb_w16(MB_RXR_LEN_OFF + i * 2u, 0);
  }
  __sync_synchronize();
  mb_w32(MB_PROTO_MAGIC_OFF, MB_PROTO_MAGIC);
  mb_w16(MB_PROTO_VER_OFF, MB_PROTO_VERSION);
  for (int i = 0; i < 6; i++) {
    mb_w8(MB_MAC_OFF + (uint32_t)i, mac[i]);
  }
  // Config block: netmask + gateway from the Pico's own lease (the STA
  // netif) so the driver can default them; ST IP stays 0 (driver keeps
  // its CPX-configured address) until a per-ST DHCP lease is
  // implemented.
  struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
  mb_w32(MB_CFG_IP_OFF, 0);
  mb_w32(MB_CFG_MASK_OFF, lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(n))));
  mb_w32(MB_CFG_GW_OFF, lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(n))));
  mb_w32(MB_CFG_DNS_OFF, 0);
  mb_w16(MB_CFG_SEQ_OFF, 1);
  mb_w16(MB_RX_SEQ_OFF, 0);
  mb_w16(MB_RX_LEN_OFF, 0);
  mb_w16(MB_TX_ACK_OFF, 0);
  mb_w16(MB_RX_CREDITS_OFF, 0);
  install_rx_tap();
  DPRINTF("mailbox: ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0],
          mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void mailbox_poll(void) {
  commemul_poll(mailbox_on_rom3_sample);
  mailbox_publish_next();  // no-ops unless the window is free
  // Periodic stats for hardware visibility.
  static uint32_t s_lastStats = 0;
  static uint32_t s_lastRx = 0, s_lastTx = 0;
  uint32_t now = time_us_32();
  uint32_t elapsed = now - s_lastStats;
  if (elapsed > 5000000u) {
    // Frames/s each way since the last report: with per-frame logging
    // off, this is how throughput gets measured.
    uint32_t secs = elapsed / 1000000u;
    uint32_t rxRate = secs ? (s_rxPublished - s_lastRx) / secs : 0;
    uint32_t txRate = secs ? (s_txFrames - s_lastTx) / secs : 0;
    s_lastStats = now;
    s_lastRx = s_rxPublished;
    s_lastTx = s_txFrames;
    DPRINTF("mailbox: rx pub=%lu (%lu/s) drop=%lu q=%u tx=%lu (%lu/s) "
            "err=%lu %s seq=%u ack=%u/%u out=%u hello=%d\n",
            (unsigned long)s_rxPublished, (unsigned long)rxRate,
            (unsigned long)s_rxDropped, (unsigned)rxq_depth(),
            (unsigned long)s_txFrames, (unsigned long)txRate,
            (unsigned long)s_txErrors, s_ringMode ? "ring" : "win",
            (unsigned)(s_ringMode ? s_rxrSeq : s_rxSeq),
            (unsigned)s_rxAckLow,
            (unsigned)((s_ringMode ? s_rxrAcked : s_rxSeq) & 0xFFu),
            (unsigned)(s_ringMode ? (uint16_t)(s_rxrSeq - s_rxrAcked)
                                  : (s_rxOutstanding ? 1u : 0u)),
            (int)s_driverHello);
  }
}

#endif  // !MAILBOX_HOST_TEST
