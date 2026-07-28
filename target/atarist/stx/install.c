/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * INSTALL.PRG -- installs the MD/Net STinG driver on an Atari ST.
 *
 * The files it installs are not shipped alongside it: the MD/Net
 * firmware publishes them in the cartridge ROM window, so this program
 * is self-contained and always installs the driver matching the
 * firmware that is actually running. Layout is written by
 * tools/mkpayload.py and staged by rp/src/payload.c.
 *
 * Output is kept within 38 columns so it reads correctly in ST low
 * resolution as well as medium/high.
 */

#include <osbind.h>

#define ROM4_BASE 0xFA0000UL
#define PAYLOAD_OFF 0x6000UL
#define PAYLOAD_MAGIC 0x4D444E50UL /* 'MDNP' */
#define PAYLOAD_VERSION 1

#define MB_CFG_IP_OFF 0x4032UL
#define MB_CFG_MASK_OFF 0x4036UL
#define MB_CFG_GW_OFF 0x403AUL
#define MB_CFG_DNS_OFF 0x403EUL

#define ENT_SIZE 24  /* name[16] + offset(4) + length(4) */
#define ENT_NAME 16

#ifndef MDNET_VERSION
#define MDNET_VERSION "v0.0.0"
#endif

/* ---- cartridge reads ------------------------------------------- */

static unsigned char rd8(unsigned long off) {
  return *(volatile unsigned char *)(ROM4_BASE + off);
}
static unsigned short rd16(unsigned long off) {
  return *(volatile unsigned short *)(ROM4_BASE + off);
}
static unsigned long rd32(unsigned long off) {
  return *(volatile unsigned long *)(ROM4_BASE + off);
}

/* ---- tiny string helpers (no stdlib) --------------------------- */

static void strcpy_(char *d, const char *s) {
  while ((*d++ = *s++) != '\0');
}
static void strcat_(char *d, const char *s) {
  while (*d) d++;
  strcpy_(d, s);
}
static void say(const char *s) { (void)Cconws(s); }

/* ---- filesystem ------------------------------------------------- */

static char dta_buf[44];

/* Scratch buffer, also used to stage payload writes: the payload lives
   in cartridge ROM, and GEMDOS hands whole-sector writes straight to
   Rwabs -- a DMA transfer on the ST, and the DMA controller can only
   read DRAM. Passing it a cartridge address would write garbage to disk
   while Fwrite still reported success, so bytes are copied here with
   the CPU first. */
static char copy_buf[8192];

/* Does `path` exist? attr 0x10 also matches subdirectories, so this
   serves for both the STING folder and the files inside it. */
static short exists(const char *path) {
  void *save = (void *)Fgetdta();
  short found;
  /* mintlib's Fsetdta macro casts to long, so no DTA type needed. */
  Fsetdta(dta_buf);
  found = (Fsfirst(path, 0x10) == 0L);
  Fsetdta(save);
  return found;
}

/* Locate a STinG installation. Drives A: and B: are skipped so a
   floppyless machine is never asked to insert a disk. */
static short find_sting(char *out) {
  char probe[16];
  char drive;
  for (drive = 'C'; drive <= 'P'; drive++) {
    probe[0] = drive;
    probe[1] = ':';
    probe[2] = '\\';
    strcpy_(probe + 3, "STING");
    if (exists(probe)) {
      strcpy_(out, probe);
      return 1;
    }
  }
  return 0;
}

/* Write one payload file into the STinG folder. */
static short write_file(const char *folder, const char *name,
                        unsigned long off, unsigned long len) {
  char path[64];
  long fh;
  unsigned long done = 0;

  strcpy_(path, folder);
  strcat_(path, "\\");
  strcat_(path, name);

  fh = Fcreate(path, 0);
  if (fh < 0) return 0;

  while (done < len) {
    unsigned long chunk = len - done;
    unsigned long i;
    if (chunk > sizeof(copy_buf)) chunk = sizeof(copy_buf);
    for (i = 0; i < chunk; i++) {
      copy_buf[i] = (char)rd8(off + done + i);  /* CPU read, not DMA */
    }
    if (Fwrite((short)fh, (long)chunk, copy_buf) != (long)chunk) {
      Fclose((short)fh);
      return 0;
    }
    done += chunk;
  }

  Fclose((short)fh);
  return 1;
}

