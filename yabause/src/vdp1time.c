/*  This file is part of Yabause.

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

/*! \file vdp1time.c
    \brief VDP1 RAM read at the chip's time for lists the CPU races.

    We draw a VDP1 command list all at once when it starts (VBlank-OUT, where
    Beetle starts it too). The chip takes its time: Rockman X3 opens every
    list with a full-screen polygon that clears the framebuffer (~54 lines),
    and uploads X's cells for the new pose in the lines after VBlank-OUT, so
    the chip reads X's sprites with the new cells, and we read the old ones
    (X built from the pieces of another pose on every pose change, issue #14).

    For a list that opens with an opaque polygon covering the whole system
    clip (a framebuffer clear), the chip's timing is modelled as Beetle has
    it: the list starts at the first HBlank after VBlank-OUT (the draw
    status, EDSR's CEF, clears there, so a VBlank-OUT interrupt handler
    still sees the last list's end); each command is read when the chip gets
    to it (Beetle's costs: a 16-cycle fetch; polygons and sprites stepped as
    lines, each 12 cycles to set up plus a cycle a pixel, +19 % in 16 bpp,
    a line wholly outside the clip area reduced to a point, a line that
    leaves it after entering stopping there); the list ends, with draw end
    (CEF, COPR, the SCU interrupt), when its last command is done. Games
    that wait for draw end rewrite their list only then. VDP1 RAM writes are
    journaled until the end, and the list is drawn then, with VDP1 RAM as
    each command read it (the writes made after a command read a byte
    undone). At VBlank-OUT only its register effects are made
    (Vdp1FakeDrawCommands), as the threaded renderer does. The chip draws on
    through the VBlank: the list is completed early only where the frame
    changes, a new list starts, or the CPU touches the framebuffer. Other
    lists are drawn and end at VBlank-OUT, as before.

    YAB_VDP1TIME=0 turns it off, 1 is the default, 2 also models every list
    (not only those that open with a clear) and logs each list where a write
    races a read; counts at exit with the variable set. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "yabause.h"
#include "vdp1.h"
#include "scu.h"

#define VDP1TIME_MAXCMDS 2048
#define VDP1TIME_MAXWRITES 16384

typedef struct
{
   double t;                /* modelled read time, lines after the draw start */
   u32 lo[5], hi[5];        /* what it reads: command, texture, colour table, Gouraud, the
                               part of one of them past the end of VDP1 RAM (it wraps) */
} Vdp1TimeCmd;

typedef struct
{
   double t;                /* lines after the draw start */
   u32 addr;
   int size;
   u8 old[4];
} Vdp1TimeWr;

int vdp1time_inflight;      /* a list is modelled as still drawing: journal writes */

static int vdp1time_mode = -1;
static u32 vdp1time_frame;  /* VBlank-OUT count */
u64 vdp1time_lines;         /* HBlank-IN count: the line clock */
static Vdp1TimeCmd vdp1time_cmds[VDP1TIME_MAXCMDS];
static int vdp1time_ncmds;
/* which commands read each 256-byte block: commands vdp1time_blkcmd[first[b] .. first[b + 1]) */
#define VDP1TIME_BLOCKS (0x80000 >> 8)
#define VDP1TIME_MAXREFS 65536
static int vdp1time_blkfirst[VDP1TIME_BLOCKS + 1];
static u16 vdp1time_blkcmd[VDP1TIME_MAXREFS];
static Vdp1TimeWr vdp1time_writes[VDP1TIME_MAXWRITES];
static int vdp1time_nwrites;
static int vdp1time_later;  /* the list opens with a clear: drawn at its end */
static int vdp1time_started; /* its start shown in the draw status yet */
static double vdp1time_start, vdp1time_end;
static VideoInterface_struct *vdp1time_core;
static Vdp1 vdp1time_regs;           /* the registers when the list started */
static unsigned long vdp1time_lists, vdp1time_raced, vdp1time_chipread, vdp1time_unhandled, vdp1time_early;
static unsigned long vdp1time_saved, vdp1time_resumed;   /* states saved / loaded with a list in flight */

