; MD/Net cartridge boot code
; (C) 2023-2026 by Diego Parrilla / Neil Rackett
; License: GPL v3

; Some technical info about the header format https://www.atari-forum.com/viewtopic.php?t=14086

; $FA0000 - CA_MAGIC. Magic number, always $abcdef42 for ROM cartridge. There is a special magic number for testing: $fa52235f.
; $FA0004 - CA_NEXT. Address of next program in cartridge, or 0 if no more.
; $FA0008 - CA_INIT. Address of optional init. routine. See below for details.
; $FA000C - CA_RUN. Address of program start. All optional inits are done before. This is required only if program runs under GEMDOS.
; $FA0010 - CA_TIME. File's time stamp. In GEMDOS format.
; $FA0012 - CA_DATE. File's date stamp. In GEMDOS format.
; $FA0014 - CA_SIZE. Lenght of app. in bytes. Not really used.
; $FA0018 - CA_NAME. DOS/TOS filename 8.3 format. Terminated with 0 .

; CA_INIT holds address of optional init. routine. Bits 24-31 aren't used for addressing, and ensure in which moment by system init prg. will be initialized and/or started. Bits have following meanings, 1 means execution:
; bit 24: Init. or start of cartridge SW after succesfull HW init. System variables and vectors are set, screen is set, Interrupts are disabled - level 7.
; bit 25: As by bit 24, but right after enabling interrupts on level 3. Before GEMDOS init.
; bit 26: System init is done until setting screen resolution. Otherwise as bit 24.
; bit 27: After GEMDOS init. Before booting from disks.
; bit 28: -
; bit 29: Program is desktop accessory - ACC .
; bit 30: TOS application .
; bit 31: TTP

ROM4_ADDR			equ $FA0000

; Shared 64 KB region layout (must match rp/src/include/cart_shared.h).
;
;   $FA0000  CARTRIDGE			m68k header + code (max 16 KB)
;   $FA4000  CMD_MAGIC_SENTINEL_ADDR	4 B  (RP->m68k command word; unused
;					      this milestone)
;   $FA4004  (reserved)			12 B
;   $FA4010  SHARED_VARIABLES		240 B (60 x 4-byte slots. Slot 0 =
;					      MDNET_STATUS; the rest are
;					      app-free.)
;   $FA4100  MDNET_MSG_ADDR		256 B (NUL-terminated boot message
;					      composed by the RP)
;   $FA4200  APP_FREE_ADDR		      (free arena to end of region)
;   $FAFFFF  end of region

CARTRIDGE_CODE_SIZE	equ $4000	; 16 KB max for cartridge header + code
SHARED_BLOCK_ADDR	equ (ROM4_ADDR + CARTRIDGE_CODE_SIZE)		; $FA4000
CMD_MAGIC_SENTINEL_ADDR	equ SHARED_BLOCK_ADDR				; $FA4000
SHARED_VARIABLES	equ (SHARED_BLOCK_ADDR + $10)			; $FA4010

; MD/Net boot status, published by the RP into SHARED_VARIABLES slot 0
; (written RP-side via cart_asM68kLong, so move.l reads the true value).
MDNET_STATUS_ADDR	equ SHARED_VARIABLES				; $FA4010
MDNET_ST_BOOTING	equ 0
MDNET_ST_CONNECTING	equ 1
MDNET_ST_CONNECTED	equ 2
MDNET_ST_FAILED		equ 3

; Boot message composed by the RP (byte-pair-swapped on write so the
; m68k reads a normal C string). Printed verbatim via GEMDOS Cconws.
MDNET_MSG_ADDR		equ (SHARED_BLOCK_ADDR + $100)			; $FA4100

APP_FREE_ADDR		equ (SHARED_BLOCK_ADDR + $200)			; $FA4200

; RP->m68k command sentinel values (kept for future use).
CMD_NOP				equ 0		; No operation command
CMD_RESET			equ 1		; Reset command
CMD_BOOT_GEM		equ 2		; Boot GEM command
CMD_START			equ 4		; Hand control to the user firmware

