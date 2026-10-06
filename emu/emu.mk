# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# make emu, included from the Makefile. It lives here rather than there so
# that changing it is an emulator-only change, which CI does not build or
# release (paths-ignore in .github/workflows).

## Build MD/Net for this computer and run it in Hatari with EmuMD (see
## README.md). TOS= a TOS image (default: EmuMD's EmuTOS); RECORD=FILE.avi
## records picture and sound until you quit; EMUMD= another EmuMD checkout.
EMUMD ?= emu/emumd
.PHONY: emu
emu:
	$(EMUMD)/tools/mdfw run $(if $(TOS),--tos "$(TOS)") $(if $(RECORD),--record "$(RECORD)")

## Test it there, headless: STinG installed from the cartridge on a fresh
## ST, then a page fetched through it from this computer (emu/test.sh).
.PHONY: emu-test
emu-test:
	MDFW=$(EMUMD)/tools/mdfw emu/test.sh
