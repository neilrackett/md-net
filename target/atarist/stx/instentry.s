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
	jsr	_install_main
	clr.w	-(sp)			| Pterm0
	trap	#1

	.bss
	.even
stack_area:
	.space	4096
stack_top:
