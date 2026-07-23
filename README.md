# MD/Net: Ethernet over WiFi for Atari ST

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://neilrackett/atarist)

## Introduction

Welcome to MD/Net: Wireless Internet for your Atari ST!

MD/Net turns your SidecarT into a wireless network adapter by emulating the [NetUSBee](https://hardware.atari.org/netusbee/netus.htm)'s NE2000 Ethernet controller on the cartridge port, enabling you to connect your Atari ST to the internet using your SidecarT's built-in WiFi, you don't even need to enter your WiFi credentials, MD/Net does it all for you.

And because MD/Net presents itself as a standard NE2000, exactly like the NetUSBee, existing Atari networking stacks work unmodified:

- **[STinG](https://github.com/th-otto/STinG)** under plain TOS, using its EtherNEC driver
- **MiNTNet / MagiCNet**, using the EtherNEC / EtherNE driver

Real-world NetUSBee + STinG setups reach FTP, HTTP and general internet use, and MD/Net aims at the same bar — no wires, no soldering, just install MD/Net and you're ready to go.

> ### ⚠️ Work in progress
>
> MD/Net is under active development, and the NE2000 emulation approach on
> this branch is **paused short of working ping**. Where it stands:
>
> **Working and hardware-verified:**
> - WiFi bootstrap: the ST prints `MD/Net connected: <IP>` at boot, then GEM.
> - The full NE2000 probe: STinG's EtherNEC driver detects the "card",
>   reads a byte-exact MAC PROM, and selects the NE2000 personality.
> - Register file, remote-DMA ring walking, BNRY/CURR bookkeeping.
> - Transmit end-to-end: STinG's ARP requests and ICMP leave over WiFi.
> - Receive delivery: LAN frames land in the ring; the driver reads
>   headers and bodies with verified byte-exact serves on most traffic.
>
> **Not working:** one residual serve corruption remains on ARP-reply
> body reads (a single byte, `hardware_space`, intermittently serves a
> stale window byte), so STinG's `process_arp` rejects every reply as
> malformed, ARP never resolves, and ping shows 100% loss.
>
> **What a fix needs:** the remote-DMA data-port serve must deliver
> byte-exact streams under *every* m68k access pattern (movep bursts,
> polled reads, chained re-arms) with zero mid-stream staleness. The
> current design (PIO tap + 4-slot pre-staged window + bus-order
> barriers, see [docs/ne2000-emulation.md](docs/ne2000-emulation.md))
> got asymptotically close — the remaining bug is a timing interaction
> between window prestaging and the driver's header→body arm sequence.
> A hardware-accurate fix likely wants the serve done *entirely in PIO*
> (address-indexed stream serving without CPU-in-the-loop), or a
> logic-analyzer capture to pin the exact cycle interleaving.
>
> Also hard-won and documented in `AGENTS.md`: this specific `ENEC.STX`
> binary (a BUGGY_HW debug build) reads the 8390 header count
> **high-byte-first**, opposite to the datasheet — headers must be
> written to match or every packet detours into its recovery path.
>
> **The active approach now lives on the `stx` branch:** a custom
> STinG driver (`MDNET.STX`) that talks to the RP over a simple
> mailbox protocol instead of NE2000 emulation — trading NetUSBee
> fidelity for robustness (and enabling DHCP).

## Hardware requirements

- [SidecarTridge Multi-device](https://sidecartridge.com) with a **Raspberry Pi Pico W** (WiFi is required)
- Atari ST, STE, MegaST, or MegaSTE
- Raspberry Pi Debug Probe or Picoprobe for flashing/debugging (optional but recommended for development)

## Installation

**Manual installation**

1. Download the latest files from the [releases page](https://github.com/neilrackett/md-net/releases).
2. Copy the `.uf2` and `.json` files to the `/apps` folder of your SidecarT's microSD card.
3. In the Booster web interface, make sure you're connected to your WiFi, so MD/Net can use the same information to connect.
4. On the Booster screen, press ESC for the app list and select the MD/Net app.
5. To return to Booster, turn on your ST while holding the SELECT button on your SidecarT.

You'll know the microfirmware is working because you'll see `MD/Net: connecting...` followed by `MD/Net connected: <your IP>` on your ST's screen as it boots, before the GEM desktop appears.

## How it works

MD/Net is not a resident cartridge — it prints its status at boot and hands control straight back to TOS, so STinG (or MiNT) runs normally on top. The RP2040 emulates the NE2000's register file and buffer RAM and bridges Ethernet frames to WiFi; the ST's driver talks to it over the cartridge bus exactly as it would to real NetUSBee hardware.

```
Atari ST (68000)                         RP2040 (Pico W)
────────────────                         ───────────────
STinG + EtherNEC driver
  │
  ├─ register write  ──ROM3 read────►  Core 0: commemul ring → NE2000 model
  │  ($FB0000+reg<<9+data<<1)                    (reg = A9-A13, data = A1-A8)
  │
  ├─ register read   ──ROM4 read────►  Core 0: romemul serves the staged
  │  ($FA0000+reg<<9)                            register value from RAM
  │
  └─ remote DMA      ──data port────►  packet bytes in/out of the ring
     ($FA2000)                                    │
                                                  ▼
                                        Core 1: lwIP / CYW43 WiFi bridge
                                                  │
                                                  ▼
                                            Internet / LAN
```

The cartridge port is read-only, so the EtherNEC hardware — and therefore MD/Net — encodes register _writes_ as dummy reads in the ROM3 window and serves register _reads_ from the ROM4 window. MD/Net decodes and answers both from the cartridge bus emulation the SidecarT already provides.

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

## Acknowledgements

- The [EtherNEC / EtherNE driver](https://github.com/EmmanuelKasper/ethernec) by Dr. Thomas Redelberger — the NE2000 register access reference and the driver STinG loads for NetUSBee.
- [STinG](https://github.com/th-otto/STinG) by Peter Rottengatter, Ulf Ronald Andersson and Thorsten Otto.
- The NetUSBee hardware by Lyndon Amsdon and the Atari hardware community.

## License

Source code is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE) for the full text.