; The RP always writes a terminal status (CONNECTED or FAILED) within
; ~60 s worst case (2 x 30 s connect attempts); this backstop only
; fires if the RP truly hung. 13000 ticks of the 200 Hz system timer
; = 65 s.
MDNET_TIMEOUT_TICKS	equ 13000
_hz_200				equ $4BA	; 200 Hz system timer tick counter

SCREEN_SIZE			equ (-4096)	; Use the memory before the screen memory to store the copied code


	include inc/sidecart_macros.s
	include inc/tos.s


; XBIOS Get Screen Base
; Return the screen memory address in D0
get_screen_base		macro
					move.w #2,-(sp)
					trap #14
					addq.l #2,sp
					endm

	section

;Rom cartridge
; The cartridge image (header + code below) MUST fit in
; CARTRIDGE_CODE_SIZE = $4000 (16 KB). The hard limit is enforced by
; target/atarist/build.sh after vlink emits BOOT.BIN; any direct vasm /
; vlink invocation that bypasses the build script is unchecked, so keep
; an eye on BOOT.BIN's size when iterating outside ./build.sh.

	org ROM4_ADDR

	dc.l $abcdef42 					; magic number
first:
	dc.l 0
	dc.l $08000000 + pre_auto		; After GEMDOS init (before booting from disks)
	dc.l 0
	dc.w GEMDOS_TIME 				;time
	dc.w GEMDOS_DATE 				;date
	dc.l end_pre_auto - pre_auto
	dc.b "MDNET",0
    even

pre_auto:
; Relocate the content of the cartridge ROM to the RAM

; Get the screen memory address to display
	get_screen_base
	move.l d0, a2

	lea SCREEN_SIZE(a2), a2		; Move to the work area just after the screen memory
	move.l a2, a3				; Save the relocation destination address in A3
	; Copy the code out of the ROM to avoid unstable behavior
    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1    ; a1 points to the start of the code in ROM
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
	jmp (a3)

start_rom_code:
; Announce ourselves, then wait for the RP to finish joining the WiFi
; network. The RP publishes a terminal status (CONNECTED / FAILED) in
; MDNET_STATUS_ADDR plus a printable message in MDNET_MSG_ADDR; both
; are served from the cart bus by PIO+DMA, so polling here never
; starves the RP while it is busy connecting. GEMDOS is up (CA_INIT
; bit 27), so Cconws works in every resolution -- no rez check needed.
	print banner_txt
	print connecting_txt

	move.l _hz_200.w, d3
	add.l #MDNET_TIMEOUT_TICKS, d3	; D3 = deadline
.poll:
	move.l MDNET_STATUS_ADDR, d0
	cmp.l #MDNET_ST_CONNECTED, d0
	beq.s .show_msg
	cmp.l #MDNET_ST_FAILED, d0
	beq.s .show_msg
	cmp.l _hz_200.w, d3			; deadline - now
	bhi.s .poll				; still ahead -> keep polling
	print timeout_txt
	bra.s boot_gem

.show_msg:
; Print the RP-composed message (includes its own CR/LF) straight from
; cartridge ROM -- Cconws just walks bytes until NUL.
	pea (MDNET_MSG_ADDR).l
	gemdos Cconws,6

boot_gem:
	; Return to TOS: continue booting from disks into GEM. The
	; cartridge does not stay resident -- the relocated copy of this
	; code below screen RAM is simply abandoned.
    rts

; Boot banner. version.inc is generated by target/atarist/Makefile from
; version.txt at build time -- it defines banner_txt as
;   dc.b "MD/Net <version> (c)2026 Neil Rackett",13,10,"GPLv3 ...",13,10,0
; so the banner always matches the shipped version. Both lines fit the
; 40-column low-res screen.
	include version.inc
connecting_txt:
	dc.b $d,$a,"MD/Net: connecting...",$d,$a,0
timeout_txt:
	dc.b "MD/Net: no response from device.",$d,$a,0
	even

end_rom_code:
end_pre_auto:
	even
	dc.l 0