/* If a NetUSBee/EtherNEC driver is present, disable it by renaming to
   .ST_ -- the same convention STinG users already use for drivers they
   want to keep but not load. Two active Ethernet drivers is the most
   common way to end up with a port that never works. */
static void disable_enec(const char *folder) {
  char from[64], to[64];
  strcpy_(from, folder);
  strcat_(from, "\\ENEC.STX");
  if (!exists(from)) return;
  strcpy_(to, folder);
  strcat_(to, "\\ENEC.ST_");
  /* Say so when it cannot be done: silently leaving two Ethernet
     drivers active is the failure this is meant to prevent, and the
     user would have no idea. */
  if (exists(to) || Frename(0, from, to) != 0L) {
    say("\r\nNOTE: could not disable ENEC.STX.\r\n");
    say("Rename it by hand or the WiFi port\r\n");
    say("may not work.\r\n");
    return;
  }
  say("Disabled ENEC.STX (now ENEC.ST_)\r\n");
}

/* ---- ROUTE.TAB --------------------------------------------------- */

/* Append a decimal number, returning the new end of the string. Takes
   a short deliberately: every value here is one octet, and 32-bit
   division on a 68000 would pull in libgcc helpers that -nostdlib
   cannot resolve. */
static char *put_num(char *d, unsigned short v) {
  char tmp[4];
  short n = 0;
  do {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v && n < 4);
  while (n > 0) *d++ = tmp[--n];
  return d;
}

static char *put_ip(char *d, unsigned long ip) {
  d = put_num(d, (unsigned short)((ip >> 24) & 0xFF)); *d++ = '.';
  d = put_num(d, (unsigned short)((ip >> 16) & 0xFF)); *d++ = '.';
  d = put_num(d, (unsigned short)((ip >> 8) & 0xFF));  *d++ = '.';
  d = put_num(d, (unsigned short)(ip & 0xFF));
  return d;
}

/* One ROUTE.TAB line: network, mask, port, gateway. STinG splits on
   spaces or tabs and matches the port name exactly, so "WiFi" must be
   spelled just so; tabs are used because every STinG accepts them. */
static char *put_route(char *d, unsigned long net, unsigned long mask,
                       unsigned long gate) {
  d = put_ip(d, net);   *d++ = '\t';
  d = put_ip(d, mask);  *d++ = '\t';
  strcpy_(d, "WiFi");   d += 4; *d++ = '\t';
  d = put_ip(d, gate);
  *d++ = '\r'; *d++ = '\n';
  return d;
}

/* Give STinG the routes for our port. Without them the WiFi port has an
   address but no way to reach anything, and STinG Port Setup complains
   when there is no ROUTE.TAB at all. An existing file that already
   mentions the port is left completely alone -- the user's routing is
   their business. */
