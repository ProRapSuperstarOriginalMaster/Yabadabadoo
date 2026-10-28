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

/*! \file vidsoftkeep.c
    \brief Layer-line reuse for the software renderer: a VDP2 layer line whose
    inputs are those of the last drawn field, which drew it, is not drawn again;
    its Titan row is kept (re-stamped for this frame).

    The inputs of an NBG line drawn by Vdp2DrawScroll(), compared with the last
    drawn field:
    - the frame registers the layer is set up from (as the field's layers saw
      them at DrawScreens), less those it never reads: status, back screen,
      sprite-only and the other layers' own registers, the fraction scroll; the
      zoom as the renderer reads it (& 0x7FF00), the colour offset and colour
      calculation enable as this layer's; plus the display toggle, bad-cycle
      emulation and per-line colour calculation;
    - the line's own registers (Vdp2Lines, the same mask) and, with vertical
      cell scroll, its cell scroll values;
    - the VRAM it reads (4 KB pages) and the colour RAM it reads (32-byte
      blocks): none of them written with a different value since (the write
      handlers in vdp2.c note it, vdp2_vram_vchg / vdp2_cram_vchg).
    What a line reads is found after the layer is drawn, by walking the line's
    tiles with the addressing of Vdp2MapCalcXY(), Vdp2PatternAddr() and
    Vdp2FetchPixelCN() (copies below): its pattern names, the tile row it reads
    from each tile and the palette entry of every dot of that row, plus the line
    window and line scroll tables. The walk takes a superset where it is
    simpler (whole tile rows, the whole line scroll table).

    Only progressive pictures, tile layers on the fast path (no bitmap, mosaic
    or bad-cycle emulation), no sprite window. Rotation
    layers are always drawn. In a file of its own: vidsoft.c sits at GCC's
    per-file inlining limit (code added there changes what gets inlined in its
    hot loops); Vdp2DrawScroll only calls VidsoftKeepRow() per line and
    VidsoftKeepDrawn() at the end.

    YAB_LNKEEP: unset = on, 0 off, 1 on and log, 2 check mode: every line is
    drawn, and one the inputs said is the same as last field's must equal the
    kept row ("lnkeep MISS" = an input the key misses).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "vidsoftkeep.h"
#include "titan/titan.h"

extern int vdp2width, vdp2height, vdp2_interlace;

extern int bad_cycle_setting[6];

/* per layer (Titan index 0..4): the rows this field keeps (in check mode: draws
   and compares) */
static u32 vidsoft_keep_bits[5][8];
static int vidsoft_keep_check;
/* what Vdp2DrawScroll drew a row with (its final y, line scroll x, zoom) */
struct VidsoftKeepLine
{
   int y, lsx;
   float zoom;
};
static struct VidsoftKeepLine vidsoft_keep_line[5][256];

#define KEEP_NREG (sizeof(Vdp2) / 2)
#define KEEP_LINES 256

static int keep_mode = -1;      /* YAB_LNKEEP */
static int keep_log;
static u16 keep_mask[5][KEEP_NREG];

/* this field and the last drawn one, as the layers draw from them */
static Vdp2 keep_regs[2];
static Vdp2 keep_lines[2][KEEP_LINES];
static u32 keep_cells[2][KEEP_LINES][2];
static int keep_glob[2];
static int keep_cur;
static int keep_active;         /* between VidsoftKeepFrame and VidsoftKeepFrameEnd */
static u32 keep_frame;          /* TitanFrame() of this field */
static u32 keep_vchg[4], keep_cchg[4];   /* written with a new value since the last drawn field */

/* a Titan row's record: the frame it was last drawn or kept in (0 = none),
   what it read */
struct KeepRow
{
   u32 frame;
   u32 vm[4], cm[4];
};
static struct KeepRow keep_row[5][KEEP_LINES];
static int keep_layer_ok[5];    /* KeepBegin: rows of this layer may be recorded */
static int keep_layer_began[5];
static u32 keep_layer_frame[5]; /* the field KeepBegin last ran for the layer */

/* check mode: the kept rows as they were before this field drew them again */
static struct PixelData *keep_save[5];
static u8 keep_save_prio[5][KEEP_LINES];

