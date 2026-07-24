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
| `$5000` | `MB_RX_BUF` | 1600 B | The published RX frame, contiguous. |

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

## ROM3 command encoding (ST writes, RP captures via commemul)

Same shape as the proven EtherNEC encoding: address bits A9-A13 select
a channel, A1-A8 carry a data byte. `chan = (addr>>9)&0x1F`,
`data = (addr>>1)&0xFF`.

| Chan | Name | Meaning of data byte |
| --- | --- | --- |
| `$00` | `MBC_NOP` | ignored (bus noise guard). |
| `$01` | `MBC_RX_ACK` | low byte of the consumed `MB_RX_SEQ` — RP may publish the next frame. |
| `$02` | `MBC_TX_START` | TX length low byte. |
| `$03` | `MBC_TX_LEN_HI` | TX length high byte (follows TX_START). |
| `$04` | `MBC_TX_DATA` | next TX payload byte (streamed len times). |
| `$05` | `MBC_TX_COMMIT` | low byte of a TX sequence number; RP validates byte count and bridges the frame. |
| `$06` | `MBC_DRIVER_HELLO` | driver version byte; announces install (RP logs it). |
| `$07` | `MBC_DRIVER_BYE` | driver uninstalling. |

TX flow: `TX_START(len_lo)` → `TX_LEN_HI(len_hi)` → len × `TX_DATA(b)`
→ `TX_COMMIT(seq)`. commemul's ring preserves order and loses nothing
(verified: this is exactly how EtherNEC TX worked flawlessly throughout
the netusbee effort). The RP sets `MB_TX_ACK = seq` when the frame is
away; the driver need not wait for it except for back-pressure.

Byte cost: one ROM3 bus read (~500 ns) per TX byte — ~1500 byte frame
≈ 0.8 ms. RX copy is ordinary memory-read speed. Both comfortably beat
the serial-era throughput STinG was designed around.

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
and block-copies from `MB_RX_BUF`; `my_send` streams the frame through
the ROM3 command channel. ARP stays in the driver (reused from the
EtherNEC source) — the RP bridges raw Ethernet frames exactly as on
the netusbee branch, so lwIP-side behaviour is unchanged.
