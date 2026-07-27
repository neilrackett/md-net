/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * File: mailbox.h
 * Description: RP side of the MD/Net cart-bus mailbox -- the RP<->ST
 *              contract that replaces the NE2000 emulation. See
 *              docs/mailbox-protocol.md for the full protocol and
 *              target/atarist/stx/mdnet.c for the m68k driver end.
 *
 * Built exclusively on the two proven-solid bus primitives: ROM4 reads
 * served from static RP RAM (romemul) and ROM3 dummy-read writes
 * captured losslessly by the commemul DMA ring. RP-side buffer contents
 * change only between sequence-numbered handshakes, never while the ST
 * might be reading them.
 */

#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

// ---- ROM4 window offsets (m68k byte offsets from $FA0000) ----
// Keep in sync with target/atarist/stx/mdnet.c (MB_* there).
#define MB_PROTO_MAGIC_OFF 0x4020u  // 4 B: 'MDNB'
#define MB_PROTO_VER_OFF 0x4024u    // 2 B: protocol version
#define MB_MAC_OFF 0x4026u          // 6 B: shared STA MAC
#define MB_CFG_SEQ_OFF 0x4030u      // 2 B: bumps when config changes
#define MB_CFG_IP_OFF 0x4032u       // 4 B: ST IP (0 = keep CPX config)
#define MB_CFG_MASK_OFF 0x4036u     // 4 B
#define MB_CFG_GW_OFF 0x403Au       // 4 B
#define MB_CFG_DNS_OFF 0x403Eu      // 4 B
#define MB_RX_SEQ_OFF 0x4044u       // 2 B: bumps per published RX frame
#define MB_RX_LEN_OFF 0x4046u       // 2 B: published frame length
#define MB_TX_ACK_OFF 0x4048u       // 2 B: echoes last committed TX seq
#define MB_RX_CREDITS_OFF 0x404Au   // 2 B: RX frames still queued (diag)
#define MB_RX_BUF_OFF 0x5000u       // 1600 B: the published RX frame

#define MB_PROTO_MAGIC 0x4D444E42u  // 'MDNB'
#define MB_PROTO_VERSION 1u

// ---- ROM3 command channels (chan = (sample>>9)&0x1F, data = (sample>>1)&0xFF)
#define MBC_NOP 0x00u
#define MBC_RX_ACK 0x01u
#define MBC_TX_START 0x02u
#define MBC_TX_LEN_HI 0x03u
#define MBC_TX_DATA 0x04u
#define MBC_TX_COMMIT 0x05u
#define MBC_DRIVER_HELLO 0x06u
#define MBC_DRIVER_BYE 0x07u

#define MB_FRAME_MAX 1600u

// Bring up the mailbox: stage magic/version/MAC/config into the ROM4
// mirror and install the WiFi RX tap. Call after WiFi is connected.
void mailbox_init(void);

// Service the mailbox: drain the commemul ROM3 ring (TX bytes +
// commands) and publish the next queued RX frame if the ST has acked
// the previous one. Call from the Core-0 main loop.
void mailbox_poll(void);

// ROM3 sample handler (exposed for the host-side unit test).
void mailbox_on_rom3_sample(uint16_t sample);

// Host-test seams: the RX publish queue and the publish step.
bool mailbox_rx_enqueue(const uint8_t *frame, uint16_t len);
#ifdef MAILBOX_HOST_TEST
void mailbox_publish_next(void);
#endif

#endif  // MAILBOX_H
