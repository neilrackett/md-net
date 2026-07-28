#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#

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
LDFLAGS = -nostdlib

OBJS = entry.o mdnet.o
INST_OBJS = instentry.o install.o

# Stamped into the installer banner so it always matches the firmware
# it shipped with; build.sh passes the real value.
MDNET_VERSION ?= v0.0.0

all: MDNET.STX INSTALL.TOS INSTCART.BIN

MDNET.STX: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,--entry,_entry -o $@ $(OBJS)

# The installer is a plain TOS program: default int size (it does not
# share the STinG SDK's Pure-C ABI) and osbind.h's inline traps, so
# -nostdlib still links.
INSTALL.TOS: $(INST_OBJS)
	$(CC) $(LDFLAGS) -Wl,--entry,_instentry -o $@ $(INST_OBJS)

instentry.o: instentry.s
	$(CC) -m68000 -c -o $@ $<

install.o: install.c
	$(CC) -m68000 -O2 -fomit-frame-pointer -Wall \
	      -DMDNET_VERSION='"$(MDNET_VERSION)"' -c -o $@ $<

# Cartridge build: -mpcrel makes every reference PC-relative, so the
# result can be copied anywhere in RAM and run. That only holds while
# the code keeps no writable data of its own, which is why install.c
# threads a workspace through instead of using statics -- the check
# below fails the build if a relocation ever creeps back in.
installpic.o: install.c
	$(CC) -m68000 -mpcrel -O2 -fomit-frame-pointer -Wall \
	      -DMDNET_VERSION='"$(MDNET_VERSION)"' -c -o $@ $<

instcart.o: instcart.s
	$(CC) -m68000 -mpcrel -c -o $@ $<

INSTCART.BIN: instcart.o installpic.o
	$(CC) $(LDFLAGS) -Wl,--entry,_instcart -o instcart.prg \
	      instcart.o installpic.o
	m68k-atari-mint-objcopy -O binary instcart.prg $@
	@if m68k-atari-mint-objdump -r instcart.prg 2>/dev/null \
	    | grep -qE "^[0-9a-f]{8} "; then \
	  echo "ERROR: cartridge installer needs relocation; it cannot be"; \
	  echo "       copied to RAM and run. Check install.c has no"; \
	  echo "       writable statics and everything built -mpcrel."; \
	  exit 1; \
	fi

entry.o: entry.s
	$(CC) -m68000 -c -o $@ $<

mdnet.o: mdnet.c TRANSPRT.H PORT.H
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(INST_OBJS) installpic.o instcart.o instcart.prg MDNET.STX INSTALL.TOS INSTCART.BIN
