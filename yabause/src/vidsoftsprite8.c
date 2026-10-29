/*  Copyright 2006 Theo Berkau (vidsoft.c, which this is derived from)

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

/*! \file vidsoftsprite8.c
    \brief The software renderer's sprite layer from an 8-bit VDP1 framebuffer
    (the common case of VidsoftDrawSprite() in vidsoft.c, same output).

    In a file of its own because vidsoft.c sits at GCC's per-file inlining
    limit: code added there changes what gets inlined in its hot layer loops.
    The few helpers it shares with vidsoft.c are copies (Sprite8*), kept the
    same as the originals named in their comments.
*/

#include <string.h>
#include "vidsoft.h"
#include "vidshared.h"
#include "vdp2.h"
#include "titan/titan.h"

extern int vdp2width, vdp2height, vdp2_x_hires, vdp2_interlace;   /* vidsoft.c */

/* vidsoft.c's COLSAT2YAB16/COLSAT2YAB32 and COLOR_ADD */
#if defined WORDS_BIGENDIAN
static INLINE u32 Sprite8Colsat16(int priority, u32 temp) { return (priority | (temp & 0x7C00) << 1 | (temp & 0x3E0) << 14 | (temp & 0x1F) << 27); }
static INLINE u32 Sprite8Colsat32(int priority, u32 temp) { return (((temp & 0xFF) << 24) | ((temp & 0xFF00) << 8) | ((temp & 0xFF0000) >> 8) | priority); }
#else
static INLINE u32 Sprite8Colsat16(int priority, u32 temp) { return (priority << 24 | (temp & 0x1F) << 3 | (temp & 0x3E0) << 6 | (temp & 0x7C00) << 9); }
static INLINE u32 Sprite8Colsat32(int priority, u32 temp) { return (priority << 24 | (temp & 0xFF0000) | (temp & 0xFF00) | (temp & 0xFF)); }
#endif

#define SPRITE8_ADDt(b)      (b>0xFF?0xFF:(b<0?0:b))
#define SPRITE8_ADDb(b1,b2)  SPRITE8_ADDt((signed) (b1) + (b2))
#ifdef WORDS_BIGENDIAN
#define SPRITE8_ADD(l,r,g,b) (l & 0xFF) | \
                             (SPRITE8_ADDb((l >> 8) & 0xFF, b) << 8) | \
                             (SPRITE8_ADDb((l >> 16) & 0xFF, g) << 16) | \
                             (SPRITE8_ADDb((l >> 24), r) << 24)
#else
#define SPRITE8_ADD(l,r,g,b) SPRITE8_ADDb((l & 0xFF), r) | \
                             (SPRITE8_ADDb((l >> 8) & 0xFF, g) << 8) | \
                             (SPRITE8_ADDb((l >> 16) & 0xFF, b) << 16) | \
                             (l & 0xFF000000)
#endif

/* vidsoft.c's Vdp2ColorRamGetColor() */
static u32 Sprite8ColorRamGetColor(u32 addr, u8 *vdp2_color_ram)
{
   switch (Vdp2Internal.ColorMode)
   {
      case 0:
      case 1:
      {
         u32 tmp;
         addr <<= 1;
         tmp = T2ReadWord(vdp2_color_ram, addr & 0xFFF);
         /* the MSB is kept for special colour calculation mode 3 */
         return (((tmp & 0x1F) << 3) | ((tmp & 0x03E0) << 6) | ((tmp & 0x7C00) << 9)) | ((tmp & 0x8000) << 16);
      }
      case 2:
         addr <<= 2;
         return T2ReadLong(vdp2_color_ram, addr & 0xFFF);
      default: break;
   }
   return 0;
}

/* The colour offset values ReadVdp2ColorOffset(regs, info, 0x40, 0x40) in
 * vidsoft.c sets (its PostPixelFetchCalc isn't used for sprites) */
static void Sprite8ColorOffset(const Vdp2 *regs, vdp2draw_struct *info)
{
   if (!(regs->CLOFEN & 0x40))
      return;
   if (regs->CLOFSL & 0x40)
   {
      info->cor = regs->COBR & 0xFF;
      if (regs->COBR & 0x100) info->cor |= 0xFFFFFF00;
      info->cog = regs->COBG & 0xFF;
      if (regs->COBG & 0x100) info->cog |= 0xFFFFFF00;
      info->cob = regs->COBB & 0xFF;
      if (regs->COBB & 0x100) info->cob |= 0xFFFFFF00;
   }
   else
   {
      info->cor = regs->COAR & 0xFF;
      if (regs->COAR & 0x100) info->cor |= 0xFFFFFF00;
      info->cog = regs->COAG & 0xFF;
      if (regs->COAG & 0x100) info->cog |= 0xFFFFFF00;
      info->cob = regs->COAB & 0xFF;
      if (regs->COAB & 0x100) info->cob |= 0xFFFFFF00;
   }
}

