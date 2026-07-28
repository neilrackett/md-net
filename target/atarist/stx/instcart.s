| Copyright (C) 2026 Neil Rackett
| SPDX-License-Identifier: GPL-3.0-or-later
|
| Cartridge entry for the installer.
|
| The desktop launches a cartridge entry by calling its CA_RUN address
| and expects a plain return, so this must not terminate the process
| the way a .PRG does. It also cannot rely on any writable data of its
| own: this code is copied to RAM by the launcher in main.s, but its
| workspace has to come from somewhere writable regardless, so the
| launcher passes one in.
|
| On entry: 4(sp) = pointer to the workspace (WORK), supplied by the
| launcher. Registers are preserved for the caller.
	.text
	.globl	_instcart
_instcart:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	11*4+4(sp),d0		| the workspace pointer
	move.l	d0,-(sp)
	bsr	_install_main		| PC-relative: this code runs wherever it is copied
	addq.l	#4,sp
	movem.l	(sp)+,d2-d7/a2-a6
	rts
