/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: emul.c
 * Description: MD/Net boot path. Brings up the cartridge bus emulator,
 *              the ROM3 capture ring, the SD card and the SELECT
 *              button, then joins the WiFi network (STA mode,
 *              credentials from the Booster global config) and
 *              publishes the outcome to the Atari through the cart
 *              shared region: a status longword the m68k boot code
 *              polls, plus a message string it prints via GEMDOS
 *              Cconws before letting TOS continue to GEM.
 *
 * The framebuffer / audio / IKBD pipelines of the template this app
 * was forked from were stripped: MD/Net's cartridge code does not stay
 * resident, so there is nothing to draw, play, or capture yet. The
 * commemul ROM3 ring stays initialised as the future NE2000
 * register-capture path.
 */

#include "emul.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "aconfig.h"
#include "cart_shared.h"
#include "commemul.h"
#include "debug.h"
#include "ff.h"
#include "gconfig.h"
#include "mailbox.h"
#include "memfunc.h"
#include "network.h"
#include "payload.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"

// Publish the MD/Net boot status to the shared-region slot the m68k
// boot code polls. Must go through cart_asM68kLong() so the m68k's
// move.l observes the intended value across the cart-bus byte-swap.
static void mdnet_setStatus(uint32_t status) {
  volatile uint32_t *slot =
      (volatile uint32_t *)((uintptr_t)&__rom_in_ram_start__ +
                            CART_MDNET_STATUS_OFFSET);
  *slot = cart_asM68kLong(status);
}

// Publish the boot message and then the terminal status, in that
// order, so the m68k can never observe a terminal status with a stale
// message.
static void mdnet_publishResult(uint32_t status, const char *msg) {
  volatile uint8_t *msgDst =
      (volatile uint8_t *)((uintptr_t)&__rom_in_ram_start__ +
                           CART_MDNET_MSG_OFFSET);
  cart_writeM68kString(msgDst, msg, CART_MDNET_MSG_SIZE);
  __sync_synchronize();
  mdnet_setStatus(status);
}

void emul_start() {
  // RP2040 RAM is undefined at power-on; firmware.py only emits the
  // bytes up to the last non-zero in BOOT.BIN (padded to 64 KB), so
  // without an explicit erase the shared region past the cart image
  // would be whatever was sitting in RAM. Zeroing it also guarantees
  // MDNET_STATUS starts as BOOTING (0) and MDNET_MSG as an empty
  // string.
  ERASE_FIRMWARE_IN_RAM();

  // Copy the cartridge image into the now-zeroed region.
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Initialise the cartridge ROM4 read engine. ROM4 reads are served
  // entirely by chained DMAs feeding the PIO TX FIFO -- no CPU/IRQ
  // involvement -- so the ST can boot and poll the status slot while
  // the blocking WiFi connect below is still running.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 cart-bus capture (PIO + 32 KB DMA ring). Nothing
  // drains it this milestone; the ring is circular and overwrites
  // itself harmlessly. It is kept warm as the NE2000 register-capture
  // path of the next milestone.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }

  mdnet_setStatus(CART_MDNET_STATUS_CONNECTING);

  // SD card -- best-effort. MD/Net has no SD dependency this
  // milestone; the folder comes from per-app config so the app can be
  // reconfigured from Booster without recompiling.
  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  const char *folderName = folder ? folder->value : "/mdnet";
  if (sdcard_initFilesystem(&fsys, folderName) != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable. Continuing without SD.\n");
  }

  // Cartridge SELECT button. Reset actions are intentionally NOT wired:
  // spurious edges were firing a "long press" and jumping to Booster,
  // killing MD/Net mid-session. Configure the pin but leave the callbacks
  // unset so a stray edge does nothing. (Power-cycle to reset; hold SELECT
  // at power-on still reaches Booster via main.c's early check.)
  select_configure();

  // Join the WiFi network in STA mode. Credentials and network
  // parameters come read-only from the Booster global config
  // (PARAM_WIFI_*). Retry only on timeout: hard errors (no SSID, bad
  // auth mode, init failure) won't improve on a second attempt, and a
  // bounded worst case keeps the RP's terminal status well inside the
  // m68k boot code's 65 s backstop.
  char msg[CART_MDNET_MSG_SIZE];
  wifi_sta_conn_process_status_t err = NETWORK_WIFI_STA_CONN_ERR_NOT_INITIALIZED;
  int initErr = network_wifiInit(WIFI_MODE_STA);
  if (initErr != 0) {
    DPRINTF("Error initializing the network: %i\n", initErr);
  } else {
    const int maxAttempts = 2;
    err = NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;
    for (int attempt = 0; attempt < maxAttempts &&
                          err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;
         attempt++) {
      err = network_wifiStaConnect();
      if (err != NETWORK_WIFI_STA_CONN_OK) {
        DPRINTF("WiFi connect attempt %d failed: %i\n", attempt + 1, err);
      }
    }
  }

  if (err == NETWORK_WIFI_STA_CONN_OK) {
    ip_addr_t ip = network_getCurrentIp();
    snprintf(msg, sizeof(msg), "MD/Net connected: %s\r\n\r\n", ip4addr_ntoa(&ip));
    mdnet_publishResult(CART_MDNET_STATUS_CONNECTED, msg);
  } else {
    snprintf(msg, sizeof(msg), "MD/Net: WiFi connection failed (%s)\r\n\r\n",
             network_WifiStaConnStatusString(err));
    mdnet_publishResult(CART_MDNET_STATUS_FAILED, msg);
  }
  DPRINTF("%s", msg);

  // Bring up the cart-bus mailbox: publishes the protocol magic, MAC
  // and network config into the ROM4 window and installs the WiFi RX
  // tap. The cartridge image (banner + magic) is left intact -- the
  // mailbox fields live outside it, so even a warm reset still boots
  // with the banner.
  mailbox_init();

  // Publish the installer's payload (the driver + its notes) into the
  // ROM4 window. Cheap, one-off, and it means INSTALL.PRG always writes
  // the driver matching this firmware.
  payload_publish();

  // Idle loop: drain the ROM3 command ring into the mailbox, publish
  // queued RX frames, service lwIP/cyw43 and the SELECT button. Nothing
  // here is timing-critical: the DMA ring absorbs bus bursts and ROM4
  // reads are served from static RAM by romemul.
  DPRINTF("Entering main loop\n");
  while (true) {
    mailbox_poll();
    network_safePoll();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(1));
    select_checkPushReset();
  }
}
