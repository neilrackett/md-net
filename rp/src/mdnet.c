/**
 * File: mdnet.c
 * Description: Cartridge-bus integration for the NE2000 model. See mdnet.h
 *              and docs/ne2000-emulation.md.
 *
 * What works with today's hardware:
 *   - register writes  (ROM3 dummy reads captured by commemul -> chip)
 *   - TX packet bytes  (data-port writes are ROM3 reads, also captured)
 *   - register reads    (staged into the ROM4 RAM mirror romemul serves)
 *
 * What still needs the dedicated data-port PIO serve path (hardware
 * bring-up): the remote-DMA data-port READ stream -- the MAC PROM the
 * probe reads and RX packet bytes. mdnet_prepareReadStream() builds the
 * byte stream that path will emit; the PIO/DMA wiring is TODO.
 */

#include "mdnet.h"

#include <string.h>

#include "cart_shared.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "ne2000.h"
#include "network.h"
#include "pico/cyw43_arch.h"

// 8390 command-register bits we watch for side effects at the bus layer.
#define CR_TRANS 0x04u
#define CR_RREAD 0x08u
#define CR_DATAPORT_REG 0x10u

static ne2000_t s_chip;
static bool s_active = false;  // ROM4 repainted as the register map?

// Linear byte stream the data-port read path will emit, rebuilt each time
// the driver arms a remote-DMA read. (Consumed by the PIO serve path once
// that lands; staged here so the logic is ready and testable.)
static uint8_t s_readStream[NE2000_MTU + 32];
static uint16_t s_readStreamLen = 0;

static volatile uint8_t *rom4_bytes(void) {
  return (volatile uint8_t *)&__rom_in_ram_start__;
}

// Stage one register's read value into the ROM4 mirror at (reg<<9)^1.
static void stage_reg(uint8_t reg) {
  uint8_t val = ne2000_reg_read(&s_chip, reg);
  rom4_bytes()[MDNET_REG_READ_OFFSET(reg)] = val;
}

// Re-stage every register the driver polls for the currently selected
// page (the page is chip state, so the same ROM4 address serves the
// page-0 or page-1 meaning depending on the last CR write). The data port
// ($10) and reset ($1f) are never staged here -- the data port is served
// by its own path, and reg reads for $00..$0f are non-mutating.
static void restage_registers(void) {
  for (uint8_t reg = 0; reg <= 0x0Fu; reg++) {
    stage_reg(reg);
  }
}

// Build the data-port read stream from the chip for the armed remote-DMA
// read (RSAR..RSAR+RCNT). This mirrors what the auto-incrementing serve
// path will emit; it reads the chip through the same non-destructive
// path the bus would, then rewinds RSAR/RCNT so the real transfer still
// happens when the ST reads it out.
static void prepare_read_stream(void) {
  uint16_t rsar = s_chip.rsar;
  uint16_t rcnt = s_chip.rcnt;
  s_readStreamLen = 0;
  uint16_t n = rcnt;
  if (n > sizeof(s_readStream)) {
    n = sizeof(s_readStream);
  }
  for (uint16_t i = 0; i < n; i++) {
    s_readStream[s_readStreamLen++] = ne2000_reg_read(&s_chip, 0x10u);
  }
  // Rewind: the stream is a preview; the ST's own reads must reproduce it.
  s_chip.rsar = rsar;
  s_chip.rcnt = rcnt;
}

// commemul callback: one ROM3 sample == one EtherNEC register/data write.
static void on_rom3_sample(uint16_t sample) {
  uint8_t reg = mdnet_sample_reg(sample);
  uint8_t data = mdnet_sample_data(sample);
  ne2000_reg_write(&s_chip, reg, data);

  // Log control-register writes (not the high-volume data port) so a
  // STinG probe shows its ei_probe1 sequence on the UART -- the on-device
  // validation for the ROM3 capture + decode path.
  if (reg != CR_DATAPORT_REG) {
    DPRINTF("mdnet W reg=%02x data=%02x\n", reg, data);
  }

  // A command-register write may arm a remote-DMA read (prep the stream)
  // or a transmit (handled in the bridge poll). Everything else just
  // updates state we re-stage below.
  if (reg == 0x00u && (data & CR_RREAD)) {
    prepare_read_stream();
  }
}

static void bridge_poll(void) {
  // TX: hand any queued frame to the WiFi side. Data-port writes captured
  // above already filled the tx page; take_tx yields the staged frame.
  static uint8_t txbuf[NE2000_MTU];
  uint16_t txlen = ne2000_take_tx(&s_chip, txbuf);
  if (txlen > 0) {
    // TODO(hardware): emit via cyw43 raw ethernet
    // (cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, txlen, txbuf,
    // false)). Needs on-device validation of the L2 bridge / MAC handling.
    DPRINTF("mdnet TX %u bytes (bridge not yet wired)\n",
            (unsigned)txlen);
  }

  // RX: frames arriving from lwIP/cyw43 are pushed via ne2000_deliver_rx()
  // from the netif input hook (TODO: install it). deliver_rx sets ISR_RX,
  // which we re-stage so the polling driver sees the pending packet.
}

void mdnet_init(void) {
  uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};  // fallback LAA
  if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) != 0) {
    DPRINTF("mdnet: cyw43 MAC unavailable, using fallback\n");
  }
  ne2000_reset(&s_chip, mac);
  DPRINTF("mdnet: NE2000 model ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void mdnet_activate(void) {
  // Zero the register-read window and stage the reset-state register file.
  // (The full boot-cartridge -> register-map repaint and the warm-reset
  // restore of the cartridge magic are hardware-bring-up items; for now we
  // just switch our staging on so the ST reads live register values.)
  restage_registers();
  s_active = true;
  DPRINTF("mdnet: register map active\n");
}

void mdnet_poll(void) {
  if (!s_active) {
    return;
  }
  commemul_poll(on_rom3_sample);
  bridge_poll();
  restage_registers();
}
