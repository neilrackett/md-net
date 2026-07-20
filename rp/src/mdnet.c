/**
 * File: mdnet.c
 * Description: Cartridge-bus integration for the NE2000 model. See mdnet.h
 *              and docs/ne2000-emulation.md.
 *
 * Core split:
 *   - Core 1 owns the NE2000 chip and all cartridge-bus servicing: it
 *     drains the commemul ROM3 ring (register + TX-byte writes), restages
 *     register reads into the ROM4 mirror, serves the data port, and moves
 *     frames in/out of the ring. Doing this on a dedicated core gives the
 *     microsecond latency the driver's back-to-back page-switch-then-read
 *     sequences need -- Core 0's poll loop was too slow, so the driver read
 *     a stale register-7 (ISR instead of CURR) and the RX loop failed.
 *   - Core 0 owns WiFi/lwIP. Frames cross between the cores through two
 *     single-producer/single-consumer queues (rx: Core 0 -> Core 1,
 *     tx: Core 1 -> Core 0), so neither core touches the other's state.
 *
 * All chip access is on Core 1; the only exception is the MAC-PROM pre-arm
 * in mdnet_activate(), done before Core 1 is launched.
 */

#include "mdnet.h"

#include <string.h>

#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "dataport.h"
#include "debug.h"
#include "lwip/pbuf.h"
#include "ne2000.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/platform.h"

// 8390 command-register bits we watch for side effects at the bus layer.
#define CR_TRANS 0x04u
#define CR_RREAD 0x08u
#define CR_DATAPORT_REG 0x10u

static ne2000_t s_chip;
static volatile bool s_active = false;
// The driver's currently selected register page, tracked low-latency from
// the ROM3 CR tap so register 7 (ISR in page 0, CURR in page 1) is staged
// with the right meaning before the driver reads it.
static uint8_t s_curpage = 0;

// Cross-core SPSC frame queues. rx: Core 0 (WiFi) -> Core 1 (ring);
// tx: Core 1 (ring) -> Core 0 (WiFi). head is written only by the
// producer, tail only by the consumer.
#define FRM_CAP 1536u
#define RXQ_SLOTS 8u
#define TXQ_SLOTS 4u

static struct {
  volatile uint16_t head, tail;
  uint16_t len[RXQ_SLOTS];
  uint8_t data[RXQ_SLOTS][FRM_CAP];
} s_rxq;

static struct {
  volatile uint16_t head, tail;
  uint16_t len[TXQ_SLOTS];
  uint8_t data[TXQ_SLOTS][FRM_CAP];
} s_txq;

// Diagnostics (Core 1 writes, Core 0 logs).
static volatile uint32_t s_rxDelivered = 0, s_rxDropped = 0;
static volatile uint32_t s_txSent = 0, s_txDropped = 0;
static volatile uint32_t s_txCmd = 0;  // CR=TRANS commands the driver issued
// Last remote-DMA read the driver armed, for on-hardware visibility into
// what it is reading (header vs body, from which page, and the first bytes
// we serve).
static volatile uint16_t s_dbgRsar = 0, s_dbgRcnt = 0;
static volatile uint8_t s_dbgB[4] = {0, 0, 0, 0};
static volatile uint32_t s_dbgRreadSeq = 0;

static bool rxq_push(const uint8_t *f, uint16_t len) {
  uint16_t h = s_rxq.head;
  uint16_t nh = (uint16_t)((h + 1u) % RXQ_SLOTS);
  if (nh == s_rxq.tail || len > FRM_CAP) {
    return false;
  }
  memcpy(s_rxq.data[h], f, len);
  s_rxq.len[h] = len;
  __sync_synchronize();
  s_rxq.head = nh;
  return true;
}

static uint16_t rxq_pop(uint8_t *out) {
  uint16_t t = s_rxq.tail;
  if (t == s_rxq.head) {
    return 0;
  }
  uint16_t len = s_rxq.len[t];
  memcpy(out, s_rxq.data[t], len);
  __sync_synchronize();
  s_rxq.tail = (uint16_t)((t + 1u) % RXQ_SLOTS);
  return len;
}

static bool txq_push(const uint8_t *f, uint16_t len) {
  uint16_t h = s_txq.head;
  uint16_t nh = (uint16_t)((h + 1u) % TXQ_SLOTS);
  if (nh == s_txq.tail || len > FRM_CAP) {
    return false;
  }
  memcpy(s_txq.data[h], f, len);
  s_txq.len[h] = len;
  __sync_synchronize();
  s_txq.head = nh;
  return true;
}

static uint16_t txq_pop(uint8_t *out) {
  uint16_t t = s_txq.tail;
  if (t == s_txq.head) {
    return 0;
  }
  uint16_t len = s_txq.len[t];
  memcpy(out, s_txq.data[t], len);
  __sync_synchronize();
  s_txq.tail = (uint16_t)((t + 1u) % TXQ_SLOTS);
  return len;
}

