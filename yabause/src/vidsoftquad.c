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

/*! \file vidsoftquad.c
    \brief VDP1 quads drawn last line first (vidsoft.c's drawQuad()), same output.

    The VDP1 draws a quad as stacked lines, and where its sides are longer
    than its height the lines overlap: in Sega Rally most points plotted are
    painted over by a later line of the same quad. When the quad's mode never
    reads the framebuffer (replace, or Gouraud on a textured quad), a pixel
    ends up with what the LAST line that writes it wrote. So drawQuad() can
    draw such a quad's lines last first and skip every point whose pixel a
    later line already wrote (stamp[pos] == gen): the same framebuffer, without
    the texel reads, colour lookups and Gouraud maths of the hidden points.
    Each line still walks all its points in their order (the texture step and
    the Gouraud sums are counted along the line, as before); the points of one
    line are distinct pixels.

    The loops here are the lean loops of vidsoft.c (QuadPlotCM1Gouraud(),
    QuadPlotLeanBody(), QuadPlotLeanRGBPlain()) with the
    stamp test before the texel read and the stamp written with the pixel;
    keep them the same as those. In a file of their own because vidsoft.c sits
    at GCC's per-file inlining limit.

    The pen a line leaves behind (QuadPen: last texel, its visibility, the
    Gouraud colours) is not read again: every point sets the texel and its
    visibility before it plots, and every Gouraud line starts from its own
    colours (drawQuad()). The visibility left behind is kept as it was; the
    texel is that of the last point read, which now is another one.
*/

#include <stdlib.h>
#include <string.h>
#include "vidsoftquad.h"
#include "memory.h"

#define COLOR(r,g,b)    (((r)&0x1F)|(((g)&0x1F)<<5)|(((b)&0x1F)<<10) |0x8000 )

/* vidsoft.c's gouraudAdjust() */
static INLINE int QuadRevGouraud(int color, int tableValue)
{
   color += (tableValue - 0x10);
   if (color < 0) color = 0;
   if (color > 0x1f) color = 0x1f;
   return color;
}

/* Which quads can be drawn last line first: every line of them, once its
 * texture step is exact (int_step in DrawLineQuad()), goes to one of the loops
 * below, and none reads the framebuffer: the lean loops of vidsoft.c on the
 * 16-bit framebuffer (replace or Gouraud, textured, no end codes, mesh,
 * MSB-on, double interlace or user clipping "outside"). Untextured quads stay
 * as they are: their points cost no more than a stamp. Not the 8-bit
 * framebuffer: Virtua Fighter 2's quads there overlap little, and drawn last
 * line first it ran 1.5 % slower (device, 2026-10-28). */
static int QuadRevMode(int fb8, int colormode, int drawmode, int isTextured, int endcodes, int mesh, int msbon,
                       int dil_skip, int user_clip)
{
   if (fb8 || !isTextured || mesh || msbon || dil_skip >= 0 || user_clip == 2 || endcodes)
      return 0;
   if (colormode == 5)
      return drawmode == 0 || drawmode == 4;
   return (drawmode == 0 && (colormode == 0 || colormode == 1 || colormode == 4)) ||
          (drawmode == 4 && colormode == 1);
}

/* one stamp per framebuffer pixel and VDP1 thread; a quad's stamp is the next value of gen, the buffer is cleared
 * (where quads stamped) when gen wraps */
static u8 quad_rev_stamp[2][0x20000];
static u32 quad_rev_gen[2];
static int quad_rev_lo[2] = { 0x20000, 0x20000 }, quad_rev_hi[2];