static unsigned long keep_st_drawn[5], keep_st_kept[5], keep_st_rec[5], keep_st_miss;
/* fields a layer was not taken: 1 interlace, 2 > 256 lines, 3 bitmap, 4 mosaic, 5 bad cycle, 6 sprite window,
   7 a line without the layer */
static unsigned long keep_st_why[5][12];
static u32 keep_st_fields;

/* the counter build's register list (lnreuse S19, tools/patches/lnreuse-counter.patch) */
#define KEEP_R(f) { #f, offsetof(Vdp2, f) / 2, sizeof(((Vdp2 *)0)->f) / 2 }
struct KeepRegDef { const char *name; int off, n; };
static const struct KeepRegDef keep_regdefs[] = {
   KEEP_R(TVMD), KEEP_R(EXTEN), KEEP_R(TVSTAT), KEEP_R(VRSIZE), KEEP_R(HCNT), KEEP_R(VCNT), KEEP_R(RAMCTL), KEEP_R(CYCA0L),
   KEEP_R(CYCA0U), KEEP_R(CYCA1L), KEEP_R(CYCA1U), KEEP_R(CYCB0L), KEEP_R(CYCB0U), KEEP_R(CYCB1L), KEEP_R(CYCB1U), KEEP_R(BGON),
   KEEP_R(MZCTL), KEEP_R(SFSEL), KEEP_R(SFCODE), KEEP_R(CHCTLA), KEEP_R(CHCTLB), KEEP_R(BMPNA), KEEP_R(BMPNB), KEEP_R(PNCN0),
   KEEP_R(PNCN1), KEEP_R(PNCN2), KEEP_R(PNCN3), KEEP_R(PNCR), KEEP_R(PLSZ), KEEP_R(MPOFN), KEEP_R(MPOFR), KEEP_R(MPABN0),
   KEEP_R(MPCDN0), KEEP_R(MPABN1), KEEP_R(MPCDN1), KEEP_R(MPABN2), KEEP_R(MPCDN2), KEEP_R(MPABN3), KEEP_R(MPCDN3),
   KEEP_R(MPABRA), KEEP_R(MPCDRA), KEEP_R(MPEFRA), KEEP_R(MPGHRA), KEEP_R(MPIJRA), KEEP_R(MPKLRA), KEEP_R(MPMNRA),
   KEEP_R(MPOPRA), KEEP_R(MPABRB), KEEP_R(MPCDRB), KEEP_R(MPEFRB), KEEP_R(MPGHRB), KEEP_R(MPIJRB), KEEP_R(MPKLRB),
   KEEP_R(MPMNRB), KEEP_R(MPOPRB), KEEP_R(SCXIN0), KEEP_R(SCXDN0), KEEP_R(SCYIN0), KEEP_R(SCYDN0), KEEP_R(ZMXN0),
   KEEP_R(ZMYN0), KEEP_R(SCXIN1), KEEP_R(SCXDN1), KEEP_R(SCYIN1), KEEP_R(SCYDN1), KEEP_R(ZMXN1), KEEP_R(ZMYN1),
   KEEP_R(SCXN2), KEEP_R(SCYN2), KEEP_R(SCXN3), KEEP_R(SCYN3), KEEP_R(ZMCTL), KEEP_R(SCRCTL), KEEP_R(VCSTA), KEEP_R(LSTA0),
   KEEP_R(LSTA1), KEEP_R(LCTA), KEEP_R(BKTAU), KEEP_R(BKTAL), KEEP_R(RPMD), KEEP_R(RPRCTL), KEEP_R(KTCTL), KEEP_R(KTAOF),
   KEEP_R(OVPNRA), KEEP_R(OVPNRB), KEEP_R(RPTA), KEEP_R(WPSX0), KEEP_R(WPSY0), KEEP_R(WPEX0), KEEP_R(WPEY0), KEEP_R(WPSX1),
   KEEP_R(WPSY1), KEEP_R(WPEX1), KEEP_R(WPEY1), KEEP_R(WCTLA), KEEP_R(WCTLB), KEEP_R(WCTLC), KEEP_R(WCTLD), KEEP_R(LWTA0),
   KEEP_R(LWTA1), KEEP_R(SPCTL), KEEP_R(SDCTL), KEEP_R(CRAOFA), KEEP_R(CRAOFB), KEEP_R(LNCLEN), KEEP_R(SFPRMD),
   KEEP_R(CCCTL), KEEP_R(SFCCMD), KEEP_R(PRISA), KEEP_R(PRISB), KEEP_R(PRISC), KEEP_R(PRISD), KEEP_R(PRINA), KEEP_R(PRINB),
   KEEP_R(PRIR), KEEP_R(CCRSA), KEEP_R(CCRSB), KEEP_R(CCRSC), KEEP_R(CCRSD), KEEP_R(CCRNA), KEEP_R(CCRNB), KEEP_R(CCRR),
   KEEP_R(CCRLB), KEEP_R(CLOFEN), KEEP_R(CLOFSL), KEEP_R(COAR), KEEP_R(COAG), KEEP_R(COAB), KEEP_R(COBR), KEEP_R(COBG),
   KEEP_R(COBB) };

