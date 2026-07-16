/* userfw.ld — absolute layout for the cartridge image
 *
 * The cartridge image is loaded verbatim at $FA0000 by the cartridge
 * port hardware, so the offsets below become runtime addresses by
 * adding ROM4_ADDR.
 *
 * MD/Net's cartridge code is main.s only: it prints the boot message,
 * waits for the RP's WiFi status, and returns to TOS. The whole image
 * must stay under CARTRIDGE_CODE_SIZE = 16 KB (the build script
 * asserts this against BOOT.BIN).
 *
 * Adding a new module is a two-step change:
 *   1. Pick an offset within the remaining cartridge budget and add a
 *      new ".text_<name> 0x????? : { <name>.o(.text) }" section here.
 *   2. Mirror the offset on the m68k side with `equ (ROM4_ADDR + $????)`
 *      in main.s, and add the .o target to target/atarist/Makefile.
 */
SECTIONS
{
    .text_main 0x000000 : {
        main.o(.text)
    }
}
