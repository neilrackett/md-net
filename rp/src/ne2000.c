/**
 * File: ne2000.c
 * Description: Hardware-independent NE2000 (DP8390 + 16 KB ring) model.
 *              See ne2000.h and docs/ne2000-emulation.md. No RP2040 or
 *              cartridge-bus dependencies -- pure state machine, host
 *              testable (tools/ne2000_test.c).
 *
 * Register and command semantics follow the EtherNEC driver (NE.S /
 * 8390.I) that STinG loads for the NetUSBee, and standard RTL8019AS
 * behaviour where the two agree.
 */

#include "ne2000.h"

#include <string.h>

// 8390 command register bits (E8390_* in 8390.I).
#define CR_STOP 0x01u
#define CR_START 0x02u
#define CR_TRANS 0x04u
#define CR_RREAD 0x08u
#define CR_RWRITE 0x10u
#define CR_RDMA_MASK 0x38u  // RREAD | RWRITE | (RSEND) -- remote DMA op
#define CR_NODMA 0x20u
#define CR_PAGE_SHIFT 6

// Register offsets (8390.I). Page selected by CR bits 6-7.
#define REG_CR 0x00u
#define REG_ISR 0x07u
#define REG_DATAPORT 0x10u
#define REG_RESET 0x1Fu

// Remote-DMA address map of a standard NE2000: the 32-byte station-address
// PROM at 0x0000, the 16 KB buffer RAM at 0x4000..0x7FFF (pages
// 0x40..0x7F). The driver reads the PROM at probe (RSAR=0) and the ring
// after init.
#define DMA_BUFFER_BASE (NE2000_RING_FIRST_PAGE * NE2000_PAGE_SIZE)  // 0x4000
#define DMA_BUFFER_END (NE2000_STOP_PAGE * NE2000_PAGE_SIZE)         // 0x8000

void ne2000_reset(ne2000_t *chip, const uint8_t mac[6]) {
  memset(chip, 0, sizeof(*chip));

  // Power-on register defaults the driver's probe assumes.
  chip->cr = CR_STOP | CR_NODMA;  // $21
  chip->isr = NE2000_ISR_RESET;   // probe requires the reset bit set
  chip->pstart = NE2000_RX_START_PAGE;
  chip->pstop = NE2000_STOP_PAGE;
  chip->bnry = NE2000_RX_START_PAGE;
  chip->curr = NE2000_RX_START_PAGE;
  chip->tpsr = NE2000_TX_START_PAGE;

  memcpy(chip->par, mac, 6);

  // Build the 32-byte PROM: each MAC byte doubled, NE2000 signature $57 at
  // bytes 14/15 (ei_probe1 checks pbSA_prom+14/+15 == $57).
  for (int i = 0; i < 6; i++) {
    chip->prom[2 * i] = mac[i];
    chip->prom[2 * i + 1] = mac[i];
  }
  chip->prom[14] = 0x57;
  chip->prom[15] = 0x57;
}

static inline uint8_t ne2000_page(const ne2000_t *chip) {
  return (chip->cr >> CR_PAGE_SHIFT) & 0x03u;
}

// One byte at remote-DMA address `addr`. PROM and out-of-range are
// read-only.
static uint8_t dma_read_byte(const ne2000_t *chip, uint16_t addr) {
  if (addr < NE2000_PROM_SIZE) {
    return chip->prom[addr];
  }
  if (addr >= DMA_BUFFER_BASE && addr < DMA_BUFFER_END) {
    return chip->mem[addr - DMA_BUFFER_BASE];
  }
  return 0;
}

static void dma_write_byte(ne2000_t *chip, uint16_t addr, uint8_t data) {
  if (addr >= DMA_BUFFER_BASE && addr < DMA_BUFFER_END) {
    chip->mem[addr - DMA_BUFFER_BASE] = data;
  }
}

// Advance the remote-DMA pointer after a data-port access, wrapping at the
// rx ring stop page back to the start page (the chip wraps while reading a
// received frame that straddles the ring end). PROM/tx-page accesses stay
// linear -- they never reach pstop.
static void dma_advance(ne2000_t *chip) {
  chip->rsar++;
  if (chip->rcnt > 0) {
    chip->rcnt--;
  }
  uint16_t stop = (uint16_t)chip->pstop * NE2000_PAGE_SIZE;
  uint16_t start = (uint16_t)chip->pstart * NE2000_PAGE_SIZE;
  if (chip->rsar >= DMA_BUFFER_BASE && chip->rsar >= stop &&
      stop > start) {
    chip->rsar = start;
  }
  if (chip->rcnt == 0) {
    chip->isr |= NE2000_ISR_RDC;  // remote DMA complete
  }
}

