# Yabadabadoo

A Sega Saturn emulator core for RetroArch, tuned to run on Miyoo Mini devices such 
as the Miyoo Mini+, Miyoo Mini V1-V4, and Miyoo Mini Flip. The release is packaged
for OnionOS though it should work for other frontends. It is a fork of the libretro 
port of Yabause.

### Notes:

This project has primarily been developed through the use of AI coding agents. Even
though I have been impressed with the results, this project should be viewed as inherently
experimental and not as a replacement for emulators written by real humans who know what
they're doing.

This project has been exclusively tested on my Miyoo Mini+. While it should work on other 
Miyoo Mini devices I can't guarantee there won't be device specific issues. The Miyoo Mini V4
for instance has a screen resolution that differs from other Miyoo devices. While that
*shouldn't* break anything I'd still be interested to hear from V4 users that scaling and 
rendering works as expected.

Version 0.2.1. This is an early release: expect bugs, and expect some games to run slowly or
not at all.

PAL ROMs can be especially useful for this emulator as they're rendered at a lower refresh
rate, and are therefore generally able to run smoother. However the implementation of proper 
PAL support into this core is still at early stages.  

## Requirements

- Miyoo Mini device with Onion OS (other devices remain untested)
- A Saturn BIOS file (not included, see below)
- Your own Saturn games

## Install

1. Download the release zip.
2. Extract it to the root of the SD card. It adds these files:

       Emu/SATURN/                                        menu entry and launch script
       Icons/Default/saturn.png                           menu icon
       RetroArch/.retroarch/cores/yabadabadoo_libretro.so the core
       RetroArch/.retroarch/cores/yabadabadoo_libretro.info
       Roms/SATURN/put_roms_here.txt

3. Put a Saturn BIOS in the `BIOS` folder of the SD card (see below).
4. Put your games in `Roms/SATURN`. Supported formats: .cue (with its .bin files), .chd,
   .iso, .mds, .ccd, .m3u (multi-disc games), .zip.
5. Sega Saturn now shows up in Onion's game list.

If you copy the files with the macOS Finder, it also leaves hidden `._` files on the card.
They are harmless; `dot_clean -m /Volumes/<card name>` removes them.

## Usage

After installation Yabadabadoo should work like any other emulator on Onion. The settings
page is currently a little cluttered, expect that to change in later updates. The most
relevant core options for solving performance issues should be changing frameskip type,
interval, and threshold.

## BIOS

A real Saturn BIOS is required. Without one the core shows a screen asking for it instead
of starting the game. Put one of these files in the `BIOS` folder:

    sega_101.bin     Japan v1.01       md5 85ec9ca47d8f6807718151cbcca8b964
    mpr-17933.bin    USA/Europe v1.00  md5 3240872c70984b6cbfda1586cab68dbe
    saturn_bios.bin  any of the above, renamed

## CPU clock

The launch script runs the Miyoo at 1600 MHz. The value is stored in
`Saves/CurrentProfile/config/Yabadabadoo/cpuclock.txt` (created on the first launch); to change
the clock, edit that file. The 1600 in `launch.sh` is only written when that file doesn't exist
yet, so changing it there does nothing after the first launch.

Every chip is a little different: many units handle more than 1600 MHz, some don't. Higher
clocks run faster but are at your own risk. If your Miyoo freezes, first try turning it off
with the power button; on some models this works even when the screen is frozen. Then set a
lower value in `cpuclock.txt` (1200 MHz is the stock clock), try again, and let me know in the
Issues tab.

## Audio driver

Yabadabadoo uses RetroArch's `sdl` audio driver instead of Onion's default `oss`. With
`oss`, RetroArch can't tell the core how full the sound buffer is, so the automatic
frameskip always skips as many frames as it is allowed to, even in scenes that could run at
full speed. The launch script sets this once, as a core override
(`Saves/CurrentProfile/config/Yabadabadoo/Yabadabadoo.cfg`); your other override settings are
kept. If you change the audio driver back in RetroArch's menu and save a core override, your
choice is kept.

## Updating

Extract the new release zip over the old one. Your settings, saves and save states are kept.
When updating from 0.1, the first launch switches the audio driver to `sdl` (see above) and
changes Frameskip from "Auto (Threshold)", 0.1's default, to "Auto", the new default (it
works properly now that the audio driver reports the buffer level). If you picked another
frameskip mode, it is kept.

