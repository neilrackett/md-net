| Copyright (C) 2026 Neil Rackett
| SPDX-License-Identifier: GPL-3.0-or-later
|
| STNGTEST.TOS's entry, as INSTALL.TOS's: TOS enters a program at the
| start of its text, so this is linked first. Our own stack, the test,
| then Pterm0: no C startup code, and no C library.
	.text
	.globl	_entry
_entry:
	lea	stack_top,sp
	jsr	_test_main
	clr.w	-(sp)			| Pterm0
	trap	#1

	.bss
	.even
stack_area:
	.space	8192
stack_top:
