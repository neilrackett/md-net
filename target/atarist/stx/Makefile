# Builds MDNET.STX (STinG port driver for the MD/Net mailbox) with the
# m68k-atari-mint GCC cross toolchain from the atarist-toolkit Docker
# image (same image the cartridge stub build uses via stcmd).
#
# -mshort       : 16-bit int, matching the Pure-C ABI the STinG SDK
#                 headers (TRANSPRT.H / PORT.H) were written for.
# -nostdlib     : no crt0 -- entry.s is the entry point and must be
#                 linked first (STinG calls the text start directly).

CC      = m68k-atari-mint-gcc
CFLAGS  = -m68000 -mshort -O2 -fomit-frame-pointer -Wall -I.
LDFLAGS = -nostdlib -Wl,--entry,_entry

OBJS = entry.o mdnet.o

MDNET.STX: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

entry.o: entry.s
	$(CC) -m68000 -c -o $@ $<

mdnet.o: mdnet.c transprt.h port.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) MDNET.STX