static int Vdp1TimeMode(void)
{
   if (vdp1time_mode < 0)
   {
      const char *env = getenv("YAB_VDP1TIME");
      vdp1time_mode = env && env[0] >= '0' && env[0] <= '2' ? env[0] - '0' : 1;
   }
   return vdp1time_mode;
}

/* Lines since some fixed point. Counted at HBlank-IN, once a line on both
   SH-2 cores: yabsys.LineCount isn't the same at VBlank-OUT on both (the
   recompiler's frame loop calls it before the count wraps) */
static double Vdp1TimeNow(void)
{
   return (double)vdp1time_lines;
}

void Vdp1TimeVBlankOut(void)
{
   /* a frame change asked for during the VBlank is made at VBlank-OUT
      (Vdp2VBlankOUT): a list still drawing is complete first. A new list
      completes it in Vdp1TimeDrawStart; else it draws on, as on the chip */
   if (vdp1time_inflight && Vdp1External.changerequest)
      Vdp1TimeFinish();
   vdp1time_frame++;
}

static s32 Vdp1TimeMax(s32 a, s32 b) { return a > b ? a : b; }

/* The clipping a command is drawn with: system clip (0,0)-(sx,sy), user
   clip (ux0,uy0)-(ux1,uy1) */
typedef struct { s32 sx, sy, ux0, uy0, ux1, uy1; } Vdp1TimeClip;

/* Cycles of one line from (x0,y0) to (x1,y1) as Beetle draws it: 4 + 8 to
   set it up; a line wholly beyond one edge of the clip area is reduced to a
   point; else one pixel a step (+ one each time both x and y step, with
   anti-aliasing), stopping where it leaves the clip area after entering it;
   pixel cycles + 19 % (16 bpp drawing timing) */
static double Vdp1TimeLineCost(s32 x0, s32 y0, s32 x1, s32 y1, int aa, int user, const Vdp1TimeClip *k, double pix)
{
   const s32 bx0 = user ? k->ux0 : 0, by0 = user ? k->uy0 : 0;
   const s32 bx1 = user ? k->ux1 : k->sx, by1 = user ? k->uy1 : k->sy;
   const s32 dx = x1 - x0, dy = y1 - y0, adx = abs(dx), ady = abs(dy);
   const s32 len = Vdp1TimeMax(adx, ady);
   double steps, tin = 0, tout = 1;
   if ((x0 < bx0 && x1 < bx0) || (x0 > bx1 && x1 > bx1) || (y0 < by0 && y1 < by0) || (y0 > by1 && y1 > by1))
      return 12 + pix * (1 + 48 / 256.0);
   steps = len + 1 + (aa ? (adx < ady ? adx : ady) : 0);
   /* the part of the segment inside the clip box (Liang-Barsky) */
   {
      const double p[4] = { -dx, dx, -dy, dy };
      const double q[4] = { x0 - bx0, bx1 - x0, y0 - by0, by1 - y0 };
      int i;
      for (i = 0; i < 4; i++)
      {
         if (p[i] == 0)
            continue;
         {
            const double r = q[i] / p[i];
            if (p[i] < 0) { if (r > tin) tin = r; }
            else if (r < tout) tout = r;
         }
      }
   }
   if (tin <= tout && tout < 1)
      steps = steps * tout + 1;   /* stops once it leaves the clip area */
   return 12 + steps * pix * (1 + 48 / 256.0);
}

/* A quad (polygon, distorted, scaled or normal sprite) as Beetle steps it:
   along edges A->D and B->C, as many steps as the longer edge, a line
   between them at each */
static double Vdp1TimeQuad(s32 xa, s32 ya, s32 xb, s32 yb, s32 xc, s32 yc, s32 xd, s32 yd, int user,
                           const Vdp1TimeClip *k, double pix)
{
   s32 dmax = Vdp1TimeMax(Vdp1TimeMax(abs(xd - xa), abs(yd - ya)), Vdp1TimeMax(abs(xc - xb), abs(yc - yb))) & 0xFFF;
   double cost = 0;
   s32 i;
   for (i = 0; i <= dmax; i++)
   {
      const s32 x0 = dmax ? xa + (xd - xa) * i / dmax : xa, y0 = dmax ? ya + (yd - ya) * i / dmax : ya;
      const s32 x1 = dmax ? xb + (xc - xb) * i / dmax : xb, y1 = dmax ? yb + (yc - yb) * i / dmax : yb;
      cost += Vdp1TimeLineCost(x0, y0, x1, y1, 1, user, k, pix);
   }
   return cost;
}

