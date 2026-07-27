# MD/Net: Ethernet over WiFi for Atari ST

Microfirmware for the [SidecarTridge Multi-device](https://sidecartridge.com) by [Neil Rackett](https://neilrackett/atarist)

## Introduction

Welcome to MD/Net: Wireless Internet for your Atari ST!

MD/Net turns your SidecarT into a wireless network adapter for your Atari ST, bridging the cartridge port to your WiFi network. It reuses the WiFi credentials you already gave your SidecarT, so there is nothing extra to configure — no wires, no soldering.

Networking on the ST is provided by **[STinG](https://github.com/th-otto/STinG)** under plain TOS, driven by MD/Net's own port driver, `MDNET.STX`, which appears in STinG as a port named "WiFi". The driver and the cartridge talk over a small purpose-built protocol described in [docs/mailbox-protocol.md](docs/mailbox-protocol.md).

An earlier approach emulated the [NetUSBee](https://hardware.atari.org/netusbee/netus.htm)'s NE2000 Ethernet controller so that stock drivers would work unmodified. That got as far as a working transmit path and byte-exact receive, but the remote-DMA data port proved too timing-critical to serve reliably from the cartridge bus. It is preserved and documented on the `netusbee` branch. Supporting MiNTNet / MagiCNet is a possible future direction.

> ### ⚠️ Work in progress
>
> **MD/Net works**: an Atari ST running STinG pings the LAN in both
> directions with 0% packet loss, over WiFi, with no wires and no
> soldering. Hardware-validated on 2026-07-27.
>
> It gets there with a **custom STinG driver** (`MDNET.STX`, which
> appears as a port named "WiFi") talking to the cartridge over a
> purpose-built mailbox protocol — not by emulating a NetUSBee. The
> NE2000-emulation approach is preserved, documented, and paused on the
> `netusbee` branch; see [docs/mailbox-protocol.md](docs/mailbox-protocol.md)
> for the design that replaced it and why.
>
> **What works today:**
> - WiFi bootstrap: the ST prints `MD/Net connected: <IP>` at boot,
>   then continues to GEM.
> - `MDNET.STX` installs into STinG and appears as the port "WiFi".
> - ARP, IP and ICMP in both directions: `PING` from the ST reaches the
>   LAN, and the ST answers pings from other machines, at 0% loss.
>
> **Known limits:**
> - Throughput is at least ~20 frames/s each way; the true ceiling is
>   not yet established (ICMP rate-limiting upstream muddied the
>   measurement). Round-trip time is ~100-170 ms, dominated by how often
>   STinG services the driver rather than by the WiFi link.
> - TCP applications (FTP, HTTP) are not yet exercised.
> - The ST's IP is still configured by hand in STNGPORT.CPX; the
>   protocol has a config block ready for the cartridge to hand the ST
>   a DHCP-derived address, but that is not wired up yet.
> - `MDNET.STX` must be copied to the ST's `STING` folder by hand for
>   now; shipping it on the cartridge with an installer is next.

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
