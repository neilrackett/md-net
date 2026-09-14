/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Automatic network configuration for the ST: see autoconf.c.
 */

#ifndef AUTOCONF_H
#define AUTOCONF_H

#include <stdbool.h>
#include <stdint.h>

// Publish the configuration the ST should adopt -- our own address,
// mask, gateway and DNS. Call once, after WiFi is up and the mailbox is
// initialised.
void autoconf_start(void);

// The address the ST will use (host byte order), or 0 if there was none
// to hand over and the ST should fall back to its own configuration.
uint32_t autoconf_address(void);

#endif  // AUTOCONF_H
