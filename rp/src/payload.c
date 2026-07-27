/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stages the cartridge payload -- the files INSTALL.PRG writes into the
 * ST's STinG folder -- into the ROM4 window, where the m68k reads them
 * directly. See tools/mkpayload.py for the layout and
 * target/atarist/stx/install.c for the reader.
 *
 * Shipping the driver inside the firmware means the two can never
 * disagree about versions: whatever the cartridge is running is what
 * gets installed.
 */

#include "payload.h"

#include "cart_shared.h"
#include "debug.h"
#include "mailbox.h"
#include "mdnet_payload.h"

// The payload sits above the mailbox's RX frame buffer in the same 64 KB
// window. Nothing else enforces that, so enforce it here: growing
// MB_FRAME_MAX or adding RX slots would otherwise walk into the payload
// silently, and the first symptom would be a corrupt driver on the ST.
_Static_assert(MDNET_PAYLOAD_OFF >= MB_RX_BUF_OFF + MB_FRAME_MAX,
               "payload overlaps the mailbox RX buffer");
_Static_assert(MDNET_PAYLOAD_OFF + sizeof(mdnet_payload) <= 0x10000u,
               "payload overflows the ROM4 window");

void payload_publish(void) {
  // The cart bus swaps bytes within each 16-bit word, so byte k of the
  // m68k's view lives at RP byte k^1.
  volatile uint8_t *rom = (volatile uint8_t *)&__rom_in_ram_start__;
  for (size_t i = 0; i < sizeof(mdnet_payload); i++) {
    rom[(MDNET_PAYLOAD_OFF + i) ^ 1u] = mdnet_payload[i];
  }
  DPRINTF("payload: %u bytes staged at $FA%04X\n",
          (unsigned)sizeof(mdnet_payload), (unsigned)MDNET_PAYLOAD_OFF);
}