static u32 Vdp1TimeNext(u32 addr, u16 ctrl, u32 *ret)
{
   switch ((ctrl >> 12) & 3)
   {
   case 1:
      return T1ReadWord(Vdp1Ram, addr + 2) * 8;
   case 2:
      if (*ret == 0xFFFFFFFF)
         *ret = addr + 0x20;
      return T1ReadWord(Vdp1Ram, addr + 2) * 8;
   case 3:
      if (*ret != 0xFFFFFFFF)
      {
         u32 a = *ret;
         *ret = 0xFFFFFFFF;
         return a;
      }
      return addr + 0x20;
   default:
      return addr + 0x20;
   }
}

/* Does the list's first drawing command clear the framebuffer: an opaque
   replace polygon (no Gouraud, mesh, MSB or user clip) covering the system
   clip, drawn without an erase before it in a plain 16 bpp framebuffer? */
static int Vdp1TimeOpensWithClear(void)
{
   u32 addr = 0, ret = 0xFFFFFFFF;
   s32 lx = Vdp1Regs->localX, ly = Vdp1Regs->localY;
   s32 cx = Vdp1Regs->systemclipX2, cy = Vdp1Regs->systemclipY2;
   int n;

   if ((Vdp1Regs->TVMR & 7) || !(Vdp1Regs->FBCR & 2) || (Vdp1Regs->FBCR & 8) || Vdp1External.manualerase)
      return 0;
   for (n = 0; n < 64; n++)
   {
      u16 ctrl = T1ReadWord(Vdp1Ram, addr);
      if (ctrl & 0x8000)
         return 0;
      if (!(ctrl & 0x4000))
      {
         const int type = ctrl & 0xF;
         if (type == 9)
         {
            cx = T1ReadWord(Vdp1Ram, addr + 0x14);
            cy = T1ReadWord(Vdp1Ram, addr + 0x16);
         }
         else if (type == 10)
         {
            lx = (s16)T1ReadWord(Vdp1Ram, addr + 0xC);
            ly = (s16)T1ReadWord(Vdp1Ram, addr + 0xE);
         }
         else if (type >= 0xC)
            return 0;
         else if (type <= 7)
         {
            u16 pmod = T1ReadWord(Vdp1Ram, addr + 4);
            s32 xa = (s16)T1ReadWord(Vdp1Ram, addr + 0xC) + lx, ya = (s16)T1ReadWord(Vdp1Ram, addr + 0xE) + ly;
            s32 xb = (s16)T1ReadWord(Vdp1Ram, addr + 0x10) + lx, yb = (s16)T1ReadWord(Vdp1Ram, addr + 0x12) + ly;
            s32 xc = (s16)T1ReadWord(Vdp1Ram, addr + 0x14) + lx, yc = (s16)T1ReadWord(Vdp1Ram, addr + 0x16) + ly;
            s32 xd = (s16)T1ReadWord(Vdp1Ram, addr + 0x18) + lx, yd = (s16)T1ReadWord(Vdp1Ram, addr + 0x1A) + ly;
            return type == 4 && (pmod & 0x40) && !(pmod & 0x8507) &&
                   xa <= 0 && ya <= 0 && xb == xc && ya == yb && xd == xa && yd == yc && xc >= cx && yc >= cy;
         }
      }
      addr = Vdp1TimeNext(addr, ctrl, &ret) & 0x7FFFF;
   }
   return 0;
}

/* Visit the 256-byte blocks command c reads */
#define VDP1TIME_FOR_BLOCKS(c, b) \
   for (k = 0; k < 5; k++) \
      if ((c)->hi[k] > (c)->lo[k]) \
         for (b = (c)->lo[k] >> 8; b <= (((c)->hi[k] - 1) >> 8) && b < VDP1TIME_BLOCKS; b++)

