/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from the EtherNEC STinG port driver ENESTNG.C,
 * Copyright (C) 2000-2002 Dr. Thomas Redelberger, which is itself
 * largely derived from code by Peter Rottengatter, the author of
 * STinG. The ARP engine, the send/receive/set_state/cntrl structure
 * and the STinG install handshake come from that driver; its NE2000
 * hardware layer is replaced here by the MD/Net cart-bus mailbox.
 *
 * That upstream is distributed "under the terms of the GNU General
 * Public License" without naming a version (its COPYING.TXT is v2), so
 * GPLv2 section 9 -- "If the Program does not specify a version number
 * of this License, you may choose any version ever published by the
 * Free Software Foundation" -- permits GPLv3. This file therefore
 * carries GPL-3.0-or-later, matching the rest of MD/Net.
 */

/**
 * File: mdnet.c
 * Description: MDNET.STX -- STinG port driver for the MD/Net cart-bus
 *              mailbox. Registers a port named "WiFi". The RP2040 side
 *              of the contract is rp/src/mailbox.c; the protocol is
 *              documented in docs/mailbox-protocol.md.
 *
 * Modeled on the EtherNEC STinG driver by Dr. Thomas Redelberger
 * (ENESTNG.C, published with the ethernec project), with the NE2000
 * hardware access replaced by the MD/Net mailbox: frames are received
 * by block-copying from a cart ROM window and sent by streaming bytes
 * through ROM3 dummy reads. ARP is handled here in the driver; the RP
 * bridges raw Ethernet frames to WiFi.
 *
 * Build: m68k-atari-mint-gcc with -mshort -nostdlib (see Makefile).
 * STinG loads this file (renamed MDNET.STX) with the command line
 * "STinG_Load" and calls the start of the text segment with the
 * basepage as a C argument; entry.s forwards that call to driver_main.
 */

#define cdecl /* Pure-C keyword; gcc's default calling convention matches */
#define NULL ((void *)0)

/* Minimal GEMDOS basepage (only the fields we use). The SDK headers
   reference the BASPAG type, so it must exist before including them. */
typedef struct baspag {
  void *p_lowtpa;
  void *p_hitpa;
  void *p_tbase;
  long p_tlen;
  void *p_dbase;
  long p_dlen;
  void *p_bbase;
  long p_blen;
  void *p_dta;
  struct baspag *p_parent;
  long p_reserved;
  char *p_env;
  char p_junk[8];
  long p_undef[18];
  char p_cmdlin[128];
} BASPAG;

/* The STinG SDK headers define int8..uint32 themselves (Pure-C sizes;
   we compile with -mshort so int is 16-bit and the layouts match). */
#include "TRANSPRT.H"
#include "PORT.H"

/* Compile-time ABI checks. The STinG SDK headers were written for
   Pure-C (16-bit int); we build with -mshort so the layouts must match
   exactly. The PORT offset is checked against the reference EtherNEC
   driver's hand-written assembler constant (NESTNG.S: ptReceive EQU
   $24), and the IP_DGRAM size against its sgIp_DgramLen (48). A silent
   mismatch here would corrupt STinG's own structures. */
#define MDNET_ASSERT(name, cond) typedef char mdnet_chk_##name[(cond) ? 1 : -1]
MDNET_ASSERT(int16, sizeof(int16) == 2);
MDNET_ASSERT(int32, sizeof(int32) == 4);
MDNET_ASSERT(ip_hdr, sizeof(IP_HDR) == 20);
MDNET_ASSERT(ip_dgram, sizeof(IP_DGRAM) == 48);
MDNET_ASSERT(port_receive, __builtin_offsetof(PORT, receive) == 0x24);
MDNET_ASSERT(port_ip, __builtin_offsetof(PORT, ip_addr) == 0x0C);

/* ---- GEMDOS bindings (no stdlib: raw trap #1) ---------------------- */

