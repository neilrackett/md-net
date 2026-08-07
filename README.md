# MD/Net: Ethernet over WiFi for Atari ST

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://neilrackett/atarist)

## Introduction

Welcome to MD/Net: Wireless Internet for your Atari ST!

MD/Net turns your SidecarT into a wireless network adapter for your Atari ST, bridging the cartridge port to your WiFi network. It reuses the WiFi credentials you already gave your SidecarT, so there is nothing extra to configure — no wires, no soldering.

Networking on the ST is provided by **[STinG](https://github.com/th-otto/STinG)** under plain TOS, driven by MD/Net's own port driver, `MDNET.STX`, which appears in STinG as a port named "WiFi". The driver and the cartridge talk over a small purpose-built protocol described in [docs/mailbox-protocol.md](docs/mailbox-protocol.md).

An earlier approach emulated the [NetUSBee](https://hardware.atari.org/netusbee/netus.htm)'s NE2000 Ethernet controller so that stock drivers would work unmodified. That got as far as a working transmit path and byte-exact receive, but the remote-DMA data port proved too timing-critical to serve reliably from the cartridge bus. It is preserved and documented on the `netusbee` branch. Supporting MiNTNet / MagiCNet is a possible future direction.

> ### ⚠️ Work in progress
>
> **MD/Net works.** An Atari ST browses the web over WiFi through a
> SidecarTridge Multi-device, with no wires and no soldering — CAB
> loading a site by hostname, on a stock 68000. Everything needed is on
> the cartridge: open it from the desktop, run `INSTALL.TOS`, and the
> driver, IP address, netmask, routing and nameserver are all set up
> for you. Hardware-validated 2026-07-28.
>
> It gets there with a **custom STinG driver** (`MDNET.STX`, which
> appears as a port named "WiFi") talking to the cartridge over a
> purpose-built mailbox protocol — not by emulating a NetUSBee. The
> NE2000-emulation approach is preserved, documented, and paused on the
> `netusbee` branch; see [docs/mailbox-protocol.md](docs/mailbox-protocol.md)
> for the design that replaced it and why.
>
> **What works today:**
> - WiFi bootstrap: the ST prints `MD/Net connected: <its own IP>` at
>   boot, then continues to GEM.
> - `INSTALL.TOS` installs the driver straight from the cartridge, so
>   the driver always matches the firmware running on it.
> - The cartridge picks a free address for the ST out of its own subnet
>   (checked by ARP probing), and the driver adopts it and installs the
>   matching routes. `ROUTE.TAB` and `NAMESERVER` are written for you.
>   Anything you set yourself always wins.
> - ARP, IP and ICMP in both directions at 0% loss.
>
> **Known limits:**
> - Throughput is capped at about **20 frames per second each way**.
>   STinG services the driver roughly every 50 ms and the driver hands
>   over one frame per service, which also puts round-trip times at
>   ~60–210 ms. Lifting that is the next piece of work; until then,
>   expect transfers to feel slow.
> - TCP applications (FTP, web browsing) are not yet exercised.
> - MiNTNet / MagiCNet are not supported — STinG only.

## Hardware requirements

- [SidecarTridge Multi-device](https://sidecartridge.com) with a **Raspberry Pi Pico W** (WiFi is required)
- Atari ST, STE, MegaST, or MegaSTE
- Raspberry Pi Debug Probe or Picoprobe for flashing/debugging (optional but recommended for development)

## Installation

### 1. The cartridge

1. Download the latest files from the [releases page](https://github.com/neilrackett/md-net/releases).
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. In the Booster web interface, make sure you're connected to your WiFi, so MD/Net can use the same information to connect.
4. On the Booster screen, press ESC for the app list and select the MD/Net app.
5. To return to Booster, turn on your ST while holding the SELECT button on your SidecarT.

You'll know it's working because you'll see `MD/Net: connecting...` followed by `MD/Net connected: <your ST's IP>` on your ST's screen as it boots, before the GEM desktop appears.

### 2. The ST

You need [STinG](https://hardware.atari.org/files/sfl.zip) installed first.

Copy **`INSTALL.TOS`** to your ST and run it. That's all: it takes the
driver and its notes out of the cartridge, writes them into your `STING`
folder, sets up `ROUTE.TAB` and `NAMESERVER`, and deactivates any
EtherNEC driver it finds. Reboot, and the "WiFi" port is ready.

Because the driver comes from the cartridge rather than alongside the
installer, it always matches the firmware you're running — there's no
way to end up with a mismatched pair.

Nothing you've configured yourself is overwritten: an address set in
STinG Port Setup, a `ROUTE.TAB` that already mentions the WiFi port, or
a nameserver you chose are all left alone.

### 3. Get browsing

Any STinG-compatible software works. For the web, grab
**[LowWire](https://github.com/neilrackett/atarist-highwire/releases)** —
a build of the HighWire browser that runs on a stock ST in every
resolution, with no SpeedoGDOS, NVDI or other dependencies, and
greyscale images in ST medium. (Its changes have been offered upstream
as [freemint/highwire#7](https://github.com/freemint/highwire/pull/7);
if they're merged, prefer official HighWire builds.)

Point it at [frogfind.com](http://frogfind.com) or
[theoldnet.com](http://theoldnet.com) — both serve the web in a form a
68000 can enjoy.

## How it works

MD/Net is not a resident cartridge — it prints its status at boot and
hands control straight back to TOS, so STinG runs normally on top. The
`MDNET.STX` driver and the RP2040 talk over a small mailbox protocol
carried by the cartridge bus, and the RP2040 bridges whole Ethernet
frames to WiFi.

```
Atari ST (68000)                          RP2040 (Pico W)
────────────────                          ───────────────
STinG + MDNET.STX ("WiFi" port)
  │
  ├─ send frame     ──ROM3 reads────►  commemul ring → reassemble
  │  ($FB0000, one byte per read)                    │
  │                                                  ▼
  │                                          lwIP / CYW43 WiFi
  ├─ poll sequence  ──ROM4 read─────►  romemul serves it from RAM
  │  ($FA4044)                                       ▲
  │                                                  │
  └─ copy frame     ──ROM4 reads────►  published frame ($FA5000)
                                                     │
                                                     ▼
                                               Internet / LAN
```

The cartridge port is read-only, so the ST "writes" by doing dummy reads
in the ROM3 window, where the address carries the data — a trick MD/Net
inherits from the EtherNEC. Reads come from the ROM4 window, which the
SidecarT serves directly out of RAM.

The design leans entirely on that asymmetry. Received frames are
published one at a time into a fixed window and only replaced once the
ST acknowledges the previous one, so the ST can copy them at its own
pace and **nothing in the protocol is timing-critical**. That is the
lesson of the earlier NE2000 emulation, which had to feed the ST's
remote-DMA port byte by byte, in step with the bus, and never quite
managed it reliably. See [docs/mailbox-protocol.md](docs/mailbox-protocol.md).

## Network configuration

MD/Net takes its network parameters from the **Booster global configuration**, so they are set once in the web interface and shared across apps:

| Setting                                                  | Notes                                         |
| -------------------------------------------------------- | --------------------------------------------- |
| `WIFI_SSID`                                              | Your network name                             |
| `WIFI_PASSWORD`                                          | Your network password                         |
| `WIFI_AUTH`                                              | Authentication mode                           |
| `WIFI_DHCP`                                              | `true` for DHCP (default), `false` for static |
| `WIFI_IP` / `WIFI_NETMASK` / `WIFI_GATEWAY` / `WIFI_DNS` | Static configuration when DHCP is off         |

If the SSID is empty or the connection fails, MD/Net prints the failure on the ST and still boots to GEM — it never leaves you stuck at a black screen.

**The ST's own address is separate, and you don't set it.** The ST
can't take a DHCP lease of its own — it shares the cartridge's MAC
address, because a WiFi station link won't carry a second one — so the
cartridge picks an address for it instead: same subnet as its own lease,
counting up from its own host number (a cartridge on `.241` offers the
ST `.242`), and checked with ARP probes so nothing already using it gets
trampled. `WIFI_DNS` is used as the ST's nameserver if your router
doesn't supply one.

To choose the ST's address yourself, set it in STinG Port Setup; an
address set by hand always wins.

## Building

To build or monitor the microfirmware, the following `make` targets are available:

```bash
# Production build
make build

# Debug build
make debug

# Monitor debug build over UART
make uart
```

MD/Net only builds for the `pico_w` board, since WiFi is required. The app UUID is taken from `uuid.txt` (generate a fresh one per app). If you'd like more information about coding for the SidecarT, [the docs are here](https://docs.sidecartridge.com/sidecartridge-multidevice/programming/).

## The road not taken

MD/Net began as a NetUSBee emulator: reproduce the NE2000 on the
cartridge bus and every existing Atari networking stack would work
unmodified. That approach got surprisingly far — a byte-exact MAC PROM,
the driver selecting the NE2000 personality, correct ring bookkeeping,
ARP requests and ICMP genuinely leaving over WiFi — and then stalled on
one thing: the NE2000's remote-DMA data port must return a new byte on
every read, at bus speed, and the ST reads it back-to-back with
`MOVEP.L`. Serving that reliably from a microcontroller reacting to bus
events proved a step too far; a single stale byte in an ARP reply was
enough for the driver to reject it.

That work is preserved on the **[`netusbee`](../../tree/netusbee)**
branch — around fifty hardware-tested builds, with the reasoning, the
diagnostic techniques and the dead ends written up in its `AGENTS.md`.
It is left there as a curiosity, and in case it is useful to anyone
attempting something similar. Notably, that branch also documents a
discovery that outlived it: the `ENEC.STX` binary in circulation reads
the 8390 receive header's byte count **high byte first**, opposite to
the datasheet.

The lesson that produced this project's design: the cartridge port is
excellent at serving static memory and capturing writes, and poor at
anything that must respond within a bus cycle. The mailbox protocol is
built entirely out of the first two.

## Acknowledgements

- The [EtherNEC / EtherNE driver](https://github.com/EmmanuelKasper/ethernec) by Dr. Thomas Redelberger, itself built on code by Peter Rottengatter. `MDNET.STX` is derived from its STinG port driver: the ARP engine, the send/receive/set_state/cntrl structure and the STinG install handshake come from there, with the NE2000 hardware layer replaced by the MD/Net mailbox. Licensed under the GPL, as this is.
- [STinG](https://github.com/th-otto/STinG) by Peter Rottengatter, Ulf Ronald Andersson and Thorsten Otto — and its source, which settled several questions this project would otherwise have had to guess at.
- The NetUSBee hardware by Lyndon Amsdon and the Atari hardware community.

## License

Source code is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text.
