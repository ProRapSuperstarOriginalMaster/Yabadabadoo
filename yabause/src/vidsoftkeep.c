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

    RBG0 when it is not rotated (Vdp2DrawRotationFP() hands it to
    Vdp2DrawScroll()) is kept the same way; its position and zoom come from the
    rotation parameter tables in VRAM, which every row of it counts as read,
    and its plane table from the frame registers (not regenerated per line).

    RBG0 rotated with coefficients per line (the Vdp2RotKLine* loops: Shining
    Force III, Panzer Dragoon): a row is also compared by the values its
    pixel loop starts from (the parameter, its start, steps, scale and
    offsets: the rotation and coefficient tables' only effect on it), and its
    read set is found by walking its pixels with that loop's arithmetic. The
    tile lookup state carries from line to line (two parameters share the
    tile fields), so a tile row is kept only when its first pixel looks a tile
    up afresh, and after it the state is set as drawing it leaves it (a lookup
    at its last pixel, VidsoftRotLookup()). In check mode the next row's call
    compares that prediction with the drawn state ("rotkeep CACHE MISS").

    Tile layers on the fast path only (no bitmap, mosaic or bad-cycle
    emulation), no sprite window. With double-density interlace each field has
    its own Titan rows (titan.c row_base) and a row is compared with the field
    before the last, which drew it. Rotated
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

extern int vdp2width, vdp2height, vdp2_interlace, rbg0width;

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
#define VW VDP2_VCHG_WORDS   /* words of a VRAM page mask */

static int keep_mode = -1;      /* YAB_LNKEEP */
static int keep_log;
static u16 keep_mask[5][KEEP_NREG];

/* the last KEEP_HIST drawn fields (this one = keep_cur), as the layers drew
   from them: registers, line registers, cell scroll, the other inputs, the
   Titan frame and field parity, and what was written with a new value between
   the field before it and it. A row is compared with the field that drew it:
   the last one, or with double-density interlace the one before (same
   parity: each field has its own Titan rows). */
#define KEEP_HIST 4
static Vdp2 keep_regs[KEEP_HIST];
static Vdp2 keep_lines[KEEP_HIST][KEEP_LINES];
static u32 keep_cells[KEEP_HIST][KEEP_LINES][2];
static int keep_glob[KEEP_HIST];
static u32 keep_hframe[KEEP_HIST];
static int keep_hpar[KEEP_HIST];
static u32 keep_vchg[KEEP_HIST][VW], keep_cchg[KEEP_HIST][4];
static u32 keep_seq;
static int keep_cur;
static int keep_active;         /* between VidsoftKeepFrame and VidsoftKeepFrameEnd */
static u32 keep_frame;          /* TitanFrame() of this field */

/* a Titan row's record: the frame it was last drawn or kept in (0 = none),
   what it read */
#define KEEP_GEOM 12
struct KeepRow
{
   u32 frame;
   u32 vm[VW], cm[4];
   u8 miss;   /* fields in a row it was recorded and then not kept (KEEP_IDLE on: recorded every 8th field) */
   u8 rot;    /* recorded by the rotated path (its geometry in g) */
   s32 g[KEEP_GEOM];
};
static struct KeepRow keep_row[5][2 * KEEP_LINES];   /* [parity * 256 + row] */
static int keep_layer_ok[5];    /* KeepBegin: rows of this layer may be recorded */
static int keep_layer_began[5];
static u32 keep_layer_frame[5]; /* the field KeepBegin last ran for the layer */
/* fields in a row the layer kept no row (scrolling, or what it reads keeps
   changing): from KEEP_IDLE on, its rows are recorded only every 8th field */
static int keep_layer_idle[5];
#define KEEP_IDLE 4

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

static void KeepLog(void);
static unsigned long keep_st_cmiss;

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
   if (keep_log)
      atexit(KeepLog);
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

static void KeepLog(void)
{
   static const char *nm[5] = { "NBG3", "NBG2", "NBG1", "NBG0", "RBG0" };
   int l;
   fprintf(stderr, "lnkeep: %u fields%s, misses %lu, cache misses %lu;", keep_st_fields,
           vidsoft_keep_check ? " (check mode)" : "", keep_st_miss, keep_st_cmiss);
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
   keep_cur = ++keep_seq % KEEP_HIST;
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
   memcpy(keep_vchg[keep_cur], vdp2_vram_vchg, sizeof(keep_vchg[0]));
   memcpy(keep_cchg[keep_cur], vdp2_cram_vchg, sizeof(keep_cchg[0]));
   memset(vdp2_vram_vchg, 0, sizeof(vdp2_vram_vchg));
   memset(vdp2_cram_vchg, 0, sizeof(vdp2_cram_vchg));
   keep_frame = TitanFrame() + 1;   /* VIDSoftVdp2DrawScreens' TitanErase */
   keep_hframe[keep_cur] = keep_frame;
   keep_hpar[keep_cur] = vdp2_interlace ? vdp2_is_odd_frame : -1;
   keep_active = 1;
   keep_st_fields++;
   if (keep_log && keep_st_fields % 300 == 0)
      KeepLog();
}

void VidsoftKeepFrameEnd(void)
{
   memset(vidsoft_keep_bits, 0, sizeof(vidsoft_keep_bits));
   keep_active = 0;
}

/* a line of this layer that is drawn (LoadLineParams' enable; RBG0's line
   parameters leave it as the frame's: every line is drawn). Special
   priority mode 1 is fine: the priority bit of a tile carries over to the next
   line only through the cell cache, which starts afresh after a kept line. */
static INLINE int KeepLineOk(int l, const Vdp2 *r)
{
   static const u16 en[4] = { 0x8, 0x4, 0x2, 0x21 };
   return l == TITAN_RBG0 || (r->BGON & en[l]) != 0;
}

/* rows a layer draws into this field (Vdp2DrawScroll's output_y), the field
   its rows are compared with (1 or 2 back), this field's rows in keep_row */
static INLINE int KeepRows(void) { return vdp2_interlace ? vdp2height / 2 : vdp2height; }
static INLINE int KeepBack(void) { return vdp2_interlace ? 2 : 1; }
static INLINE int KeepSlot0(void) { return vdp2_interlace && vdp2_is_odd_frame ? KEEP_LINES : 0; }

/* where Vdp2ReadRotationTableFP() reads the rotation parameter tables: A at
   this address, B at its 0x80 (each up to 0x6C bytes) */
static INLINE u32 KeepRotTable(const Vdp2 *r)
{
   return (r->RPTA.all << 1) & 0x000FFF7C;
}

static void KeepBegin(int layer, vdp2draw_struct *info)
{
   const int back = KeepBack(), pi = (keep_seq - back) % KEEP_HIST;
   const Vdp2 *fr = &keep_regs[keep_cur], *pfr = &keep_regs[pi];
   const Vdp2 *lines = keep_lines[keep_cur], *plines = keep_lines[pi];
   const int n = KeepRows(), w = vdp2width, s0 = KeepSlot0();
   const u32 prev = keep_frame - back;
   u32 *bits = vidsoft_keep_bits[layer];
   u32 vchg[VW], cchg[4];
   int y, wctl, cells, k;

   keep_layer_began[layer] = 0;
   if (!keep_active || TitanFrame() != keep_frame || (layer == TITAN_NBG0 && (fr->BGON & 0x20)))
   {
      keep_st_why[layer][!keep_active ? 8 : TitanFrame() != keep_frame ? 9 : 10]++;
      return;   /* RBG1 (Vdp2DrawRotationFP falls through to Vdp2DrawScroll when not rotated) */
   }
   keep_layer_began[layer] = 1;
   keep_layer_ok[layer] = 0;
   wctl = info->wctl;
   {
      const int why = n > KEEP_LINES ? 2 : info->isbitmap && layer != TITAN_RBG0 ? 3 : info->mosaicxmask != 1 ? 4 :
                      (keep_glob[keep_cur] & 0x200) ? 5 : (wctl & 0x20) || (fr->WCTLD & 0x2000) ? 6 :
                      layer == TITAN_RBG0 && KeepRotTable(fr) + 0x100 > 0x80000 ? 11 : 0;
      if (why)
      {
         keep_st_why[layer][why]++;
         return;
      }
   }
   for (y = 0; y < n && layer != TITAN_RBG0; y++)
      if (!(lines[y].BGON & (layer == TITAN_NBG0 ? 0x21 : 8 >> layer)))
      {
         keep_st_why[layer][7]++;
         return;   /* a line without this layer: the rows shift (Vdp2DrawScroll's output_y) */
      }
   keep_layer_ok[layer] = 1;
   keep_layer_idle[layer]++;

   if (keep_seq < (u32)back + 1 || keep_hframe[pi] != prev || keep_hpar[pi] != keep_hpar[keep_cur] ||
       keep_glob[keep_cur] != keep_glob[pi] || !KeepRegsSame(layer, fr, pfr))
      return;
   /* written since the field that drew the rows */
   for (k = 0; k < VW; k++)
      vchg[k] = keep_vchg[keep_cur][k] | (back == 2 ? keep_vchg[(keep_seq - 1) % KEEP_HIST][k] : 0);
   for (k = 0; k < 4; k++)
      cchg[k] = keep_cchg[keep_cur][k] | (back == 2 ? keep_cchg[(keep_seq - 1) % KEEP_HIST][k] : 0);
   cells = (layer == TITAN_NBG0 || layer == TITAN_NBG1) && (fr->SCRCTL & 0x101);
   for (y = 0; y < n; y++)
   {
      struct KeepRow *R = &keep_row[layer][s0 + y];
      if (R->frame != prev || R->rot || TitanLineFrame(layer, y) != prev)
         continue;
      if (R->miss < 255)
         R->miss++;   /* until kept below */
      {
         u32 d = (R->cm[0] & cchg[0]) | (R->cm[1] & cchg[1]) | (R->cm[2] & cchg[2]) | (R->cm[3] & cchg[3]);
         for (k = 0; k < VW; k++)
            d |= R->vm[k] & vchg[k];
         if (d)
            continue;
      }
      if (!KeepLineOk(layer, &lines[y]) || !KeepRegsSame(layer, &lines[y], &plines[y]))
         continue;
      if (cells && (keep_cells[keep_cur][y][0] != keep_cells[pi][y][0] ||
                    keep_cells[keep_cur][y][1] != keep_cells[pi][y][1]))
         continue;
      bits[y >> 5] |= 1u << (y & 31);
      keep_layer_idle[layer] = 0;
      R->miss = 0;
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
   vm[a >> (VDP2_VCHG_SHIFT + 5)] |= 1u << ((a >> VDP2_VCHG_SHIFT) & 31);
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
   int xs, xe, xx, step = 1, lastblk = -1, lastcell = -1;
   const int zoomed = L->zoom != 1.0f;
   u32 charaddr = 0, paladdr = 0;
   int flipfunction = 0;
   (void)layer;

   if (zoomed)
   {
      xs = info->x - 1;
      xe = (int)(info->x + (vdp2width - 1) * L->zoom) + 1;
   }
   else
   {
      xs = info->x;
      xe = info->x + vdp2width - 1;
   }
   for (xx = xs; xx <= xe; xx += step)
   {
      int x = xx & xmask, y = y0, tx, ty, i;
      u32 row, len;
      if (L->lsx)
         x = (x + L->lsx) & 0x3FF;
      /* unzoomed, x steps by one per pixel: go to the next 8-pixel block, or
         to where xx wraps at the map's width (the line scroll can put that
         inside a block) */
      if (!zoomed)
      {
         step = 8 - (x & 7);
         if (step > xmask + 1 - (xx & xmask))
            step = xmask + 1 - (xx & xmask);
      }
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

/* The same for a bitmap line (unrotated RBG0 only: an NBG bitmap's address
   can change per line): every x the pixel loop can compute (the bitmap's
   width wraps, then the line scroll) and the dot there */
static void KeepWalkBitmapLine(vdp2draw_struct *info, const struct VidsoftKeepLine *L, u8 *ram, u32 *vm, u32 *cm)
{
   const int colormode = Vdp2Internal.ColorMode;
   const int cn = info->colornumber, cellw = info->cellw, xmask = info->cellw - 1, y = L->y;
   const u32 charaddr = info->charaddr, paladdr = info->paladdr;
   const int zoomed = L->zoom != 1.0f;
   int xs, xe, xx;
   if (zoomed)
   {
      xs = info->x - 1;
      xe = (int)(info->x + (vdp2width - 1) * L->zoom) + 1;
   }
   else
   {
      xs = info->x;
      xe = info->x + vdp2width - 1;
   }
   if (xe - xs > 2 * 1024)
      xe = xs + 2 * 1024;   /* every x of the bitmap row and the line scroll (wraps at 1024 and the width) */
   for (xx = xs; xx <= xe; xx++)
   {
      int x = xx & xmask;
      u32 a, dot;
      if (L->lsx)
         x = (x + L->lsx) & 0x3FF;
      /* Vdp2FetchPixelCN */
      switch (cn)
      {
         case 0:
            a = charaddr + ((y * cellw) + x) / 2;
            dot = T1ReadByte(ram, a & 0x7FFFF);
            if (!(x & 1))
               dot >>= 4;
            KeepMarkV(vm, a);
            if ((dot & 0xF) || !info->transparencyenable)
               KeepMarkC(cm, colormode, info->coloroffset + (paladdr | (dot & 0xF)));
            break;
         case 1:
            a = charaddr + (y * cellw) + x;
            dot = T1ReadByte(ram, a & 0x7FFFF);
            KeepMarkV(vm, a);
            if (dot || !info->transparencyenable)
               KeepMarkC(cm, colormode, info->coloroffset + (paladdr | dot));
            break;
         case 2:
            a = charaddr + ((y * cellw) + x) * 2;
            dot = T1ReadWord(ram, a & 0x7FFFF);
            KeepMarkVRange(vm, a, 2);
            if (dot || !info->transparencyenable)
               KeepMarkC(cm, colormode, info->coloroffset + dot);
            break;
         case 3:
            KeepMarkVRange(vm, charaddr + ((y * cellw) + x) * 2, 2);
            break;
         case 4:
            KeepMarkVRange(vm, charaddr + ((y * cellw) + x) * 4, 4);
            break;
      }
   }
}

static void KeepEnd(int layer, vdp2draw_struct *info, u8 *ram)
{
   const Vdp2 *fr = &keep_regs[keep_cur];
   const Vdp2 *lines = keep_lines[keep_cur];
   const int n = KeepRows(), w = vdp2width, s0 = KeepSlot0();
   const int par = vdp2_interlace ? vdp2_is_odd_frame : 0;   /* Vdp2GetInterlaceInfo's start_line */
   const u32 *bits = vidsoft_keep_bits[layer];
   u32 lw[VW];
   /* this field's number among those of its parity (the probes below must reach both fields) */
   const u32 pf = vdp2_interlace ? keep_frame >> 1 : keep_frame;
   const int record = keep_layer_idle[layer] < KEEP_IDLE || (pf & 7) == 0;
   int y;

   if (!keep_layer_began[layer])
      return;
   keep_layer_began[layer] = 0;
   if (!keep_layer_ok[layer])
   {
      memset(keep_row[layer], 0, sizeof(keep_row[layer]));
      return;
   }
   memset(lw, 0, sizeof(lw));
   /* the line scroll table, read for every line before the line loop (to where it stopped) */
   if (info->islinescroll)
   {
      const u32 lsta = layer == TITAN_NBG0 ? fr->LSTA0.all : fr->LSTA1.all;
      u32 a = (lsta & 0x7FFFE) << 1, end = info->linescrolltbl + 4;
      for (; a < end; a += 1u << VDP2_VCHG_SHIFT)
         KeepMarkV(lw, a);
      KeepMarkV(lw, end);
   }
   /* RBG0: its position and zoom (the rotation parameter tables) */
   if (layer == TITAN_RBG0)
   {
      const u32 a = KeepRotTable(fr);
      KeepMarkVRange(lw, a, 0x6C);
      KeepMarkVRange(lw, a | 0x80, 0x6C);
   }
   for (y = 0; y < n; y++)
   {
      struct KeepRow *R = &keep_row[layer][s0 + y];
      /* the display line drawn into this row (line window tables are per display line) */
      const int j = vdp2_interlace ? 2 * y + par : y;
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
      if (!record || (R->miss >= KEEP_IDLE && ((pf + y) & 7)))
      {
         R->frame = 0;
         continue;
      }
      {
         /* the line's plane table (LoadLineParams' GeneratePlaneAddrTable) */
         u32 planetbl[16];
         int i;
         for (i = 0; i < info->mapwh * info->mapwh; i++)
         {
            info->PlaneAddr(info, i, (Vdp2 *)(layer == TITAN_RBG0 ? fr : &lines[y]));
            planetbl[i] = info->addr;
         }
         memcpy(R->vm, lw, sizeof(lw));
         memset(R->cm, 0, sizeof(R->cm));
         if (info->islinewindow & 1)
            KeepMarkVRange(R->vm, ((fr->LWTA0.all & 0x7FFFE) << 1) + j * 4, 4);
         if (info->islinewindow & 2)
            KeepMarkVRange(R->vm, ((fr->LWTA1.all & 0x7FFFE) << 1) + j * 4, 4);
         if (info->isbitmap)
            KeepWalkBitmapLine(info, &vidsoft_keep_line[layer][y], ram, R->vm, R->cm);
         else
         KeepWalkLine(layer, info, planetbl, &vidsoft_keep_line[layer][y], fr, ram, R->vm, R->cm);
         R->rot = 0;
         R->frame = keep_frame;
         keep_st_rec[layer]++;
      }
   }
}

int VidsoftKeepRow(vdp2draw_struct *info, int row, int y, int lsx)
{
   const int layer = info->titan_which_layer;
   struct VidsoftKeepLine *L;
   if (layer > TITAN_RBG0)
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
   if (layer <= TITAN_RBG0 && keep_layer_began[layer])
      KeepEnd(layer, info, ram);
}

/* Rotated RBG0 (the coefficient path's per-line loops, Vdp2RotKLine*) */

/* this field: began / may keep rows / frame inputs as the last field's / the
   writes since it */
static u32 rot_frame;
static int rot_began, rot_ok, rot_same;
static u32 rot_vchg[VW], rot_cchg[4];
/* per row this field: the geometry its loop started from, the screen of its
   parameter, whether it may be recorded */
struct KeepRotLine
{
   u32 frame;
   s32 g[KEEP_GEOM];
   int which, rec;
};
static struct KeepRotLine rot_line[KEEP_LINES];
static screeninfo_struct rot_sinfo[2];
static u32 rot_sinfo_frame[2];
/* check mode: the lookup state predicted after a kept row (row, its screen) */
static int rot_pred_row = -1;
static u32 rot_pred_frame;
static screeninfo_struct *rot_pred_qs;
static int rot_pred_oc;
static u32 rot_pred_t[6];

/* the tile fields of the lookup state (Vdp2PatternAddr) */
static INLINE void RotTileGet(const vdp2draw_struct *info, u32 *t)
{
   t[0] = info->charaddr; t[1] = info->paladdr; t[2] = info->flipfunction;
   t[3] = info->specialfunction; t[4] = info->specialcolorfunction; t[5] = info->priority;
}

static void KeepBeginRot(vdp2draw_struct *info)
{
   const int layer = TITAN_RBG0, pi = (keep_seq - 1) % KEEP_HIST;
   const Vdp2 *fr = &keep_regs[keep_cur], *pfr = &keep_regs[pi];
   int why, k;
   rot_began = rot_ok = rot_same = 0;
   if (!keep_active || TitanFrame() != keep_frame)
   {
      keep_st_why[layer][!keep_active ? 8 : 9]++;
      return;
   }
   rot_began = 1;
   why = vdp2_interlace ? 1 : vdp2height > KEEP_LINES ? 2 : info->mosaicxmask != 1 || info->mosaicymask != 1 ? 4 :
         (keep_glob[keep_cur] & 0x200) ? 5 : (info->wctl & 0x20) || (fr->WCTLD & 0x2000) ? 6 : 0;
   if (why)
   {
      keep_st_why[layer][why]++;
      return;
   }
   rot_ok = 1;
   keep_layer_idle[layer]++;
   rot_same = keep_seq >= 2 && keep_hframe[pi] == keep_frame - 1 && keep_hpar[pi] == keep_hpar[keep_cur] &&
              keep_glob[keep_cur] == keep_glob[pi] && KeepRegsSame(layer, fr, pfr);
   for (k = 0; k < VW; k++)
      rot_vchg[k] = keep_vchg[keep_cur][k];
   for (k = 0; k < 4; k++)
      rot_cchg[k] = keep_cchg[keep_cur][k];
}

/* the position of pixel i of a line as the per-line loop computes it (masked;
   0 = the screen-over rule drops it) */
static INLINE int RotPos(const s32 *g, const screeninfo_struct *qs, int i, int *px, int *py)
{
   const u32 cx = (u32)g[0] + (u32)g[6] * (u32)i, cy = (u32)g[1] + (u32)g[7] * (u32)i;
   int x = touint(mulfixed(g[2], (fixed32)cx) + g[4]);
   int y = touint(mulfixed(g[3], (fixed32)cy) + g[5]);
   const int over = g[8] & 3;
   if (over <= 1)
   {
      x &= qs->xmask;
      y &= qs->ymask;
   }
   else if (over == 2)
   {
      if (x > qs->xmask || y > qs->ymask)
         return 0;
   }
   else if (x > 512 || y > 512)
      return 0;
   *px = x;
   *py = y;
   return 1;
}

/* the pixel loop's window test (a table, or one answer for the line) */
static INLINE int RotWin(const vdp2draw_struct *info, const u8 *wintab, int i)
{
   return (info->wctl & 0x2a) == 0 ? !(info->wctl & 0x80) : wintab[i];
}

int VidsoftKeepRot(vdp2draw_struct *info, vdp2rotationparameterfp_struct *q, screeninfo_struct *qs,
                   fixed32 Xs, fixed32 Ys, Vdp2 *regs, u8 *ram, const u8 *wintab, int j)
{
   const int layer = info->titan_which_layer;
   const int w = rbg0width, cellwh = 2 + info->patternwh;
   struct KeepRotLine *L;
   struct KeepRow *R;
   s32 g[KEEP_GEOM];
   int which, i0, i1, x0 = 0, y0 = 0, x1 = 0, y1 = 0, fresh = 1, k;
   if (layer != TITAN_RBG0 || keep_mode == 0)
      return 0;
   if (rot_frame != keep_frame)
   {
      rot_frame = keep_frame;
      KeepBeginRot(info);
   }
   if (vidsoft_keep_check && rot_pred_row >= 0)
   {
      /* the lookup state the last row was predicted to leave */
      if (rot_pred_frame == keep_frame && rot_pred_row == j - 1)
      {
         u32 t[6];
         RotTileGet(info, t);
         if (rot_pred_qs->oldcellcheck != rot_pred_oc || memcmp(t, rot_pred_t, sizeof(t)))
         {
            keep_st_cmiss++;
            fprintf(stderr, "rotkeep CACHE MISS: field %u row %d (cell %x/%x) (%lu)\n", keep_frame, j - 1,
                    rot_pred_qs->oldcellcheck, rot_pred_oc, keep_st_cmiss);
         }
      }
      rot_pred_row = -1;
   }
   if (!rot_ok || j < 0 || j >= vdp2height || j >= KEEP_LINES)
      return 0;
   which = q->PlaneAddr == (void FASTCALL (*)(void *, int, Vdp2 *))&Vdp2ParameterBPlaneAddr;
   g[0] = Xs; g[1] = Ys; g[2] = q->kx; g[3] = q->ky; g[4] = q->Xp; g[5] = q->Yp; g[6] = q->dX; g[7] = q->dY;
   g[8] = q->screenover | (which << 2) | (info->isbitmap ? 8 : 0) | (info->colornumber << 4);
   g[9] = info->priority; g[10] = info->alpha; g[11] = info->linescreen;
   L = &rot_line[j];
   L->frame = keep_frame;
   memcpy(L->g, g, sizeof(g));
   L->which = which;
   L->rec = 0;
   if (rot_sinfo_frame[which] != keep_frame)
   {
      rot_sinfo_frame[which] = keep_frame;
      rot_sinfo[which] = *qs;
   }
   if (info->specialprimode == 1 || ((info->wctl & 0x2a) && !wintab) || info->colornumber > 4)
      return 0;
   /* the first and the last pixel the loop draws (window and screen-over) */
   for (i0 = 0; i0 < w && !(RotWin(info, wintab, i0) && RotPos(g, qs, i0, &x0, &y0)); i0++)
      ;
   if (i0 == w)
      return 0;   /* draws nothing */
   for (i1 = w - 1; !(RotWin(info, wintab, i1) && RotPos(g, qs, i1, &x1, &y1)); i1--)
      ;
   if (!info->isbitmap)
   {
      /* the row's pixels depend only on its key when its first pixel looks a
         tile up (not the cached one: two parameters share its fields) */
      fresh = ((((y0 >> cellwh) << 16) | (x0 >> cellwh)) != qs->oldcellcheck);
      if (!fresh)
         return 0;
   }
   L->rec = 1;
   R = &keep_row[layer][j];
   if (!rot_same || R->frame != keep_frame - 1 || !R->rot || TitanLineFrame(layer, j) != keep_frame - 1)
      return 0;
   if (R->miss < 255)
      R->miss++;   /* until kept below */
   {
      u32 d = (R->cm[0] & rot_cchg[0]) | (R->cm[1] & rot_cchg[1]) | (R->cm[2] & rot_cchg[2]) | (R->cm[3] & rot_cchg[3]);
      for (k = 0; k < VW; k++)
         d |= R->vm[k] & rot_vchg[k];
      if (d || memcmp(R->g, g, sizeof(g)))
         return 0;
   }
   {
      const int pi = (keep_seq - 1) % KEEP_HIST;
      if (!KeepLineOk(layer, &keep_lines[keep_cur][j]) || !KeepRegsSame(layer, &keep_lines[keep_cur][j], &keep_lines[pi][j]))
         return 0;
   }
   vidsoft_keep_bits[layer][j >> 5] |= 1u << (j & 31);
   keep_layer_idle[layer] = 0;
   R->miss = 0;
   if (vidsoft_keep_check)
   {
      if (!keep_save[layer])
         keep_save[layer] = (struct PixelData *)malloc(sizeof(struct PixelData) * 704 * KEEP_LINES);
      memcpy(keep_save[layer] + j * 704, TitanGetLayerLine(layer, j), sizeof(struct PixelData) * vdp2width);
      keep_save_prio[layer][j] = TitanLineMaxPriority(layer, j);
      if (!info->isbitmap)
      {
         /* predict the lookup state drawing the row leaves (as below), then
            put it back: this row is drawn */
         u32 t[6], addr = info->addr;
         const int ox = qs->oldcellx, oy = qs->oldcelly, oc = qs->oldcellcheck;
         RotTileGet(info, t);
         VidsoftRotLookup(info, qs, regs, ram, x1, y1);
         rot_pred_oc = qs->oldcellcheck;
         RotTileGet(info, rot_pred_t);
         rot_pred_qs = qs;
         rot_pred_row = j;
         rot_pred_frame = keep_frame;
         info->charaddr = t[0]; info->paladdr = t[1]; info->flipfunction = t[2];
         info->specialfunction = t[3]; info->specialcolorfunction = t[4]; info->priority = t[5];
         info->addr = addr;
         qs->oldcellx = ox; qs->oldcelly = oy; qs->oldcellcheck = oc;
      }
      return 0;
   }
   TitanKeepLayerLine(layer, j);
   if (!info->isbitmap)
      VidsoftRotLookup(info, qs, regs, ram, x1, y1);   /* the lookup state the row's last pixel leaves */
   return 1;
}

/* What one drawn rotated row reads: every pixel's position as its loop
   computes it (the window is left out: a superset), the tile there
   (Vdp2MapCalcXYFast + Vdp2PatternAddr), the dot (Vdp2RotKLine*'s fetch)
   and its palette entry */
static void KeepWalkRot(vdp2draw_struct *info, const s32 *g, const screeninfo_struct *qs, const Vdp2 *regs, u8 *ram,
                        u32 *vm, u32 *cm)
{
   const int colormode = Vdp2Internal.ColorMode;
   const int cn = info->colornumber, bm = info->isbitmap, w = rbg0width;
   const int pagesize_bits = info->pagewh_bits * 2;
   const int cellwh = 2 + info->patternwh;
   const int cellw = info->cellw, transparent = info->transparencyenable;
   u32 charaddr = info->charaddr, paladdr = info->paladdr;
   int flipfunction = 0, lastcell = -1, i;
   for (i = 0; i < w; i++)
   {
      int x, y;
      u32 a, dot;
      if (!RotPos(g, qs, i, &x, &y))
         continue;
      if (!bm)
      {
         const int check = ((y >> cellwh) << 16) | (x >> cellwh);
         if (check != lastcell)
         {
            /* Vdp2MapCalcXYFast + Vdp2PatternAddr */
            const int planenum = ((y >> qs->planepixelheight_bits) * info->mapwh) + (x >> qs->planepixelwidth_bits);
            const int xp = x & qs->planepixelwidth_mask, yp = y & qs->planepixelheight_mask;
            lastcell = check;
            a = qs->planetbl[planenum];
            a += ((((yp >> qs->pagepixelwh_bits) << pagesize_bits) << info->planew_bits) +
                  ((xp >> qs->pagepixelwh_bits) << pagesize_bits) +
                  (((yp & qs->pagepixelwh_mask) >> cellwh) << info->pagewh_bits) +
                  ((xp & qs->pagepixelwh_mask) >> cellwh)) << (info->patterndatasize_bits + 1);
            if (info->patterndatasize == 1)
            {
               const u16 tmp = T1ReadWord(ram, a & 0x7FFFF);
               KeepMarkVRange(vm, a, 2);
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
               const u16 tmp1 = T1ReadWord(ram, a & 0x7FFFF), tmp2 = T1ReadWord(ram, (a + 2) & 0x7FFFF);
               KeepMarkVRange(vm, a, 4);
               charaddr = tmp2 & 0x7FFF;
               flipfunction = (tmp1 & 0xC000) >> 14;
               paladdr = cn == 0 ? (tmp1 & 0x7F) << 4 : (tmp1 & 0x70) << 4;
            }
            if (!(regs->VRSIZE & 0x8000))
               charaddr &= 0x3FFF;
            charaddr *= 0x20;
         }
         /* the pixel in the tile (Vdp2MapCalcXYFast) */
         x &= qs->planepixelwidth_mask;
         y &= qs->planepixelheight_mask;
         if (info->patternwh == 1)
         {
            x &= 7;
            y &= 7;
            if (flipfunction & 1)
               x = 7 - x;
            if (flipfunction & 2)
               y = 7 - y;
         }
         else if (flipfunction)
         {
            y &= 15;
            if (flipfunction & 2)
               y = !(y & 8) ? 8 - 1 - y + 16 : 16 - 1 - y;
            else if (y & 8)
               y += 8;
            if (flipfunction & 1)
            {
               if (!(x & 8))
                  y += 8;
               x = 7 - (x & 7);
            }
            else if (x & 8)
            {
               y += 8;
               x &= 7;
            }
            else
               x &= 7;
         }
         else
         {
            y &= 15;
            if (y & 8)
               y += 8;
            if (x & 8)
               y += 8;
            x &= 7;
         }
      }
      /* the dot (Vdp2DrawRotationKLineBody) and its palette entry */
      switch (cn)
      {
         case 0:
            a = charaddr + ((y * cellw) + x) / 2;
            dot = T1ReadByte(ram, a & 0x7FFFF);
            if (!(x & 1))
               dot >>= 4;
            KeepMarkV(vm, a);
            if ((dot & 0xF) || !transparent)
               KeepMarkC(cm, colormode, info->coloroffset + (paladdr | (dot & 0xF)));
            break;
         case 1:
            a = charaddr + (y * cellw) + x;
            dot = T1ReadByte(ram, a & 0x7FFFF);
            KeepMarkV(vm, a);
            if (dot || !transparent)
               KeepMarkC(cm, colormode, info->coloroffset + (paladdr | dot));
            break;
         case 2:
            a = charaddr + ((y * cellw) + x) * 2;
            dot = T1ReadWord(ram, a & 0x7FFFF);
            KeepMarkVRange(vm, a, 2);
            if (dot || !transparent)
               KeepMarkC(cm, colormode, info->coloroffset + dot);
            break;
         case 3:
            KeepMarkVRange(vm, charaddr + ((y * cellw) + x) * 2, 2);
            break;
         default:
            KeepMarkVRange(vm, charaddr + ((y * cellw) + x) * 4, 4);
            break;
      }
   }
}

void VidsoftKeepRotDrawn(vdp2draw_struct *info, u8 *ram)
{
   const int layer = TITAN_RBG0;
   const Vdp2 *fr = &keep_regs[keep_cur];
   const u32 *bits = vidsoft_keep_bits[layer];
   const int n = vdp2height < KEEP_LINES ? vdp2height : KEEP_LINES;
   const u32 pf = keep_frame;
   const int record = keep_layer_idle[layer] < KEEP_IDLE || (pf & 7) == 0;
   int y;
   if (!rot_began || rot_frame != keep_frame)
      return;
   rot_began = 0;
   if (!rot_ok)
   {
      memset(keep_row[layer], 0, sizeof(keep_row[layer]));
      return;
   }
   for (y = 0; y < n; y++)
   {
      struct KeepRow *R = &keep_row[layer][y];
      const struct KeepRotLine *L = &rot_line[y];
      if (bits[y >> 5] & (1u << (y & 31)))
      {
         keep_st_kept[layer]++;
         if (vidsoft_keep_check)
         {
            const struct PixelData *now = TitanGetLayerLine(layer, y);
            if (memcmp(now, keep_save[layer] + y * 704, sizeof(struct PixelData) * vdp2width) ||
                TitanLineMaxPriority(layer, y) != keep_save_prio[layer][y])
            {
               int x = 0;
               while (x < vdp2width && !memcmp(now + x, keep_save[layer] + y * 704 + x, sizeof(struct PixelData)))
                  x++;
               keep_st_miss++;
               fprintf(stderr, "lnkeep MISS: field %u layer %d (rotated) row %d (first differing x %d, prio %d/%d) (%lu misses)\n",
                       keep_frame, layer, y, x, TitanLineMaxPriority(layer, y), keep_save_prio[layer][y], keep_st_miss);
            }
         }
         R->frame = keep_frame;
         continue;
      }
      if (TitanLineFrame(layer, y) != keep_frame || L->frame != keep_frame || !L->rec ||
          !KeepLineOk(layer, &keep_lines[keep_cur][y]))
      {
         R->frame = 0;
         continue;
      }
      keep_st_drawn[layer]++;
      if (!record || (R->miss >= KEEP_IDLE && ((pf + y) & 7)))
      {
         R->frame = 0;
         continue;
      }
      memset(R->vm, 0, sizeof(R->vm));
      memset(R->cm, 0, sizeof(R->cm));
      if (info->islinewindow & 1)
         KeepMarkVRange(R->vm, ((fr->LWTA0.all & 0x7FFFE) << 1) + y * 4, 4);
      if (info->islinewindow & 2)
         KeepMarkVRange(R->vm, ((fr->LWTA1.all & 0x7FFFE) << 1) + y * 4, 4);
      KeepWalkRot(info, L->g, &rot_sinfo[L->which], fr, ram, R->vm, R->cm);
      memcpy(R->g, L->g, sizeof(R->g));
      R->rot = 1;
      R->frame = keep_frame;
      keep_st_rec[layer]++;
   }
}
