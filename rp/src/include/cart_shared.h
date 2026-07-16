/**
 * File: cart_shared.h
 * Description: Cart 64 KB shared-region layout + cart-bus helpers.
 *
 * The cart shared region at $FA0000..$FAFFFF on the m68k mirrors RP
 * RAM starting at __rom_in_ram_start__. This header defines the
 * region's sub-block offsets (cart image, command sentinel, indexed
 * shared variables, MD/Net status + message, APP_FREE) plus the
 * cart_asM68kLong() / cart_writeM68kString() helpers for exact-value
 * RP→m68k writes across the cart-bus byte-swap.
 *
 * Layout must match target/atarist/src/main.s on the m68k side.
 */

#ifndef CART_SHARED_H
#define CART_SHARED_H

#include <inttypes.h>
#include <stdbool.h>

/* All offsets are relative to __rom_in_ram_start__, which mirrors
 * ROM4_ADDR ($FA0000) on the m68k side. Layout (single source of
 * truth, must match target/atarist/src/main.s):
 *
 *   $FA0000  CARTRIDGE             m68k header + code (max 16 KB)
 *   $FA4000  CMD_MAGIC_SENTINEL    4 B  (RP→m68k command word;
 *                                        unused this milestone, RP
 *                                        leaves CART_CMD_NOP)
 *   $FA4004  (reserved)           12 B
 *   $FA4010  SHARED_VARIABLES    240 B  (60 indexed 4-byte slots.
 *                                        Slot 0 = MDNET_STATUS; the
 *                                        rest are app-free.)
 *   $FA4100  MDNET_MSG           256 B  (NUL-terminated boot message
 *                                        composed by the RP)
 *   $FA4200  APP_FREE                   (free arena to end of region)
 *   $FAFFFF  end of region
 */
#define CART_CARTRIDGE_CODE_SIZE         0x4000  /* 16 KB cart-image budget */
#define CART_SHARED_BLOCK_OFFSET         CART_CARTRIDGE_CODE_SIZE
#define CART_CMD_SENTINEL_OFFSET         CART_SHARED_BLOCK_OFFSET
#define CART_SHARED_VARIABLES_OFFSET     (CART_SHARED_BLOCK_OFFSET + 0x10)
#define CART_SHARED_VARIABLES_SLOTS      60      /* 240 bytes total */

/* MD/Net boot status, polled by the m68k boot code (SHARED_VARIABLES
 * slot 0). Written RP-side via cart_asM68kLong(). Terminal states
 * (CONNECTED / FAILED) must be written AFTER the message buffer, with
 * __sync_synchronize() in between, so the m68k never prints a stale
 * message. The m68k long-read is two word reads, but tearing is
 * benign here: the m68k-visible high word is 0 for every state. */
#define CART_MDNET_STATUS_SLOT           0
#define CART_MDNET_STATUS_OFFSET                                              \
  (CART_SHARED_VARIABLES_OFFSET + (CART_MDNET_STATUS_SLOT * 4))
#define CART_MDNET_STATUS_BOOTING        0u
#define CART_MDNET_STATUS_CONNECTING     1u
#define CART_MDNET_STATUS_CONNECTED      2u
#define CART_MDNET_STATUS_FAILED         3u

/* Boot message shown on the ST via GEMDOS Cconws: a NUL-terminated
 * string (CR/LF included by the RP), stored byte-pair-swapped via
 * cart_writeM68kString(). The boot-time ERASE_FIRMWARE_IN_RAM()
 * zero-fill guarantees it is always a valid (empty) string. */
#define CART_MDNET_MSG_OFFSET            (CART_SHARED_BLOCK_OFFSET + 0x100)
#define CART_MDNET_MSG_SIZE              256

/* APP_FREE arena runs from the end of the message buffer to the top
 * of the 64 KB region. */
#define CART_APP_FREE_OFFSET             (CART_MDNET_MSG_OFFSET + CART_MDNET_MSG_SIZE)
#define CART_REGION_END                  0x10000  /* 64 KB shared region top */

/* RP→m68k command sentinel values. Retained for future use (the
 * m68k boot code no longer stays resident to poll them). Must match
 * the m68k-side equs in target/atarist/src/main.s. */
#define CART_CMD_NOP        0u
#define CART_CMD_RESET      1u
#define CART_CMD_BOOT_GEM   2u
#define CART_CMD_START      4u

/* The cart bus byte-swaps WITHIN each 16-bit word: RP stores LE,
 * m68k reads BE, and the swap makes that transparent for uint16_t.
 * For uint32_t, m68k's BE long-read is two word reads in (high, low)
 * order, but the two 16-bit halves stay in their RP-LE positions --
 * so m68k sees the halves SWAPPED.
 *
 * For exact-value RP→m68k longword protocols (CMD_MAGIC_SENTINEL,
 * etc.) the RP must store the half-swapped value so the m68k's
 * move.l observes the intended uint32_t. Protocols that only care
 * about inequality (the FB dirty-frame counter is the canonical
 * example) don't need this -- both sides see distinct values for
 * distinct writes regardless of the swap. */
static inline uint32_t cart_asM68kLong(uint32_t v) {
  return (v << 16) | (v >> 16);
}

/* Write a C string so the m68k reads it byte-for-byte: because of the
 * within-word byte-swap, m68k byte [k] is RP byte [k ^ 1]. Always
 * NUL-terminates (truncating to maxLen - 1 source bytes). The swap
 * touches dst[i ^ 1], so the buffer must be at least an even number
 * of bytes long and word-aligned (both true for CART_MDNET_MSG). */
static inline void cart_writeM68kString(volatile uint8_t *dst, const char *src,
                                        uint32_t maxLen) {
  uint32_t i = 0;
  for (; src[i] != '\0' && i < maxLen - 1; i++) {
    dst[i ^ 1] = (uint8_t)src[i];
  }
  dst[i ^ 1] = 0;
}

#endif /* CART_SHARED_H */