static int32 gemdos1l(int16 op, int32 a) {
  register int32 ret __asm__("d0");
  __asm__ volatile(
      "move.l %2,-(%%sp)\n\t"
      "move.w %1,-(%%sp)\n\t"
      "trap   #1\n\t"
      "addq.l #6,%%sp"
      : "=r"(ret)
      : "r"(op), "r"(a)
      : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
  return ret;
}

static int32 gemdos_ptermres(int32 keep, int16 rc) {
  register int32 ret __asm__("d0");
  __asm__ volatile(
      "move.w %2,-(%%sp)\n\t"
      "move.l %1,-(%%sp)\n\t"
      "move.w #49,-(%%sp)\n\t"
      "trap   #1\n\t"
      "addq.l #8,%%sp"
      : "=r"(ret)
      : "r"(keep), "r"(rc)
      : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
  return ret;
}

/* XBIOS Supexec (38): run fn in supervisor mode. Required for anything
   touching the system-variable page ($0-$7FF), which is bus-error
   protected in user mode -- STinG Pexec's this module, so driver_main
   can run unprivileged. The reference driver wraps its cookie-jar walk
   the same way (ENESTNG.C: Supexec(get_sting_cookie)). Cart-space
   access needs no such wrapper: the reference calls its bus routines
   directly from my_send/my_receive. */
static int32 xbios_supexec(long (*fn)(void)) {
  register int32 ret __asm__("d0");
  __asm__ volatile(
      "move.l %1,-(%%sp)\n\t"
      "move.w #38,-(%%sp)\n\t"
      "trap   #14\n\t"
      "addq.l #6,%%sp"
      : "=r"(ret)
      : "r"(fn)
      : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
  return ret;
}

#define Cconws(s) gemdos1l(9, (int32)(s))
#define Pterm(rc) gemdos1l(76, (int32)(rc))
#define Ptermres(keep, rc) gemdos_ptermres((keep), (rc))

/* ---- Mailbox hardware interface ------------------------------------ */

#define ROM4_BASE 0xFA0000UL
#define ROM3_BASE 0xFB0000UL

#define MB_PROTO_MAGIC_OFF 0x4020UL
#define MB_PROTO_VER_OFF 0x4024UL
#define MB_MAC_OFF 0x4026UL
#define MB_CFG_SEQ_OFF 0x4030UL
#define MB_CFG_IP_OFF 0x4032UL
#define MB_CFG_MASK_OFF 0x4036UL
#define MB_CFG_GW_OFF 0x403AUL
#define MB_CFG_DNS_OFF 0x403EUL
#define MB_RX_SEQ_OFF 0x4044UL
#define MB_RX_LEN_OFF 0x4046UL
#define MB_TX_ACK_OFF 0x4048UL
#define MB_RX_BUF_OFF 0x5000UL

#define MB_PROTO_MAGIC 0x4D444E42UL /* 'MDNB' */
#define MB_PROTO_VERSION 1

#define MBC_RX_ACK 0x01
#define MBC_TX_START 0x02
#define MBC_TX_LEN_HI 0x03
#define MBC_TX_DATA 0x04
#define MBC_TX_COMMIT 0x05
#define MBC_DRIVER_HELLO 0x06
#define MBC_DRIVER_BYE 0x07

#define DRIVER_VERSION_BYTE 1

#define mb_r8(off) (*(volatile uint8 *)(ROM4_BASE + (off)))
#define mb_r16(off) (*(volatile uint16 *)(ROM4_BASE + (off)))
#define mb_r32(off) (*(volatile uint32 *)(ROM4_BASE + (off)))

/* A "write" is a dummy read in ROM3: channel in A9-A13, data in A1-A8. */
static void mb_cmd(uint16 chan, uint16 data) {
  volatile uint8 *p =
      (volatile uint8 *)(ROM3_BASE + ((uint32)chan << 9) + ((uint32)data << 1));
  (void)*p;
}

/* ---- Ethernet / ARP structures (wire format, big-endian native) ---- */

#define TYPE_IP 0x0800
#define TYPE_ARP 0x0806

typedef struct {
  uint8 destination[6];
  uint8 source[6];
  uint16 type;
} ETH_HDR;

typedef struct arp_pkt {
  uint16 hardware_space;
  uint16 protocol_space;
  uint8 hardware_len;
  uint8 protocol_len;
  uint16 op_code;
  uint8 src_ether[6];
  uint32 src_ip;
  uint8 dest_ether[6];
  uint32 dest_ip;
} ARP;

MDNET_ASSERT(eth_hdr, sizeof(ETH_HDR) == 14);
MDNET_ASSERT(arp_pkt, sizeof(ARP) == 28);

#define ARP_HARD_ETHER 1
#define ARP_OP_REQ 1
#define ARP_OP_ANS 2

/* ---- Driver state -------------------------------------------------- */

TPL *tpl;
STX *stx;

static int16 cdecl my_set_state(PORT *port, int16 state);
static int16 cdecl my_cntrl(PORT *port, uint32 argument, int16 code);
static void cdecl my_send(PORT *port);
static void cdecl my_receive(PORT *port);

static PORT my_port = {
    "WiFi", L_SER_BUS, FALSE, 0L, 0xffffffffUL, 0xffffffffUL,
    1500,   1500,      0L,    NULL, 0L,          NULL,
    0,      NULL,      NULL};

static DRIVER my_driver = {
    my_set_state, my_cntrl, my_send, my_receive, "MD/Net WiFi", "01.00",
    ((2026 - 1980) << 9) | (7 << 5) | 23, "Neil Rackett", NULL, NULL};

static char *suppHardware[] = {"No selection", "WiFi (MD/Net)", NULL};

static uint8 my_mac[6];
static uint16 last_rx_seq = 0;

/* Outgoing ARP packet (static, reused). */
static struct {
  ETH_HDR eh;
  ARP arp;
  uint8 padbytes[60 - sizeof(ETH_HDR) - sizeof(ARP)];
} arpEthPckt;

static uint16 doTxArp = FALSE;
static uint16 waitArp = 0;

/* Scratch header block for outgoing IP frames (eth + IP hdr + options). */
static struct {
  ETH_HDR eh;
  uint8 ed[sizeof(IP_HDR) + 200];
} ipEthPckt;

/* ---- Small helpers (no stdlib) ------------------------------------- */

static void memcpN(uint8 *d, const uint8 *s, int16 n) {
  while (--n >= 0) *d++ = *s++;
}
static void memsetN(uint8 *d, uint8 v, int16 n) {
  while (--n >= 0) *d++ = v;
}
static int16 str_eq(const char *s, const char *t) {
  for (; *s == *t; s++, t++)
    if (*s == '\0') return TRUE;
  return FALSE;
}

/* ---- TX: stream a frame through the ROM3 command channel ----------- */

static uint16 tx_seq = 0;

/* Send hdr[0..hlen) followed by body[0..blen); pads to 60 bytes min. */
static int16 mb_tx_frame(const uint8 *hdr, int16 hlen, const uint8 *body,
                         int16 blen) {
  int16 total = hlen + blen;
  int16 pad = 0;
  int16 i;
  if (total < 60) {
    pad = 60 - total;
    total = 60;
  }
  mb_cmd(MBC_TX_START, (uint16)(total & 0xFF));
  mb_cmd(MBC_TX_LEN_HI, (uint16)((total >> 8) & 0xFF));
  for (i = 0; i < hlen; i++) mb_cmd(MBC_TX_DATA, hdr[i]);
  for (i = 0; i < blen; i++) mb_cmd(MBC_TX_DATA, body[i]);
  for (i = 0; i < pad; i++) mb_cmd(MBC_TX_DATA, 0);
  tx_seq++;
  mb_cmd(MBC_TX_COMMIT, (uint16)(tx_seq & 0xFF));
  return 0;
}

/* ---- ARP cache (lifted from the EtherNEC driver) ------------------- */

typedef struct arp_entry {
  uint32 ip_addr;
  uint8 ether[6];
  uint16 used;
} ARP_ENTRY;

#define ARP_NUM 32

int arpNentries = ARP_NUM;
static ARP_ENTRY *arpRecnt = NULL;
static ARP_ENTRY arpEntries[ARP_NUM];

static void arp_init(void) {
  ARP_ENTRY *walk;
  int16 i;
  for (i = ARP_NUM, walk = arpEntries; --i >= 0; walk++) {
    walk->ip_addr = 0;
    memsetN(walk->ether, 0, 6);
    walk->used = 0;
  }
  arpRecnt = arpEntries;
}

static uint8 *arp_cache(uint32 ip_addr) {
  ARP_ENTRY *walk;
  int16 i;
  for (i = ARP_NUM, walk = arpRecnt; --i >= 0;) {
    if (walk->used && walk->ip_addr == ip_addr) {
      arpRecnt = walk;
      return walk->ether;
    }
    if (--walk < arpEntries) walk = arpEntries + ARP_NUM - 1;
  }
  return NULL;
}

static void arp_enter(uint32 ip_addr, uint8 ether_addr[6]) {
  if (++arpRecnt >= arpEntries + ARP_NUM) arpRecnt = arpEntries;
  arpRecnt->ip_addr = ip_addr;
  memcpN(arpRecnt->ether, ether_addr, 6);
  arpRecnt->used = 1;
}

/* process_arp: same acceptance logic as the EtherNEC driver. */
static void process_arp(ARP *arp, int16 length) {
  uint8 *cachedEther;
  int16 update = TRUE;

  if (arp->hardware_space != ARP_HARD_ETHER || arp->hardware_len != 6 ||
      arp->protocol_space != TYPE_IP || arp->protocol_len != 4)
    return;

  my_port.stat_rcv_data += length;

  if ((cachedEther = arp_cache(arp->src_ip)) != NULL) {
    update = FALSE;
    memcpN(cachedEther, arp->src_ether, 6);
  }

  if (arp->dest_ip != my_port.ip_addr) return;

  if (update) arp_enter(arp->src_ip, arp->src_ether);

  if (arp->op_code == ARP_OP_ANS) {
    waitArp = 0;
    return;
  }

  /* It was a request for us: queue an answer. */
  memcpN(arpEthPckt.eh.destination, arp->src_ether, 6);
  arpEthPckt.arp.op_code = ARP_OP_ANS;
  arpEthPckt.arp.src_ip = my_port.ip_addr;
  memcpN(arpEthPckt.arp.dest_ether, arp->src_ether, 6);
  arpEthPckt.arp.dest_ip = arp->src_ip;
  doTxArp = TRUE;
}

/* ---- RX: consume the published frame from the mailbox window ------- */

static void deliver_ip_dgram(const volatile uint8 *frame, int16 flen) {
  IP_DGRAM *dgram, *walk;
  const volatile uint8 *ip = frame + sizeof(ETH_HDR);
  int16 hd_len, opt_len, data_len;
  uint16 ip_total;
  uint32 ip_dest;

  if (flen < (int16)(sizeof(ETH_HDR) + sizeof(IP_HDR))) return;

  hd_len = (int16)((ip[0] & 0x0F) * 4);
  if (hd_len < (int16)sizeof(IP_HDR)) return;
  /* Compare unsigned throughout: a frame claiming an IP length of
     0x8000+ would go negative as int16, pass a signed bounds check and
     then underflow into a ~32 KB allocation and copy. */
  ip_total = (uint16)((ip[2] << 8) | ip[3]);
  if (ip_total > (uint16)(flen - (int16)sizeof(ETH_HDR))) return;
  if (ip_total < (uint16)hd_len) return;
  opt_len = hd_len - (int16)sizeof(IP_HDR);
  data_len = (int16)(ip_total - (uint16)hd_len);

  ip_dest = ((uint32)ip[16] << 24) | ((uint32)ip[17] << 16) |
            ((uint32)ip[18] << 8) | (uint32)ip[19];

  /* Deliver our unicast + broadcasts. The reference EtherNEC driver
     filters nothing here (its NE2000 filtered by MAC), but our bridge
     hands over everything the shared MAC sees, so drop other hosts'
     unicast traffic rather than KRmalloc it. If the port IP is not
     configured yet, accept everything -- a wrong filter here would
     silently black-hole all traffic, which is far worse than noise. */
  if (my_port.ip_addr != 0 && my_port.ip_addr != 0xffffffffUL) {
    if (ip_dest != my_port.ip_addr && ip_dest != 0xffffffffUL &&
        ip_dest != (my_port.ip_addr | ~my_port.sub_mask))
      return;
  }

  dgram = KRmalloc(sizeof(IP_DGRAM));
  if (dgram == NULL) return;

  memcpN((uint8 *)&dgram->hdr, (const uint8 *)ip, sizeof(IP_HDR));
  dgram->options = NULL;
  dgram->opt_length = opt_len;
  if (opt_len > 0) {
    dgram->options = KRmalloc(opt_len);
    if (dgram->options == NULL) {
      KRfree(dgram);
      return;
    }
    memcpN((uint8 *)dgram->options, (const uint8 *)(ip + sizeof(IP_HDR)),
           opt_len);
  }
  dgram->pkt_data = NULL;
  dgram->pkt_length = data_len;
  if (data_len > 0) {
    dgram->pkt_data = KRmalloc(data_len);
    if (dgram->pkt_data == NULL) {
      if (dgram->options) KRfree(dgram->options);
      KRfree(dgram);
      return;
    }
    memcpN((uint8 *)dgram->pkt_data, (const uint8 *)(ip + hd_len), data_len);
  }
  dgram->ip_gateway = 0;
  dgram->recvd = &my_port;
  dgram->next = NULL;
  set_dgram_ttl(dgram);

  if (my_port.receive == NULL) {
    my_port.receive = dgram;
  } else {
    for (walk = my_port.receive; walk->next; walk = walk->next);
    walk->next = dgram;
  }
  my_port.stat_rcv_data += flen;
}

static void mb_rx_service(void) {
  uint16 seq = mb_r16(MB_RX_SEQ_OFF);
  uint16 len;
  const volatile uint8 *frame;
  uint16 type;
  static ARP arpCopy;

  if (seq == last_rx_seq || seq == 0) return;

  len = mb_r16(MB_RX_LEN_OFF);
  frame = (const volatile uint8 *)(ROM4_BASE + MB_RX_BUF_OFF);

  if (len >= 14 && len <= 1600) {
    type = (uint16)((frame[12] << 8) | frame[13]);
    if (type == TYPE_ARP && len >= 14 + (uint16)sizeof(ARP)) {
      memcpN((uint8 *)&arpCopy, (const uint8 *)(frame + 14), sizeof(ARP));
      process_arp(&arpCopy, (int16)len);
    } else if (type == TYPE_IP) {
      deliver_ip_dgram(frame, (int16)len);
    }
  }

  last_rx_seq = seq;
  mb_cmd(MBC_RX_ACK, (uint16)(seq & 0xFF));
}

/* Install routes for this port if nothing already covers it.

   Verified against STinG's routing code: the table is scanned in order
   and the first match wins, and set_route_entry(-1, ...) appends a new
   entry. So anything the user put in ROUTE.TAB keeps priority, and what
   we add is only a fallback for a machine that has none. That is what
   makes a stock ST work with no ROUTE.TAB at all. */
static void install_routes(uint32 gateway) {
  uint32 tmplt, mask, gway;
  PORT *port;
  int16 i;

  if (my_port.ip_addr == 0 || my_port.ip_addr == 0xffffffffUL) return;

  for (i = 0; get_route_entry(i, &tmplt, &mask, &port, &gway) != -1; i++) {
    if (port == &my_port) return; /* already routed; leave it alone */
  }

  /* Our own subnet, then everything else via the gateway. */
  set_route_entry(-1, my_port.ip_addr & my_port.sub_mask, my_port.sub_mask,
                  &my_port, 0);
  if (gateway != 0) {
    set_route_entry(-1, 0, 0, &my_port, gateway);
  }
}

/* ---- STinG port driver entry points -------------------------------- */

static void cdecl my_send(PORT *port) {
  uint8 *cachedEther;
  uint32 network, ip_address;

  if (doTxArp) {
    mb_tx_frame((const uint8 *)&arpEthPckt, sizeof(arpEthPckt), NULL, 0);
    doTxArp = FALSE;
    return;
  }

  if (port != &my_port || my_port.active == 0) return;

  for (;;) {
    IP_DGRAM *next;
    if (my_port.send == NULL) return;
    next = my_port.send->next;
    if (check_dgram_ttl(my_port.send) == E_NORMAL) break;
    my_port.send = next;
  }

  network = my_port.ip_addr & my_port.sub_mask;

  if ((my_port.send->hdr.ip_dest & my_port.sub_mask) == network) {
    ip_address = my_port.send->hdr.ip_dest;
  } else {
    if ((my_port.send->ip_gateway & my_port.sub_mask) == network) {
      ip_address = my_port.send->ip_gateway;
    } else {
      IP_DGRAM *next = my_port.send->next;
      IP_discard(my_port.send, TRUE);
      my_port.send = next;
      my_port.stat_dropped++;
      return;
    }
  }

  if ((my_port.send->hdr.ip_dest & ~my_port.sub_mask) == ~my_port.sub_mask) {
    memsetN(ipEthPckt.eh.destination, 0xff, 6);
  } else {
    if ((cachedEther = arp_cache(ip_address)) != NULL) {
      memcpN(ipEthPckt.eh.destination, cachedEther, 6);
    } else {
      if (waitArp > 0) {
        --waitArp;
        return;
      }
      memsetN(arpEthPckt.eh.destination, 0xff, 6);
      arpEthPckt.arp.op_code = ARP_OP_REQ;
      arpEthPckt.arp.src_ip = my_port.ip_addr;
      memsetN(arpEthPckt.arp.dest_ether, 0, 6);
      arpEthPckt.arp.dest_ip = ip_address;
      if (mb_tx_frame((const uint8 *)&arpEthPckt, sizeof(arpEthPckt), NULL,
                      0) == 0) {
        my_port.stat_sd_data += sizeof(arpEthPckt);
        waitArp = 200;
      }
      return;
    }
  }

  /* Destination MAC established: assemble header block and send. */
  {
    uint8 *work = ipEthPckt.ed;
    int16 len1, len2;

    memcpN(work, (const uint8 *)&my_port.send->hdr, sizeof(IP_HDR));
    work += sizeof(IP_HDR);
    if (my_port.send->opt_length > 0) {
      memcpN(work, (const uint8 *)my_port.send->options,
             my_port.send->opt_length);
    }

    len1 = (int16)(sizeof(ETH_HDR) + sizeof(IP_HDR)) + my_port.send->opt_length;
    len2 = my_port.send->pkt_length;
    if (mb_tx_frame((const uint8 *)&ipEthPckt, len1,
                    (const uint8 *)my_port.send->pkt_data, len2) == 0) {
      IP_DGRAM *next = my_port.send->next;
      IP_discard(my_port.send, TRUE);
      my_port.send = next;
      my_port.stat_sd_data += len1 + len2;
    }
  }
}

static void cdecl my_receive(PORT *port) {
  int16 budget;
  if (port != &my_port || my_port.active == 0) return;
  /* Consume up to a few frames per slice: after the ack the RP
     publishes the next queued frame within ~1 ms, so a short second
     look often pays off; the budget bounds our time in STinG's slice. */
  for (budget = 4; --budget >= 0;) {
    uint16 before = last_rx_seq;
    mb_rx_service();
    if (last_rx_seq == before) break;
  }
}

static int16 cdecl my_set_state(PORT *port, int16 state) {
  if (port != &my_port) return FALSE;

  if (state) {
    int16 i;
    arp_init();
    for (i = 0; i < 6; i++) my_mac[i] = mb_r8(MB_MAC_OFF + i);
    memcpN(ipEthPckt.eh.source, my_mac, 6);
    ipEthPckt.eh.type = TYPE_IP;
    memcpN(arpEthPckt.eh.source, my_mac, 6);
    memcpN(arpEthPckt.arp.src_ether, my_mac, 6);
    arpEthPckt.eh.type = TYPE_ARP;
    arpEthPckt.arp.hardware_space = ARP_HARD_ETHER;
    arpEthPckt.arp.protocol_space = TYPE_IP;
    arpEthPckt.arp.hardware_len = 6;
    arpEthPckt.arp.protocol_len = 4;
    /* Read the config block seq-stably: the cartridge bumps MB_CFG_SEQ
       after writing it, so reading the sequence either side of the
       fields catches a publish that lands mid-read. */
    {
      uint32 cfg_ip, cfg_mask, cfg_gw;
      uint16 seq, guard = 0;
      do {
        seq = mb_r16(MB_CFG_SEQ_OFF);
        cfg_ip = mb_r32(MB_CFG_IP_OFF);
        cfg_mask = mb_r32(MB_CFG_MASK_OFF);
        cfg_gw = mb_r32(MB_CFG_GW_OFF);
      } while (seq != mb_r16(MB_CFG_SEQ_OFF) && ++guard < 4);

      /* Adopt the address the cartridge chose, but only if the user has
         not set one: an explicit STNGPORT.CPX address always wins. An
         unconfigured port is 0xffffffff (see the initialiser above),
         which is also what STinG stores for "not set". */
      if ((my_port.ip_addr == 0 || my_port.ip_addr == 0xffffffffUL) &&
          cfg_ip != 0) {
        my_port.ip_addr = cfg_ip;
        if (cfg_mask != 0) my_port.sub_mask = cfg_mask;
      } else if (my_port.sub_mask == 0xffffffffUL && cfg_mask != 0) {
        my_port.sub_mask = cfg_mask;
      }
      install_routes(cfg_gw);
    }
    /* Resync the RX handshake, then latch. Order matters: HELLO first
       frees any publication stranded before we existed, and only THEN
       do we latch what is in the window, acking it so the RP is never
       left waiting on a frame we skipped. Latching before HELLO left a
       window where the RP could re-publish over a frame we had already
       started copying; latching after HELLO without acking could strand
       a publication instead. This ordering does neither. */
    mb_cmd(MBC_DRIVER_HELLO, DRIVER_VERSION_BYTE);
    last_rx_seq = mb_r16(MB_RX_SEQ_OFF);
    mb_cmd(MBC_RX_ACK, (uint16)(last_rx_seq & 0xFF));
  } else {
    doTxArp = FALSE;
    waitArp = 0;
    mb_cmd(MBC_DRIVER_BYE, 0);
    {
      IP_DGRAM *walk, *next;
      for (walk = my_port.send; walk; walk = next) {
        next = walk->next;
        IP_discard(walk, TRUE);
      }
      my_port.send = NULL;
      for (walk = my_port.receive; walk; walk = next) {
        next = walk->next;
        IP_discard(walk, TRUE);
      }
      my_port.receive = NULL;
    }
  }
  return TRUE;
}

static int16 cdecl my_cntrl(PORT *port, uint32 argument, int16 code) {
  int16 result = E_NORMAL;
  static int16 type = -1;

  if (port != &my_port) return E_PARAMETER;

  switch (code) {
    case CTL_ETHER_GET_MAC:
      memcpN((uint8 *)argument, my_mac, 6);
      break;
    case CTL_ETHER_INQ_SUPPTYPE:
      *((char ***)argument) = suppHardware;
      break;
    case CTL_ETHER_SET_TYPE:
      type = ((int16)argument) & 7;
      break;
    case CTL_ETHER_GET_TYPE:
      *((int16 *)argument) = type;
      break;
    default:
      result = E_FNAVAIL;
  }
  return result;
}

/* ---- Install ------------------------------------------------------- */

static long get_sting_cookie(void) {
  long *p;
  for (p = *(long **)0x5a0L; *p; p += 2)
    if (*p == 0x5354694BL /* 'STiK' */) return *++p;
  return 0L;
}

static void install(BASPAG *BasPag) {
  PORT *ports;
  DRIVER *driver;

  query_chains((void **)&ports, (void **)&driver, NULL);

  (my_port.driver = &my_driver)->basepage = BasPag;

  while (ports->next) ports = ports->next;
  ports->next = &my_port;

  while (driver->next) driver = driver->next;
  driver->next = &my_driver;
}

/* Entry: called by STinG (via entry.s) with the basepage as argument. */
void cdecl driver_main(BASPAG *bp) {
  static char fault[] =
      "MDNET.STX: STinG extension module. To be started by STinG!\r\n";
  static char nohw[] = "MDNET.STX: MD/Net cartridge not found.\r\n";
  static char badver[] =
      "MDNET.STX: driver/firmware version mismatch. Update both.\r\n";
  DRV_LIST *sting_drivers;
  long PgmSize = (long)bp->p_bbase + bp->p_blen - (long)bp;

  bp->p_cmdlin[1 + bp->p_cmdlin[0]] = '\0';

  if (!str_eq(bp->p_cmdlin + 1, "STinG_Load")) {
    Cconws(fault);
    goto errExit;
  }

  /* Probe: the MD/Net firmware publishes its magic in the ROM4 window.
     The version must match too -- a driver and firmware that disagree
     about the mailbox layout would fail in far more confusing ways. */
  if (mb_r32(MB_PROTO_MAGIC_OFF) != MB_PROTO_MAGIC) {
    Cconws(nohw);
    goto errExit;
  }
  if (mb_r16(MB_PROTO_VER_OFF) != MB_PROTO_VERSION) {
    Cconws(badver);
    goto errExit;
  }

  {
    int16 i;  /* publish the MAC now: STNGPORT.CPX may query it before
                 the port is ever activated */
    for (i = 0; i < 6; i++) my_mac[i] = mb_r8(MB_MAC_OFF + i);
  }

  sting_drivers = (DRV_LIST *)xbios_supexec(get_sting_cookie);
  if (sting_drivers == NULL) goto errExit;
  {
    const char *m = MAGIC;
    const char *g = sting_drivers->magic;
    for (; *m; m++, g++)
      if (*m != *g) goto errExit;
  }

  tpl = (TPL *)(*sting_drivers->get_dftab)(TRANSPORT_DRIVER);
  stx = (STX *)(*sting_drivers->get_dftab)(MODULE_DRIVER);

  if (tpl != NULL && stx != NULL) {
    install(bp);
    Ptermres(PgmSize, 0);
  }

errExit:
  Pterm(-1);
}