static u8 *QuadRevBegin(int tid, int width, int xmax, int miny, int maxy, u32 *gen)
{
   int lo, hi;
   if (++quad_rev_gen[tid] > 0xFF)
   {
      if (quad_rev_hi[tid] > quad_rev_lo[tid])
         memset(quad_rev_stamp[tid] + quad_rev_lo[tid], 0, quad_rev_hi[tid] - quad_rev_lo[tid]);
      quad_rev_lo[tid] = 0x20000;
      quad_rev_hi[tid] = 0;
      quad_rev_gen[tid] = 1;
   }
   /* the positions this quad can stamp: row * width + x for its rows
    * (framebuffer rows, already divided for double interlace) and x within
    * the system clip (which may be wider than the framebuffer) */
   lo = miny < 0 ? 0 : miny * width;
   hi = maxy * width + (xmax > width ? xmax : width) + 1;
   if (hi > 0x20000)
      hi = 0x20000;
   if (lo < quad_rev_lo[tid]) quad_rev_lo[tid] = lo;
   if (hi > quad_rev_hi[tid]) quad_rev_hi[tid] = hi;
   *gen = quad_rev_gen[tid];
   return quad_rev_stamp[tid];
}

/* the line template of each VDP1 thread's current last-line-first quad */
static QuadLine quad_rev_line[2];
static int quad_rev_inbuf[2];   /* 16-bit: every point inside the clip area and rows is below 0x20000 */

/* drawQuad(), for a quad on the fast path: whether to draw it last line
 * first. If so, its stamps are set up and the per-quad part of its lines
 * (QuadLine, as DrawLineQuad() fills it) is kept for QuadRevLine(). The mode
 * as Vdp1QuadSetup() reads it. YAB_QUADREV=0: never (for comparisons). */
int QuadRevStart(int tid, const vdp1cmd_struct *cmd, Vdp1 *regs, u8 *ram, u8 *fb, int fb8, int dil_skip, int cw,
                 int interlace, int width, int row_lo, int row_hi, int miny, int maxy, const u32 *pts)
{
   static int on = -1;
   QuadLine *L = &quad_rev_line[tid];
   const int pmod = cmd->CMDPMOD, shape = cmd->CMDCTRL & 0x7;
   const int isTextured = !(shape == 4 || shape == 5 || shape == 6);
   const int user_clip = (pmod & 0x0400) ? ((((pmod >> 9) & 0x3) == 0x3) ? 2 : 1) : 0;

   if (on < 0)
   {
      const char *e = getenv("YAB_QUADREV");
      on = !(e && e[0] == '0');
   }
   if (!on || !QuadRevMode(fb8, (pmod >> 3) & 0x7, pmod & 0x7, isTextured, isTextured && !(pmod & 0x80),
                           pmod & 0x0100, pmod & 0x8000, dil_skip, user_clip))
      return 0;
   L->pts = pts; L->fb = (u16 *)fb; L->fb8 = fb; L->ram = ram; L->regs = regs; L->cmd = (vdp1cmd_struct *)cmd;
   L->colorbank = cmd->CMDCOLR; L->colorlut = (u32)cmd->CMDCOLR << 3;
   L->colormode = (pmod >> 3) & 0x7; L->drawmode = pmod & 0x7; L->flip = (cmd->CMDCTRL & 0x30) >> 4;
   L->SPD = (pmod & 0x40) != 0; L->endcodes = isTextured && !(pmod & 0x80); L->isTextured = isTextured;
   L->untexturedColor = 0; L->mesh = 0; L->msbon = 0; L->dil_skip = dil_skip; L->cw = cw;
   L->interlace = interlace; L->width = width; L->user_clip = user_clip;
   L->row_lo = row_lo; L->row_hi = row_hi;
   {
      int ymax = regs->systemclipY2 < row_hi - 1 ? regs->systemclipY2 : row_hi - 1;
      quad_rev_inbuf[tid] = ymax < 0x20000 && regs->systemclipX2 < 0x20000 &&
                            (s64)ymax * width + regs->systemclipX2 < 0x20000;
   }
   L->stamp = QuadRevBegin(tid, width, regs->systemclipX2, miny, maxy, &L->gen);
   return 1;
}

