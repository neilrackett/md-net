#!/bin/bash
#
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the ready-to-go STinG bundle for the MD/Net release page: the
# stock sfl.zip rearranged so "copy the contents onto C:" is the whole
# install, with two deliberate changes:
#
#  - STING/ROUTE.TAB is removed. The stock file ends with a default
#    route via the (inactive) "Modem 1" port, and STinG routing is
#    first-match-wins -- so it would shadow the default route MD/Net's
#    installer appends and internet traffic would die with "host
#    unreachable" on a completely stock setup. With no ROUTE.TAB,
#    INSTALL.TOS creates a correct one from scratch.
#  - A BUNDLE.TXT records exactly what this is, where the original came
#    from, and what was changed. Everything else, including STinG's own
#    README.1ST, docs and credits, ships untouched.
#
# Not part of the firmware build (it downloads from the network); run it
# by hand when preparing a release:  tools/make_sting_bundle.sh <outdir>

set -e

OUT="${1:-dist}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

curl -sfL --max-time 120 "https://hardware.atari.org/files/sfl.zip" \
    -o "$WORK/sfl.zip"
unzip -q "$WORK/sfl.zip" -d "$WORK/sfl"

# Sanity: the layout this script rearranges. If upstream changes, stop
# rather than ship something unreviewed.
for f in AUTO/STING.PRG AUTO/STING.INF STING/DEFAULT.CFG CPX/STNGPORT.CPX \
         XCONTROL.ACC CONTROL.INF README.1ST; do
    if [ ! -e "$WORK/sfl/$f" ]; then
        echo "ERROR: sfl.zip layout changed ($f missing); review before shipping."
        exit 1
    fi
done
grep -q "^ACTIVATE *= *TRUE" "$WORK/sfl/STING/DEFAULT.CFG" || {
    echo "ERROR: stock DEFAULT.CFG no longer sets ACTIVATE=TRUE; review."
    exit 1
}

B="$WORK/bundle"
mkdir -p "$B"
cp -R "$WORK/sfl/AUTO" "$B/AUTO"
cp -R "$WORK/sfl/STING" "$B/STING"
cp -R "$WORK/sfl/CPX" "$B/CPX"
cp "$WORK/sfl/XCONTROL.ACC" "$WORK/sfl/CONTROL.INF" "$WORK/sfl/README.1ST" "$B/"

# The one functional change, and the reason this bundle exists at all.
rm -f "$B/STING/ROUTE.TAB"

printf '%s\r\n' \
'STinG, ready for MD/Net' \
'----------------------------------------------------------------------' \
'' \
'This is the standard STinG distribution (sfl.zip, the STinG' \
'Evolution Team release from hardware.atari.org), rearranged so that' \
'installing it is one step:' \
'' \
'  Copy the contents of this folder onto your boot drive (C:),' \
'  merging the AUTO folder with your own, then reboot.' \
'' \
'If you already use XCONTROL, keep your own XCONTROL.ACC and' \
'CONTROL.INF and just add the CPX files to your CPX folder.' \
'' \
'One file has been removed compared to stock: STING\ROUTE.TAB.' \
'Its placeholder routes would silently block internet access on a' \
'fresh setup; MD/Net'"'"'s INSTALL.TOS creates a correct ROUTE.TAB for' \
'your network instead.' \
'' \
'After rebooting, run INSTALL.TOS from the MD/Net cartridge, reboot' \
'again, and turn the WiFi port on in STinG Port Setup.' \
'' \
'STinG is by Peter Rottengatter and the STinG Evolution Team; see' \
'README.1ST and STING\DOCS for its documentation and credits.' \
> "$B/BUNDLE.TXT"

mkdir -p "$OUT"
( cd "$B" && zip -qr9 - . ) > "$OUT/sting-for-mdnet.zip"
echo "built $OUT/sting-for-mdnet.zip:"
unzip -l "$OUT/sting-for-mdnet.zip" | tail -3
