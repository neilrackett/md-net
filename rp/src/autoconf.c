/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Picks a network address for the Atari ST so the user never has to.
 *
 * The ST cannot simply take a DHCP lease of its own: it shares the
 * Pico's MAC address, because 802.11 station mode will not carry a
 * foreign MAC, and a DHCP server keyed on that MAC would hand back the
 * address the Pico already holds. So instead we derive the ST's
 * configuration from the Pico's own lease -- same subnet, mask, gateway
 * and DNS -- and choose a host address that nothing is using, verified
 * by ARP probing (the mechanism RFC 5227 defines for exactly this).
 *
 * The result goes into the mailbox config block; MDNET.STX applies it
 * at port activation, and installs matching routes, so a stock ST needs
 * no address set in STinG Port Setup and no ROUTE.TAB editing.
 *
 * Addresses are probed from the Pico's own host number upwards, so a
 * Pico on .241 offers the ST .242 -- stable across reboots, and outside
 * the low end of the pool where DHCP servers usually start.
 */

#include "autoconf.h"

#ifdef AUTOCONF_HOST_TEST
#include <stdio.h>
#else
#include <stdio.h>
#include <string.h>

#include "cart_shared.h"
#include "debug.h"
#include "gconfig.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "mailbox.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#endif

#define PROBE_COUNT 8u          // candidates to try before giving up
#define PROBE_TRIES 3u          // probes per candidate (RFC 5227 uses 3)
#define PROBE_GAP_US 200000u    // 200 ms between probes
#define PROBE_WAIT_US 400000u   // final wait for a defender to answer

// Choose the nth candidate host address in our subnet, counting up from
// our own host number. Skips the network and broadcast addresses and
// our own; returns 0 when the subnet cannot offer another host (a /31
// or /32, say).
uint32_t autoconf_candidate(uint32_t our_ip, uint32_t mask, uint8_t n) {
  uint32_t network = our_ip & mask;
  uint32_t hostmask = ~mask;
  uint32_t our_host = our_ip & hostmask;
  uint32_t tries = 0;
  uint32_t host = our_host;

  if (hostmask < 3u) {
    return 0;  // no room for another host
  }
  // Walk forward until we have skipped n valid candidates.
  while (tries <= n) {
    host = (host + 1u) & hostmask;
    if (host == 0u || host == hostmask) {
      continue;  // network / broadcast address
    }
    if (host == our_host) {
      return 0;  // wrapped all the way round: subnet is exhausted
    }
    if (tries == n) {
      return network | host;
    }
    tries++;
  }
  return 0;
}

#ifndef AUTOCONF_HOST_TEST

static enum { AC_IDLE, AC_PROBING, AC_DONE, AC_FAILED } s_state = AC_IDLE;
static uint32_t s_ourIp, s_mask, s_gw, s_dns;
static uint32_t s_candidate;
static uint8_t s_index;
static uint32_t s_probeStart;
static uint8_t s_tries;
static bool s_defended;

// Send an ARP probe: an ARP request for the candidate with sender IP
// 0.0.0.0, so nothing caches a binding for an address we may not end up
// using (RFC 5227 probe semantics).
static void send_probe(uint32_t target) {
  uint8_t frame[42];
  uint8_t mac[6];
  int i;

  if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) != 0) {
    return;
  }
  for (i = 0; i < 6; i++) {
    frame[i] = 0xFFu;      // broadcast
    frame[6 + i] = mac[i]; // sender hardware address
  }
  frame[12] = 0x08u;
  frame[13] = 0x06u;                      // ethertype ARP
  frame[14] = 0x00u; frame[15] = 0x01u;   // hardware: Ethernet
  frame[16] = 0x08u; frame[17] = 0x00u;   // protocol: IPv4
  frame[18] = 6u; frame[19] = 4u;         // address lengths
  frame[20] = 0x00u; frame[21] = 0x01u;   // opcode: request
  for (i = 0; i < 6; i++) {
    frame[22 + i] = mac[i];               // sender hardware address
  }
  frame[28] = 0u; frame[29] = 0u;
  frame[30] = 0u; frame[31] = 0u;         // sender IP 0.0.0.0 (a probe)
  for (i = 0; i < 6; i++) {
    frame[32 + i] = 0u;                   // target hardware unknown
  }
  frame[38] = (uint8_t)(target >> 24);
  frame[39] = (uint8_t)(target >> 16);
  frame[40] = (uint8_t)(target >> 8);
  frame[41] = (uint8_t)target;

  cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, sizeof(frame), frame,
                      false);
}

// Anything that speaks for the candidate address -- a reply to our
// probe, or its own traffic -- means the address is taken.
void autoconf_observe(const uint8_t *frame, uint16_t len) {
  uint32_t sender;
  if (s_state != AC_PROBING || len < 42u) {
    return;
  }
  if (frame[12] != 0x08u || frame[13] != 0x06u) {
    return;  // not ARP
  }
  sender = ((uint32_t)frame[28] << 24) | ((uint32_t)frame[29] << 16) |
           ((uint32_t)frame[30] << 8) | (uint32_t)frame[31];
  if (sender == s_candidate) {
    s_defended = true;
  }
}