// Execute a command-register write (side effects: DMA setup, transmit).
static void ne2000_command(ne2000_t *chip, uint8_t cmd) {
  chip->cr = cmd;

  if (cmd & CR_STOP) {
    chip->started = false;
  }
  if (cmd & CR_START) {
    chip->started = true;
  }

  // Transmit: hand the staged frame at the tx page to the bridge. The
  // frame was just written there via remote-DMA write; TCNT holds its
  // length. Latch it for ne2000_take_tx() and report success.
  if (cmd & CR_TRANS) {
    chip->tsr = 0x01u;  // ENTSR_PTX -- transmitted without error
    chip->isr |= NE2000_ISR_TX;
    // pending-tx latch: a non-zero tcnt with TRANS set means "take it".
    // ne2000_take_tx() reads tcnt/tpsr directly and clears the latch.
  }
}

void ne2000_reg_write(ne2000_t *chip, uint8_t reg, uint8_t data) {
  reg &= 0x1Fu;

  if (reg == REG_CR) {
    ne2000_command(chip, data);
    return;
  }
  if (reg == REG_DATAPORT) {
    dma_write_byte(chip, chip->rsar, data);
    dma_advance(chip);
    return;
  }
  if (reg == REG_RESET) {
    // Write to the reset port completes the soft reset.
    chip->isr |= NE2000_ISR_RESET;
    return;
  }

  if (ne2000_page(chip) == 1) {
    // Page 1: PAR0-5 ($01-$06), CURR ($07), MAR0-7 ($08-$0f).
    if (reg >= 0x01u && reg <= 0x06u) {
      chip->par[reg - 0x01u] = data;
    } else if (reg == 0x07u) {
      chip->curr = data;
    } else if (reg >= 0x08u && reg <= 0x0Fu) {
      chip->mar[reg - 0x08u] = data;
    }
    return;
  }

  // Page 0 writes.
  switch (reg) {
    case 0x01u: chip->pstart = data; break;  // STARTPG
    case 0x02u: chip->pstop = data; break;   // STOPPG
    case 0x03u: chip->bnry = data; break;    // BNRY
    case 0x04u: chip->tpsr = data; break;    // TPSR
    case 0x05u: chip->tcnt_lo = data; break; // TCNTLO
    case 0x06u: chip->tcnt_hi = data; break; // TCNTHI
    case 0x07u: chip->isr &= (uint8_t)~data; break;  // ISR: write-1-clear
    case 0x08u: chip->rsar = (chip->rsar & 0xFF00u) | data; break;  // RSARLO
    case 0x09u:
      chip->rsar = (uint16_t)((chip->rsar & 0x00FFu) | ((uint16_t)data << 8));
      break;  // RSARHI
    case 0x0Au: chip->rcnt = (chip->rcnt & 0xFF00u) | data; break;  // RCNTLO
    case 0x0Bu:
      chip->rcnt = (uint16_t)((chip->rcnt & 0x00FFu) | ((uint16_t)data << 8));
      break;  // RCNTHI
    case 0x0Cu: chip->rcr = data; break;  // RXCR
    case 0x0Du: chip->tcr = data; break;  // TXCR
    case 0x0Eu: chip->dcr = data; break;  // DCFG
    case 0x0Fu: chip->imr = data; break;  // IMR
    default: break;
  }
}