/* The readers of each block (counting sort); 0 if there are too many */
static int Vdp1TimeIndex(void)
{
   static int count[VDP1TIME_BLOCKS], last[VDP1TIME_BLOCKS];
   int i, k, total = 0;
   u32 b;
   /* a command reading one block through two ranges is listed once */
   memset(count, 0, sizeof(count));
   memset(last, 0xFF, sizeof(last));
   for (i = 0; i < vdp1time_ncmds; i++)
      VDP1TIME_FOR_BLOCKS(&vdp1time_cmds[i], b)
         if (last[b] != i)
         {
            last[b] = i;
            count[b]++;
         }
   for (b = 0; b < VDP1TIME_BLOCKS; b++)
   {
      vdp1time_blkfirst[b] = total;
      total += count[b];
   }
   vdp1time_blkfirst[VDP1TIME_BLOCKS] = total;
   if (total > VDP1TIME_MAXREFS)
      return 0;
   memset(count, 0, sizeof(count));
   memset(last, 0xFF, sizeof(last));
   for (i = 0; i < vdp1time_ncmds; i++)
      VDP1TIME_FOR_BLOCKS(&vdp1time_cmds[i], b)
         if (last[b] != i)
         {
            last[b] = i;
            vdp1time_blkcmd[vdp1time_blkfirst[b] + count[b]++] = (u16)i;
         }
   return 1;
}

/* Each command's modelled read time and what it reads; the list's end.
   Returns 0 if the walk can't be modelled (longer than drawing walks, a loop) or a later
   system clip goes beyond the clear. */
