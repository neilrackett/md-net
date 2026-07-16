# MD/Net milestone 2 — NE2000 emulation design

Status: **design + hardware-independent core.** The register-file state
machine (`rp/src/ne2000.c`) and its host test are done; the cartridge-bus
integration (serving register reads, the data-port auto-increment path)
is specified here but not yet wired — it needs on-hardware iteration.

This document is the implementation spec. The reference sources are the
EtherNEC/EtherNE driver (`SRC/NE.S`, `SRC/8390.I`, `SRC/BUSENEC.I`,
`HARDWARE.TXT` in EmmanuelKasper/ethernec, GPL) — the same driver STinG
loads for NetUSBee — and the RTL8019AS / DP8390 datasheets.

## Goal

Present a NE2000 (NatSemi DP8390 + 16 KB ring) to the Atari over the
cartridge port, decoded exactly the way EtherNEC hardware decodes it, so
STinG's stock EtherNEC `.STX` driver probes and drives it unmodified,
bridging Ethernet frames to the LAN over the Pico W's WiFi.

## The EtherNEC address decode (the load-bearing fact)

The cartridge port is read-only and has no A0 (it has /UDS,/LDS). EtherNEC
exploits that:

| Operation | m68k access | Address formula | Encodes |
| --- | --- | --- | --- |
| **Register read** | `move.b (reg<<9)(a6)`, a6=`$FA0000` | `$FA0000 + reg*512` | reg in A9–A13; data returned on bus |
| **Register write** | `tst.b ((reg<<8)\|data)<<1(a5)`, a5=`$FB0000` | `$FB0000 + reg*512 + data*2` | reg in A9–A13, data in A1–A8; **read data ignored** |

So a register number is always address bits **A9–A13** (5 bits → regs
0–31, matching `NE_IO_EXTENT=$20`). Reads live in the **ROM4** window,
writes in the **ROM3** window. Source: `HARDWARE.TXT` lines 29–47,
`BUSENEC.I` macros `getBUS` (`(reg)<<9(RdBUS)`), `putBUSi`
(`((reg<<8)!(data))<<1(RcBUS)`).

This maps directly onto the existing bus emulation:

- **ROM3 writes** — `commemul` already captures every ROM3 read into its
  DMA ring as a 16-bit address sample. Decode each sample:
  `reg = (sample >> 9) & 0x1F`, `data = (sample >> 1) & 0xFF`. Feed
  `ne2000_reg_write(reg, data)`. The m68k's returned data is "harmless"
  (`HARDWARE.TXT` line 47), so ROM3 need not drive the data bus — which
  is exactly today's behaviour (romemul serves ROM4 only; ROM3 is
  capture-only). **This half already works with the current hardware.**

- **ROM4 register reads** — `romemul` serves `$FA0000+off` from
  `RAM[off]` with a fresh per-cycle DMA lookup (no caching:
  `romemul.c` chains addr-capture → `al3_read_addr_trig` → RAM fetch →
  PIO TX every read). So if the RP keeps `RAM[reg*512]` equal to the live
  register value, the next m68k read returns it. Register reads are
  therefore *servable by staging RAM* — with two caveats below.

## Open problem 1 — the data port has no static-RAM answer

Remote-DMA moves packet bytes through the data port `NE_DATAPORT=$10`,
i.e. address `$FA2000`. Crucially **every** data-port access is the *same*
address `$FA2000` (`getBUS/getMore NE_DATAPORT`, and `NE2RAM`'s
`movep.l NE_DATAPORT<<9(a6)` repeated). The real chip returns the next
ring byte each time via an internal auto-incrementing pointer; the address
never changes. `romemul` would serve `RAM[$2000]` forever — static.

There is no per-read hook on the RP side (romemul is autonomous PIO+DMA),
so the RP CPU cannot "advance a pointer between reads". Options:

1. **Dedicated data-port PIO+DMA path (recommended).** A second serve SM
   that responds only when A9–A13 == `$10`, sourced by a DMA channel with
   `read_increment = true` walking a pre-staged packet buffer. When the
   driver issues "start remote DMA read" (a ROM3 `CR` write with
   `E8390_RREAD`), the RP has already seen `RSAR`/`RCNT` writes, so it
   knows the start page and byte count; it points the data-port DMA at the
   corresponding ring bytes and arms it for that many transfers. The
   existing ROM4 romemul SM must be gated to *not* answer `$FA2000` so the
   two don't both drive the bus. This is the piece that needs PIO work and
   hardware validation.
2. **RP-serviced reads via a tight Core-1 loop** — infeasible at cartridge
   speed (a bus read is ~250 ns; the M0+ can't turn around a captured
   address into a driven data bus in time — this is the same saturation
   that sank the first IKBD ROM4-DMA-IRQ attempt, per AGENTS.md).

Option 1 is the plan. Remote-DMA **writes** (TX) go the other way: the
driver `putBUS`es frame bytes as ROM3 writes to the data port, which
`commemul` already captures — decode reg==`$10` samples as sequential
frame bytes into the TX staging buffer.

## Open problem 2 — the register window collides with the cartridge

The register-read window `$FA0000–$FA3FFF` (reg 0–31 × 512) overlaps the
16 KB cartridge image, and reg 0 read = `$FA0000` = the `$abcdef42`
cartridge magic TOS needs at every reset. Real EtherNEC is not a
cartridge (no magic, no boot code); MD/Net must be both.

