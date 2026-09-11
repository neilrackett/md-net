# MD/Net mailbox protocol (`stx` branch)

The cart-bus contract between the RP2040 firmware and the custom STinG
driver (`MDNET.STX`). It replaces the NE2000 register emulation with a
design built exclusively on the two bus primitives that proved
bulletproof across the `netusbee` branch's ~50 hardware builds:

1. **ST reads of ROM4** (`$FA0000-$FAFFFF`): served from RP RAM by
   romemul with no RP CPU involvement. Byte-swapped per 16-bit word
   (m68k byte `k` = RP byte `k^1`). Rock solid at any read pace,
   any access pattern (movem/movep/byte loops) — *as long as the RAM
   contents are static while the ST reads*.
2. **ST "writes" via ROM3 dummy reads** (`$FB0000-$FBFFFF`): the read
   address carries the payload; the commemul PIO+DMA ring captures every
   access losslessly and in order, drained at leisure.

**The one design rule:** RP-side buffer contents change only *between*
handshake phases, signalled by sequence counters — never while the ST
might be reading them. No byte streams served at fixed addresses, no
CPU-in-the-loop serving, no timing dependence at all.

## ROM4 window layout (ST reads, RP writes)

All offsets relative to `$FA0000`. All multi-byte fields are m68k
big-endian, written by the RP with the `k^1` swap.

| Offset | Name | Size | Purpose |
| --- | --- | --- | --- |
| `$4010` | `MB_STATUS` | 4 B | Link status: 0=boot, 1=connecting, 2=up, 3=failed (existing MDNET_STATUS slot). |
| `$4020` | `MB_PROTO_MAGIC` | 4 B | `'MDNB'` — driver checks before installing. |
| `$4024` | `MB_PROTO_VER` | 2 B | Protocol version, currently 1. |
| `$4026` | `MB_MAC` | 6 B | The shared STA MAC. |
| `$4030` | `MB_CFG_SEQ` | 2 B | Increments when the config block below changes. |
| `$4032` | `MB_CFG_IP` | 4 B | ST's IP (RP-acquired DHCP lease, or static). |
| `$4036` | `MB_CFG_MASK` | 4 B | Netmask. |
| `$403A` | `MB_CFG_GW` | 4 B | Gateway. |
| `$403E` | `MB_CFG_DNS` | 4 B | DNS server. |
| `$4044` | `MB_RX_SEQ` | 2 B | Increments when a new RX frame is published. |
| `$4046` | `MB_RX_LEN` | 2 B | Length of the published RX frame (0 = none). |
| `$4048` | `MB_TX_ACK` | 2 B | Echoes the last committed TX sequence (flow control). |
| `$404A` | `MB_RX_CREDITS` | 2 B | RX frames queued on the RP beyond the published one (diagnostic). |
| `$404C` | `MB_CAPS` | 2 B | Firmware capabilities. Bit 0: the RX ring below exists. Zero on firmware that predates it. |
| `$404E` | `MB_RXR_SEQ` | 2 B | Ring: sequence of the newest published frame. |
| `$4050` | `MB_RXR_LEN` | 8 × 2 B | Ring: length of the frame in each slot. |
| `$5000` | `MB_RX_BUF` | 1600 B | The published RX frame, contiguous (single-window mode). |
| `$6000` | payload | ~12 KB | The driver and its notes, read by `INSTALL.TOS`. |
| `$C000` | `MB_RXR_BUF` | 8 × 2048 B | Ring: slot *i* holds the frame whose sequence is *i* mod 8. |

Publication order (RP side): frame bytes → `MB_RX_LEN` → memory barrier
→ `MB_RX_SEQ`++. The driver polls `MB_RX_SEQ`; on change it reads len,
copies the frame with an ordinary ascending loop (every address read
once — romemul-perfect), then ACKs. The RP publishes the next frame
only after the ACK, so the buffer is never rewritten under a reader.
That invariant is enforced inside `mailbox_publish_next()` itself, not
by its caller — it is the one property the whole timing-free design
rests on.

Two details that are load-bearing rather than incidental:

- **16-bit fields are published with a single store.** A little-endian
  16-bit write at an even RP offset is presented to the m68k as the
  correct big-endian word, atomically. Two byte writes would tear: a
  sequence crossing 255→256 could be read as 511, whose low byte then
  ACKs a sequence the RP never published — wedging the handshake for
  good.
- **`MBC_DRIVER_HELLO` resyncs the RX handshake.** The RP starts
  bridging LAN broadcasts the moment WiFi is up, long before the user
  reaches STinG, so a frame is normally published (and left unacked)
  before the driver exists. Without the reset on hello, that stranded
  publication blocks every later one and RX is dead for the entire
  session. `MBC_DRIVER_BYE` does the same on the way out.

## RX ring (driver version 2)

One frame per publish-and-ack was the throughput ceiling: STinG calls a
port's `receive` once per timeslice, so the single window moved one
frame per slice, whatever the slice length. The ring keeps up to eight
frames published at once.

Negotiation is the driver's choice, because the driver is the side that
cannot be updated in the field: a cartridge can be reflashed without
anyone touching the ST. The RP advertises the ring in `MB_CAPS`; a
driver that reads it sends `MBC_DRIVER_HELLO` with version byte 2, and
only then does the RP switch to ring mode. Every other pairing gets the
single window, unchanged:

| Firmware | Driver v1 | Driver v2 |
| --- | --- | --- |
| without `MB_CAPS` | single window | single window (`MB_CAPS` reads 0) |
| with the ring | single window (HELLO says 1) | ring |