static void KeepClr(u16 *m, const char *names)
{
   char buf[16];
   const char *p = names;
   while (*p)
   {
      int n = 0, i, k;
      while (*p == ' ') p++;
      while (*p && *p != ' ' && n < 15) buf[n++] = *p++;
      buf[n] = 0;
      if (!n) break;
      for (i = 0; i < (int)(sizeof(keep_regdefs) / sizeof(keep_regdefs[0])); i++)
         if (!strcmp(keep_regdefs[i].name, buf))
            break;
      if (i == (int)(sizeof(keep_regdefs) / sizeof(keep_regdefs[0])))
      {
         fprintf(stderr, "lnkeep: no register %s\n", buf);
         abort();
      }
      for (k = 0; k < keep_regdefs[i].n; k++)
         m[keep_regdefs[i].off + k] = 0;
   }
}

static void KeepInit(void)
{
   /* titan index 0..4 = NBG3, NBG2, NBG1, NBG0, RBG0 */
   static const char *own[5] = {
      "PNCN3 MPABN3 MPCDN3 SCXN3 SCYN3",
      "PNCN2 MPABN2 MPCDN2 SCXN2 SCYN2",
      "PNCN1 MPABN1 MPCDN1 SCXIN1 SCXDN1 SCYIN1 SCYDN1 ZMXN1 ZMYN1 LSTA1",
      "PNCN0 MPABN0 MPCDN0 SCXIN0 SCXDN0 SCYIN0 SCYDN0 ZMXN0 ZMYN0 LSTA0",
      "PNCR MPABRA MPCDRA MPEFRA MPGHRA MPIJRA MPKLRA MPMNRA MPOPRA MPABRB MPCDRB MPEFRB MPGHRB MPIJRB MPKLRB MPMNRB MPOPRB "
      "RPMD RPRCTL KTCTL KTAOF OVPNRA OVPNRB RPTA" };
   static const u16 ccbit[5] = { 0x8, 0x4, 0x2, 0x1, 0x10 };
   const char *e = getenv("YAB_LNKEEP");
   int l, o;
   keep_mode = e ? atoi(e) : 1;
   keep_log = e && (keep_mode == 1 || keep_mode == 2);
   vidsoft_keep_check = keep_mode == 2;
   for (l = 0; l < 5; l++)
   {
      u16 *m = keep_mask[l];
      int i;
      for (i = 0; i < (int)KEEP_NREG; i++)
         m[i] = 0;
      for (i = 0; i < (int)(sizeof(keep_regdefs) / sizeof(keep_regdefs[0])); i++)
      {
         int k;
         for (k = 0; k < keep_regdefs[i].n; k++)
            m[keep_regdefs[i].off + k] = 0xFFFF;
      }
      /* status, back screen, sprite-only */
      KeepClr(m, "TVSTAT HCNT VCNT BKTAU BKTAL PRISA PRISB PRISC PRISD CCRSA CCRSB CCRSC CCRSD");
      for (o = 0; o < 5; o++)
         if (o != l && !(l == TITAN_NBG0 && o == TITAN_RBG0)) /* NBG0 may be RBG1: keeps the rotation registers */
            KeepClr(m, own[o]);
      /* never read (fraction scroll), compared as read (zoom, colour offset: KeepRegsSame) */
      KeepClr(m, "SCXDN0 SCYDN0 SCXDN1 SCYDN1 ZMXN0 ZMYN0 ZMXN1 ZMYN1 COAR COAG COAB COBR COBG COBB CLOFEN CLOFSL");
      /* colour calculation: this layer's enable and the shared bits (the other layers' enables are theirs) */
      m[offsetof(Vdp2, CCCTL) / 2] = 0xFFA0 | ccbit[l];
   }
}