static void write_routes(const char *folder) {
  char path[64];
  char text[160];
  char *end = text;
  unsigned long ip, mask, gw;
  long fh, len;

  ip = rd32(MB_CFG_IP_OFF);
  mask = rd32(MB_CFG_MASK_OFF);
  gw = rd32(MB_CFG_GW_OFF);
  if (ip == 0 || mask == 0) {
    /* The cartridge had not chosen an address yet -- say so, rather
       than let the summary imply routing was set up. */
    say("\r\nNOTE: no address from the cartridge\r\n");
    say("yet, so ROUTE.TAB was not written.\r\n");
    return;
  }

  strcpy_(path, folder);
  strcat_(path, "\\ROUTE.TAB");

  if (exists(path)) {
    /* Already routed for us? Then leave it be. */
    fh = Fopen(path, 0);
    if (fh < 0) return;
    len = Fread((short)fh, (long)sizeof(copy_buf) - 1, copy_buf);
    Fclose((short)fh);
    if (len < 0) return;
    copy_buf[len] = '\0';
    {
      char *p;
      for (p = copy_buf; *p; p++) {
        if (p[0] == 'W' && p[1] == 'i' && p[2] == 'F' && p[3] == 'i') return;
      }
    }
    /* Present but not ours: append rather than replace. */
    fh = Fopen(path, 2); /* read/write, does not truncate */
    if (fh < 0) return;
    len = Fseek(0L, (short)fh, 2); /* to the end; returns the size */
    if (len > 0) {
      /* Start on a fresh line. Hand-edited files often have no trailing
         newline, and appending straight onto the last line would
         corrupt the user's route as well as losing ours. */
      char last = '\n';
      Fseek(len - 1L, (short)fh, 0);
      if (Fread((short)fh, 1L, &last) == 1L && last != '\n') {
        *end++ = '\r';
        *end++ = '\n';
      }
      Fseek(0L, (short)fh, 2);
    }
    end = put_route(end, ip & mask, mask, 0);
    if (gw != 0) end = put_route(end, 0, 0, gw);
    if (Fwrite((short)fh, (long)(end - text), text) == (long)(end - text)) {
      say("Added WiFi to ROUTE.TAB\r\n");
    }
    Fclose((short)fh);
    return;
  }

  strcpy_(text, "# Written by MD/Net\r\n");
  end = text;
  while (*end) end++;
  end = put_route(end, ip & mask, mask, 0);
  if (gw != 0) end = put_route(end, 0, 0, gw);

  fh = Fcreate(path, 0);
  if (fh < 0) return;
  if (Fwrite((short)fh, (long)(end - text), text) == (long)(end - text)) {
    say("Wrote ROUTE.TAB\r\n");
  }
  Fclose((short)fh);
}

/* ---- DEFAULT.CFG -------------------------------------------------- */

/* Read a whole file into copy_buf, NUL-terminated. Returns its length,
   or -1. */
static long slurp(const char *path) {
  long fh, len;
  fh = Fopen(path, 0);
  if (fh < 0) return -1L;
  len = Fread((short)fh, (long)sizeof(copy_buf) - 1, copy_buf);
  Fclose((short)fh);
  if (len < 0) return -1L;
  copy_buf[len] = '\0';
  return len;
}