/* drawQuad()'s line walk (iterateOverLine() with greedy set), see QuadWalk().
 * x major: a point per x step and, for every y step, one more at (x + ax, old
 * y) when the line goes the same way on both axes ("same"), else at (x, new
 * y). The error term of the original (a += dy against dx, with dx negated
 * when ax != ay) always has the sign of the minor step or is 0, so it is kept
 * here as a non-negative b against the absolute deltas. */
static INLINE __attribute__((always_inline)) void QuadWalkX(const int same, u32 *p, int x, u32 Y, int ax, u32 ystep,
                                                            int adx, int ady)
{
   int b = 0, i;
   for (i = adx; i != 0; i--, x += ax)
   {
      *p++ = ((u32)x & 0xFFFF) | Y;
      b += ady;
      if (b >= adx)
      {
         b -= adx;
         if (same)
         {
            *p++ = ((u32)(x + ax) & 0xFFFF) | Y;
            Y += ystep;
         }
         else
         {
            Y += ystep;
            *p++ = ((u32)x & 0xFFFF) | Y;
         }
      }
   }
}

/* y major: a point per y step and, for every x step, one more at (new x, y)
 * ("same"), else at (old x, y + ay) */
static INLINE __attribute__((always_inline)) void QuadWalkY(const int same, u32 *p, int x, u32 Y, int ax, u32 ystep,
                                                            int adx, int ady)
{
   int b = 0, i;
   for (i = ady; i != 0; i--, Y += ystep)
   {
      *p++ = ((u32)x & 0xFFFF) | Y;
      b += adx;
      if (b >= ady)
      {
         b -= ady;
         if (same)
         {
            x += ax;
            *p++ = ((u32)x & 0xFFFF) | Y;
         }
         else
         {
            *p++ = ((u32)x & 0xFFFF) | (Y + ystep);
            x += ax;
         }
      }
   }
}

/* The points of the line from (x1, y1) to (x2, y2) as drawQuad() walks them,
 * into pts as (y << 16) | (u16)x in walking order, |dx| + |dy| + 1 of them
 * (the last is (x2, y2)); returns how many. Endpoints within 999 of each
 * other. In small loops of their own so they keep their values in registers
 * (inside drawQuad() the walk spilled three to the stack). */
int QuadWalk(u32 *pts, int x1, int y1, int x2, int y2)
{
   const int dx = x2 - x1, dy = y2 - y1;
   const int ax = dx >= 0 ? 1 : -1, ay = dy >= 0 ? 1 : -1;
   const int adx = abs(dx), ady = abs(dy), n = adx + ady + 1;
   const u32 ystep = (u32)ay << 16, Y = (u32)y1 << 16;

   pts[n - 1] = ((u32)x2 & 0xFFFF) | ((u32)y2 << 16);
   if (adx > ady)
   {
      if (ax == ay)
         QuadWalkX(1, pts, x1, Y, ax, ystep, adx, ady);
      else
         QuadWalkX(0, pts, x1, Y, ax, ystep, adx, ady);
   }
   else if (ax == ay)
      QuadWalkY(1, pts, x1, Y, ax, ystep, adx, ady);
   else
      QuadWalkY(0, pts, x1, Y, ax, ystep, adx, ady);
   return n;
}