`MBC_DRIVER_BYE` returns the RP to single-window mode, so whatever
loads next starts from the v1 contract.

Publication: frame *s* goes into slot *s* mod 8, its length into
`MB_RXR_LEN[slot]`, then a memory barrier, then `MB_RXR_SEQ = s`. The
driver reads `MB_RXR_SEQ`, consumes every sequence after the last one it
handled (up to its per-slice budget), and acks once with the low byte of
the last sequence consumed. The ack is cumulative; with at most eight
frames outstanding the byte is unambiguous, and an ack that claims more
than is outstanding is ignored. The sequence wraps freely through 0.

The invariant is the same as the window's, held per slot: the RP never
rewrites a slot until the frame in it has been acked, so whatever the
ST is copying is static under it. A gap of more than eight between
`MB_RXR_SEQ` and the driver's count cannot come from the RP; the driver
treats it as the two sides having lost each other, skips to the newest
sequence and acks it.

## ROM3 command encoding (ST writes, RP captures via commemul)

Same shape as the proven EtherNEC encoding: address bits A9-A13 select
a channel, A1-A8 carry a data byte. `chan = (addr>>9)&0x1F`,
`data = (addr>>1)&0xFF`.

| Chan | Name | Meaning of data byte |
| --- | --- | --- |
| `$00` | `MBC_NOP` | ignored (bus noise guard). |
| `$01` | `MBC_RX_ACK` | Single window: low byte of the consumed `MB_RX_SEQ` — RP may publish the next frame. Ring: low byte of the last sequence consumed, cumulative. |
| `$02` | `MBC_TX_START` | TX length low byte. |
| `$03` | `MBC_TX_LEN_HI` | TX length high byte (follows TX_START). |
| `$04` | `MBC_TX_DATA` | next TX payload byte (streamed len times). |
| `$05` | `MBC_TX_COMMIT` | low byte of a TX sequence number; RP validates byte count and bridges the frame. |
| `$06` | `MBC_DRIVER_HELLO` | driver version byte; announces install and resyncs RX. 1 = single window, 2 or more = ring. The RP takes it as the mode without cross-checking `MB_CAPS`, so the driver sends what it will actually do. |
| `$07` | `MBC_DRIVER_BYE` | driver uninstalling. |

TX flow: `TX_START(len_lo)` → `TX_LEN_HI(len_hi)` → len × `TX_DATA(b)`
→ `TX_COMMIT(seq)`. commemul's ring preserves order and loses nothing
(verified: this is exactly how EtherNEC TX worked flawlessly throughout
the netusbee effort). The RP sets `MB_TX_ACK = seq` when the frame is
away; the driver need not wait for it except for back-pressure. The
driver streams several frames back to back in one `send` call (about
3.1 KB per call); the 32 KB ring holds ten full frames (one 16-bit
sample per byte) and is drained at least every millisecond, so it
never fills.

Byte cost, on the ST: the TX loop is four instructions per byte, about
4 µs, so a full frame costs ~6 ms; the RX copy uses long moves, ~1 ms
per full frame. Both are paid by the 68000, which is what sets the
bulk rate now that frames are no longer rationed per timeslice.

## What the RP forwards

Every frame handed over costs the ST a service slot whether it wants
the frame or not, so the RP filters first: ARP always; IP only when it
is not addressed to the RP's own address (lwIP's traffic) and not
multicast (STinG has none); nothing else (IPv6, LLDP, and so on). The
driver still applies its own IP-address check on what arrives.

## DHCP

The RP holds its own lease (its lwIP STA address) and additionally
acquires the ST's config: initially the same subnet derived statically,
later a true second lease via lwIP's DHCP on a secondary identity.
Whatever the source, the result lands in the `MB_CFG_*` block and
`MDNET.STX` applies it at `set_state(ACTIVE)` time, so the ST needs no
manual STinG IP configuration.

## m68k driver (`MDNET.STX`)

A STinG port driver, port name **"WiFi"**, modeled on the EtherNEC
STinG driver's structure (`my_send` / `my_receive` / `my_set_state` /
`my_cntrl`, installed via the STinG cookie handshake). Differences:
no NE2000 probe, no ring arithmetic — `my_receive` polls `MB_RX_SEQ`
and block-copies from `MB_RX_BUF`, or drains the ring; `my_send`
streams frames through the ROM3 command channel. ARP stays in the
driver (reused from the EtherNEC source) — the RP bridges raw Ethernet
frames exactly as on the netusbee branch, so lwIP-side behaviour is
unchanged.

While the port is active the driver also keeps STinG's timeslice at
its shortest (`set_sysvars`, THREADING = 10) unless it is already
shorter, and restores the previous value when the port goes inactive.
It is re-applied from `my_receive` on every slice because STinG's
loader and STinG Port Setup both set the slice after the driver has
loaded. Frames per slice are no longer the limit, but the slice still
sets how long an inbound segment waits for its ACK to leave.

If the cartridge restarts under a live driver it comes back in
single-window mode with a fresh ring sequence. The driver notices
either sign (the single window's sequence moving, or an impossible gap
in the ring sequence) and repeats the HELLO handshake, but only once
the magic is back: while the cartridge boots the window reads as
zeros, and `MB_CAPS` is staged before the magic so the magic vouches
for it. Should an ack ever be lost, the ring would stall with frames
waiting on the RP (`MB_RX_CREDITS` non-zero) and nothing new
published; the driver re-sends its ack after three such slices, which
is harmless when nothing was lost.
