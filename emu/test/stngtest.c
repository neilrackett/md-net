/*
 * Copyright (C) 2026 Neil Rackett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * STNGTEST.TOS -- MD/Net tested under EmuMD (emu/test.sh runs it): fetches
 * a web page through STinG and MDNET.STX, from the server C:\TEST.TXT
 * names ("a.b.c.d port /path"), and writes the reply, or what went wrong,
 * to C:\RESULT.TXT, with STinG's ports after it. "done" ends it.
 *
 * Built with -mshort, as STinG's API takes Pure C's 16-bit ints, and so,
 * like MDNET.STX and INSTALL.TOS, without the C library (its -mshort
 * build is not to be trusted with varargs): its own traps and number
 * formatting.
 */

#define cdecl /* Pure C's keyword; gcc's default calling convention matches */
#define NULL ((void *)0)

/* STinG's headers point to a basepage, by Pure C's name for it. */
typedef struct baspag BASPAG;

#include "TRANSPRT.H"
#include "PORT.H"

DRV_LIST *drivers;
TPL *tpl;
STX *stx;

/* ---- TOS --------------------------------------------------------- */

static long trap1_wl(int16 fn, int16 w, long l) { /* fn(w, l) */
  register long ret __asm__("d0");
  __asm__ volatile("move.l %3,-(%%sp)\n\tmove.w %2,-(%%sp)\n\tmove.w %1,-(%%sp)\n\t"
                   "trap #1\n\taddq.l #8,%%sp"
                   : "=r"(ret) : "r"(fn), "r"(w), "r"(l)
                   : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
  return ret;
}

static long trap1_lw(int16 fn, long l, int16 w) { /* fn(l, w) */
  register long ret __asm__("d0");
  __asm__ volatile("move.w %3,-(%%sp)\n\tmove.l %2,-(%%sp)\n\tmove.w %1,-(%%sp)\n\t"
                   "trap #1\n\taddq.l #8,%%sp"
                   : "=r"(ret) : "r"(fn), "r"(l), "r"(w)
                   : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
  return ret;
}

static long trap1_wll(int16 fn, int16 w, long l1, long l2) { /* fn(w, l1, l2) */
  register long ret __asm__("d0");
  __asm__ volatile("move.l %4,-(%%sp)\n\tmove.l %3,-(%%sp)\n\tmove.w %2,-(%%sp)\n\t"
                   "move.w %1,-(%%sp)\n\ttrap #1\n\tlea 12(%%sp),%%sp"
                   : "=r"(ret) : "r"(fn), "r"(w), "r"(l1), "r"(l2)
                   : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
  return ret;
}

static long trap14_l(int16 fn, long l) { /* fn(l) */
  register long ret __asm__("d0");
  __asm__ volatile("move.l %2,-(%%sp)\n\tmove.w %1,-(%%sp)\n\ttrap #14\n\taddq.l #6,%%sp"
                   : "=r"(ret) : "r"(fn), "r"(l)
                   : "d1", "d2", "a0", "a1", "a2", "cc", "memory");
  return ret;
}

static void vsync(void) {
  __asm__ volatile("move.w #37,-(%%sp)\n\ttrap #14\n\taddq.l #2,%%sp"
                   ::: "d0", "d1", "d2", "a0", "a1", "a2", "cc", "memory");
}

#define Fcreate(name) trap1_lw(60, (long)(name), 0)
#define Fopen(name) trap1_lw(61, (long)(name), 0)
#define Fclose(h) trap1_wl(62, (h), 0) /* the long is ignored */
#define Fread(h, n, buf) trap1_wll(63, (h), (n), (long)(buf))
#define Fwrite(h, n, buf) trap1_wll(64, (h), (n), (long)(buf))
#define Supexec(fn) trap14_l(38, (long)(fn))

/* ---- Output --------------------------------------------------------- */

static int16 out = -1;

static int16 length(const char *s) {
  int16 n = 0;
  while (s[n]) n++;
  return n;
}

static void put(const char *s) {
  if (out >= 0) Fwrite(out, length(s), s);
}

static void putn(uint32 n) {
  char buf[12], *p = buf + sizeof(buf) - 1;
  *p = 0;
  do {
    *--p = (char)('0' + n % 10);
    n /= 10;
  } while (n);
  put(p);
}

static void put_ip(uint32 ip) {
  putn((ip >> 24) & 255), put("."), putn((ip >> 16) & 255), put(".");
  putn((ip >> 8) & 255), put("."), putn(ip & 255);
}

static void put_error(const char *what, int16 err) {
  put(what), put(": "), put(get_err_text(err)), put("\r\n");
}

/* STinG's ports, as it sees them. */
static void ports(void) {
  PORT *p = NULL;
  int16 n = 0;
  query_chains((void **)&p, NULL, NULL);
  for (; p && n < 8; p = p->next, n++) {
    put("port "), put(p->name), put(p->active ? " active " : " inactive ");
    put_ip(p->ip_addr), put(" sent "), putn((uint32)p->stat_sd_data);
    put(" received "), putn((uint32)p->stat_rcv_data), put("\r\n");
  }
}

/* ---- The test ------------------------------------------------------- */

static long sting_cookie(void) {
  long *p;
  for (p = *(long **)0x5a0L; *p; p += 2)
    if (*p == 0x5354694BL /* 'STiK' */) return *++p;
  return 0L;
}

static const char *number(const char *s, uint32 *n) {
  *n = 0;
  while (*s >= '0' && *s <= '9') *n = *n * 10 + (uint32)(*s++ - '0');
  return s;
}

static char reply[4096];

static void fetch(uint32 ip, uint16 port, const char *path) {
  int16 cn = TCP_open(ip, port, 0, 2000);
  if (cn < 0) {
    put_error("TCP_open", cn);
    return;
  }
  int16 rc = TCP_wait_state(cn, TESTABLISH, 30);
  if (rc < 0) {
    put_error("TCP_wait_state", rc);
    TCP_close(cn, 0, NULL);
    return;
  }

  static char request[200];
  char *r = request;
  const char *parts[] = {"GET ", path, " HTTP/1.0\r\n\r\n"};
  int16 i;
  for (i = 0; i < 3; i++) {
    const char *s = parts[i];
    while (*s) *r++ = *s++;
  }
  rc = TCP_send(cn, request, (int16)(r - request));
  if (rc < 0) {
    put_error("TCP_send", rc);
    TCP_close(cn, 0, NULL);
    return;
  }

  /* Until the server closes the connection, or 30 seconds. */
  int16 len = 0, frames;
  for (frames = 0; frames < 30 * 50; frames++) {
    int16 n = CNbyte_count(cn);
    if (n == E_EOF) break;
    if (n > 0) {
      if (n > (int16)sizeof(reply) - 1 - len) n = (int16)sizeof(reply) - 1 - len;
      if (n > 0 && CNget_block(cn, reply + len, n) == n) len += n;
      if (len == (int16)sizeof(reply) - 1) break;
      continue;
    }
    if (n < 0 && n != E_NODATA) {
      put_error("CNbyte_count", n);
      break;
    }
    vsync();
  }
  TCP_close(cn, 0, NULL);
  reply[len] = 0;
  put(reply);
}

void test_main(void) {
  static char spec[128];
  out = (int16)Fcreate("C:\\RESULT.TXT");
  long in = Fopen("C:\\TEST.TXT");
  long got = in >= 0 ? Fread((int16)in, sizeof(spec) - 1, spec) : -1;
  if (in >= 0) Fclose((int16)in);
  if (out < 0) return;

  /* "a.b.c.d port /path" */
  uint32 a, b, c, d, port;
  char *s = spec, *path;
  if (got > 0) spec[got] = 0;
  s = (char *)number(s, &a) + 1, s = (char *)number(s, &b) + 1;
  s = (char *)number(s, &c) + 1, s = (char *)number(s, &d) + 1;
  s = (char *)number(s, &port) + 1;
  path = s;
  while (*s > ' ') s++;
  *s = 0;

  drivers = (DRV_LIST *)Supexec(sting_cookie);
  if (got <= 0 || *path != '/') {
    put("no C:\\TEST.TXT\r\n");
  } else if (!drivers) {
    put("no STinG\r\n");
  } else if (!(tpl = (TPL *)get_dftab(TRANSPORT_DRIVER)) ||
             !(stx = (STX *)get_dftab(MODULE_DRIVER))) {
    put("no STinG layers\r\n");
  } else {
    fetch((a << 24) | (b << 16) | (c << 8) | d, (uint16)port, path);
    put("\r\n");
    ports();
  }
  put("done\r\n");
  Fclose(out);
}
