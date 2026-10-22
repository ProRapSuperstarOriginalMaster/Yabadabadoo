/*  Copyright 2012 Guillaume Duhamel

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef TITAN_H
#define TITAN_H

#include "../core.h"
#include "../vidshared.h"

#define TITAN_BLEND_TOP     0
#define TITAN_BLEND_BOTTOM  1
#define TITAN_BLEND_ADD     2

#define TITAN_SPRITE 5
#define TITAN_RBG0 4
#define TITAN_NBG0 3
#define TITAN_NBG1 2
#define TITAN_NBG2 1
#define TITAN_NBG3 0
#define TITAN_BACK -1

#define TITAN_NORMAL_SHADOW 1
#define TITAN_MSB_SHADOW 2

/* One pixel of a layer buffer; `meta` packs the four byte fields so a pixel
   can be written with two stores */
struct PixelData
{
   u32 pixel;
   union {
      struct {
#if defined(WORDS_BIGENDIAN)
         u8 shadow_enabled;
         u8 shadow_type;
         u8 linescreen;
         u8 priority;
#else
         u8 priority;
         u8 linescreen;
         u8 shadow_type;
         u8 shadow_enabled;
#endif
      };
      u32 meta;
   };
};

static INLINE u32 TitanPackMeta(int priority, int linescreen, int shadow_type, int shadow_enabled)
{
#if defined(WORDS_BIGENDIAN)
   return (u32)((priority << 24) | (linescreen << 16) | (shadow_type << 8) | shadow_enabled);
#else
   return (u32)(priority | (linescreen << 8) | (shadow_type << 16) | (shadow_enabled << 24));
#endif
}

/* Direct access for renderers that write whole lines. Layer lines are cleared
   lazily: call TitanClaimLayerLine before writing line y. If it returns 1 the
   line holds stale data from an earlier frame and every pixel of it must be
   written (zero for transparent); if it returns 0 the line was already started
   this frame and only opaque pixels should be written. */
struct PixelData *TitanGetLayerLine(int layer, int y);
int TitanClaimLayerLine(int layer, int y);
/* Optional, after a fresh claim: no pixel in the line has a priority above
   maxprio (a claim alone assumes 7). Lets the compositor skip layers. */
void TitanSetLineMaxPriority(int layer, int y, int maxprio);
void TitanSetLayerDirty(int layer);

int TitanInit();
int TitanDeInit();
void TitanErase();

void TitanSetResolution(int width, int height);
void TitanGetResolution(int * width, int * height);

void TitanSetBlendingMode(int blend_mode);
/* CCCTL of every displayed line ORed together, when colour calculation
   changes inside the display (0 otherwise): the frame then needs full
   compositing even if the register at VBlank-OUT has it off */
void TitanSetLineColorCalc(u16 ccctl);

void TitanPutBackHLine(s32 y, u32 color);

void TitanPutLineHLine(int linescreen, s32 y, u32 color);
/* Line colour per pixel instead of per line: a pixel whose linescreen is
   TITAN_LINESCREEN_DOT + n takes its line colour from
   TitanGetDotLineColors(n)[y * width + x] (same layout as the layer). */
#define TITAN_LINESCREEN_DOT 4
u32 *TitanGetDotLineColors(int n);

void TitanPutPixel(int priority, s32 x, s32 y, u32 color, int linescreen, vdp2draw_struct* info);
void TitanPutHLine(int priority, s32 x, s32 y, s32 width, u32 color);

void TitanRender(pixel_t * dispbuffer);

void TitanWriteColor(pixel_t * dispbuffer, s32 bufwidth, s32 x, s32 y, u32 color);

#endif
