#!/bin/sh
echo $0 $*
progdir=`dirname "$0"`
homedir=`dirname "$1"`

# Onion's RetroArch sets the CPU clock from cpuclock.txt in the core's config folder.
# Start at 1600 MHz unless the user already chose a value. To change the clock, edit
# cpuclock.txt: the value here is only written when that file doesn't exist yet.
cfg=/mnt/SDCARD/Saves/CurrentProfile/config/Yabadabadoo
[ -f "$cfg/cpuclock.txt" ] || { mkdir -p "$cfg"; echo 1600 > "$cfg/cpuclock.txt"; }

# Once per profile, on the first launch of 0.2.1: 0.1 and 0.2 started at 1700 MHz, which
# freezes some Mini V4 units before the BIOS screen. Lower an untouched 1700 to 1600, but
# only if the core never got as far as a normal exit (RetroArch writes the .opt files
# then): whoever has played at 1700 keeps it.
if [ ! -f "$cfg/.updated-0.2.1" ]; then
	played=
	for opt in "$cfg"/*.opt; do
		[ -f "$opt" ] && played=1
	done
	if [ -z "$played" ] && [ "$(tr -d ' \r\n' < "$cfg/cpuclock.txt")" = 1700 ]; then
		echo 1600 > "$cfg/cpuclock.txt"
	fi
	touch "$cfg/.updated-0.2.1"
fi

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