static int KeepRegsSame(int l, const Vdp2 *a, const Vdp2 *b)
{
   static const u16 bit[5] = { 8, 4, 2, 1, 0x10 };
   const u16 *x = (const u16 *)a, *y = (const u16 *)b, *m = keep_mask[l];
   const u16 c = bit[l];
   u16 d = 0;
   int i;
   for (i = 0; i < (int)KEEP_NREG; i++)
      d |= (x[i] ^ y[i]) & m[i];
   if (d)
      return 0;
   if (l == TITAN_NBG0 && ((a->ZMXN0.all ^ b->ZMXN0.all) & 0x7FF00 || (a->ZMYN0.all ^ b->ZMYN0.all) & 0x7FF00))
      return 0;
   if (l == TITAN_NBG1 && ((a->ZMXN1.all ^ b->ZMXN1.all) & 0x7FF00 || (a->ZMYN1.all ^ b->ZMYN1.all) & 0x7FF00))
      return 0;
   if ((a->CLOFEN & c) != (b->CLOFEN & c))
      return 0;
   if (a->CLOFEN & c)
   {
      if ((a->CLOFSL & c) != (b->CLOFSL & c))
         return 0;
      if (a->CLOFSL & c)
         return a->COBR == b->COBR && a->COBG == b->COBG && a->COBB == b->COBB;
      return a->COAR == b->COAR && a->COAG == b->COAG && a->COAB == b->COAB;
   }
   return 1;
}

/* VidsoftCheckColorCalcLines' test (vidsoft.c): do the lines differ in colour
   calculation (then each layer line takes it from its own line)? */
static int KeepColorCalcLines(void)
{
   const int n = 224 + (Vdp2Regs->TVMD & 0x30) < 270 ? 224 + (Vdp2Regs->TVMD & 0x30) : 270;
   const Vdp2 *l = Vdp2Lines;
   u16 diff = 0;
   int i;
   for (i = 0; i < n; i++)
      diff |= (l[i].CCCTL ^ l[0].CCCTL) | (l[i].CCRNA ^ l[0].CCRNA) |
              (l[i].CCRNB ^ l[0].CCRNB) | (l[i].CCRR ^ l[0].CCRR);
   return diff != 0;
}

void VidsoftKeepFrame(void)
{
   const int bad_cycle = bad_cycle_setting[0] | bad_cycle_setting[1] | bad_cycle_setting[2] |
                         bad_cycle_setting[3] | bad_cycle_setting[4];
   int i, n;
   if (keep_mode < 0)
      KeepInit();
   memset(vidsoft_keep_bits, 0, sizeof(vidsoft_keep_bits));
   keep_active = 0;
   if (keep_mode == 0)
      return;
   keep_cur ^= 1;
   keep_regs[keep_cur] = *Vdp2Regs;
   n = vdp2height < KEEP_LINES ? vdp2height : KEEP_LINES;
   memcpy(keep_lines[keep_cur], Vdp2Lines, sizeof(Vdp2) * n);
   for (i = 0; i < n; i++)
   {
      keep_cells[keep_cur][i][0] = cell_scroll_data[i].data[0];
      keep_cells[keep_cur][i][1] = cell_scroll_data[i].data[1];
   }
   keep_glob[keep_cur] = (Vdp2External.disptoggle & 0xFF) | (KeepColorCalcLines() ? 0x100 : 0) | (bad_cycle ? 0x200 : 0) |
                         (Vdp2Internal.ColorMode << 10);
   memcpy(keep_vchg, vdp2_vram_vchg, sizeof(keep_vchg));
   memcpy(keep_cchg, vdp2_cram_vchg, sizeof(keep_cchg));
   memset(vdp2_vram_vchg, 0, sizeof(vdp2_vram_vchg));
   memset(vdp2_cram_vchg, 0, sizeof(vdp2_cram_vchg));
   keep_frame = TitanFrame() + 1;   /* VIDSoftVdp2DrawScreens' TitanErase */
   keep_active = 1;
   keep_st_fields++;
   if (keep_log && keep_st_fields % 300 == 0)
   {
      static const char *nm[5] = { "NBG3", "NBG2", "NBG1", "NBG0", "RBG0" };
      int l;
      fprintf(stderr, "lnkeep: %u fields%s, misses %lu;", keep_st_fields, vidsoft_keep_check ? " (check mode)" : "",
              keep_st_miss);
      for (l = 0; l < 5; l++)
      {
         int k;
         if (keep_st_drawn[l] + keep_st_kept[l])
            fprintf(stderr, " %s kept %lu of %lu rec %lu;", nm[l], keep_st_kept[l], keep_st_drawn[l] + keep_st_kept[l],
                    keep_st_rec[l]);
         for (k = 1; k < 12; k++)
            if (keep_st_why[l][k])
               fprintf(stderr, " %s not taken (%d) %lu;", nm[l], k, keep_st_why[l][k]);
      }
      fprintf(stderr, "\n");
   }
}

