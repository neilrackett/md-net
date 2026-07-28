| Copyright (C) 2026 Neil Rackett
| SPDX-License-Identifier: GPL-3.0-or-later
|
| INSTALL.PRG entry stub. TOS enters a PRG at the start of its text
| segment, so this must be linked first. It switches to our own stack
| before calling C: a freshly launched program otherwise runs on the
| parent's stack, whose size depends on whichever desktop or shell
| started us. Then Pterm0, so no C startup code (and no libc) is needed.
	.text
	.globl	_instentry
_instentry:
	lea	stack_top,sp
	pea	workspace		| install_main takes its scratch space from
	jsr	_install_main		| the caller: it keeps nothing writable of
	addq.l	#4,sp			| its own, so the same code can run from ROM
	clr.w	-(sp)			| Pterm0
	trap	#1

	.bss
	.even
stack_area:
	.space	4096
stack_top:
workspace:
	.space	8240			| sizeof(WORK): 44 + 8192, rounded up