uint8_t ne2000_reg_read(ne2000_t *chip, uint8_t reg) {
  reg &= 0x1Fu;

  if (reg == REG_DATAPORT) {
    uint8_t b = dma_read_byte(chip, chip->rsar);
    dma_advance(chip);
    return b;
  }
  if (reg == REG_RESET) {
    // Reading the reset port triggers a soft reset (probe does this).
    chip->isr |= NE2000_ISR_RESET;
    chip->started = false;
    return 0;
  }
  if (reg == REG_CR) {
    return chip->cr;
  }

  if (ne2000_page(chip) == 1) {
    if (reg >= 0x01u && reg <= 0x06u) {
      return chip->par[reg - 0x01u];
    }
    if (reg == 0x07u) {
      return chip->curr;  // CURPAG
    }
    if (reg >= 0x08u && reg <= 0x0Fu) {
      return chip->mar[reg - 0x08u];
    }
    return 0;
  }

  // Page 0 reads.
  switch (reg) {
    case 0x03u: return chip->bnry;                  // BNRY
    case 0x04u: return chip->tsr;                   // TSR
    case 0x07u: return chip->isr;                   // ISR
    case 0x08u: return (uint8_t)(chip->rsar & 0xFFu);       // CRDALO
    case 0x09u: return (uint8_t)((chip->rsar >> 8) & 0xFFu);// CRDAHI
    case 0x0Cu: return 0x01u;  // RSR: report ENRSR_RXOK-ish idle
    default: return 0;
  }
}

bool ne2000_deliver_rx(ne2000_t *chip, const uint8_t *frame, uint16_t len) {
  if (!chip->started) {
    return false;
  }
  // Pad to the 60-byte Ethernet minimum, then append a 4-byte CRC slot the
  // driver discards; the on-ring frame is (padded frame + CRC).
  uint16_t frame_len = len < 60u ? 60u : len;
  uint16_t on_ring = frame_len + 4u;  // + CRC
  if (on_ring > NE2000_MTU) {
    return false;  // oversize, drop
  }
  // Standard 8390 header byte count includes the 4 header bytes.
  uint16_t count = on_ring + 4u;
  uint16_t pages = (uint16_t)((count + NE2000_PAGE_SIZE - 1u) /
                              NE2000_PAGE_SIZE);

  uint8_t start_page = chip->curr;
  uint8_t next_page = (uint8_t)(start_page + pages);
  if (next_page >= chip->pstop) {
    next_page = (uint8_t)(chip->pstart + (next_page - chip->pstop));
  }

  // Ring-full check: dropping into the boundary page means the driver
  // hasn't consumed yet. Signal overrun and drop.
  if (next_page == chip->bnry) {
    chip->isr |= NE2000_ISR_OVER;
    return false;
  }

  // Write header + frame into the ring starting at start_page, wrapping at
  // pstop. mem[] is indexed from page pstart-relative... actually from the
  // ring first page (0x40); pages pstart.. live at mem[(page-0x40)*256].
  uint16_t ring_off =
      (uint16_t)((start_page - NE2000_RING_FIRST_PAGE) * NE2000_PAGE_SIZE);
  uint16_t ring_size = NE2000_RING_BYTES;

  uint8_t header[4] = {
      0x01u,                      // RSR: ENRSR_RXOK
      next_page,                  // next packet page
      (uint8_t)(count & 0xFFu),   // count low
      (uint8_t)((count >> 8) & 0xFFu),  // count high
  };
  for (int i = 0; i < 4; i++) {
    chip->mem[ring_off] = header[i];
    ring_off = (uint16_t)((ring_off + 1u) % ring_size);
  }
  for (uint16_t i = 0; i < frame_len; i++) {
    chip->mem[ring_off] = (i < len) ? frame[i] : 0u;  // pad tail with zeros
    ring_off = (uint16_t)((ring_off + 1u) % ring_size);
  }
  for (int i = 0; i < 4; i++) {  // CRC placeholder
    chip->mem[ring_off] = 0u;
    ring_off = (uint16_t)((ring_off + 1u) % ring_size);
  }

  chip->curr = next_page;
  chip->isr |= NE2000_ISR_RX;
  return true;
}

uint16_t ne2000_take_tx(ne2000_t *chip, uint8_t *out) {
  uint16_t len = (uint16_t)(((uint16_t)chip->tcnt_hi << 8) | chip->tcnt_lo);
  if (len == 0u || !(chip->cr & CR_TRANS)) {
    return 0u;
  }
  if (len > NE2000_MTU) {
    len = NE2000_MTU;
  }
  uint16_t src = (uint16_t)((chip->tpsr - NE2000_RING_FIRST_PAGE) *
                            NE2000_PAGE_SIZE);
  for (uint16_t i = 0; i < len; i++) {
    out[i] = chip->mem[(uint16_t)((src + i) % NE2000_RING_BYTES)];
  }
  // Clear the transmit latch so we hand each frame out exactly once.
  chip->cr &= (uint8_t)~CR_TRANS;
  chip->tcnt_lo = 0;
  chip->tcnt_hi = 0;
  return len;
}
