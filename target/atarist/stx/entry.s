| Copyright (C) 2026 Neil Rackett
| SPDX-License-Identifier: GPL-3.0-or-later

| MDNET.STX entry stub. STinG Pexec-loads the module and calls the start
| of the text segment with the basepage pointer as a C argument. This
| stub must be linked FIRST so it sits at the text start; it forwards
| the call (stack unchanged: 4(sp) = basepage) to the C entry point.
	.text
	.globl	_entry
_entry:
	jmp	_driver_main
