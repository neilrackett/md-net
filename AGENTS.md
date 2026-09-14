# AGENTS.md — MD/Net

This file is the playbook for coding agents working in this repository. It is
the single source of truth; `CLAUDE.md` is a one-line `@AGENTS.md` import, so
Claude Code loads this file too. Edit this file, not `CLAUDE.md`.

See also: `README.md` (user-facing overview), `docs/ne2000-emulation.md`
(emulation design notes), and the private `docs/epics/` folder (internal
planning notes — **not committed to the repo**; never link or cite its
content, see the rule below).

## ⛔ Never reference the epic docs in shipped material

The epic planning docs live in the private `docs/epics/` folder —
**internal planning notes that are not committed to the repo**. Do NOT
reference them — no links to `docs/epics/*.md`, and no "Epic N" /
"Story X.Y" citations — anywhere, including **this file** and **anything
that ships or is user-facing**: `README.md`, any skill, the public docs,
and code/header comments. Describe the behaviour or the code directly
instead. When you touch a comment that cites an epic/story, rephrase it.
This is a hard rule.

## What this repo is

**MD/Net** turns a SidecarTridge Multi-device (Raspberry Pi Pico W /
RP2040 in the Atari ST cartridge slot) into a **wireless NetUSBee**: it
emulates the NetUSBee's NE2000 (RTL8019AS) Ethernet controller on the
cartridge bus and bridges its traffic to the LAN over the Pico W's WiFi,
so the stock STinG EtherNEC driver (`ENEC.STX`) — and eventually
MiNTNet — drive it unmodified. Forked from `md-framebuffer-template`;
the entire A/V stack (framebuffer, audio, IKBD, demos) was stripped and
the WiFi stack (`network.c`, lwIP) restored from
`md-microfirmware-template` upstream.

Milestones: **v0.1.0** — WiFi bootstrap, `Cconws` boot banner
("MD/Net connected: <IP>"), return to GEM (done, hardware-validated).
**v0.2.0** — working ST networking: ping in both directions at 0% loss
(done, hardware-validated 2026-07-27, tagged). **v0.3.0** — the driver
installed from the cartridge and the ST configured for the user
(done, hardware-validated 2026-07-27: INSTALL.TOS writes the driver,
STinG Port Setup opens pre-filled with an ARP-probed address, ping 0%
loss both ways; the user still ticks Active once). **v0.4.0** — the
installer ships on the cartridge itself and the web works: CAB loaded
frogfind.com by hostname over WiFi on a stock ST (done,
hardware-validated 2026-07-28). **v0.5.0** — true zero-config: the
port activates itself, the installer ships next to a ready-to-go STinG
bundle, and a fresh machine goes bundle → installer → reboot → browsing
with no configuration touched at all (done, hardware-validated
2026-08-10, twice, from a clean setup). **v0.6.0** — throughput: the
RX ring, several datagrams per `send`, the 10 ms slice and the faster
copy loops, negotiated so any firmware works with any driver (stable
on hardware 2026-09-10; speed not yet measured -- see Performance
below). The self-activation mechanics:
`driver_main` activates its
own port during `load_stx()` (directly via `my_set_state`, not
`on_port`, so a port-name collision cannot redirect it), gated on the
seq-stably adopted address. Safe because the boot stub blocks until the
config is published, a WiFi failure means the mailbox magic is absent
so the driver never installs at all, and a user's saved control-panel
settings are applied later in the boot and still win — including a
saved ACTIVE=0, which STinG Port Setup's boot pass enforces with
`off_port` (verified in its source, `config/ports/stngport.c`). Routes
follow the port's address from `my_receive` (append-only, never
touching user entries): that covers both the kernel rebuilding its
table from `ROUTE.TAB` after module load — note it only rebuilds when
the file EXISTS; when missing, `routing_table()` returns `E_NODATA`
without touching the table (verified in `sting/ip.c`) — and the control
panel applying a different saved address after boot. The first serviced
slice is provably after the rebuild: the 200 Hz handler gates on
`active`, which `set_sysvars` sets only after `routing_table()`.

The minor version bumps per milestone; **every `make debug` auto-bumps
the patch** so each flashed image is identifiable. **Tagging a
milestone does not bump the minor — do it as a separate step**, editing
all four `version.txt` files (root, `target/`, `target/atarist/`,
`rp/`). Miss it and development builds keep climbing the old series, so
the banner stops saying which milestone a build belongs to (v0.2.0 was
tagged while builds ran on to v0.1.69).

## ⚠️ Branch strategy: `netusbee` (paused) vs `stx` (active)

The NE2000-emulation approach lives on the **`netusbee`** branch and is
**paused short of working ping** (~50 hardware-tested builds). Status:
probe/PROM/MAC byte-exact; register file, ring walking, BNRY/CURR all
correct; TX end-to-end over WiFi (ARP requests + ICMP verified leaving);
RX delivery + header reads verified byte-exact (`srv==b==got`); the
driver runs its PRIMARY receive path with sane lengths once headers are
written count-high-first. The single remaining defect: ARP-reply body
reads intermittently serve one stale byte (offset 3, the ARP
`hardware_space` field, serving `40` instead of `01`) — STinG's
`process_arp` rejects the reply as "funny ARP", ARP never resolves,
ping is 100% lost. IP-packet body reads of the same sizes are clean, so
it is a timing interaction specific to the header→body chained-arm
sequence, not a general corruption. A fix likely needs the data-port
serve moved fully into PIO (no CPU in the serve loop) or a
logic-analyzer capture of the exact bus interleaving. Do not burn more
build-flash-test cycles guessing: the diagnosis loop hit diminishing
returns — three "certain" root causes in a row were falsified on
hardware before the current understanding stabilized.

The **`stx`** branch is the active approach and **it works**: a custom
STinG driver (`MDNET.STX`, port name "WiFi") talking to the RP over a
purpose-built cart-bus mailbox instead of NE2000 register emulation.
Hardware-validated 2026-07-27 -- ping in both directions at 0% loss,
the handshake in perfect lockstep (`seq == ack`, `err=0`). It reuses
the WiFi bridge and everything learned about STinG (below) and drops
the timing-critical serve entirely. The driver is derived from the
EtherNEC STinG driver (`EmmanuelKasper/ethernec`: `ENESTNG.C`), which
implements the same port API (`my_send` / `my_receive` /
`my_set_state` / `my_cntrl`).

**Performance.** Measured on v0.5 (single window, one frame per
`send`/`receive` call): the bridge moved **~21 KB/s each way**
(1408-byte ping flood: 15 frames/s each way, queue full, drops
climbing). That was not saturation of the bus but of the handshake:
STinG's `poll_ports` (`sting/kernel.c`, driven from `thread.s` every
`fraction` ticks of the 200 Hz timer, `fraction = THREADING/5`, 50 ms
in the stock config) calls each port's `receive` once and `send` once
per slice, and the driver moved one frame per call — 20 frames/s by
arithmetic. The current design lifts it in four places, none of which
has been measured on hardware yet: the RX ring (eight slots, drained up
to four per call), several datagrams per `send` (4.6 KB budget), the
driver holding STinG's slice at 10 ms via `set_sysvars` while active
(re-applied from `my_receive`, because `sting/install.c` calls
`set_sysvars(1, THREADING/5)` after `load_stx()` and STinG Port
Setup's boot pass sets its saved value too -- a one-off at activation
is undone before the first slice), and long-word / four-instruction
copy loops. The RP also filters what
it forwards (ARP, IP for the ST; not its own or multicast), since every
frame costs the ST a slot. Expect the ST's CPU to be the limit now,
somewhere in the tens of KB/s; measure before quoting a number.

FTP on v0.5 moved **~2 KB/s**, one 536-byte segment per ~266 ms, with
the queue empty and nothing dropped. The slice lock-step does not
explain that (it allows ~5 KB/s with one segment in flight), so it is
something in STinG's TCP or the client, and a packet capture of the
data connection is the next step if it persists. Two facts from
STinG's source for that investigation: an outgoing datagram lands on
the Internal port's receive queue and only reaches the WiFi port on the
*next* slice (so an ACK leaves one to two slices after its data
arrived), and `timer_work` skips a connection whose semaphore the
application holds.

Four theories were falsified against hardware, each cheap to re-invent:
(1) RX queue overflow causing retransmits — `drop` is flat throughout
FTP; (2) a multi-slot RX ring would help — FTP never approaches the
ceiling; (3) per-frame UART logging was the latency — production build
A/B showed no change; (4) STinG's `THREADING` service cadence was the
limit — 50 vs 10 changed nothing across 100 KB and 1 MB transfers,
after a power cycle, despite the mechanism being confirmed in STinG's
source (`fraction` divides the 200 Hz timer, `THREADING/5`). Going
further needs instrumentation inside STinG, not more firmware changes.

Other v0.5 measurements: round-trip time ~60-210 ms (mean ~105 ms),
which is two 50 ms slices plus phase (the reply is picked up at one
slice; the ICMP path runs in the same slice, but a TCP ACK would leave
at the next). WiFi power save is not a factor: `WIFI_POWER` defaults
to 0, which is the CYW43 "disabled" value.

**Compatibility rule for the mailbox.** Any firmware must keep working
with any driver: a SidecarT can be reflashed without the ST being
touched, and a dead network is a support call. So `MB_PROTO_VERSION`
stays 1, new firmware features are advertised in `MB_CAPS`, and the
driver opts in through the version byte it sends with
`MBC_DRIVER_HELLO` (1 = single window, 2 = ring). The single window
at `$5000` and its fields are never moved. The host test
(`tools/mailbox_test.c`) covers both modes and the switch between them.

## Build

```bash
make debug    # debug build, UART logging on, bumps patch version
make build    # production build, UART off, SKIP_VERSION_BUMP=1
```

Every build now also produces **`dist/MDNET.STX`** — the custom STinG
driver (see `target/atarist/stx/`), compiled with the
`m68k-atari-mint-gcc` cross toolchain inside the same atarist-toolkit
Docker image. The user installs it in the ST's `STING` folder (with
`ENEC.STX` removed/disabled) and selects the "WiFi" port in STinG
Port Setup.

Requirements: ARM GNU toolchain (`PICO_TOOLCHAIN_PATH`),
`atarist-toolkit-docker` (`stcmd`) for the m68k cartridge stub — **Docker
must be running** — and the pinned submodules (pico-sdk 2.2.0 etc.,
re-pinned by the build).

### Releasing

The build names the firmware `<uuid>-<version>.uf2` (deliberately — the
version in the filename is how a flashed image is identified) but the
descriptor is always `<uuid>.json`. **The SidecarT only lists an app
when the two filenames match**, so the UF2 must be renamed to plain
`<uuid>.uf2` when it is attached to a release or copied to `/apps`.
Upload the build output unrenamed and the app simply never appears in
the menu, with nothing to say why.

Releases are rolling and automatic: every push to `main` runs
`.github/workflows/release.yml`, which builds with `make`, tags the
version from `version.txt` (leaving an existing tag alone), moves the
`latest` tag, and replaces the UF2 (renamed), the JSON and
`sting-for-mdnet.zip` on the existing "latest" release. The UUID comes
from the `APP_UUID_KEY` repository secret. Pull requests run
`pr.yml`, the same build without the publish. Both use the
`neilrackett/atarist-toolkit-docker-x86_64:latest` image (the only
x86_64 tag published), so CI can drift from the `1.2.1` arm64 image
local builds pin in `build.sh`.

### ⚠️ Build-system lessons (learned the hard way)

- **`build.sh` has `set -e` — keep it.** It once didn't: a compile error
  in the RP step was silently survived, the script copied the previous
  (stale) UF2 from `rp/dist/` into `dist/` under the new version name and
  printed "Build completed successfully". Five "different" builds shipped
  identical stale firmware and days of hardware tests were invalidated.
- **Verify every build by decoding the UF2 payload**, not by trusting the
  filename: parse the UF2 blocks, byte-swap 16-bit words, and check the
  embedded banner string reads the expected `MD/Net vX.Y.Z`. The banner
  shown on the ST at boot is the same string — ask for it on every
  hardware test.
- If the m68k step fails (Docker down), the previous `BOOT.BIN` /
  `target_firmware.h` silently survives; the banner version is the tell.
- `dist/` is wiped (`rm -rf`) at the start of every build — only the
  newest UF2 exists.
- Never grep build logs through filters that exclude `pico-sdk` paths
  when hunting errors — compile errors triggered *inside* SDK headers
  carry SDK paths and vanish from the filtered view.
- Host-side tests: `make test` (the `cc` line is in the Makefile, with
  the `MAILBOX_HOST_TEST` guard macro) — run after any `mailbox.c`
  change. CI runs it first, before either toolchain. (The netusbee
  branch keeps the NE2000 model tests.)

## Architecture

### Two-target split
- `target/atarist/src/main.s` — tiny m68k cartridge stub: prints the
  versioned banner (from generated `version.inc`), polls
  `MDNET_STATUS` (`$FA4010`), prints `MDNET_MSG` (`$FA4100`), returns to
  GEM. No resident code. Built via `stcmd`, embedded into the RP firmware
  as `target_firmware.h` (16-bit words, cart byte order).
- `rp/src/` — everything else. `emul.c` boots WiFi and enters the Core-0
  loop; `mdnet.c` + `ne2000.c` + `dataport.c/.pio` implement the NE2000.

### Cartridge-bus interface (EtherNEC conventions)
- The ST **reads** NE2000 registers from ROM4: `$FA0000 + reg<<9`;
  register values are staged in the ROM4 RAM mirror at `(reg<<9)^1`
  (the cart bus swaps bytes within each 16-bit word).
- The ST **writes** registers by "dummy reads" in ROM3:
  `$FB0000 + reg<<9 + data<<1` (reg in A9–A13, data in A1–A8). Decode:
  `reg=(sample>>9)&0x1F`, `data=(sample>>1)&0xFF`.
- Capture paths: `commemul.c` (32 KB DMA ring, all ROM3 samples, ~1 µs+
  latency, lossless) and two `dataport.pio` taps on pio1 — the ROM4 tap
  (data-port + register reads) and the ROM3 "crtap" (low-latency
  register-write visibility). Tap events **push at bus-cycle END**
  (sample mid-cycle, explicit blocking `push`) — cycle-start delivery
  let re-staging race the same cycle's romemul fetch, and `push noblock`
  dropped events on a full FIFO (blocking = delayed, never lost).

### ⚠️ THE cardinal rule: true bus order

The bus is one serialized stream; our capture is two FIFOs (reads vs
writes) with no cross-FIFO ordering. **Events must be applied in bus
order**, enforced by two invariants (in `dataport_serve_burst` and
`crtap_service`):
1. **A pending write preempts read serving** — the burst exits when the
   crtap FIFO is non-empty, before popping more read events.
2. **Every write is a read barrier** — before a ROM3 write is applied,
   all queued read events are served under the *current* stream cursor
   (valid because cycle-end pushes guarantee all pre-write reads are
   already queued).

Violating this serves reads against the wrong stream when the driver
chains remote-DMA arms back-to-back (its normal ring-walking pattern) —
producing "impossible" corruption that mimics byte-order bugs.

### Data-port serve (the timing-critical core)
- The data port is one register ($10) but four bus addresses
  (`$FA2000/2/4/6` → RAM slots `$2001/3/5/7`). Plain polls (`move.b`)
  always hit slot 0 (~1–2.5 µs apart); the driver's ARP body copy uses
  **MOVEP.L** — four ascending-address reads in back-to-back ~250 ns
  cycles that no reactive per-event serve can feed.
- Therefore the serve stages a **4-byte window** (slot k = stream byte
  base+k), consumption is slot-aware (`(addr>>1)&3`), and stream
  advance uses the **group-delta rule**: ascending slot = movep
  continuation (advance by delta), repeated/lower slot = new access
  group (advance slot+1). Consumed slots are refilled immediately
  (always safe — bursts read ascending); the full window restages
  deferred (~1 µs of quiet) or at stream boundaries.
- RSAR/RCNT (page 0) are **crtap-owned**: tracked in the write stream in
  true order, window prestaged at the RSARHI write (µs before the arm).
  `on_rom3_sample` (commemul) must **skip** page-0 regs $08–$0B — its
  delayed replay would rewind an advanced stream — and the skip gate
  must use the **in-stream page** (`chip->cr` bits), not the crtap-side
  page tracker.
- On a read arm (CR=$0A via crtap), enter `dataport_serve_burst`
  **immediately** — any preamble work (draining commemul, diagnostics)
  delayed burst entry past the driver's first reads and corrupted them.

### NE2000 model (`ne2000.c` — hardware-independent, host-tested)
- PROM = doubled MAC + `$57` signature. **The driver chooses its whole
  personality from probe quality**: clean doubled PROM → NE2000 layout
  (TX page `$40`, ring `$46–$60`); any corruption → NE1000 8-bit
  fallback (TX `$20`, ring `$26+`). Both layouts are supported (buffer
  window `$2000–$7FFF`), but a corrupt PROM also corrupts the MAC the
  driver installs — and a wrong source MAC means every LAN reply is
  addressed to a MAC the CYW43 filter rejects: **TX appears fine,
  nothing ever comes back.**
- 8390 rx header = `[status, next_page, count_lo, count_hi]`
  (datasheet order; count = frame+CRC, excluding the 4-byte header).
  A high-first swap once *appeared* correct — it was compensating for
  the bus-order bug above. Don't repeat that detour.
- TX upload: the driver arms remote-write with inconsistent RSAR scales;
  frames are captured **positionally** into `txstage[]` (armed at CR
  RWRITE, filled per data-port write) — immune to address-scale quirks.
