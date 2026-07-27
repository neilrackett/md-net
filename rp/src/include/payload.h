/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Cartridge payload staging: see payload.c.
 */

#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

// Copy the payload (MDNET.STX, MDNET.TXT) into the ROM4 window so
// INSTALL.PRG on the ST can read it. Call once, after romemul is up.
void payload_publish(void);

#endif  // PAYLOAD_H