/* vidsoft.c's QuadLeanRun() */
static INLINE __attribute__((always_inline)) void QuadRevRun(const QuadLine *Lp, int *k0p, int *k1p)
{
   const u32 *const pts = Lp->pts;
   const int n = Lp->n;
   int k0 = 0, k1 = n - 1;
   int xlo = 0, xhi = Lp->regs->systemclipX2;
   int ylo = Lp->row_lo, yhi = Lp->row_hi - 1;

   if (Lp->regs->systemclipY2 < yhi)
      yhi = Lp->regs->systemclipY2;
   if (ylo < 0)
      ylo = 0;
   if (Lp->user_clip == 1)
   {
      if (Lp->regs->userclipX1 > xlo) xlo = Lp->regs->userclipX1;
      if (Lp->regs->userclipX2 < xhi) xhi = Lp->regs->userclipX2;
      if (Lp->regs->userclipY1 > ylo) ylo = Lp->regs->userclipY1;
      if (Lp->regs->userclipY2 < yhi) yhi = Lp->regs->userclipY2;
   }
#define LEAN_IN(pt) ((s16)(pt) >= xlo && (s16)(pt) <= xhi && ((s32)(pt) >> 16) >= ylo && ((s32)(pt) >> 16) <= yhi)
   while (k0 < n && !LEAN_IN(pts[k0]))
      k0++;
   while (k1 >= k0 && !LEAN_IN(pts[k1]))
      k1--;
#undef LEAN_IN
   if (k0 > k1)
      k1 = k0 - 1;
   *k0p = k0;
   *k1p = k1;
}

/* QuadPlotCM1Gouraud(): CM1 Gouraud, the whole line inside the clip window */
static __attribute__((noinline)) QuadPen QuadRevCM1Gouraud(const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   u8 *const stamp = Lp->stamp;
   const u32 gen = Lp->gen;
   const u8 *const ram = Lp->ram;
   const u32 rowaddr = Lp->rowaddr, colorlut = Lp->colorlut;
   const int SPD = Lp->SPD, width = Lp->width, row_lo = Lp->row_lo, row_hi = Lp->row_hi;
   const int flip = Lp->flip & 1, cwm1 = Lp->cw - 1;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength, n = Lp->n;
   const double texturestep = Lp->texturestep;
   const double rs = Lp->xredstep, gs = Lp->xgreenstep, bs = Lp->xbluestep;
   double lr = pen.lr, lg = pen.lg, lb = pen.lb;
   int cp = pen.cp, k, quot = 0, rem = 0;

   for (k = 0; k < n; k++)
   {
      const u32 pt = pts[k];
      const int px = (s16)pt, py = (s32)pt >> 16;
      int cs, pos;
      u32 idx, dots;

      lr += rs; lg += gs; lb += bs;
      if (rem == 0 && step_r != 0)
         cs = (int)k * texturestep;
      else
         cs = quot;
      quot += step_q;
      rem += step_r;
      if (rem >= linelength)
      {
         rem -= linelength;
         quot++;
      }
      pos = py * width + px;
      if (py < row_lo || py >= row_hi || pos >= 0x20000)
         continue;
      if (stamp[pos] == gen)
         continue;                                     /* a later line wrote it */
      idx = flip ? (u32)(cwm1 - cs) : (u32)cs;
      dots = ram[(rowaddr + (idx >> 1)) & 0x7FFFF];
      cp = (idx & 1) ? (dots & 0xF) : (dots >> 4);
      if (cp == 0 && !SPD)
         continue;                                     /* transparent */
      cp = T1ReadWord((u8 *)ram, (cp * 2 + colorlut) & 0x7FFFF);
      if (SPD || cp)
      {
         fb[pos] = COLOR(QuadRevGouraud(cp & 0x001F, (int)lr),
                         QuadRevGouraud((cp & 0x03e0) >> 5, (int)lg),
                         QuadRevGouraud((cp & 0x7c00) >> 10, (int)lb));
         stamp[pos] = gen;
      }
   }

   pen.cp = cp;
   pen.vis = 0xffff;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
}

