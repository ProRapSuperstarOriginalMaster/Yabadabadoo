#!/bin/sh
echo $0 $*
progdir=`dirname "$0"`
homedir=`dirname "$1"`

# Onion's RetroArch sets the CPU clock from cpuclock.txt in the core's config folder.
# Start at 1700 MHz (safe on every Miyoo Mini model) unless the user already chose a value.
cfg=/mnt/SDCARD/Saves/CurrentProfile/config/Yabadabadoo
[ -f "$cfg/cpuclock.txt" ] || { mkdir -p "$cfg"; echo 1700 > "$cfg/cpuclock.txt"; }

cd /mnt/SDCARD/RetroArch/
HOME=/mnt/SDCARD/RetroArch/ $progdir/../../RetroArch/retroarch -v -L $progdir/../../RetroArch/.retroarch/cores/yabadabadoo_libretro.so "$1"
