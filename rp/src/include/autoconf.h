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

// Begin choosing an address for the ST. Call once, after WiFi is up and
// the mailbox is initialised.
void autoconf_start(void);

// Drive the probe state machine. Call from the Core-0 main loop.
void autoconf_poll(void);

// Feed inbound frames in (from the RX tap) so ARP replies can be seen.
void autoconf_observe(const uint8_t *frame, uint16_t len);

// True once address selection has finished, successfully or not.
bool autoconf_done(void);

// The address chosen for the ST (host byte order), or 0 if none was
// found and the ST should fall back to its own configuration.
uint32_t autoconf_address(void);

// Host-test seam: pick the nth candidate address for a given lease.
// Returns 0 when there are no more sensible candidates.
uint32_t autoconf_candidate(uint32_t our_ip, uint32_t mask, uint8_t n);

#endif  // AUTOCONF_H