static volatile uint8_t *rom4_bytes(void) {
  return (volatile uint8_t *)&__rom_in_ram_start__;
}

// Re-stage every register the driver polls for the currently selected
// page (register 7 is ISR in page 0 but CURR in page 1, at the same ROM4
// address -- so the page-select CR write must flip the staged byte before
// the driver's next read). The data port ($10) and reset ($1f) are never
// staged here.
static void restage_registers(void) {
  volatile uint8_t *r = rom4_bytes();
  for (uint8_t reg = 0; reg <= 0x0Fu; reg++) {
    r[MDNET_REG_READ_OFFSET(reg)] = ne2000_reg_read(&s_chip, reg);
  }
}

// Register 7's value for the tracked page: ISR in page 0, CURR in page 1.
static uint8_t reg7_value(void) {
  return (s_curpage == 1u) ? s_chip.curr : s_chip.isr;
}

// The data port serves the chip's live remote-DMA byte. next_byte (called
// by dataport_service after each data-port read) steps to the next byte;
// the loop also refreshes the served byte each iteration so an RSAR change
// from the register path is reflected before the ST reads.
static uint8_t mdnet_dp_next(void) {
  ne2000_dma_advance(&s_chip);
  return ne2000_dma_current(&s_chip);
}

// Drain the low-latency ROM3 CR tap: for each command-register write, track
// the selected page and flip register 7 immediately. This is the fast path
// that beats the driver's page-select -> CURR-read window (the commemul DMA
// ring was too slow, leaving register 7 stale).
static void crtap_service(void) {
  uint16_t addr;
  while (dataport_crtap_get(&addr)) {
    if (mdnet_sample_reg(addr) == 0x00u) {  // command-register write
      uint8_t cr = mdnet_sample_data(addr);
      s_curpage = (uint8_t)((cr >> 6) & 0x03u);
      rom4_bytes()[MDNET_REG_READ_OFFSET(0x07u)] = reg7_value();
      if (cr & CR_RREAD) {  // remote-DMA read armed: sample what we'll serve
        s_dbgRsar = s_chip.rsar;
        s_dbgRcnt = s_chip.rcnt;
        s_dbgB[0] = ne2000_dma_current(&s_chip);
        s_dbgRreadSeq++;
      }
    }
  }
}

// Fast restage of only the registers the driver polls in steady state.
// Register 7 is served for the tracked page; BNRY/TSR/RSR are page-0
// registers the driver reads directly. Runs every Core-1 iteration instead
// of the full 16-register restage so the loop stays short.
static void stage_hot(void) {
  volatile uint8_t *r = rom4_bytes();
  r[MDNET_REG_READ_OFFSET(0x07u)] = reg7_value();  // ISR (page 0) / CURR (page 1)
  r[MDNET_REG_READ_OFFSET(0x03u)] = s_chip.bnry;   // BNRY
  r[MDNET_REG_READ_OFFSET(0x04u)] = s_chip.tsr;    // TSR
  r[MDNET_REG_READ_OFFSET(0x0Cu)] = 0x01u;         // RSR: ENRSR_RXOK-ish idle
}

// commemul callback (Core 1): one ROM3 sample == one EtherNEC register or
// data-port write. This drives the full chip model (state, TX-byte writes
// into the ring). The two latency-critical actions -- the register-7 page
// flip and arming a remote-DMA read -- are handled by the faster CR tap
// (crtap_service), not here.
static void on_rom3_sample(uint16_t sample) {
  uint8_t reg = mdnet_sample_reg(sample);
  uint8_t data = mdnet_sample_data(sample);
  ne2000_reg_write(&s_chip, reg, data);
  if (reg == 0x00u && (data & CR_TRANS)) {
    s_txCmd++;  // the driver asked the chip to transmit (e.g. an ARP reply)
  }
}

// Core 1: the NE2000 servicing loop. The data port and the command bus are
// never busy at the same instant (the driver either writes registers or
// reads the data port), so a single non-blocking loop keeps both
// responsive.
static void __not_in_flash_func(mdnet_core1_loop)(void) {
  static uint8_t rxbuf[FRM_CAP];
  static uint8_t txbuf[NE2000_MTU];
  for (;;) {
    // CR tap first, every iteration: flip register 7's page with minimal
    // latency so it's correct before the driver's next read. Then the full
    // command-bus drain (chip state), the data-port serve, and the fast
    // register restage.
    crtap_service();
    commemul_poll(on_rom3_sample);         // register + TX-byte writes
    dataport_service(mdnet_dp_next);       // advance the served byte per read
    dataport_set_byte(ne2000_dma_current(&s_chip));  // serve the live byte
    stage_hot();                           // fast restage of the polled registers

    // Deliver at most ONE queued frame per iteration, then service the
    // taps again right away.
    uint16_t n = rxq_pop(rxbuf);
    if (n > 0) {
      if (ne2000_deliver_rx(&s_chip, rxbuf, n)) {
        s_rxDelivered++;
      } else {
        s_rxDropped++;
      }
      crtap_service();
      commemul_poll(on_rom3_sample);
      dataport_service(mdnet_dp_next);
      dataport_set_byte(ne2000_dma_current(&s_chip));
    }

    uint16_t t = ne2000_take_tx(&s_chip, txbuf);  // hand off transmit frames
    if (t > 0) {
      if (txq_push(txbuf, t)) {
        s_txSent++;
      } else {
        s_txDropped++;
      }
    }
  }
}

