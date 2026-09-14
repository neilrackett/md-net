/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hands the Atari ST the cartridge's own network configuration, so the
 * user never has to set one.
 *
 * The ST cannot take a DHCP lease of its own: it shares the Pico's MAC
 * address, because 802.11 station mode will not carry a foreign MAC,
 * and a DHCP server keyed on that MAC would hand back the address the
 * Pico already holds. So the ST simply uses that address -- the same
 * address, mask, gateway and DNS the Pico obtained (or the static
 * configuration set in Booster). One MAC, one lease, one address on
 * the network: the router sees a single device, and a DHCP reservation
 * for the cartridge's MAC fixes the ST's address.
 *
 * That only works because lwIP here is mute on the shared address:
 * TCP and ICMP are compiled out (see lwipopts.h) and the RX filter in
 * mailbox.c keeps only DHCP replies for lwIP, so the ST is the one
 * host answering.
 *
 * The result goes into the mailbox config block; MDNET.STX applies it
 * at port activation, and installs matching routes, so a stock ST needs
 * no address set in STinG Port Setup and no ROUTE.TAB editing.
 */

#include "autoconf.h"

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

static enum { AC_IDLE, AC_DONE, AC_FAILED } s_state = AC_IDLE;
static uint32_t s_ip, s_mask, s_gw, s_dns;

void autoconf_start(void) {
  struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
  const ip_addr_t *dns = dns_getserver(0);

  s_ip = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(n)));
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

  if (s_ip == 0u || s_mask == 0u) {
    DPRINTF("autoconf: no address of our own; leaving the ST to the CPX\n");
    s_state = AC_FAILED;
    return;
  }

  mailbox_publish_config(s_ip, s_mask, s_gw, s_dns);
  s_state = AC_DONE;
  DPRINTF("autoconf: ST gets %lu.%lu.%lu.%lu mask %lu.%lu.%lu.%lu "
          "gw %lu.%lu.%lu.%lu dns %lu.%lu.%lu.%lu\n",
          (unsigned long)(s_ip >> 24), (unsigned long)((s_ip >> 16) & 0xFF),
          (unsigned long)((s_ip >> 8) & 0xFF), (unsigned long)(s_ip & 0xFF),
          (unsigned long)(s_mask >> 24), (unsigned long)((s_mask >> 16) & 0xFF),
          (unsigned long)((s_mask >> 8) & 0xFF), (unsigned long)(s_mask & 0xFF),
          (unsigned long)(s_gw >> 24), (unsigned long)((s_gw >> 16) & 0xFF),
          (unsigned long)((s_gw >> 8) & 0xFF), (unsigned long)(s_gw & 0xFF),
          (unsigned long)(s_dns >> 24), (unsigned long)((s_dns >> 16) & 0xFF),
          (unsigned long)((s_dns >> 8) & 0xFF), (unsigned long)(s_dns & 0xFF));
}

uint32_t autoconf_address(void) { return (s_state == AC_DONE) ? s_ip : 0u; }