void VidsoftKeepFrameEnd(void)
{
   memset(vidsoft_keep_bits, 0, sizeof(vidsoft_keep_bits));
   keep_active = 0;
}

/* a line of this layer that is drawn (LoadLineParams' enable). Special
   priority mode 1 is fine: the priority bit of a tile carries over to the next
   line only through the cell cache, which starts afresh after a kept line. */
static INLINE int KeepLineOk(int l, const Vdp2 *r)
{
   static const u16 en[4] = { 0x8, 0x4, 0x2, 0x21 };
   return (r->BGON & en[l]) != 0;
}

static void KeepBegin(int layer, vdp2draw_struct *info)
{
   const Vdp2 *fr = &keep_regs[keep_cur], *pfr = &keep_regs[keep_cur ^ 1];
   const Vdp2 *lines = keep_lines[keep_cur], *plines = keep_lines[keep_cur ^ 1];
   const int n = vdp2height, w = vdp2width;
   const u32 prev = keep_frame - 1;
   u32 *bits = vidsoft_keep_bits[layer];
   int y, wctl, cells;

   keep_layer_began[layer] = 0;
   if (!keep_active || layer > TITAN_NBG0 || TitanFrame() != keep_frame || (layer == TITAN_NBG0 && (fr->BGON & 0x20)))
   {
      keep_st_why[layer][!keep_active ? 8 : TitanFrame() != keep_frame ? 9 : 10]++;
      return;   /* RBG0 / RBG1 (Vdp2DrawRotationFP falls through to Vdp2DrawScroll when not rotated) */
   }
   keep_layer_began[layer] = 1;
   keep_layer_ok[layer] = 0;
   wctl = info->wctl;
   {
      const int why = vdp2_interlace ? 1 : n > KEEP_LINES ? 2 : info->isbitmap ? 3 : info->mosaicxmask != 1 ? 4 :
                      (keep_glob[keep_cur] & 0x200) ? 5 : (wctl & 0x20) || (fr->WCTLD & 0x2000) ? 6 : 0;
      if (why)
      {
         keep_st_why[layer][why]++;
         return;
      }
   }
   for (y = 0; y < n; y++)
      if (!(lines[y].BGON & (layer == TITAN_NBG0 ? 0x21 : 8 >> layer)))
      {
         keep_st_why[layer][7]++;
         return;   /* a line without this layer: the rows shift (Vdp2DrawScroll's output_y) */
      }
   keep_layer_ok[layer] = 1;

   if (keep_glob[keep_cur] != keep_glob[keep_cur ^ 1] || !KeepRegsSame(layer, fr, pfr))
      return;
   cells = (layer == TITAN_NBG0 || layer == TITAN_NBG1) && (fr->SCRCTL & 0x101);
   for (y = 0; y < n; y++)
   {
      const struct KeepRow *R = &keep_row[layer][y];
      if (R->frame != prev || TitanLineFrame(layer, y) != prev)
         continue;
      if ((R->vm[0] & keep_vchg[0]) | (R->vm[1] & keep_vchg[1]) | (R->vm[2] & keep_vchg[2]) | (R->vm[3] & keep_vchg[3]) |
          (R->cm[0] & keep_cchg[0]) | (R->cm[1] & keep_cchg[1]) | (R->cm[2] & keep_cchg[2]) | (R->cm[3] & keep_cchg[3]))
         continue;
      if (!KeepLineOk(layer, &lines[y]) || !KeepRegsSame(layer, &lines[y], &plines[y]))
         continue;
      if (cells && (keep_cells[keep_cur][y][0] != keep_cells[keep_cur ^ 1][y][0] ||
                    keep_cells[keep_cur][y][1] != keep_cells[keep_cur ^ 1][y][1]))
         continue;
      bits[y >> 5] |= 1u << (y & 31);
      if (vidsoft_keep_check)
      {
         if (!keep_save[layer])
            keep_save[layer] = (struct PixelData *)malloc(sizeof(struct PixelData) * 704 * KEEP_LINES);
         memcpy(keep_save[layer] + y * 704, TitanGetLayerLine(layer, y), sizeof(struct PixelData) * w);
         keep_save_prio[layer][y] = TitanLineMaxPriority(layer, y);
      }
      else
         TitanKeepLayerLine(layer, y);
   }
}

