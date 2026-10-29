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
    (X built from the pieces of another pose on every pose change, U57).

    For a list that opens with an opaque polygon covering the whole system
    clip (a framebuffer clear), each command's read time is modelled from the
    draw start (Beetle's costs: a 16-cycle fetch, polygons and sprites drawn
    as lines of about one cycle a pixel, +19 % in 16 bpp) and VDP1 RAM writes
    are journaled until the list's modelled end. If a write landed before a
    command's modelled read and touched what it reads (command words,
    texture, colour table, Gouraud table), the list is drawn again with VDP1
    RAM as each command would have read it. The clear covers everything the
    first draw left, so drawing over it is exact. Lists without that clear are
    drawn once, as before.

    YAB_VDP1TIME=0 turns it off, 1 is the default, 2 also models every list
    (not only those that open with a clear) and logs each list where a write
    races a read, redrawn or not; counts at exit with the variable set. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "yabause.h"
#include "vdp1.h"

#define VDP1TIME_MAXCMDS 2048
#define VDP1TIME_MAXWRITES 16384

typedef struct
{
   double t;                /* modelled read time, lines after the draw start */
   u32 lo[4], hi[4];        /* what it reads: command, texture, colour table, Gouraud */
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
static double vdp1time_base; /* lines up to the last VBlank-OUT */
static Vdp1TimeCmd vdp1time_cmds[VDP1TIME_MAXCMDS];
static int vdp1time_ncmds;
/* which commands read each 256-byte block: commands vdp1time_blkcmd[first[b] .. first[b + 1]) */
#define VDP1TIME_BLOCKS (0x80000 >> 8)
#define VDP1TIME_MAXREFS 65536
static int vdp1time_blkfirst[VDP1TIME_BLOCKS + 1];
static u16 vdp1time_blkcmd[VDP1TIME_MAXREFS];
static Vdp1TimeWr vdp1time_writes[VDP1TIME_MAXWRITES];
static int vdp1time_nwrites, vdp1time_overflow;
static int vdp1time_cover;  /* the list opens with an opaque full clear */
static double vdp1time_start, vdp1time_end;
static VideoInterface_struct *vdp1time_core;
static Vdp1 vdp1time_regs;           /* the registers when the list started */
static unsigned long vdp1time_lists, vdp1time_raced, vdp1time_redrawn, vdp1time_unhandled;

static int Vdp1TimeMode(void)
{
   if (vdp1time_mode < 0)
   {
      const char *env = getenv("YAB_VDP1TIME");
      vdp1time_mode = env && env[0] >= '0' && env[0] <= '2' ? env[0] - '0' : 1;
   }
   return vdp1time_mode;
}

/* Lines since some fixed point; the line count as the CPU sees it (at
   VBlank-OUT LineCount is still the field's last line + 1, then 0) */
static double Vdp1TimeNow(void)
{
   int line = yabsys.LineCount >= yabsys.MaxLineCount ? 0 : yabsys.LineCount;
   return vdp1time_base + line;
}

void Vdp1TimeVBlankOut(void)
{
   if (vdp1time_inflight)
      Vdp1TimeFinish();
   vdp1time_frame++;
   vdp1time_base += yabsys.MaxLineCount;
}

static s32 Vdp1TimeMax(s32 a, s32 b) { return a > b ? a : b; }

/* Beetle's polygon walk: steps along edges A->D and B->C, the longer one's
   length in steps; each step draws a line between them */
static double Vdp1TimeQuad(s32 xa, s32 ya, s32 xb, s32 yb, s32 xc, s32 yc, s32 xd, s32 yd)
{
   s32 e0 = Vdp1TimeMax(abs(xd - xa), abs(yd - ya));
   s32 e1 = Vdp1TimeMax(abs(xc - xb), abs(yc - yb));
   s32 l0 = Vdp1TimeMax(abs(xb - xa), abs(yb - ya));
   s32 l1 = Vdp1TimeMax(abs(xc - xd), abs(yc - yd));
   return (Vdp1TimeMax(e0, e1) + 1) * ((l0 + l1) / 2.0 + 1 + 8);
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
   for (k = 0; k < 4; k++) \
      if ((c)->hi[k] > (c)->lo[k]) \
         for (b = (c)->lo[k] >> 8; b <= (((c)->hi[k] - 1) >> 8) && b < VDP1TIME_BLOCKS; b++)

/* The readers of each block (counting sort); 0 if there are too many */
static int Vdp1TimeIndex(void)
{
   static int count[VDP1TIME_BLOCKS];
   int i, k, total = 0;
   u32 b;
   memset(count, 0, sizeof(count));
   for (i = 0; i < vdp1time_ncmds; i++)
      VDP1TIME_FOR_BLOCKS(&vdp1time_cmds[i], b)
         count[b]++;
   for (b = 0; b < VDP1TIME_BLOCKS; b++)
   {
      vdp1time_blkfirst[b] = total;
      total += count[b];
   }
   vdp1time_blkfirst[VDP1TIME_BLOCKS] = total;
   if (total > VDP1TIME_MAXREFS)
      return 0;
   memset(count, 0, sizeof(count));
   for (i = 0; i < vdp1time_ncmds; i++)
      VDP1TIME_FOR_BLOCKS(&vdp1time_cmds[i], b)
      {
         /* a command reading one block twice (two ranges) is listed once */
         int f = vdp1time_blkfirst[b];
         if (count[b] && vdp1time_blkcmd[f + count[b] - 1] == i)
            continue;
         vdp1time_blkcmd[f + count[b]++] = (u16)i;
      }
   return 1;
}

/* Each command's modelled read time and what it reads; the list's end.
   Returns 0 if the walk can't be modelled (too long, a loop) or a later
   system clip goes beyond the clear. */
static int Vdp1TimeWalk(int cover)
{
   const double dta = 1 + 48 / 256.0;          /* 16 bpp drawing */
   u32 addr = 0, ret = 0xFFFFFFFF;
   s32 cx = 0, cy = 0, lx = Vdp1Regs->localX, ly = Vdp1Regs->localY;
   double t = 0;
   int n, first = 1;

   vdp1time_ncmds = 0;
   for (n = 0; n < 4000; n++)
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

         switch (type)
         {
         case 0:
            cost += h * (w + 8.0) + (cm == 1 ? 16 : 0);
            break;
         case 1:
            if (((ctrl >> 8) & 0xF) == 0)
               cost += (abs(yc - ya) + 1) * (abs(xc - xa) + 1 + 8.0);
            else
               cost += (abs(yb) + 1) * (abs(xb) + 1 + 8.0);
            cost += cm == 1 ? 16 : 0;
            break;
         case 2: case 3: case 4:
            cost += Vdp1TimeQuad(xa, ya, xb, yb, xc, yc, xd, yd) + (cm == 1 && type != 4 ? 16 : 0);
            break;
         case 5: case 7:
            cost += (abs(xb - xa) + abs(yb - ya) + abs(xc - xb) + abs(yc - yb) +
                     abs(xd - xc) + abs(yd - yc) + abs(xa - xd) + abs(ya - yd)) + 4 * 8;
            break;
         case 6:
            cost += Vdp1TimeMax(abs(xb - xa), abs(yb - ya)) + 1 + 8;
            break;
         case 9:
            /* a system clip set after the clear may reach what it didn't cover */
            if (!first && cover && (xc > cx || yc > cy))
               return 0;
            cx = xc; cy = yc;
            break;
         case 10:
            lx = xa; ly = ya;
            break;
         default:
            break;
         }
         if (type <= 7)
         {
            cost *= dta;
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

/* Vdp1Draw, before the list is drawn */
void Vdp1TimeDrawStart(void)
{
   int cover;
   if (vdp1time_inflight)
      Vdp1TimeFinish();
   if (!Vdp1TimeMode() || VIDCore->id != 2 /* VIDCORE_SOFT */)
      return;
   cover = Vdp1TimeOpensWithClear();
   if (!cover && vdp1time_mode != 2)
      return;
   if (!Vdp1TimeWalk(cover))
      return;
   vdp1time_cover = cover;
   vdp1time_core = VIDCore;
   vdp1time_regs = *Vdp1Regs;
   vdp1time_start = Vdp1TimeNow();
   {
      const double hz = yabsys.IsPal ? 50 : 59.94;
      const double clock = yabsys.CurSH2FreqType == CLKTYPE_26MHZ ? 26874100 : 28636400;
      vdp1time_end = vdp1time_start + vdp1time_end / (clock / (hz * yabsys.MaxLineCount));
      /* command times from cycles to lines after the start */
      {
         int i;
         for (i = 0; i < vdp1time_ncmds; i++)
            vdp1time_cmds[i].t /= clock / (hz * yabsys.MaxLineCount);
      }
   }
   vdp1time_nwrites = 0;
   vdp1time_overflow = 0;
   vdp1time_lists++;
   vdp1time_inflight = 1;
}

/* A VDP1 RAM write while a list is modelled as drawing, before it is made */
void Vdp1TimeWrite(u32 addr, int size)
{
   Vdp1TimeWr *w;
   int i;
   if (vdp1time_nwrites >= VDP1TIME_MAXWRITES)
   {
      vdp1time_overflow = 1;
      return;
   }
   w = &vdp1time_writes[vdp1time_nwrites++];
   w->t = Vdp1TimeNow() + 0.5 - vdp1time_start;   /* somewhere in this line */
   w->addr = addr;
   w->size = size;
   for (i = 0; i < size; i++)
      w->old[i] = T1ReadByte(Vdp1Ram, (addr + i) & 0x7FFFF);
}

/* Every line: has the modelled list ended? */
void Vdp1TimeLine(void)
{
   if (Vdp1TimeNow() >= vdp1time_end)
      Vdp1TimeFinish();
}

static int Vdp1TimeReads(const Vdp1TimeCmd *c, u32 a)
{
   int k;
   for (k = 0; k < 4; k++)
      if (a >= c->lo[k] && a < c->hi[k])
         return 1;
   return 0;
}

/* The list has ended (or something needs the framebuffer, or the next list
   starts): did a write land before a read it changes? Then draw it again. */
void Vdp1TimeFinish(void)
{
   static u32 patch_addr[VDP1TIME_MAXWRITES * 4];
   static u8 patch_live[VDP1TIME_MAXWRITES * 4];
   int raced = 0, mixed = 0, npatch = 0, i, j, b;

   vdp1time_inflight = 0;
   if (!vdp1time_nwrites)
      return;

   /* per written byte: the chip read it after the write (raced: we read
      the old value), before it (we must not see the new one when drawing
      again), or both (one RAM image can't hold that) */
   for (i = 0; i < vdp1time_nwrites; i++)
   {
      const Vdp1TimeWr *w = &vdp1time_writes[i];
      for (b = 0; b < w->size; b++)
      {
         const u32 a = (w->addr + b) & 0x7FFFF;
         int before = 0, after = 0;
         for (j = vdp1time_blkfirst[a >> 8]; j < vdp1time_blkfirst[(a >> 8) + 1]; j++)
         {
            const Vdp1TimeCmd *c = &vdp1time_cmds[vdp1time_blkcmd[j]];
            if (!Vdp1TimeReads(c, a))
               continue;
            if (c->t >= w->t)
               after = 1;
            else
               before = 1;
         }
         raced |= after;
         mixed |= after && before;
      }
   }
   if (!raced)
      return;
   vdp1time_raced++;
   if (!vdp1time_cover || mixed || vdp1time_overflow)
   {
      vdp1time_unhandled++;
      if (vdp1time_mode == 2)
         fprintf(stderr, "vdp1time: frame %u: list raced, not redrawn (%s)\n", vdp1time_frame,
                 !vdp1time_cover ? "no clear first" : mixed ? "a byte read before and after a write" : "journal full");
      return;
   }

   /* VDP1 RAM as each command read it: undo, newest first, the writes made
      after the commands that read those bytes */
   for (i = vdp1time_nwrites - 1; i >= 0; i--)
   {
      const Vdp1TimeWr *w = &vdp1time_writes[i];
      for (b = 0; b < w->size; b++)
      {
         const u32 a = (w->addr + b) & 0x7FFFF;
         int read_before = 0;
         for (j = vdp1time_blkfirst[a >> 8]; j < vdp1time_blkfirst[(a >> 8) + 1] && !read_before; j++)
         {
            const Vdp1TimeCmd *c = &vdp1time_cmds[vdp1time_blkcmd[j]];
            read_before = c->t < w->t && Vdp1TimeReads(c, a);
         }
         if (!read_before)
            continue;
         patch_addr[npatch] = a;
         patch_live[npatch++] = T1ReadByte(Vdp1Ram, a);
         T1WriteByte(Vdp1Ram, a, w->old[b]);
         VDP1_RAM_MARK(a);
      }
   }

   /* draw the list again over the first draw (its clear covers what that
      left), from the registers it started with: a frame change asked for
      since, or the clipping the list set, must not change it */
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
   vdp1time_redrawn++;
   if (vdp1time_mode == 2)
      fprintf(stderr, "vdp1time: frame %u: list redrawn (%d writes, %d bytes as read before them)\n",
              vdp1time_frame, vdp1time_nwrites, npatch);

   /* the live RAM back, newest value last */
   for (i = npatch - 1; i >= 0; i--)
   {
      T1WriteByte(Vdp1Ram, patch_addr[i], patch_live[i]);
      VDP1_RAM_MARK(patch_addr[i]);
   }
}

/* A state load or reset: the RAM the journal describes is gone */
void Vdp1TimeDiscard(void)
{
   vdp1time_inflight = 0;
}

void Vdp1TimeStats(void)
{
   if (getenv("YAB_VDP1TIME"))
      fprintf(stderr, "vdp1time: %lu lists modelled, %lu raced, %lu redrawn, %lu not redrawn\n",
              vdp1time_lists, vdp1time_raced, vdp1time_redrawn, vdp1time_unhandled);
}