static int Vdp1TimeWalk(int cover)
{
   u32 addr = 0, ret = 0xFFFFFFFF;
   s32 cx = 0, cy = 0, lx = Vdp1Regs->localX, ly = Vdp1Regs->localY;
   Vdp1TimeClip clip;
   double t = 0;
   int n, first = 1;

   clip.sx = Vdp1Regs->systemclipX2; clip.sy = Vdp1Regs->systemclipY2;
   clip.ux0 = Vdp1Regs->userclipX1; clip.uy0 = Vdp1Regs->userclipY1;
   clip.ux1 = Vdp1Regs->userclipX2; clip.uy1 = Vdp1Regs->userclipY2;
   vdp1time_ncmds = 0;
   for (n = 0; n < 2000; n++)   /* as many as Vdp1DrawCommands visits */
   {
      u16 ctrl = T1ReadWord(Vdp1Ram, addr);
      double cost = 16;
      Vdp1TimeCmd *c;

      /* every command fetched is read, skipped ones (their jumps) and the
         end too */
      if (vdp1time_ncmds >= VDP1TIME_MAXCMDS)
         return 0;
      c = &vdp1time_cmds[vdp1time_ncmds++];
      memset(c, 0, sizeof(*c));
      c->t = t;
      c->lo[0] = addr; c->hi[0] = addr + 0x20;
      if (ctrl & 0x8000)
      {
         vdp1time_end = t;
         return Vdp1TimeIndex();
      }
      if (!(ctrl & 0x4000))
      {
         const int type = ctrl & 0xF;
         const u16 pmod = T1ReadWord(Vdp1Ram, addr + 4), colr = T1ReadWord(Vdp1Ram, addr + 6);
         const u32 srca = T1ReadWord(Vdp1Ram, addr + 8) * 8, size = T1ReadWord(Vdp1Ram, addr + 0xA);
         const s32 xa = (s16)T1ReadWord(Vdp1Ram, addr + 0xC), ya = (s16)T1ReadWord(Vdp1Ram, addr + 0xE);
         const s32 xb = (s16)T1ReadWord(Vdp1Ram, addr + 0x10), yb = (s16)T1ReadWord(Vdp1Ram, addr + 0x12);
         const s32 xc = (s16)T1ReadWord(Vdp1Ram, addr + 0x14), yc = (s16)T1ReadWord(Vdp1Ram, addr + 0x16);
         const s32 xd = (s16)T1ReadWord(Vdp1Ram, addr + 0x18), yd = (s16)T1ReadWord(Vdp1Ram, addr + 0x1A);
         const u32 w = ((size >> 8) & 0x3F) * 8, h = size & 0xFF;
         const int cm = (pmod >> 3) & 7;

         if (type <= 3)
         {
            c->lo[1] = srca;
            c->hi[1] = srca + (cm == 5 ? w * h * 2 : cm >= 2 ? w * h : w * h / 2);
            if (cm == 1)
            {
               c->lo[2] = colr * 8;
               c->hi[2] = colr * 8 + 32;
            }
         }
         if (type <= 7 && (pmod & 4))
         {
            c->lo[3] = T1ReadWord(Vdp1Ram, addr + 0x1C) * 8;
            c->hi[3] = c->lo[3] + 8;
         }
         {
            int k;
            for (k = 1; k < 4; k++)
               if (c->hi[k] > 0x80000)
               {
                  c->lo[4] = 0;
                  c->hi[4] = c->hi[k] - 0x80000;
                  c->hi[k] = 0x80000;
                  break;
               }
         }
         /* drawing stops at an invalid command (Vdp1DrawCommands) */
         if (type >= 0xC)
         {
            vdp1time_end = t;
            return Vdp1TimeIndex();
         }

         {
            /* pixel cycles: 1, +5 where the pixel behind is read (MSB on,
               shadow, half-transparency); user clipping (draw inside) */
            const double pix = (pmod & 0x8000) || (pmod & 1) ? 6 : 1;
            const int user = (pmod & 0x600) == 0x400;
            const s32 ax = xa + lx, ay = ya + ly, bx = xb + lx, by = yb + ly;
            const s32 cxx = xc + lx, cyy = yc + ly, dxx = xd + lx, dyy = yd + ly;
            switch (type)
            {
            case 0:
               cost += Vdp1TimeQuad(ax, ay, ax + (s32)w - 1, ay, ax + (s32)w - 1, ay + (s32)h - 1, ax, ay + (s32)h - 1, user, &clip, pix)
                       + (cm == 1 ? 16 : 0);
               break;
            case 1:
            {
               s32 x0 = ax, y0 = ay, x1 = cxx, y1 = cyy;
               if (((ctrl >> 8) & 0xF) != 0)
               {
                  x1 = ax + xb; y1 = ay + yb;   /* size from the zoom point (its position approximated) */
               }
               cost += Vdp1TimeQuad(x0, y0, x1, y0, x1, y1, x0, y1, user, &clip, pix) + (cm == 1 ? 16 : 0);
               break;
            }
            case 2: case 3: case 4:
               cost += Vdp1TimeQuad(ax, ay, bx, by, cxx, cyy, dxx, dyy, user, &clip, pix) + (cm == 1 && type != 4 ? 16 : 0);
               break;
            case 5: case 7:
               cost += Vdp1TimeLineCost(ax, ay, bx, by, 0, user, &clip, pix) + Vdp1TimeLineCost(bx, by, cxx, cyy, 0, user, &clip, pix) +
                       Vdp1TimeLineCost(cxx, cyy, dxx, dyy, 0, user, &clip, pix) + Vdp1TimeLineCost(dxx, dyy, ax, ay, 0, user, &clip, pix);
               break;
            case 6:
               cost += Vdp1TimeLineCost(ax, ay, bx, by, 0, user, &clip, pix);
               break;
            case 8: case 11:
               clip.ux0 = xa; clip.uy0 = ya; clip.ux1 = xc; clip.uy1 = yc;
               break;
            case 9:
               /* a system clip set after the clear may reach what it didn't cover */
               if (!first && cover && (xc > cx || yc > cy))
                  return 0;
               cx = xc; cy = yc;
               clip.sx = xc; clip.sy = yc;
               break;
            case 10:
               lx = xa; ly = ya;
               break;
            default:
               break;
            }
         }
         if (type <= 7)
         {
            if (first && cover)
            {
               cx = xc + lx;
               cy = yc + ly;
            }
            first = 0;
         }
      }
      t += cost;
      addr = Vdp1TimeNext(addr, ctrl, &ret) & 0x7FFFF;
   }
   return 0;
}

/* The walk's times from cycles to lines after the start */
static void Vdp1TimeToLines(void)
{
   const double hz = yabsys.IsPal ? 50 : 59.94;
   const double clock = yabsys.CurSH2FreqType == CLKTYPE_26MHZ ? 26874100 : 28636400;
   const double cycles_per_line = clock / (hz * yabsys.MaxLineCount);
   int i;
   vdp1time_end /= cycles_per_line;
   for (i = 0; i < vdp1time_ncmds; i++)
      vdp1time_cmds[i].t /= cycles_per_line;
}