0.1 and 0.2 started at 1700 MHz, which might cause black screens and hangups on some Mini V4
units. If you've ever had a black screen and a frozen Miyoo when starting a game, delete
`Saves/CurrentProfile/config/Yabadabadoo/cpuclock.txt` and install 0.2.1. The next launch
creates the file again at 1600 MHz. If Yabadabadoo has always run fine on your device, your
clock stays where it is, and you don't need to update.

## Beta builds

Between releases, every update of the `dev` branch is published as a beta on the
[releases page](https://github.com/ProRapSuperstarOriginalMaster/Yabadabadoo/releases), marked
"Pre-release" and named like `0.4-beta.3`. Betas get fixes to you sooner, but they are less tested
than releases: if you just want to play, use the latest release.

A beta installs next to the release, not over it. Extract its zip at the root of the SD card: it
adds "Saturn (beta)" to Onion's Expert section, with its own core file
(`yabadabadoo_beta_libretro.so`) and its own settings, saves and save states (the
`Yabadabadoo Beta` folders). Your release install and its saves are not touched. If your game
list has no Expert tab, turn it on in Onion's Tweaks app (user interface settings: show the Expert tab). To try a
release save state in the beta, copy it from `Saves/CurrentProfile/states/Yabadabadoo` to
`Saves/CurrentProfile/states/Yabadabadoo Beta`; a state saved by a beta may not load in an older
release. To remove a beta, delete `RApp/yabadabadoo_beta` and
`RetroArch/.retroarch/cores/yabadabadoo_beta_libretro.*`.

When you report a problem with a beta, include its version string (RetroArch: Information >
Core Information), e.g. `0.4-beta.3 (Yabause 0.9.15) 1a2b3c4d5`.

## Known issues

- FMV's in some games still have some unresolved hiccups
- The PAL version of Nights into Dreams shows a black screen instead of an initial FMV. Pressing start lets you skip it and play as normal.
- Shining Force III has a texture flickering issue in its opening scene
- The icon isn't the prettiest. I thought there already was an OnionOS icon for Saturn but when Claude couldn't find one it decided
to generate its own version on its own initiative. I promise I'll clean it up manually in a later release. 

## Building

The core is built with the Miyoo Mini toolchain:
https://github.com/shauninman/union-miyoomini-toolchain

1. Set up the toolchain as described in its README and open its build shell (`make shell`).
2. Inside the shell, in the toolchain's `workspace` folder:

       git clone https://github.com/ProRapSuperstarOriginalMaster/Yabadabadoo.git
       cd Yabadabadoo/yabause/src/libretro
       make platform=miyoomini -j4

   This produces `yabadabadoo_libretro.so`.
3. To make the release zip (on any machine with `zip`):

       packaging/make-onion-zip.sh yabause/src/libretro/yabadabadoo_libretro.so yabadabadoo.zip

   A beta is built from its tag (`v0.4-beta.N`) with
   `make platform=miyoomini -j4 BETA=1 BETA_VERSION=0.4-beta.N` and packaged with
   `packaging/make-onion-beta-zip.sh yabause/src/libretro/yabadabadoo_beta_libretro.so 0.4-beta.N out.zip`.

## License

Yabadabadoo is free software, licensed under the GNU General Public License version 2 or
(at your option) any later version, like Yabause. See `yabause/COPYING`. The source code
for every release is the matching tag of this repository.

Bundled third-party code keeps its own license: libchdr, zlib, LZMA SDK and FLAC
(`yabause/src/libchdr`), Musashi (`yabause/src/musashi`), JUnzip (`yabause/src/junzip.c`)
and libretro-common (`yabause/src/libretro/libretro-common`). Their notices are collected in
`packaging/THIRD-PARTY-NOTICES.txt`, which the release zip includes in `Emu/SATURN/`.

Changes from upstream: this fork changes the libretro port of Yabause for speed on the
Miyoo Mini (software renderer, SH-2 recompiler, threading, frame skipping) and for
packaging on Onion OS. The full list is in the git history.

## Credits

- Yabause, by Guillaume Duhamel, Theo Berkau and the Yabause team and contributors
  (see `yabause/AUTHORS`): https://github.com/Yabause/yabause
- The libretro port of Yabause: https://github.com/libretro/yabause
- Onion OS, whose emulator packaging this release follows: https://github.com/OnionUI/Onion
- The Miyoo Mini toolchain by Shaun Inman: https://github.com/shauninman/union-miyoomini-toolchain
- Claude, I find crediting AI models extremely corny but in this case I want to once again highlight
this is a very AI driven project.

Sega and Sega Saturn are trademarks of Sega. This project is not affiliated with or
endorsed by Sega. No BIOS files or games are included.