static INLINE void KeepMarkV(u32 *vm, u32 a)
{
   a &= 0x7FFFF;
   vm[a >> 17] |= 1u << ((a >> 12) & 31);
}

static INLINE void KeepMarkVRange(u32 *vm, u32 a, u32 size)
{
   KeepMarkV(vm, a);
   KeepMarkV(vm, a + size - 1);
}

/* the colour RAM entry idx as Vdp2ColorRamGetColorCM() reads it */
static INLINE void KeepMarkC(u32 *cm, int colormode, u32 idx)
{
   u32 a;
   if (colormode > 2)
      return;
   a = (idx << (colormode == 2 ? 2 : 1)) & 0xFFF;
   cm[a >> 10] |= 1u << ((a >> 5) & 31);
}

/* What one drawn line of a tile layer reads (Vdp2DrawScroll's pixel loop and
   its fast paths): the x of every pixel as that loop computes it (a range
   that covers it when zoomed), the tile of each 8-pixel block as
   Vdp2MapCalcXY() finds it, the row of it that Vdp2FetchPixelCN() reads and
   the palette entries of that row's dots */
static void KeepWalkLine(int layer, vdp2draw_struct *info, const u32 *planetbl, const struct VidsoftKeepLine *L,
                         const Vdp2 *regs, u8 *ram, u32 *vm, u32 *cm)
{
   const int colormode = Vdp2Internal.ColorMode;
   const int cn = info->colornumber;
   const int planepixelwidth_bits = 8 + info->planew, planepixelheight_bits = 8 + info->planeh;
   const int planepixelwidth_mask = (1 << planepixelwidth_bits) - 1;
   const int planepixelheight_mask = (1 << planepixelheight_bits) - 1;
   const int xmask = info->mapwh * info->planew * 512 - 1;
   const int pagesize_bits = info->pagewh_bits * 2;
   const int cellwh = 2 + info->patternwh;
   const int y0 = L->y;
   const int bpp_shift = cn == 0 ? -1 : cn == 1 ? 0 : cn <= 3 ? 1 : 2;   /* bytes per dot: 1/2, 1, 2, 4 */
   int xs, xe, xx, lastblk = -1, lastcell = -1;
   u32 charaddr = 0, paladdr = 0;
   int flipfunction = 0;
   (void)layer;

   if (L->zoom != 1.0f)
   {
      xs = info->x - 1;
      xe = (int)(info->x + (vdp2width - 1) * L->zoom) + 1;
   }
   else
   {
      xs = info->x;
      xe = info->x + vdp2width - 1;
   }
   for (xx = xs; xx <= xe; xx++)
   {
      int x = xx & xmask, y = y0, tx, ty, i;
      u32 row, len;
      if (L->lsx)
         x = (x + L->lsx) & 0x3FF;
      if ((x >> 3) == lastblk)
         continue;   /* same 8-pixel block: same tile and row */
      lastblk = x >> 3;
      if ((((y >> cellwh) << 16) | (x >> cellwh)) != lastcell)
      {
         /* Vdp2MapCalcXY + Vdp2PatternAddr */
         const int planenum = ((y >> planepixelheight_bits) * info->mapwh) + (x >> planepixelwidth_bits);
         const int xp = x & planepixelwidth_mask, yp = y & planepixelheight_mask;
         u32 addr;
         lastcell = ((y >> cellwh) << 16) | (x >> cellwh);
         addr = planetbl[planenum];
         addr += ((((yp >> 9) << pagesize_bits) << info->planew_bits) + ((xp >> 9) << pagesize_bits) +
                  (((yp & 511) >> cellwh) << info->pagewh_bits) + ((xp & 511) >> cellwh)) << (info->patterndatasize_bits + 1);
         if (info->patterndatasize == 1)
         {
            const u16 tmp = T1ReadWord(ram, addr & 0x7FFFF);
            KeepMarkVRange(vm, addr, 2);
            paladdr = cn == 0 ? ((tmp & 0xF000) >> 8) | ((info->supplementdata & 0xE0) << 3) : (tmp & 0x7000) >> 4;
            if (info->auxmode == 0)
            {
               flipfunction = (tmp & 0xC00) >> 10;
               charaddr = info->patternwh == 1 ? (tmp & 0x3FF) | ((info->supplementdata & 0x1F) << 10) :
                          ((tmp & 0x3FF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x1C) << 10);
            }
            else
            {
               flipfunction = 0;
               charaddr = info->patternwh == 1 ? (tmp & 0xFFF) | ((info->supplementdata & 0x1C) << 10) :
                          ((tmp & 0xFFF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x10) << 10);
            }
         }
         else
         {
            const u16 tmp1 = T1ReadWord(ram, addr & 0x7FFFF), tmp2 = T1ReadWord(ram, (addr + 2) & 0x7FFFF);
            KeepMarkVRange(vm, addr, 4);
            charaddr = tmp2 & 0x7FFF;
            flipfunction = (tmp1 & 0xC000) >> 14;
            paladdr = cn == 0 ? (tmp1 & 0x7F) << 4 : (tmp1 & 0x70) << 4;
         }
         if (!(regs->VRSIZE & 0x8000))
            charaddr &= 0x3FFF;
         charaddr *= 0x20;
      }
      /* the pixel in the tile (Vdp2MapCalcXY): the row it reads */
      tx = x & planepixelwidth_mask;
      ty = y & planepixelheight_mask;
      if (info->patternwh == 1)
      {
         ty &= 7;
         if (flipfunction & 2)
            ty = 7 - ty;
      }
      else
      {
         ty &= 15;
         if (flipfunction)
         {
            if (flipfunction & 0x2)
               ty = !(ty & 8) ? 8 - 1 - ty + 16 : 16 - 1 - ty;
            else if (ty & 8)
               ty += 8;
            if (flipfunction & 0x1)
            {
               if (!(tx & 8))
                  ty += 8;
            }
            else if (tx & 8)
               ty += 8;
         }
         else
         {
            if (ty & 8)
               ty += 8;
            if (tx & 8)
               ty += 8;
         }
      }
      /* Vdp2FetchPixelCN: dots (ty * cellw + 0..7) */
      if (cn > 4)
         continue;
      row = bpp_shift < 0 ? charaddr + (u32)(ty * info->cellw) / 2 : charaddr + ((u32)(ty * info->cellw) << bpp_shift);
      len = bpp_shift < 0 ? 4 : 8u << bpp_shift;
      KeepMarkVRange(vm, row, len);
      if (cn <= 1)
         for (i = 0; i < 8; i++)
         {
            const u32 dot = cn == 0 ? (T1ReadByte(ram, (row + i / 2) & 0x7FFFF) >> ((i & 1) ? 0 : 4)) & 0xF :
                                      T1ReadByte(ram, (row + i) & 0x7FFFF);
            KeepMarkC(cm, colormode, info->coloroffset + (paladdr | dot));
         }
      else if (cn == 2)
         for (i = 0; i < 8; i++)
            KeepMarkC(cm, colormode, info->coloroffset + T1ReadWord(ram, (row + i * 2) & 0x7FFFF));
   }
}

