#!/bin/sh
# Yabadabadoo beta, in Onion's Expert section. Installs next to the release "Sega Saturn" entry:
# own core file, settings, saves and states ("Yabadabadoo Beta" folders); same ROM folder.
echo $0 $*
progdir=`dirname "$0"`

# Onion's RetroArch sets the CPU clock from cpuclock.txt in the core's config folder.
# Start at 1600 MHz (the release default) unless the user already chose a value.
cfg="/mnt/SDCARD/Saves/CurrentProfile/config/Yabadabadoo Beta"
[ -f "$cfg/cpuclock.txt" ] || { mkdir -p "$cfg"; echo 1600 > "$cfg/cpuclock.txt"; }

# Once per profile, as the release does: RetroArch's audio driver "sdl" for this core (core
# override). With Onion's default "oss" the frontend never reports how full the sound buffer is,
# so auto frameskip always skips the most frames it may. Other lines in the override are kept.
if [ ! -f "$cfg/.updated-beta" ]; then
	ovr="$cfg/Yabadabadoo Beta.cfg"
	if [ -f "$ovr" ]; then
		grep -v '^ *audio_driver *=' "$ovr" > "$ovr.tmp"
		mv "$ovr.tmp" "$ovr"
	fi
	echo 'audio_driver = "sdl"' >> "$ovr"
	touch "$cfg/.updated-beta"
fi

cd /mnt/SDCARD/RetroArch/
HOME=/mnt/SDCARD/RetroArch/ $progdir/../../RetroArch/retroarch -v -L $progdir/../../RetroArch/.retroarch/cores/yabadabadoo_beta_libretro.so "$1"