/* Vdp1Draw, before the list is drawn. Returns 1 if this list is drawn at
   its modelled end instead (Vdp1Draw then only walks it for the registers) */
int Vdp1TimeDrawStart(void)
{
   int cover;
   if (vdp1time_inflight)
      Vdp1TimeFinish();
   if (!Vdp1TimeMode() || VIDCore->id != 2 /* VIDCORE_SOFT */)
      return 0;
   cover = Vdp1TimeOpensWithClear();
   if (!cover && vdp1time_mode != 2)
      return 0;
   if (!Vdp1TimeWalk(cover))
      return 0;
   vdp1time_later = cover;
   vdp1time_core = VIDCore;
   vdp1time_regs = *Vdp1Regs;
   vdp1time_start = Vdp1TimeNow();
   Vdp1TimeToLines();
   vdp1time_end += vdp1time_start;
   vdp1time_nwrites = 0;
   vdp1time_lists++;
   vdp1time_inflight = 1;
   vdp1time_started = !vdp1time_later;
   return vdp1time_later;
}

/* A VDP1 RAM write while a list is modelled as drawing, before it is made */
void Vdp1TimeWrite(u32 addr, int size)
{
   Vdp1TimeWr *w;
   int i;
   if (vdp1time_nwrites >= VDP1TIME_MAXWRITES)
   {
      /* the journal is full: the list is done as of now */
      vdp1time_early++;
      Vdp1TimeFinish();
      return;
   }
   w = &vdp1time_writes[vdp1time_nwrites++];
   w->t = Vdp1TimeNow() + 0.5 - vdp1time_start;   /* somewhere in this line */
   w->addr = addr;
   w->size = size;
   for (i = 0; i < size; i++)
      w->old[i] = T1ReadByte(Vdp1Ram, (addr + i) & 0x7FFFF);
}

/* The chip starts a list at the first HBlank after VBlank-OUT (Beetle): the
   VBlank-OUT interrupt handler still sees the last list's draw end (CEF) */
static void Vdp1TimeShowStart(void)
{
   vdp1time_started = 1;
   Vdp1Regs->EDSR >>= 1;   /* BEF <- CEF, CEF <- 0 */
   Vdp1Regs->COPR = 0;
}

/* Every line: has the modelled list started, or ended? */
void Vdp1TimeLine(void)
{
   if (!vdp1time_started)
      Vdp1TimeShowStart();
   if (Vdp1TimeNow() >= vdp1time_end)
      Vdp1TimeFinish();
}

static int Vdp1TimeReads(const Vdp1TimeCmd *c, u32 a)
{
   int k;
   for (k = 0; k < 5; k++)
      if (a >= c->lo[k] && a < c->hi[k])
         return 1;
   return 0;
}

/* Did command reads of byte a come before write w, after it, or both? */
static void Vdp1TimeReaders(const Vdp1TimeWr *w, u32 a, int *before, int *after)
{
   int j;
   *before = *after = 0;
   for (j = vdp1time_blkfirst[a >> 8]; j < vdp1time_blkfirst[(a >> 8) + 1]; j++)
   {
      const Vdp1TimeCmd *c = &vdp1time_cmds[vdp1time_blkcmd[j]];
      if (!Vdp1TimeReads(c, a))
         continue;
      if (c->t >= w->t)
         *after = 1;
      else
         *before = 1;
   }
}

/* The list has ended (or something needs the framebuffer, or the next list
   starts). A list that opens with a clear is drawn now, with VDP1 RAM as its
   commands read it; others were drawn at VBlank-OUT, as before. */