// RX tap (Core 0): every frame the CYW43 receives passes through the STA
// netif input. Queue it for Core 1, then chain to lwIP so the Pico's own
// stack keeps working. Frames arrive already addressed to our shared MAC
// (+ broadcast/multicast), so no promiscuous mode is needed.
static netif_input_fn s_orig_input = NULL;

static err_t mdnet_netif_input(struct pbuf *p, struct netif *inp) {
  uint16_t len = p->tot_len;
  if (s_active && len >= 14u && len <= NE2000_MTU) {
    static uint8_t rxbuf[NE2000_MTU];
    pbuf_copy_partial(p, rxbuf, len, 0);
    // Diagnostic: surface every ARP request so the UART shows whether the
    // laptop's who-has for the ST's IP actually reaches the NE2000 queue.
    if (len >= 42u && rxbuf[12] == 0x08u && rxbuf[13] == 0x06u &&
        rxbuf[21] == 0x01u) {
      DPRINTF("mdnet: ARP who-has %u.%u.%u.%u\n", rxbuf[38], rxbuf[39],
              rxbuf[40], rxbuf[41]);
    }
    if (!rxq_push(rxbuf, len)) {
      s_rxDropped++;
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
  if (n->input != mdnet_netif_input) {
    s_orig_input = n->input;
    n->input = mdnet_netif_input;
    DPRINTF("mdnet: RX tap installed on STA netif\n");
  }
}

void mdnet_init(void) {
  uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};  // fallback LAA
  if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) != 0) {
    DPRINTF("mdnet: cyw43 MAC unavailable, using fallback\n");
  }
  ne2000_reset(&s_chip, mac);
  dataport_init();   // ROM4 tap (PIO only; serviced from Core 1 below)
  install_rx_tap();  // intercept WiFi RX frames into the rx queue
  DPRINTF("mdnet: NE2000 model ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void mdnet_activate(void) {
  restage_registers();
  // The data port now serves the chip's live remote-DMA byte, so the MAC
  // PROM read (RSAR=0) is served straight from chip->prom -- no pre-arm.
  s_active = true;
  __sync_synchronize();
  multicore_launch_core1(mdnet_core1_loop);
  DPRINTF("mdnet: Core 1 servicing started; register map active\n");
}

void mdnet_poll(void) {
  if (!s_active) {
    return;
  }
  // TX drain: send frames Core 1 queued from the NE2000 tx page.
  static uint8_t txbuf[FRM_CAP];
  uint16_t t;
  while ((t = txq_pop(txbuf)) > 0) {
    int err = cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, t, txbuf, false);
    if (err != 0) {
      DPRINTF("mdnet TX %u bytes failed: %d\n", (unsigned)t, err);
    }
  }

  // Periodic stats so the bridge can be watched on hardware.
  static uint32_t lastDp = 0, lastRx = 0, lastTx = 0;
  uint32_t dp = dataport_readCount(), rx = s_rxDelivered, tx = s_txSent;
  if (dp != lastDp || rx != lastRx || tx != lastTx) {
    DPRINTF("mdnet: dp-reads=%lu rx=%lu(drop %lu) tx=%lu(cmd %lu drop %lu)\n",
            (unsigned long)dp, (unsigned long)rx, (unsigned long)s_rxDropped,
            (unsigned long)tx, (unsigned long)s_txCmd,
            (unsigned long)s_txDropped);
    lastDp = dp;
    lastRx = rx;
    lastTx = tx;
  }

  // Sample the most recent remote-DMA read the driver armed. rcnt=4 is a
  // packet header and b0 is its status byte -- should be 01 (RXOK) and the
  // rsar should be a real ring page ($47..$60). rcnt=32 from rsar=0000 is
  // the MAC PROM (b0 = first MAC byte).
  static uint32_t lastRread = 0;
  uint32_t rr = s_dbgRreadSeq;
  if (rr != lastRread) {
    DPRINTF("mdnet: RREAD rsar=%04x rcnt=%u b0=%02x\n", (unsigned)s_dbgRsar,
            (unsigned)s_dbgRcnt, s_dbgB[0]);
    lastRread = rr;
  }
}
