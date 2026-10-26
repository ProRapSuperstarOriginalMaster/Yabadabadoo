#!/bin/sh
# make-onion-beta-zip.sh <yabadabadoo_beta_libretro.so> <version> <out.zip>
# Builds the Onion OS zip of a beta (core built with make BETA=1 BETA_VERSION=<version>): extract
# it at the root of the SD card. It adds "Saturn (beta)" to the Expert section and touches no file
# of the release install. The entry and the .info are made from the release ones, so they stay in step.
set -e
[ $# -eq 3 ] || { echo "usage: $0 <yabadabadoo_beta_libretro.so> <version> <out.zip>" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
core=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
ver=$2
out=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
rel=$here/onion
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -R "$here/onion-beta/." "$tmp/"
e=$tmp/RApp/yabadabadoo_beta
c=$tmp/RetroArch/.retroarch/cores
mkdir -p "$c"
sed -e 's/"label":[^,]*,/"label":	"Saturn (beta)",/' \
    -e 's#"icon":[^,]*,#"icon":	"/mnt/SDCARD/RApp/yabadabadoo_beta/saturn.png",#' \
    "$rel/Emu/SATURN/config.json" > "$e/config.json"
cp "$rel/Emu/SATURN/advcommand.sh" "$e/advcommand.sh"
cp "$rel/Icons/Default/saturn.png" "$e/saturn.png"
sed -e 's/^display_name = .*/display_name = "Sega - Saturn (Yabadabadoo Beta)"/' \
    -e 's/^corename = .*/corename = "Yabadabadoo Beta"/' \
    -e "s/^display_version = .*/display_version = \"$ver\"/" \
    "$rel/RetroArch/.retroarch/cores/yabadabadoo_libretro.info" > "$c/yabadabadoo_beta_libretro.info"
grep -q '"Saturn (beta)"' "$e/config.json" && grep -q 'RApp/yabadabadoo_beta/saturn.png' "$e/config.json" \
  && grep -q "^display_version = \"$ver\"" "$c/yabadabadoo_beta_libretro.info" \
  && grep -q '^corename = "Yabadabadoo Beta"' "$c/yabadabadoo_beta_libretro.info" \
  || { echo "make-onion-beta-zip: the release entry or .info changed shape; update the sed lines" >&2; exit 1; }
cp "$core" "$c/yabadabadoo_beta_libretro.so"
cp "$here/../yabause/COPYING" "$e/LICENSE-yabadabadoo.txt"
cp "$here/THIRD-PARTY-NOTICES.txt" "$e/THIRD-PARTY-NOTICES.txt"
find "$tmp" \( -name '.DS_Store' -o -name '._*' \) -exec rm -f {} +
chmod +x "$e/"*.sh
rm -f "$out"
(cd "$tmp" && COPYFILE_DISABLE=1 zip -qrX "$out" RApp RetroArch)
echo "$out"