void Vdp1TimeFinish(void)
{
   static u32 patch_addr[VDP1TIME_MAXWRITES * 4];
   static u8 patch_live[VDP1TIME_MAXWRITES * 4];
   int raced = 0, mixed = 0, npatch = 0, i, b, before, after;

   vdp1time_inflight = 0;
   if (!vdp1time_started)
      Vdp1TimeShowStart();

   /* per written byte: the chip read it after the write (we read the old
      value at VBlank-OUT), before it, or both (one RAM image can't hold
      that: drawn as at VBlank-OUT, as before) */
   for (i = 0; i < vdp1time_nwrites; i++)
   {
      const Vdp1TimeWr *w = &vdp1time_writes[i];
      for (b = 0; b < w->size; b++)
      {
         Vdp1TimeReaders(w, (w->addr + b) & 0x7FFFF, &before, &after);
         raced |= after;
         mixed |= after && before;
      }
   }
   if (raced)
   {
      vdp1time_raced++;
      if (!vdp1time_later || mixed)
      {
         vdp1time_unhandled++;
         if (vdp1time_mode == 2)
            fprintf(stderr, "vdp1time: frame %u: list raced, drawn as at its start (%s)\n", vdp1time_frame,
                    !vdp1time_later ? "no clear first" : "a byte read before and after a write");
      }
      else
         vdp1time_chipread++;
   }
   if (!vdp1time_later)
      return;

   /* VDP1 RAM as the commands read it: undo, newest first, the writes made
      after the commands that read those bytes (all of them when the list
      can't be drawn as the chip read it: then as at VBlank-OUT) */
   for (i = vdp1time_nwrites - 1; i >= 0; i--)
   {
      const Vdp1TimeWr *w = &vdp1time_writes[i];
      for (b = 0; b < w->size; b++)
      {
         const u32 a = (w->addr + b) & 0x7FFFF;
         if (!mixed)
         {
            Vdp1TimeReaders(w, a, &before, &after);
            if (!before)
               continue;
         }
         patch_addr[npatch] = a;
         patch_live[npatch++] = T1ReadByte(Vdp1Ram, a);
         T1WriteByte(Vdp1Ram, a, w->old[b]);
         VDP1_RAM_MARK(a);
      }
   }

   /* draw it from the registers it started with (a frame change asked for
      since must not change it), then the registers as they are now back */
   {
      const Vdp1 regs = *Vdp1Regs;
      const int incomplete = Vdp1ListIncomplete, erase = Vdp1External.manualerase;
      *Vdp1Regs = vdp1time_regs;
      Vdp1External.manualerase = 0;
      vdp1time_core->Vdp1DrawStart();
      vdp1time_core->Vdp1DrawEnd();
      *Vdp1Regs = regs;
      Vdp1ListIncomplete = incomplete;
      Vdp1External.manualerase = erase;
   }
   if (vdp1time_mode == 2 && raced)
      fprintf(stderr, "vdp1time: frame %u: list drawn as the chip read it (%d writes, %d bytes as read before them)\n",
              vdp1time_frame, vdp1time_nwrites, npatch);

   /* the live RAM back, newest value last */
   for (i = npatch - 1; i >= 0; i--)
   {
      T1WriteByte(Vdp1Ram, patch_addr[i], patch_live[i]);
      VDP1_RAM_MARK(patch_addr[i]);
   }

   /* the list has ended: draw end, as Vdp1Draw reports it for other lists
      (after the live RAM is back: a DMA started by the draw end sees it) */
   Vdp1Regs->EDSR |= 2;
   Vdp1Regs->COPR = Vdp1Regs->addr >> 3;
   ScuSendDrawEnd();
}

/* A reset: the RAM the journal describes is gone */
void Vdp1TimeDiscard(void)
{
   vdp1time_inflight = 0;
}

/* States are saved between frames, just after VBlank-OUT, where a modelled
   list has started and not yet ended. The state keeps what resumes it:
   its start registers, how far it got, and the writes journaled since. */
void Vdp1TimeSaveState(void **stream)
{
   const u32 inflight = vdp1time_inflight;
   MemStateWrite((void *)&inflight, 4, 1, stream);
   if (inflight)
   {
      const double elapsed = Vdp1TimeNow() - vdp1time_start, len = vdp1time_end - vdp1time_start;
      const u32 later = vdp1time_later, started = vdp1time_started, nwrites = vdp1time_nwrites;
      u32 i;
      MemStateWrite((void *)&vdp1time_regs, sizeof(Vdp1), 1, stream);
      MemStateWrite((void *)&later, 4, 1, stream);
      MemStateWrite((void *)&started, 4, 1, stream);
      MemStateWrite((void *)&elapsed, sizeof(double), 1, stream);
      MemStateWrite((void *)&len, sizeof(double), 1, stream);
      MemStateWrite((void *)&nwrites, 4, 1, stream);
      for (i = 0; i < nwrites; i++)
      {
         const Vdp1TimeWr *w = &vdp1time_writes[i];
         const u32 addr = w->addr, size = w->size;
         MemStateWrite((void *)&w->t, sizeof(double), 1, stream);
         MemStateWrite((void *)&addr, 4, 1, stream);
         MemStateWrite((void *)&size, 4, 1, stream);
         MemStateWrite((void *)w->old, 1, 4, stream);
      }
      vdp1time_saved++;
   }
}

