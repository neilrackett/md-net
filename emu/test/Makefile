#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# STNGTEST.TOS, for emu/test.sh. Build from the repo root:
#   STCMD_NO_TTY=1 ST_WORKING_FOLDER=$PWD stcmd make -C emu/test
#

STX_DIR = ../../target/atarist/stx
DIST_DIR = ./dist
BIN = STNGTEST.TOS

CC = m68k-atari-mint-gcc
# -mshort: STinG's API takes Pure C's 16-bit ints. No C library, as for
# MDNET.STX: entry.s starts it.
CFLAGS = -m68000 -mshort -std=gnu99 -O2 -fomit-frame-pointer -Wall -I$(STX_DIR)
LDFLAGS = -nostdlib -Wl,--entry,_entry

.PHONY: all clean

all: $(DIST_DIR)/$(BIN)

$(DIST_DIR):
	mkdir -p $@

$(DIST_DIR)/$(BIN): entry.s stngtest.c $(STX_DIR)/TRANSPRT.H $(STX_DIR)/PORT.H | $(DIST_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) entry.s stngtest.c -lgcc -o $@

clean:
	rm -rf $(DIST_DIR)