/* QuadPlotLeanBody(): CM0/CM1/CM4 replace, CM1 Gouraud */
static INLINE __attribute__((always_inline)) QuadPen QuadRevLeanBody(const int CM, const int DM, const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   u8 *const stamp = Lp->stamp;
   const u32 gen = Lp->gen;
   const u8 *const ram = Lp->ram;
   const u32 rowaddr = Lp->rowaddr, colorlut = Lp->colorlut, colorbank = Lp->colorbank;
   const int SPD = Lp->SPD, width = Lp->width, n = Lp->n;
   const int flip = Lp->flip & 1, cwm1 = Lp->cw - 1;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength;
   const double texturestep = Lp->texturestep;
   const double rs = Lp->xredstep, gs = Lp->xgreenstep, bs = Lp->xbluestep;
   double lr = pen.lr, lg = pen.lg, lb = pen.lb;
   int cp = pen.cp, k, k0, k1, quot, rem, read = 0;

   QuadRevRun(Lp, &k0, &k1);

   quot = k0 * step_q + (k0 * step_r) / (linelength > 0 ? linelength : 1);
   rem = (k0 * step_r) % (linelength > 0 ? linelength : 1);

   for (k = DM == 4 ? 0 : k0; k <= (DM == 4 ? n - 1 : k1); k++)
   {
      u32 pt;
      int px, py;
      int cs, pos;
      u32 idx;

      if (DM == 4)
      {
         lr += rs; lg += gs; lb += bs;
         if (k < k0 || k > k1)
            continue;
      }
      pt = pts[k];
      px = (s16)pt;
      py = (s32)pt >> 16;
      if (rem == 0 && step_r != 0)
         cs = (int)k * texturestep;
      else
         cs = quot;
      quot += step_q;
      rem += step_r;
      if (rem >= linelength)
      {
         rem -= linelength;
         quot++;
      }
      pos = py * width + px;
      if (pos >= 0x20000)
         continue;
      read = 1;
      if (stamp[pos] == gen)
         continue;                                     /* a later line wrote it */
      idx = flip ? (u32)(cwm1 - cs) : (u32)cs;
      if (CM == 0 || CM == 1)
      {
         u32 dots = ram[(rowaddr + (idx >> 1)) & 0x7FFFF];
         cp = (idx & 1) ? (dots & 0xF) : (dots >> 4);
         if (cp == 0 && !SPD)
            continue;
         if (CM == 0)
            cp = (colorbank & 0xfff0) | cp;
         else
            cp = T1ReadWord((u8 *)ram, (cp * 2 + colorlut) & 0x7FFFF);
         if (!(SPD || (cp & (CM == 0 ? 0xf : 0xffff))))
            continue;
      }
      else /* CM == 4 */
      {
         cp = ram[(rowaddr + idx) & 0x7FFFF];
         if (cp == 0 && !SPD)
            continue;
         cp = (colorbank & 0xff00) | cp;
         if (!(SPD || (cp & 0xff)))
            continue;
      }
      if (DM == 0)
         fb[pos] = cp;
      else
         fb[pos] = COLOR(QuadRevGouraud(cp & 0x001F, (int)lr),
                         QuadRevGouraud((cp & 0x03e0) >> 5, (int)lg),
                         QuadRevGouraud((cp & 0x7c00) >> 10, (int)lb));
      stamp[pos] = gen;
   }
   pen.cp = cp;
   if (read)
      pen.vis = CM == 0 ? 0xf : CM == 4 ? 0xff : 0xffff;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
}