static void publish(uint32_t ip) {
  mailbox_publish_config(ip, s_mask, s_gw, s_dns);
  DPRINTF("autoconf: ST gets %lu.%lu.%lu.%lu mask %lu.%lu.%lu.%lu "
          "gw %lu.%lu.%lu.%lu dns %lu.%lu.%lu.%lu\n",
          (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 0xFF),
          (unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF),
          (unsigned long)(s_mask >> 24), (unsigned long)((s_mask >> 16) & 0xFF),
          (unsigned long)((s_mask >> 8) & 0xFF), (unsigned long)(s_mask & 0xFF),
          (unsigned long)(s_gw >> 24), (unsigned long)((s_gw >> 16) & 0xFF),
          (unsigned long)((s_gw >> 8) & 0xFF), (unsigned long)(s_gw & 0xFF),
          (unsigned long)(s_dns >> 24), (unsigned long)((s_dns >> 16) & 0xFF),
          (unsigned long)((s_dns >> 8) & 0xFF), (unsigned long)(s_dns & 0xFF));
}

void autoconf_start(void) {
  struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
  const ip_addr_t *dns = dns_getserver(0);

  s_ourIp = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(n)));
  s_mask = lwip_ntohl(ip4_addr_get_u32(netif_ip4_netmask(n)));
  s_gw = lwip_ntohl(ip4_addr_get_u32(netif_ip4_gw(n)));
  // Prefer what DHCP gave us -- it is the one that resolves local names
  // as well. network.c only applies the configured WIFI_DNS on the
  // static-IP path, so on DHCP that setting is otherwise unused; fall
  // back to it, since it is a deliberate choice the user can change in
  // Booster. The gateway is the last resort: most home routers proxy
  // DNS, but that is a guess rather than a setting.
  s_dns = dns ? lwip_ntohl(ip4_addr_get_u32(dns)) : 0u;
  if (s_dns == 0u) {
    SettingsConfigEntry *entry =
        settings_find_entry(gconfig_getContext(), PARAM_WIFI_DNS);
    if (entry != NULL && entry->value[0] != '\0') {
      char copy[NETWORK_MAX_STRING_LENGTH * 2 + 2] = {0};
      char *first = copy;
      char *comma;
      u32_t parsed;
      snprintf(copy, sizeof(copy), "%s", entry->value);
      comma = strchr(first, ',');  // "8.8.8.8, 8.8.4.4" -- take the first
      if (comma != NULL) {
        *comma = '\0';
      }
      while (*first == ' ' || *first == '\t') {
        first++;  // network.c's trim helper is static, and this is all
      }           // the trimming a leading-space entry needs
      parsed = ipaddr_addr(first);
      if (first[0] != '\0' && parsed != IPADDR_NONE) {
        s_dns = lwip_ntohl(parsed);
      }
    }
  }
  if (s_dns == 0u) {
    s_dns = s_gw;
  }

  if (s_ourIp == 0u || s_mask == 0u) {
    DPRINTF("autoconf: no lease of our own; leaving the ST to the CPX\n");
    s_state = AC_FAILED;
    return;
  }

  s_index = 0;
  s_candidate = autoconf_candidate(s_ourIp, s_mask, s_index);
  if (s_candidate == 0u) {
    s_state = AC_FAILED;
    return;
  }
  s_defended = false;
  s_tries = 1;
  s_probeStart = time_us_32();
  s_state = AC_PROBING;
  send_probe(s_candidate);
}

bool autoconf_done(void) {
  return s_state == AC_DONE || s_state == AC_FAILED;
}

uint32_t autoconf_address(void) {
  return (s_state == AC_DONE) ? s_candidate : 0u;
}

void autoconf_poll(void) {
  if (s_state != AC_PROBING) {
    return;
  }
  if (s_defended) {
    // Someone owns it; try the next one.
    s_index++;
    if (s_index >= PROBE_COUNT) {
      DPRINTF("autoconf: no free address found; leaving the ST to the CPX\n");
      s_state = AC_FAILED;
      return;
    }
    s_candidate = autoconf_candidate(s_ourIp, s_mask, s_index);
    if (s_candidate == 0u) {
      s_state = AC_FAILED;
      return;
    }
    s_defended = false;
    s_tries = 1;
    s_probeStart = time_us_32();
    send_probe(s_candidate);
    return;
  }
  if (s_tries < PROBE_TRIES) {
    // Repeat the probe: one is not enough, since a host in power-save
    // can easily miss a single request and we would then take an
    // address that is already in use.
    if (time_us_32() - s_probeStart >= PROBE_GAP_US) {
      s_tries++;
      s_probeStart = time_us_32();
      send_probe(s_candidate);
    }
    return;
  }
  if (time_us_32() - s_probeStart >= PROBE_WAIT_US) {
    publish(s_candidate);  // silence from three probes means it is free
    s_state = AC_DONE;
  }
}

#endif  // !AUTOCONF_HOST_TEST
