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
	@# Position-independence gate, in two parts. objdump -r on the LINKED
	@# a.out-mintprg shows nothing even for absolute code (proven with a
	@# deliberately non-PIC test build), so a linked-file check is a
	@# no-op. What does work:
	@#  1. absolute relocations (type 32) in the OBJECTS -- present the
	@#     moment -mpcrel is dropped;
	@#  2. nonzero .data/.bss in the linked image -- present the moment a
	@#     writable static creeps back (which with -mpcrel emits NO
	@#     relocation at all, so only a section check can catch it; and
	@#     objcopy would silently drop the .bss, corrupting the workspace
	@#     that follows the blob).
	@if m68k-atari-mint-objdump -r instcart.o installpic.o 2>/dev/null \
	    | grep -qE "^[0-9a-f]{8} +32"; then \
	  echo "ERROR: cartridge installer has absolute relocations; it"; \
	  echo "       cannot be copied to RAM and run. Was -mpcrel dropped?"; \
	  exit 1; \
	fi
	@if m68k-atari-mint-objdump -h instcart.prg 2>/dev/null \
	    | awk '($$2==".data"||$$2==".bss") && $$3 !~ /^0+$$/ {bad=1} \
	           END {exit bad}'; \
	  then :; else \
	  echo "ERROR: cartridge installer has writable data (.data/.bss)."; \
	  echo "       All mutable state must live in the WORK struct."; \
	  exit 1; \
	fi

entry.o: entry.s
	$(CC) -m68000 -c -o $@ $<

mdnet.o: mdnet.c TRANSPRT.H PORT.H
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(INST_OBJS) installpic.o instcart.o instcart.prg MDNET.STX INSTALL.TOS INSTCART.BIN