static void KeepEnd(int layer, vdp2draw_struct *info, u8 *ram)
{
   const Vdp2 *fr = &keep_regs[keep_cur];
   const Vdp2 *lines = keep_lines[keep_cur];
   const int n = vdp2height, w = vdp2width;
   const u32 *bits = vidsoft_keep_bits[layer];
   u32 lw[4] = { 0, 0, 0, 0 };
   int y;

   if (!keep_layer_began[layer])
      return;
   keep_layer_began[layer] = 0;
   if (!keep_layer_ok[layer])
   {
      memset(keep_row[layer], 0, sizeof(keep_row[layer]));
      return;
   }
   /* the line scroll table, read for every line before the line loop (to where it stopped) */
   if (info->islinescroll)
   {
      const u32 lsta = layer == TITAN_NBG0 ? fr->LSTA0.all : fr->LSTA1.all;
      u32 a = (lsta & 0x7FFFE) << 1, end = info->linescrolltbl + 4;
      for (; a < end; a += 0x1000)
         KeepMarkV(lw, a);
      KeepMarkV(lw, end);
   }
   for (y = 0; y < n; y++)
   {
      struct KeepRow *R = &keep_row[layer][y];
      if (bits[y >> 5] & (1u << (y & 31)))
      {
         keep_st_kept[layer]++;
         if (vidsoft_keep_check)
         {
            const struct PixelData *now = TitanGetLayerLine(layer, y);
            if (memcmp(now, keep_save[layer] + y * 704, sizeof(struct PixelData) * w) ||
                TitanLineMaxPriority(layer, y) != keep_save_prio[layer][y])
            {
               int x = 0;
               while (x < w && !memcmp(now + x, keep_save[layer] + y * 704 + x, sizeof(struct PixelData)))
                  x++;
               keep_st_miss++;
               fprintf(stderr, "lnkeep MISS: field %u layer %d row %d (first differing x %d, prio %d/%d) (%lu misses)\n",
                       keep_frame, layer, y, x, TitanLineMaxPriority(layer, y), keep_save_prio[layer][y], keep_st_miss);
            }
         }
         R->frame = keep_frame;
         continue;
      }
      if (TitanLineFrame(layer, y) != keep_frame || !KeepLineOk(layer, &lines[y]))
      {
         R->frame = 0;
         continue;
      }
      keep_st_drawn[layer]++;
      {
         /* the line's plane table (LoadLineParams' GeneratePlaneAddrTable) */
         u32 planetbl[16];
         int i;
         for (i = 0; i < info->mapwh * info->mapwh; i++)
         {
            info->PlaneAddr(info, i, (Vdp2 *)&lines[y]);
            planetbl[i] = info->addr;
         }
         memcpy(R->vm, lw, sizeof(lw));
         memset(R->cm, 0, sizeof(R->cm));
         if (info->islinewindow & 1)
            KeepMarkVRange(R->vm, ((fr->LWTA0.all & 0x7FFFE) << 1) + y * 4, 4);
         if (info->islinewindow & 2)
            KeepMarkVRange(R->vm, ((fr->LWTA1.all & 0x7FFFE) << 1) + y * 4, 4);
         KeepWalkLine(layer, info, planetbl, &vidsoft_keep_line[layer][y], fr, ram, R->vm, R->cm);
         R->frame = keep_frame;
         keep_st_rec[layer]++;
      }
   }
}

int VidsoftKeepRow(vdp2draw_struct *info, int row, int y, int lsx)
{
   const int layer = info->titan_which_layer;
   struct VidsoftKeepLine *L;
   if (layer > TITAN_NBG0)
      return 0;
   if (keep_layer_frame[layer] != keep_frame)
   {
      keep_layer_frame[layer] = keep_frame;
      KeepBegin(layer, info);
   }
   L = &vidsoft_keep_line[layer][row & 255];
   L->y = y;
   L->lsx = lsx;
   L->zoom = info->coordincx;
   return !vidsoft_keep_check && (vidsoft_keep_bits[layer][(row >> 5) & 7] >> (row & 31) & 1);
}

void VidsoftKeepDrawn(vdp2draw_struct *info, u8 *ram)
{
   const int layer = info->titan_which_layer;
   if (layer <= TITAN_NBG0 && keep_layer_began[layer])
      KeepEnd(layer, info, ram);
}