/* What the generic loop of VidsoftDrawSprite() makes of each value of an
 * 8-bit framebuffer pixel, for one draw */
struct Sprite8Table
{
   u32 color[256];   /* before the colour offset */
   u32 meta[256];    /* Titan meta; 0 = not drawn (the empty pixel, priority 0) */
   u8 shadow[256];   /* a normal shadow: the colour offset doesn't apply */
};

/* Runs the generic loop's 8-bit pixel code on every byte value; cc = the
 * colour calc window test (the same for every pixel here) and CCCTL bit 6 */
static void Sprite8Setup(struct Sprite8Table *t, const Vdp2 *regs, const u8 *prioritytable, const u8 *colorcalctable,
   int cc, int linescreen, u8 *color_ram)
{
   const int type = regs->SPCTL & 0xF, SPCCCS = (regs->SPCTL >> 12) & 0x3, SPCCN = (regs->SPCTL >> 8) & 0x7;
   const u32 coloroffset = (regs->CRAOFB & 0x70) << 4;
   int b;

   t->meta[0] = 0;
   for (b = 1; b < 256; b++)
   {
      spritepixelinfo_struct spi;
      u16 pixel = b;
      u8 alpha = 0x3F;
      u32 dot;
      int priority;

      Vdp1GetSpritePixelInfo(type, &pixel, &spi);
      priority = prioritytable[spi.priority];
      t->meta[b] = priority ? TitanPackMeta(priority, linescreen, spi.normalshadow ? TITAN_NORMAL_SHADOW : 0, 0) : 0;
      t->shadow[b] = spi.normalshadow != 0;
      if (spi.normalshadow)
      {
         t->color[b] = Sprite8Colsat16(0x3f, 0);
         continue;
      }

      dot = Sprite8ColorRamGetColor(coloroffset + pixel, color_ram);
      if (cc)
      {
         int transparent = 0;
         switch (SPCCCS) {
         case 0: if (priority <= SPCCN) transparent = 1; break;
         case 1: if (priority == SPCCN) transparent = 1; break;
         case 2: if (priority >= SPCCN) transparent = 1; break;
         case 3: if (dot & 0x80000000) transparent = 1; break;
         }
         if (regs->CCCTL & 0x200) {
            alpha = colorcalctable[spi.colorcalc];
            if (transparent) alpha |= 0x80;
         }
         else if (transparent) {
            alpha = colorcalctable[spi.colorcalc];
            if (regs->CCCTL & 0x100) alpha |= 0x80;
         }
      }
      t->color[b] = Sprite8Colsat32(alpha, dot);
   }
}

/* VidsoftSpritePut()'s claim of line y */
static __attribute__((noinline)) struct PixelData *Sprite8Claim(int y)
{
   struct PixelData *line = TitanGetLayerLine(TITAN_SPRITE, y);
   if (TitanClaimLayerLine(TITAN_SPRITE, y))
      memset(line, 0, sizeof(struct PixelData) * vdp2width);
   return line;
}

/* One sprite layer line from an 8-bit framebuffer row (x = i) through the
 * table: the generic loop's pixels. Four empty pixels cost one load. */
static INLINE __attribute__((always_inline)) void Sprite8LineBody(const int OFS, const struct Sprite8Table *t,
   const u8 *row, int width, int y, const vdp2draw_struct *info)
{
   struct PixelData *line = NULL;
   int i = 0, k;

#define SPRITE8_PUT(x) \
   { \
      const int b = row[x]; \
      const u32 meta = t->meta[b]; \
      if (meta) \
      { \
         u32 color = t->color[b]; \
         if (OFS && !t->shadow[b]) \
            color = SPRITE8_ADD(color, info->cor, info->cog, info->cob); \
         if (!line) \
            line = Sprite8Claim(y); \
         line[x].pixel = color; \
         line[x].meta = meta; \
      } \
   }
   for (; i + 4 <= width; i += 4)
   {
      if (*(const u32 *)(row + i) == 0)
         continue;
      for (k = i; k < i + 4; k++)
         SPRITE8_PUT(k)
   }
   for (; i < width; i++)
      SPRITE8_PUT(i)
#undef SPRITE8_PUT
}

static __attribute__((noinline)) void Sprite8Line(const struct Sprite8Table *t, const u8 *row, int width, int y, const vdp2draw_struct *info)
{
   Sprite8LineBody(0, t, row, width, y, info);
}

static __attribute__((noinline)) void Sprite8LineOfs(const struct Sprite8Table *t, const u8 *row, int width, int y, const vdp2draw_struct *info)
{
   Sprite8LineBody(1, t, row, width, y, info);
}

/* VidsoftDrawSprite() for an 8-bit framebuffer (vdp1width wide) where every
 * pixel value means the same all frame: a 1024-wide framebuffer on a hi-res
 * screen (x = i), not rotated, no sprite window, no window on the layer or the
 * colour calc window. The same per-frame values, lines and pixels; with no
 * window enabled there are no line windows to read and the window tests give
 * the same answer for every pixel. 0 = it doesn't apply, nothing was drawn. */
int VidsoftDrawSprite8(Vdp2 *regs, u8 *fb, Vdp1 *vdp1_regs, Vdp2 *lines, u8 *color_ram, int vdp1width)
{
   struct Sprite8Table t;
   vdp2draw_struct info = { 0 };
   u8 prioritytable[8], colorcalctable[8];
   int i2, start_line = 0, line_increment = 0, readout_y = 0, output_y = 0, clof_on;

   if (vdp1width != 1024 || !vdp2_x_hires || (vdp1_regs->TVMR & 2) || (regs->SPCTL & 0x10) ||
       ((regs->WCTLC >> 8) & 0x2a) || ((regs->WCTLD >> 8) & 0x2a) || vdp2width > 704 ||
       !(Vdp1External.disptoggle && (regs->TVMD & 0x8000)))
      return 0;

   prioritytable[0] = regs->PRISA & 0x7;
   prioritytable[1] = (regs->PRISA >> 8) & 0x7;
   prioritytable[2] = regs->PRISB & 0x7;
   prioritytable[3] = (regs->PRISB >> 8) & 0x7;
   prioritytable[4] = regs->PRISC & 0x7;
   prioritytable[5] = (regs->PRISC >> 8) & 0x7;
   prioritytable[6] = regs->PRISD & 0x7;
   prioritytable[7] = (regs->PRISD >> 8) & 0x7;
   colorcalctable[0] = ((~regs->CCRSA & 0x1F) << 1) + 1;
   colorcalctable[1] = ((~regs->CCRSA >> 7) & 0x3E) + 1;
   colorcalctable[2] = ((~regs->CCRSB & 0x1F) << 1) + 1;
   colorcalctable[3] = ((~regs->CCRSB >> 7) & 0x3E) + 1;
   colorcalctable[4] = ((~regs->CCRSC & 0x1F) << 1) + 1;
   colorcalctable[5] = ((~regs->CCRSC >> 7) & 0x3E) + 1;
   colorcalctable[6] = ((~regs->CCRSD & 0x1F) << 1) + 1;
   colorcalctable[7] = ((~regs->CCRSD >> 7) & 0x3E) + 1;
   Sprite8Setup(&t, regs, prioritytable, colorcalctable, !((regs->WCTLD >> 8) & 0x80) && (regs->CCCTL & 0x40),
                (regs->LNCLEN >> 5) & 1, color_ram);

   /* with no window enabled, bit 7 alone hides every pixel */
   if ((regs->WCTLC >> 8) & 0x80)
      return 1;

   Sprite8ColorOffset(regs, &info);
   clof_on = (regs->CLOFEN & 0x40) != 0;
   Vdp2GetInterlaceInfo(&start_line, &line_increment);
   for (i2 = start_line; i2 < vdp2height; i2 += line_increment)
   {
      const int l = vdp2_interlace ? i2 / 2 : i2;
      const int y = vdp2_interlace ? readout_y++ : i2;

      if (l <= 270)   /* LoadLineParamsSprite() */
         Sprite8ColorOffset(lines + l, &info);
      if (clof_on && (info.cor | info.cog | info.cob))
         Sprite8LineOfs(&t, fb + y * vdp1width, vdp2width, output_y, &info);
      else
         Sprite8Line(&t, fb + y * vdp1width, vdp2width, output_y, &info);
      output_y++;
   }
   return 1;
}