static char upper(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Is NAMESERVER already set to something?

   This has to be a line scan, not a substring search: DEFAULT.CFG
   explains NAMESERVER in its own comments, so the word appears several
   times in text STinG ignores.

   The rules are STinG's own, taken from its config loader rather than
   guessed: a setting is only recognised when its name starts at the
   very beginning of a line (an indented line is skipped, which is also
   why "# NAMESERVER ..." in a comment cannot match), and names are
   compared case-insensitively. Matching those exactly means we agree
   with STinG about what counts as "already set". */
static short has_nameserver(void) {
  char *p = copy_buf;
  while (*p) {
    char *line = p;
    while (*p && *p != '\r' && *p != '\n') p++;
    {
      const char *want = "NAMESERVER";
      char *w = line;
      const char *n = want;
      while (*n && w < p && upper(*w) == *n) { w++; n++; }
      if (*n == '\0') {
        while (w < p && (*w == ' ' || *w == '\t')) w++;
        if (w < p && *w == '=') {
          w++;
          while (w < p && (*w == ' ' || *w == '\t')) w++;
          if (w < p && *w >= '0' && *w <= '9') return 1;  /* has a value */
        }
      }
    }
    while (*p == '\r' || *p == '\n') p++;
  }
  return 0;
}

/* Point STinG's resolver at the DNS server the cartridge is using, so
   host names work without the user editing anything. Only ever appends,
   and only when nothing is set: a nameserver the user chose is theirs
   to keep. DEFAULT.CFG is never created from scratch -- STinG needs a
   complete one, and inventing a minimal file would do more harm than
   the missing line. */
static void write_nameserver(const char *folder) {
  char path[64];
  char text[48];
  char *end = text;
  unsigned long dns;
  long fh, len;

  dns = rd32(MB_CFG_DNS_OFF);
  if (dns == 0) {
    /* Say so: silently skipping looks identical to having worked, and
       the symptom (host names fail, numeric addresses work) is easy to
       mistake for a network fault. */
    say("\r\nNOTE: no DNS from the cartridge, so\r\n");
    say("NAMESERVER was not set.\r\n");
    return;
  }

  strcpy_(path, folder);
  strcat_(path, "\\DEFAULT.CFG");
  if (!exists(path)) return;

  len = slurp(path);
  if (len < 0) return;
  if (has_nameserver()) return;

  fh = Fopen(path, 2); /* read/write, does not truncate */
  if (fh < 0) return;
  len = Fseek(0L, (short)fh, 2);
  if (len > 0) {
    char last = '\n';
    Fseek(len - 1L, (short)fh, 0);
    if (Fread((short)fh, 1L, &last) == 1L && last != '\n') {
      *end++ = '\r';
      *end++ = '\n';
    }
    Fseek(0L, (short)fh, 2);
  }
  strcpy_(end, "NAMESERVER  = ");
  while (*end) end++;
  end = put_ip(end, dns);
  *end++ = '\r';
  *end++ = '\n';
  if (Fwrite((short)fh, (long)(end - text), text) == (long)(end - text)) {
    say("Set NAMESERVER in DEFAULT.CFG\r\n");
  }
  Fclose((short)fh);
}

/* ---- main ------------------------------------------------------- */

void install_main(void) {
  char folder[16];
  unsigned short count, i;
  short installed = 0, failed = 0;

  say("\r\nMD/Net " MDNET_VERSION
      " (c)2026 Neil Rackett\r\n");
  say("GPLv3 neilrackett.com/atarist\r\n\r\n");

  if (rd32(PAYLOAD_OFF) != PAYLOAD_MAGIC ||
      rd16(PAYLOAD_OFF + 4) != PAYLOAD_VERSION) {
    say("MD/Net cartridge not found.\r\n\r\n");
    say("Check the cartridge is fitted and\r\n");
    say("the ST was powered off and on,\r\n");
    say("not just reset.\r\n");
    goto done;
  }

  if (!find_sting(folder)) {
    say("No STING folder found.\r\n\r\n");
    say("Install STinG first, from:\r\n");
    say("hardware.atari.org/files/sfl.zip\r\n");
    goto done;
  }

  say("Installing to ");
  say(folder);
  say("\r\n");

  count = rd16(PAYLOAD_OFF + 6);
  for (i = 0; i < count; i++) {
    unsigned long e = PAYLOAD_OFF + 8 + ((unsigned long)i * ENT_SIZE);
    char name[ENT_NAME + 1];
    unsigned short n;
    for (n = 0; n < ENT_NAME; n++) name[n] = (char)rd8(e + n);
    name[ENT_NAME] = '\0';
    if (write_file(folder, name, rd32(e + 16), rd32(e + 20))) {
      say("  ");
      say(name);
      say("\r\n");
      installed++;
    } else {
      say("  FAILED: ");
      say(name);
      say("\r\n");
      failed++;
    }
  }

  if (failed == 0 && installed > 0) {
    disable_enec(folder);
    write_routes(folder);
    write_nameserver(folder);
    say("\r\nDone. Now:\r\n");
    say("1. Reboot your ST.\r\n");
    say("2. Open STinG Port Setup and set\r\n");
    say("   up the port named \"WiFi\".\r\n");
    say("\r\nSee MDNET.TXT for the details.\r\n");
  } else if (failed) {
    say("\r\nInstall failed. Is the disk\r\n");
    say("write protected or full?\r\n");
  }

done:
  say("\r\nPress any key.\r\n");
  Cconin();
}
