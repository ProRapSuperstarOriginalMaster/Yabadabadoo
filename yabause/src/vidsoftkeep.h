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

#ifndef VIDSOFTKEEP_H
#define VIDSOFTKEEP_H

#include "vidshared.h"

/* Layer-line reuse (vidsoftkeep.c). From vdp2.c, for the software
   renderer: before VIDSoftVdp2DrawScreens draws a field, and after DrawEnd */
void VidsoftKeepFrame(void);
void VidsoftKeepFrameEnd(void);
/* From Vdp2DrawScroll, for each line it would draw into Titan row `row`
   (y = its final y, lsx = its line scroll x): 1 = the row was kept, don't
   draw it. The first call for a layer in a field decides its rows. */
int VidsoftKeepRow(vdp2draw_struct *info, int row, int y, int lsx);
/* At the end of Vdp2DrawScroll (ram = the VDP2 RAM it drew from): note what
   the drawn rows read */
void VidsoftKeepDrawn(vdp2draw_struct *info, u8 *ram);

#endif
