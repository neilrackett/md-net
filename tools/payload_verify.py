#!/usr/bin/env python3
#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Proves the cartridge payload survives the trip to the ST: applies the
# firmware's byte-swapped staging (rp/src/payload.c) to the generated
# header, then reads it back exactly as INSTALL.PRG does and compares
# against the files on disk.
#
# The cart bus swaps bytes within each 16-bit word, which is easy to get
# subtly wrong and impossible to see from either side alone -- so check
# it here rather than on hardware.
#
#   usage: payload_verify.py <mdnet_payload.h> <dir-with-the-files>

import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: payload_verify.py <payload.h> <file-dir>")
    header, file_dir = sys.argv[1], sys.argv[2].rstrip("/")

    src = open(header).read()
    off = int(re.search(r"MDNET_PAYLOAD_OFF 0x([0-9A-Fa-f]+)u", src).group(1), 16)
    body = src.split("mdnet_payload[] = {")[1].split("};")[0]
    blob = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body))

    rom = bytearray(0x10000)
    for i, byte in enumerate(blob):  # payload_publish()
        rom[(off + i) ^ 1] = byte

    def m8(addr):
        return rom[addr ^ 1]

    def m16(addr):
        return (m8(addr) << 8) | m8(addr + 1)

    def m32(addr):
        return (m16(addr) << 16) | m16(addr + 2)

    if bytes(m8(off + i) for i in range(4)) != b"MDNP":
        sys.exit("payload magic wrong as seen by the m68k")
    if m16(off + 4) != 1:
        sys.exit("payload version wrong as seen by the m68k")

    for index in range(m16(off + 6)):
        entry = off + 8 + index * 24
        name = bytes(m8(entry + n) for n in range(16)).split(b"\0")[0].decode()
        start, length = m32(entry + 16), m32(entry + 20)
        seen = bytes(m8(start + k) for k in range(length))
        with open("%s/%s" % (file_dir, name), "rb") as handle:
            if seen != handle.read():
                sys.exit("payload content mismatch for %s" % name)
        print("payload verified: %-11s $FA%04X %6d bytes" % (name, start, length))


if __name__ == "__main__":
    main()
