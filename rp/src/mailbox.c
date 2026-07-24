/**
 * File: mailbox.c
 * Description: RP side of the MD/Net cart-bus mailbox. See mailbox.h and
 *              docs/mailbox-protocol.md.
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

#define RXQ_SLOTS 8u
static struct {
  volatile uint16_t head, tail;
  uint16_t len[RXQ_SLOTS];
  uint8_t data[RXQ_SLOTS][MB_FRAME_MAX];
} s_rxq;

static uint16_t s_rxSeq = 0;       // last published sequence number
static uint8_t s_rxAckLow = 0;     // low byte of the last MBC_RX_ACK
static bool s_rxOutstanding = false;  // a published frame awaits its ack

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
  const uint8_t *f = s_rxq.data[t];
  for (uint16_t i = 0; i < len; i++) {
    mb_w8(MB_RX_BUF_OFF + i, f[i]);
  }
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
    DPRINTF("mailbox: TX len=%u [%02x %02x %02x %02x %02x %02x | %02x %02x "
            "%02x %02x %02x %02x | %02x %02x]\n",
            (unsigned)s_txLen, s_txBuf[0], s_txBuf[1], s_txBuf[2], s_txBuf[3],
            s_txBuf[4], s_txBuf[5], s_txBuf[6], s_txBuf[7], s_txBuf[8],
            s_txBuf[9], s_txBuf[10], s_txBuf[11], s_txBuf[12], s_txBuf[13]);
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
      if (s_rxOutstanding && data == (uint8_t)s_rxSeq) {
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
      s_rxOutstanding = false;
      s_rxq.tail = s_rxq.head;
      s_txActive = false;
      s_txLen = 0;
      s_txFill = 0;
      s_driverHello = true;
      DPRINTF("mailbox: driver hello, version %u (rx resync at seq %u)\n",
              (unsigned)data, (unsigned)s_rxSeq);
      break;
    case MBC_DRIVER_BYE:
      DPRINTF("mailbox: driver bye\n");
      s_driverHello = false;
      s_rxOutstanding = false;  // no consumer left; do not block publishes
      s_rxq.tail = s_rxq.head;
      break;
    default:
      break;  // MBC_NOP + unassigned channels: ignore
  }
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
    mailbox_rx_enqueue(rxbuf, len);
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

void mailbox_init(void) {
  uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};  // fallback LAA
  if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) != 0) {
    DPRINTF("mailbox: cyw43 MAC unavailable, using fallback\n");
  }
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
  uint32_t now = time_us_32();
  if (now - s_lastStats > 5000000u) {
    s_lastStats = now;
    DPRINTF("mailbox: rx pub=%lu drop=%lu q=%u tx=%lu err=%lu seq=%u ack=%u "
            "hello=%d\n",
            (unsigned long)s_rxPublished, (unsigned long)s_rxDropped,
            (unsigned)rxq_depth(), (unsigned long)s_txFrames,
            (unsigned long)s_txErrors, (unsigned)s_rxSeq, (unsigned)s_rxAckLow,
            (int)s_driverHello);
  }
}

#endif  // !MAILBOX_HOST_TEST