/* A state load (after the registers and VDP1 RAM): resume the list that
   was drawing, as Beetle does; the walk is redone on VDP1 RAM as the list
   started (the journaled writes undone). Older states hold none: a list
   drawing then is lost, as before. */
void Vdp1TimeLoadState(const void *stream, int version)
{
   u32 inflight = 0, later, started, nwrites, i;
   double elapsed, len;
   Vdp1 regs;
   vdp1time_inflight = 0;
   if (version < 2)
      return;
   MemStateRead((void *)&inflight, 4, 1, stream);
   if (!inflight)
      return;
   MemStateRead((void *)&regs, sizeof(Vdp1), 1, stream);
   MemStateRead((void *)&later, 4, 1, stream);
   MemStateRead((void *)&started, 4, 1, stream);
   MemStateRead((void *)&elapsed, sizeof(double), 1, stream);
   MemStateRead((void *)&len, sizeof(double), 1, stream);
   MemStateRead((void *)&nwrites, 4, 1, stream);
   if (nwrites > VDP1TIME_MAXWRITES)
      return;
   for (i = 0; i < nwrites; i++)
   {
      Vdp1TimeWr *w = &vdp1time_writes[i];
      u32 addr, size;
      MemStateRead((void *)&w->t, sizeof(double), 1, stream);
      MemStateRead((void *)&addr, 4, 1, stream);
      MemStateRead((void *)&size, 4, 1, stream);
      MemStateRead((void *)w->old, 1, 4, stream);
      w->addr = addr;
      w->size = size > 4 ? 4 : size;
   }

   if (VIDCore == NULL || VIDCore->id != 2 /* VIDCORE_SOFT */)
   {
      /* nothing to draw it with here: end it now, as other lists */
      if (!started)
         Vdp1TimeShowStart();
      Vdp1Regs->EDSR |= 2;
      ScuSendDrawEnd();
      return;
   }
   {
      u8 *live = (u8 *)malloc(0x80000);
      const Vdp1 now = *Vdp1Regs;
      int ok;
      if (live == NULL)
         return;
      memcpy(live, Vdp1Ram, 0x80000);
      for (i = nwrites; i-- > 0;)
      {
         const Vdp1TimeWr *w = &vdp1time_writes[i];
         int b;
         for (b = 0; b < w->size; b++)
            T1WriteByte(Vdp1Ram, (w->addr + b) & 0x7FFFF, w->old[b]);
      }
      *Vdp1Regs = regs;
      ok = Vdp1TimeWalk(later);
      *Vdp1Regs = now;
      memcpy(Vdp1Ram, live, 0x80000);
      free(live);
      if (!ok)
         return;
   }
   Vdp1TimeToLines();
   vdp1time_regs = regs;
   vdp1time_later = later;
   vdp1time_started = started;
   vdp1time_core = VIDCore;
   vdp1time_start = Vdp1TimeNow() - elapsed;
   vdp1time_end = vdp1time_start + len;
   vdp1time_nwrites = nwrites;
   vdp1time_inflight = 1;
   vdp1time_resumed++;
}

void Vdp1TimeStats(void)
{
   if (getenv("YAB_VDP1TIME"))
      fprintf(stderr, "vdp1time: %lu lists modelled, %lu raced, %lu drawn as the chip read them, %lu raced but drawn as at their start, %lu ended early (journal full)\n",
              vdp1time_lists, vdp1time_raced, vdp1time_chipread, vdp1time_unhandled, vdp1time_early);
   if (getenv("YAB_VDP1TIME") && (vdp1time_saved || vdp1time_resumed))
      fprintf(stderr, "vdp1time: %lu states saved with a list in flight, %lu such lists resumed at a load\n",
              vdp1time_saved, vdp1time_resumed);
}
