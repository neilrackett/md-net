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

/* Copy buffer. The payload lives in cartridge ROM, and GEMDOS hands
   whole-sector writes straight to Rwabs -- which on the ST is a DMA
   transfer, and the DMA controller can only read DRAM. Passing it a
   cartridge address writes garbage to disk while Fwrite still reports
   success, so every byte is copied here with the CPU first. */
static char copy_buf[2048];

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
