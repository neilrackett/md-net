#!/bin/sh
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# MD/Net in Hatari, with EmuMD (emu/emumd), the whole way: the firmware
# joins "WiFi" (this computer's network) and publishes its lease; on the
# ST, the STinG bundle (tools/make_sting_bundle.sh, made once into
# build/) and INSTALL.TOS set STinG up from the cartridge; then, after a
# reboot, STNGTEST.TOS (emu/test) fetches a page through STinG,
# MDNET.STX and the cartridge from a server on this computer (10.0.2.2
# to the ST). Needs stcmd, libslirp, python3, and the network once (for
# STinG).
#
#   emu/test.sh          (MDFW=.../tools/mdfw for another EmuMD)

set -eu
cd "$(dirname "$0")/.."
MDFW=${MDFW:-emu/emumd/tools/mdfw}

$MDFW build
STCMD="env STCMD_NO_TTY=1 ST_WORKING_FOLDER=$PWD stcmd"
$STCMD make -s -C emu/test >/dev/null
$STCMD make -s -C target/atarist/stx INSTALL.TOS \
    MDNET_VERSION="$(tr -d '\r\n ' < version.txt)" >/dev/null
[ -f build/sting-for-mdnet.zip ] || tools/make_sting_bundle.sh build >/dev/null

WORK=$(mktemp -d)
SERVER=
cleanup() {
    set +e
    [ -n "$SERVER" ] && kill "$SERVER" && wait "$SERVER" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
mkdir "$WORK/c" "$WORK/www"
(cd "$WORK/c" && unzip -q "$OLDPWD/build/sting-for-mdnet.zip")
cp target/atarist/stx/INSTALL.TOS emu/test/dist/STNGTEST.TOS "$WORK/c/"

# st PROGRAM FRAMES: boot the ST with drive C:, running PROGRAM.
st() {
    $MDFW run --headless --frames "$2" --timeout 300 --harddrive "$WORK/c" \
        --log "$WORK/hatari.log" -V -- --auto "C:\\$1" >/dev/null
}

fail=0
expect() { # FILE TEXT
    grep -qF "$2" "$1" || { echo "test: expected $2"; fail=1; }
}

# The installer: the driver and the routes, from the cartridge.
st INSTALL.TOS 1500
expect "$WORK/hatari.log" "autoconf: ST gets 10.0.2.15"
[ -f "$WORK/c/STING/MDNET.STX" ] || { echo "test: INSTALL.TOS installed nothing"; exit 1; }
tr -d '\r' < "$WORK/c/STING/ROUTE.TAB" | grep -v '^#' > "$WORK/routes"
expect "$WORK/routes" "WiFi	10.0.2.2"

# A page from this computer, through STinG and the cartridge.
printf 'Hello from this computer, via STinG and MD/Net\n' > "$WORK/www/hello.txt"
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$WORK/www" >/dev/null 2>&1 &
SERVER=$!
printf '10.0.2.2 %s /hello.txt\r\n' "$PORT" > "$WORK/c/TEST.TXT"
st STNGTEST.TOS 2500
[ -f "$WORK/c/RESULT.TXT" ] || { echo "test: STNGTEST.TOS wrote nothing"; exit 1; }
tr -d '\r' < "$WORK/c/RESULT.TXT" | tee "$WORK/result"
expect "$WORK/result" "HTTP/1.0 200 OK"
expect "$WORK/result" "Hello from this computer, via STinG and MD/Net"
expect "$WORK/result" "port WiFi active 10.0.2.15"
expect "$WORK/result" "done"

[ $fail = 0 ] && echo "test: all good"
exit $fail
