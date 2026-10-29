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

/*! \file vidsoftquad.h
    \brief The per-line state of the software renderer's VDP1 quad plotting
    (vidsoft.c) and the last-line-first plot loops in vidsoftquad.c.
*/

#ifndef VIDSOFTQUAD_H
#define VIDSOFTQUAD_H

#include "vdp1.h"

/* per-line values for QuadPlotPoints(), passed by value so they stay in
 * registers */
typedef struct {
   const u32 *pts;
   u16 *fb;
   u8 *fb8;
   u8 *ram;
   Vdp1 *regs;
   vdp1cmd_struct *cmd;
   u32 rowaddr, colorbank, colorlut;
   int colormode, drawmode, flip, SPD, endcodes, isTextured, untexturedColor;
   int mesh, msbon, dil_skip, cw, interlace, width, noclip, n, user_clip;
   int step_q, step_r, linelength, row_lo, row_hi;
   double texturestep, xredstep, xgreenstep, xbluestep;
   u8 *stamp;   /* last-line-first quads (vidsoftquad.c only): the pixels later lines wrote */
   u32 gen;     /* the value of stamp[] that marks them */
} QuadLine;

typedef struct {
   int cp, vis;
   double lr, lg, lb;
} QuadPen;

/* the pen of vidsoft.c's VDP1 drawing threads (thread-local there) */
typedef struct _COLOR_PARAMS
{
   double r,g,b;
} COLOR_PARAMS;
#if defined(__GNUC__) || defined(__clang__)
#define VDP1_TLS __thread __attribute__((tls_model("initial-exec")))
#else
#define VDP1_TLS
#endif
extern VDP1_TLS COLOR_PARAMS leftColumnColor;
extern VDP1_TLS int currentPixel;
extern VDP1_TLS int currentPixelIsVisible;

/* vidsoftquad.c */
int QuadRevStart(int tid, const vdp1cmd_struct *cmd, Vdp1 *regs, u8 *ram, u8 *fb, int fb8, int dil_skip, int cw,
                 int interlace, int width, int row_lo, int row_hi, int miny, int maxy, const u32 *pts);
int QuadWalk(u32 *pts, int x1, int y1, int x2, int y2);
void QuadRevLine(int tid, int n, u32 rowaddr, int noclip, int int_step, int step_q, int step_r, int linelength,
                 int x1, int y1, int x2, int y2, double texturestep, double xredstep, double xgreenstep,
                 double xbluestep);

#endif