static __attribute__((noinline)) QuadPen QuadRevLeanCM0(const QuadLine *Lp, QuadPen pen) { return QuadRevLeanBody(0, 0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadRevLeanCM1(const QuadLine *Lp, QuadPen pen) { return QuadRevLeanBody(1, 0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadRevLeanCM4(const QuadLine *Lp, QuadPen pen) { return QuadRevLeanBody(4, 0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadRevLeanCM1G(const QuadLine *Lp, QuadPen pen) { return QuadRevLeanBody(1, 4, Lp, pen); }

/* QuadPlotLeanRGBPlain() without end codes: RGB textures, replace or Gouraud */
static INLINE __attribute__((always_inline)) QuadPen QuadRevRGBBody(const int DM, const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   u8 *const stamp = Lp->stamp;
   const u32 gen = Lp->gen;
   const u8 *const ram = Lp->ram;
   const u32 rowaddr = Lp->rowaddr;
   const int SPD = Lp->SPD, width = Lp->width, n = Lp->n;
   const int flip = Lp->flip & 1, cwm1 = Lp->cw - 1;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength;
   const double texturestep = Lp->texturestep;
   const double rs = Lp->xredstep, gs = Lp->xgreenstep, bs = Lp->xbluestep;
   double lr = pen.lr, lg = pen.lg, lb = pen.lb;
   int cp = pen.cp, vis = pen.vis, k, k0, k1, kfirst, klast, quot, rem;

   QuadRevRun(Lp, &k0, &k1);
   kfirst = DM == 4 ? 0 : k0;
   klast = DM == 4 ? n - 1 : k1;
   quot = kfirst * step_q + (kfirst * step_r) / (linelength > 0 ? linelength : 1);
   rem = (kfirst * step_r) % (linelength > 0 ? linelength : 1);

   for (k = kfirst; k <= klast; k++)
   {
      const u32 pt = pts[k];
      const int px = (s16)pt, py = (s32)pt >> 16;
      const int inside = k >= k0 && k <= k1;
      int cs, pos;
      u32 idx;

      if (DM == 4)
      {
         lr += rs; lg += gs; lb += bs;
      }
      if (rem == 0 && step_r != 0)
         cs = (int)k * texturestep;
      else
         cs = quot;
      quot += step_q;
      rem += step_r;
      if (rem >= linelength)
      {
         rem -= linelength;
         quot++;
      }
      pos = py * width + px;
      if (!inside || pos >= 0x20000)
         continue;
      vis = 0xffff;
      if (stamp[pos] == gen)
         continue;                                     /* a later line wrote it */
      idx = flip ? (u32)(cwm1 - cs) : (u32)cs;
      cp = T1ReadWord((u8 *)ram, (rowaddr + 2 * idx) & 0x7FFFF);
      if (!(cp & 0x8000) && !SPD)
         cp = 0;
      if (SPD || cp)
      {
         if (DM == 0)
            fb[pos] = cp;
         else
            fb[pos] = COLOR(QuadRevGouraud(cp & 0x001F, (int)lr),
                            QuadRevGouraud((cp & 0x03e0) >> 5, (int)lg),
                            QuadRevGouraud((cp & 0x7c00) >> 10, (int)lb));
         stamp[pos] = gen;
      }
   }
   pen.cp = cp;
   pen.vis = vis;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
}

static __attribute__((noinline)) QuadPen QuadRevRGB(const QuadLine *Lp, QuadPen pen) { return QuadRevRGBBody(0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadRevRGBG(const QuadLine *Lp, QuadPen pen) { return QuadRevRGBBody(4, Lp, pen); }

/* the texture step where the quotient/remainder count has a remainder of 0:
 * the double product (vidsoft.c's QuadStepExact()) */
static __attribute__((noinline)) int QuadRevStepExact(int k, double texturestep)
{
   return (int)k * texturestep;
}

/* QuadRevLeanBody(1, 0, ...) for Sega Rally's lines (16-colour textures
 * through a colour table, replace), with what the general copy keeps in
 * registers cut down so the loop doesn't spill: the flip and SPD are
 * constants of a copy, the texel row and the colour table are pointers (the
 * caller checked that neither wraps at the end of VDP1 RAM), every point of
 * the run is inside the framebuffer (quad_rev_inbuf), the texture step has a
 * remainder (step_r != 0) and its rare exact case is a call. The same pixels
 * and stamps as QuadRevLeanBody(1, 0, ...). */
static INLINE __attribute__((always_inline)) QuadPen QuadRevCM1Body(const int FLIP, const int SPD, const QuadLine *Lp,
                                                                    QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   u8 *const stamp = Lp->stamp;
   const u32 gen = Lp->gen;
   const u8 *const row = Lp->ram + Lp->rowaddr;
   const u8 *const lut = Lp->ram + Lp->colorlut;
   const int width = Lp->width, cwm1 = Lp->cw - 1;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength;
   const double texturestep = Lp->texturestep;
   const u32 *p, *pend;
   int k0, k1, quot, rem;

   QuadRevRun(Lp, &k0, &k1);
   if (k1 < k0)
      return pen;
   quot = k0 * step_q + (k0 * step_r) / linelength;
   rem = (k0 * step_r) % linelength;
   pend = pts + k1 + 1;
   for (p = pts + k0; p < pend; p++)
   {
      const u32 pt = *p;
      const int pos = ((s32)pt >> 16) * width + (s16)pt;
      int cs;
      u32 idx, dots, t, c;

      if (__builtin_expect(rem == 0, 0))
         cs = QuadRevStepExact((int)(p - pts), texturestep);
      else
         cs = quot;
      quot += step_q;
      rem += step_r;
      if (rem >= linelength)
      {
         rem -= linelength;
         quot++;
      }
      if (stamp[pos] == gen)
         continue;                                     /* a later line wrote it */
      idx = FLIP ? (u32)(cwm1 - cs) : (u32)cs;
      dots = row[idx >> 1];
      t = (idx & 1) ? (dots & 0xF) : (dots >> 4);
      if (!SPD && t == 0)
         continue;
      c = __builtin_bswap16(*(const u16 *)(lut + 2 * t));
      if (!SPD && c == 0)
         continue;
      fb[pos] = c;
      stamp[pos] = gen;
   }
   pen.vis = 0xffff;   /* the run's points were read (the texel left behind isn't used again) */
   return pen;
}

static __attribute__((noinline)) QuadPen QuadRevCM1F0S0(const QuadLine *Lp, QuadPen pen) { return QuadRevCM1Body(0, 0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadRevCM1F1S0(const QuadLine *Lp, QuadPen pen) { return QuadRevCM1Body(1, 0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadRevCM1F0S1(const QuadLine *Lp, QuadPen pen) { return QuadRevCM1Body(0, 1, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadRevCM1F1S1(const QuadLine *Lp, QuadPen pen) { return QuadRevCM1Body(1, 1, Lp, pen); }

/* A line of a quad QuadRevMode() accepted, with an exact texture step: the
 * loop DrawLineQuad() would take, with the stamps */
static QuadPen QuadRevPlot(const QuadLine *Lp, QuadPen pen, int inbuf)
{
   switch (Lp->colormode * 8 + Lp->drawmode)
   {
   case 0 * 8 + 0: return QuadRevLeanCM0(Lp, pen);
   case 1 * 8 + 0:
      if (inbuf && Lp->step_r != 0 && Lp->linelength > 0 &&
          Lp->rowaddr + ((u32)Lp->cw >> 1) <= 0x80000 && Lp->colorlut + 32 <= 0x80000)
      {
         if (Lp->flip & 1)
            return Lp->SPD ? QuadRevCM1F1S1(Lp, pen) : QuadRevCM1F1S0(Lp, pen);
         return Lp->SPD ? QuadRevCM1F0S1(Lp, pen) : QuadRevCM1F0S0(Lp, pen);
      }
      return QuadRevLeanCM1(Lp, pen);
   case 1 * 8 + 4: return Lp->noclip ? QuadRevCM1Gouraud(Lp, pen) : QuadRevLeanCM1G(Lp, pen);
   case 4 * 8 + 0: return QuadRevLeanCM4(Lp, pen);
   case 5 * 8 + 0: return QuadRevRGB(Lp, pen);
   default:        return QuadRevRGBG(Lp, pen);
   }
}

/* A line of such a quad whose texture step isn't exact: DrawLineQuad() then
 * calls DrawLine(x1, y1, x2, y2) with x1, y1 where its own walk ended, and
 * DrawLine() plots the greedy walk from there with currentStep = (int)i *
 * texturestep (getpixel(), then putpixel(): what the loops
 * above do for the same points). The same walk here, then the loop with that
 * step: a remainder step of 1 in 1 makes every point take the double product.
 * Lp->pts is DrawLineQuad()'s point buffer, free again at this point. */
static QuadPen QuadRevDrawLine(QuadLine *Lp, int x1, int y1, int x2, int y2, QuadPen pen, int inbuf)
{
   u32 *const pt = (u32 *)Lp->pts;
   int a = 0, ax, ay, dx = x2 - x1, dy = y2 - y1, n = 0;

   ax = (dx >= 0) ? 1 : -1;
   ay = (dy >= 0) ? 1 : -1;
   if (abs(dx) > 999 || abs(dy) > 999)   /* iterateOverLine() draws nothing */
      return pen;
#define QUAD_REV_PT(X, Y) (((u32)(X) & 0xFFFF) | ((u32)(Y) << 16))
   if (abs(dx) > abs(dy)) {
      if (ax != ay) dx = -dx;
      for (; x1 != x2; x1 += ax) {
         pt[n++] = QUAD_REV_PT(x1, y1);
         a += dy;
         if (abs(a) >= abs(dx)) {
            a -= dx;
            y1 += ay;
            if (ax == ay) { pt[n++] = QUAD_REV_PT(x1 + ax, y1 - ay); }
            else { pt[n++] = QUAD_REV_PT(x1, y1); }
         }
      }
   } else {
      if (ax != ay) dy = -dy;
      for (; y1 != y2; y1 += ay) {
         pt[n++] = QUAD_REV_PT(x1, y1);
         a += dx;
         if (abs(a) >= abs(dy)) {
            a -= dy;
            x1 += ax;
            if (ay == ax) { pt[n++] = QUAD_REV_PT(x1, y1); }
            else { pt[n++] = QUAD_REV_PT(x1 - ax, y1 + ay); }
         }
      }
   }
   pt[n++] = QUAD_REV_PT(x2, y2);
#undef QUAD_REV_PT
   Lp->n = n;
   Lp->step_q = 0;
   Lp->step_r = 1;
   Lp->linelength = 1;
   Lp->noclip = 0;
   return QuadRevPlot(Lp, pen, inbuf);
}

/* DrawLineQuad(), for a line of a quad QuadRevStart() accepted: the line's
 * own values (the points are in DrawLineQuad()'s buffer), the pen
 * (vidsoft.c's currentPixel, currentPixelIsVisible, leftColumnColor) in and
 * out as there */
void QuadRevLine(int tid, int n, u32 rowaddr, int noclip, int int_step, int step_q, int step_r, int linelength,
                 int x1, int y1, int x2, int y2, double texturestep, double xredstep, double xgreenstep,
                 double xbluestep)
{
   QuadLine *L = &quad_rev_line[tid];
   QuadPen pen;
   pen.cp = currentPixel;
   pen.vis = currentPixelIsVisible;
   pen.lr = leftColumnColor.r;
   pen.lg = leftColumnColor.g;
   pen.lb = leftColumnColor.b;
   L->n = n; L->rowaddr = rowaddr; L->noclip = noclip;
   L->step_q = step_q; L->step_r = step_r; L->linelength = linelength;
   L->texturestep = texturestep; L->xredstep = xredstep; L->xgreenstep = xgreenstep; L->xbluestep = xbluestep;
   pen = int_step ? QuadRevPlot(L, pen, quad_rev_inbuf[tid]) :
                    QuadRevDrawLine(L, x1, y1, x2, y2, pen, quad_rev_inbuf[tid]);
   currentPixel = pen.cp;
   currentPixelIsVisible = pen.vis;
   leftColumnColor.r = pen.lr;
   leftColumnColor.g = pen.lg;
   leftColumnColor.b = pen.lb;
}