Plan: the ROM4 RAM mirror is **mode-switched**. At boot it holds the
cartridge header + `main.s` (prints the MD/Net message, returns to GEM).
Once `emul.c` sees WiFi bring-up finish — and after the m68k has left the
cartridge init (it no longer reads the magic until the next reset) — the
RP repaints the ROM4 mirror as the NE register-read staging map. On a warm
ST reset (RP not power-cycled) the magic must be restored first; detecting
that reliably is a hardware-iteration item (candidate: watch the ROM4
read pattern, or a bus-idle heuristic, or accept that a warm reset needs
the SELECT-reboot). This mode-switch is why register reads are staged in
the same RAM romemul already serves, rather than a separate buffer.

## What STinG's driver requires (hard compatibility constraints)

From `NE.S`. The emulator must satisfy these exactly or probe fails.

### Probe / detect (`ei_probe1`)
1. Read `NE_RESET` (`$1F`), write it back → soft reset. Emulator sets
   `ISR |= ENISR_RESET ($80)`.
2. After a short delay, read `EN0_ISR` and require `ENISR_RESET` set, else
   "NE Reset Bit not set. Fatal".
3. `CR = $21` (NODMA+PAGE0+STOP), `DCR = $48`, clear `RCNT`, `IMR=0`,
   `ISR=$ff`, `RXCR = $20` (monitor), `TXCR = $02` (loopback).
4. Remote-DMA read of 32 bytes from ring address `$0000` (`RCNT=32`,
   `RSAR=0`, `CR=E8390_RREAD+START`), reading `NE_DATAPORT` 16× as words.
   **This is the MAC PROM read.** Each of the 6 MAC bytes appears
   *doubled* in the PROM (byte i at PROM[2i] and PROM[2i+1]); the driver
   checks `d0==d1` to confirm a 16-bit card (`pbWordLen`). PROM bytes 14
   and 15 must both be `$57` (the NE2000 signature) or the driver prints
   "No NE1000" but still proceeds. **Emulator must present a 32-byte PROM:
   `PROM[2i]=PROM[2i+1]=MAC[i]` for i<6, and `PROM[14]=PROM[15]=$57`.**
5. `CR=$20` (NODMA+START), `ISR=ENISR_RDC` ack.

### Init (`NS8390_init`)
`CR=$21`, `DCR=$48`, clear RCNT, `RXCR=$20`, `TXCR=$02`,
`TPSR=tx_start_page ($40)`, `BNRY=STARTPG=rx_start_page ($46)`,
`STOPPG=$80`, `ISR=$ff`, `IMR=0`; page 1: `PAR0–5 = MAC`, `MAR0–7=$ff`,
`CURR=rx_start_page+1`; back to page 0; then `CR=$22` (START),
`TXCR=$00` (tx on), `RXCR=$04` (broadcast+ok). Ring geometry for the
NE2000/16 KB clone: **tx start page `$40`, rx start `$46` (tx + 6 pages),
stop `$80`.** (`NESM_START_PG=$40`, `NESM_STOP_PG=$80`, `TX_PAGES=6`.)

### Transmit (`ei_start_xmit`)
`RCNT = len`, `RSAR = 0`, `RSARHI = tx_start_page`, `CR=E8390_RWRITE+START`,
then frame bytes out the data port, `CR=$20`, `ISR=RDC` ack,
`TCNT = len`, `CR=E8390_TRANS+START ($26)`. Emulator: on the `$26`
command, take the `len` bytes staged at the tx page and hand them to the
WiFi bridge; then set `ISR |= ENISR_TX ($02)` and `TSR = ENTSR_PTX ($01)`.

### Receive (polled — STinG has no cartridge IRQ)
Driven from STinG's timeslice, `ei_interrupt` polls `EN0_ISR`. On
`ENISR_RX`, `ei_receive` loops: read `EN1_CURPAG` (page 1), compare to its
`read page`; while they differ, remote-DMA-read the 4-byte 8390 header
(status, next-page, len-lo, len-hi) then the frame, and advance `BNRY`.
**Emulator delivering an RX frame must:** write the 4-byte header +
frame into the ring at `CURR`, advance `CURR` by the page count, set
`ISR |= ENISR_RX ($01)`. The header's next-page and length must be
consistent or the driver drops the frame (it validates
`rx_start_page ≤ next ≤ stop_page` and `64 ≤ len ≤ 1518`).

### Timing
Delays are `_hz_200`-based busy-waits (`ADelay`), 2–10 ms around reset —
generous. No microsecond-tight loops. Polling cadence is STinG's
timeslice (config `threading`), typically a few ms. TX timeout is 1000 ms.

## Packet bridge (Core 1)

- **RX**: cyw43 STA netif receives a frame → push into the NE ring via
  `ne2000_deliver_rx(frame, len)` (adds 8390 header, advances CURR, sets
  ISR bit). Guard against ring-full (drop, set overrun per datasheet).
- **TX**: on the transmit command, `ne2000_take_tx()` yields the staged
  frame → hand to cyw43 for 802.11 egress.
- lwIP is already linked (`pico_cyw43_arch_lwip_poll`). The bridge can use
  a raw `netif`/`cyw43_send_ethernet` path; the NE ring *is* the buffer,
  so no extra copy beyond ring↔pbuf.

## Register-file core (done)

`rp/src/ne2000.{c,h}` implements the hardware-independent DP8390 model:
command/page decode, the four register pages, remote-DMA read/write state
(RSAR/RCNT auto-increment over a 16 KB ring + 32-byte PROM), CURR/BNRY
ring management, ISR/IMR, and `deliver_rx`/`take_tx`. It has **no** RP or
cart dependencies and is covered by `tools/ne2000_test.c` (host build),
which drives the exact `ei_probe1` sequence and asserts the doubled-MAC
PROM read-back, plus a deliver-RX → ring-read round trip and a TX
round trip. Bus glue (commemul decode, ROM4 staging, the data-port PIO
path, the mode-switch) lands on top of this core in the next step.
