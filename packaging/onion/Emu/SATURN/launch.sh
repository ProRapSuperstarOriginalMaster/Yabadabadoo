#!/bin/sh
echo $0 $*
progdir=`dirname "$0"`
homedir=`dirname "$1"`

# Onion's RetroArch sets the CPU clock from cpuclock.txt in the core's config folder.
# Start at 1700 MHz (safe on every Miyoo Mini model) unless the user already chose a value.
cfg=/mnt/SDCARD/Saves/CurrentProfile/config/Yabadabadoo
[ -f "$cfg/cpuclock.txt" ] || { mkdir -p "$cfg"; echo 1700 > "$cfg/cpuclock.txt"; }

# Once per profile, on the first launch of 0.2 (marker file; later choices of the user are kept):
# - RetroArch's audio driver "sdl" for this core (core override Yabadabadoo.cfg). With Onion's
#   default "oss" the frontend never reports how full the sound buffer is, so auto frameskip
#   always skips the most frames it may. Other lines in the override are left alone.
# - Frameskip "Auto (Threshold)" (0.1's default, saved in the options files) becomes "Auto",
#   0.2's default. Other frameskip choices are left alone.
if [ ! -f "$cfg/.updated-0.2" ]; then
	mkdir -p "$cfg"
	ovr="$cfg/Yabadabadoo.cfg"
	if [ -f "$ovr" ]; then
		grep -v '^ *audio_driver *=' "$ovr" > "$ovr.tmp"
		mv "$ovr.tmp" "$ovr"
	fi
	echo 'audio_driver = "sdl"' >> "$ovr"
	for opt in "$cfg"/*.opt; do
		[ -f "$opt" ] && sed -i 's/^\( *yabadabadoo_frameskip *= *\)"auto_threshold"/\1"auto"/' "$opt"
	done
	touch "$cfg/.updated-0.2"
fi

cd /mnt/SDCARD/RetroArch/
HOME=/mnt/SDCARD/RetroArch/ $progdir/../../RetroArch/retroarch -v -L $progdir/../../RetroArch/.retroarch/cores/yabadabadoo_libretro.so "$1"
