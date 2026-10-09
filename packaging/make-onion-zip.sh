#!/bin/sh
# make-onion-zip.sh <yabadabadoo_libretro.so> <out.zip>
# Builds the Onion OS release zip: extract it at the root of the SD card.
set -e
[ $# -eq 2 ] || { echo "usage: $0 <yabadabadoo_libretro.so> <out.zip>" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
core=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -R "$here/onion/." "$tmp/"
cp "$core" "$tmp/RetroArch/.retroarch/cores/yabadabadoo_libretro.so"
cp "$here/../yabause/COPYING" "$tmp/Emu/SATURN/LICENSE-yabadabadoo.txt"
cp "$here/THIRD-PARTY-NOTICES.txt" "$tmp/Emu/SATURN/THIRD-PARTY-NOTICES.txt"
find "$tmp" \( -name '.DS_Store' -o -name '._*' \) -exec rm -f {} +
chmod +x "$tmp/Emu/SATURN/"*.sh
rm -f "$out"
(cd "$tmp" && COPYFILE_DISABLE=1 zip -qrX "$out" Emu Icons RetroArch Roms)
echo "$out"