- Ring delivery validates PSTART/PSTOP/CURR before touching `mem[]`
  (driver init transients), wraps at the rx ring (not the buffer edge),
  and has **no delivery pacing** (two historical deadlocks; the
  ring-full `ISR_OVER` check is the real 8390 semantics — and note the
  driver's overrun handler does a STOP/loopback/drain dance).

### Core split & bridging
- **Core 1** owns the chip + all bus servicing: hot/cold loop (taps every
  lap; commemul drain, register restage, RX delivery, TX handoff every
  64th lap). The entire hot path is RAM-resident (`__not_in_flash_func`
  / `NE2000_TIME_CRITICAL` — which must include `pico.h`, **not**
  `pico/platform.h`, which `#error`s). Frame delivery yields to the taps
  every 64 bytes (`ne2000_set_yield`).
- **Core 0** owns WiFi/lwIP. The RX tap wraps the STA netif input:
  queue for Core 1, then chain to lwIP. SPSC queues both directions; RX
  consume is zero-copy (peek/advance). Shared-MAC *and* shared-address
  design: STinG uses the Pico's own CYW43 MAC and the address lwIP
  leased, so lwIP is compiled mute — `LWIP_TCP` and `LWIP_ICMP` are 0,
  and the RX filter keeps only DHCP replies (UDP port 68) for lwIP,
  handing the ST everything else. **Never re-enable TCP or ICMP, or put
  a server on the Pico, without solving that first**: lwIP would RST the
  ST's inbound connections and answer its pings twice.
- **UART logging blocks Core 0** (~87 µs/char at 115200): a long debug
  line stalls WiFi servicing ~20 ms. Keep diagnostics terse, filter idle
  spam, or they perturb the very traffic under test (production `make
  build` removes them entirely — useful as an observer-effect A/B).

### The driver (STinG `ENEC.STX`)
Reference source: `EmmanuelKasper/ethernec` (`SRC/NE.S`, `NESTNG.S`,
`8390.I`); the shipped binary (MD5 `98db4e73…`, 5815 B) is a
`BUGGY_HW`+debug build but its receive checks match the source:
status&`$5F`==`$01`, next in [PSTART,PSTOP], count in [64,1518] → else a
shifted-header recovery path that can mass-resync (BNRY=CURR−1) in ways
that *look* like normal consumption. `rtrvPckt` type-peeks every
accepted packet (`0806`→ARP path with a **50-byte ARP buffer cap**;
`0800`→IP path that aborts early for non-local dst — short body reads on
broadcasts are normal). Disassembly recipe: GEMDOS header `0x1C`,
capstone M68K (verify against raw bytes — skipdata desyncs); the ROM3
write macro composes `(reg<<8|data)` then doubles it.

## Debugging methodology (what actually worked)

- **Never trust an intended-value log**: capture what was *actually
  delivered* (`got=` slot content at event time) alongside memory (`b=`)
  and bookkeeping (`srv=`). All three agreeing is the only "serve OK".
- The unified R/W event trace (every register read with served value +
  every write with data, in order) is the driver's complete sensory
  input — decisive when driver behaviour contradicts its source.
- Diagnostics print once per boot (`PROM served [...]` is the canary:
  byte-exact `28 28 cd cd … 57 57` or the boot is compromised) and every
  2 s (`c1=` heartbeat distinguishes a hung Core 1 from a silent bus).
- Hardware-test ritual: flash → power-cycle → **confirm the on-screen
  banner version** → capture UART from power-on → `PING 192.168.1.1`
  from the ST (never the ST's own address — STinG answers that
  internally).
- **Every build is reviewed by a different model than wrote it** before
  it goes to hardware (Opus writes → Fable reviews, and vice versa; use
  the Agent tool's model override). Self-review repeatedly missed
  defects that cost hardware sessions; the first cross-model pass found
  three real bugs, and the second found a `Supexec` omission that would
  have bombed the ST before the driver installed. Hardware cycles are
  the scarce resource, not tokens. Ask the reviewer to verify each
  finding against the code and the reference sources, and to label
  findings CONFIRMED or SPECULATIVE.
- When deduction and measurement deadlock, run a cheap intervention
  experiment (e.g. the padding experiment) — its side effects often
  reveal the real mechanism.

## ST-side configuration (known-good)

- STinG port IP lives in **STinG Port Setup** (saved to `STING.PRT` —
  check the hex: a mis-saved netmask `fffcff00` cost a day).
  `ROUTE.TAB` is four whitespace-separated fields — network, mask,
  port, gateway — and `INSTALL.TOS` now writes it, so hand-editing is
  the fallback. Two things verified in STinG's own parser
  (`sting/ip.c`, `fetch_line`): fields may be separated by **spaces or
  tabs** (the "must be TAB-separated" lore is stricter than reality,
  though we write tabs since every version takes them), and the port
  name is matched with `strcmp` — so it must read exactly `WiFi`.
  Lines not starting with a digit are skipped, so comments are safe.
- `DEFAULT.CFG` holds `NAMESERVER`, which `INSTALL.TOS` fills in when it
  is unset. Its rules also come from STinG's own loader
  (`sting/install.c`), not guesswork: a setting is recognised only when
  its name starts at **column 0** (indented lines are ignored, which is
  also why the word inside a `#` comment cannot match), names compare
  **case-insensitively** (`toupper`), and `setvstr` replaces — so **the
  last assignment wins**, which is what makes appending at the end of
  the file take effect.
- Only one Ethernet driver active: `INSTALL.TOS` renames `ENEC.STX` to
  `ENEC.ST_`, the same convention STinG users already use.
- To pin the ST's address, reserve it in the router's DHCP settings
  against the cartridge's MAC: the ST uses the cartridge's lease.
- Warm-reset caveat: after `mdnet_activate()` repaints the register map,
  the cartridge magic is gone — a warm ST reset boots without the
  banner (networking still works); power-cycle the SidecarT to see it.
- Boot-time SELECT→Booster (in `main.c`) works; runtime SELECT reset
  callbacks are deliberately not wired (spurious edges killed sessions).

## Known platform quirks (not bugs to chase)

- **Returning from a cartridge entry throws a bus error (two bombs).**
  Running `INSTALL.TOS` from the cartridge works and writes everything
  correctly; the bombs come as it returns to the desktop, and nothing
  afterwards is affected. It is not ours: MD/JS's cartridge demo does
  exactly the same, and removing the `Mfree` from the exit path changed
  nothing, so it is neither memory ownership nor specific to this
  installer. Do not spend hardware sessions on it without new evidence.
- Cartridge code cannot have writable data — the window is served
  read-only, so writes vanish silently. Anything run from the cartridge
  must keep its state in RAM it was given (see `install_cart_run` in
  `target/atarist/src/main.s`, and the `WORK` struct threaded through
  `install.c`). The Makefile fails the build if the cartridge installer
  ever needs a relocation, which is what would let a static creep back.

## Editing guardrails

- **Never modify** `pico-sdk/`, `pico-extras/`, or `fatfs-sdk/`
  (pinned submodules; FatFs config override lives at `rp/src/ff/ffconf.h`).
- Don't add features to `main.c` — start in `emul.c` / `mdnet.c`.
- Match the existing C style (`.clang-format` / `.clang-tidy`).
- **Call ST software by the name users see, not its filename**: it is
  **STinG Port Setup**, not `STNGPORT.CPX`. Applies to anything a user
  reads (the installer, `MDNET.TXT`, `README.md`) and to comments, so
  the two never drift apart. Data files users actually type or rename
  (`STING.PRT`, `ROUTE.TAB`, `MDNET.STX`) keep their filenames.
- **New or significantly modified files carry a GPL header**:
  `Copyright (C) <year> Neil Rackett` + `SPDX-License-Identifier:
  GPL-3.0-or-later` (see `tools/bump_version.sh` for the shell form).
  Do **not** add it to third-party files — the STinG SDK headers and
  the EtherNEC reference sources in `target/atarist/stx/` keep their
  own. `target/atarist/stx/mdnet.c` is derived from Redelberger's
  GPL'd `ENESTNG.C`; its header records the provenance and why
  GPL-3.0-or-later applies (the upstream names no GPL version, and
  GPLv2 §9 lets the recipient choose one).

---

## Working style

These behavioral guidelines bias toward caution over speed. For trivial tasks, use judgment.

### 1. Think before coding

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### 2. Simplicity first

Minimum code that solves the problem. Nothing speculative.
- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask: "Would a senior engineer say this is overcomplicated?" If yes, simplify.
In this repo specifically: reactive timing patches accrete — when a
serve/timing mechanism grows a fifth special case, stop patching and
re-derive the invariant it should enforce.

### 3. Surgical changes

Touch only what you must. Clean up only your own mess.
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it — don't delete it.
- When your changes orphan an import/variable/function, remove it. Don't remove pre-existing dead code unless asked.

The test: every changed line should trace directly to the user's request.

### 4. Goal-driven execution

Define success criteria. Loop until verified.
- For this repo: "fixed" means **verified on hardware** (banner-checked
  build, UART evidence), not "builds and the theory is sound". Host
  tests gate model changes; only the ST proves serve/timing changes.

### 5. No AI attribution

Never add AI-tool attribution to commits, PR descriptions, code comments,
docs, or any other artifact. This means **no**:
- "Generated with Claude Code", "Co-authored by Claude", "Made with ChatGPT",
  or any similar phrasing.
- `Co-Authored-By: Claude …`, `Co-Authored-By: ChatGPT …`, or any other
  AI co-author trailer.
- "AI-assisted", "written with the help of an LLM", etc., as comments or
  changelog entries.

Write the message as the human author. Do not mention AI tools used to
produce the work.
