/*  Copyright 2026 Yabadabadoo contributors

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

/* SH-2 divide unit, 64-bit / 32-bit signed divide without a 64-bit division.
 *
 * Gives exactly what the former `s64 quotient = dividend / divisor; s32 remainder = dividend % divisor;`
 * gave, quirks included, but on 32-bit ARM that expression is a libgcc call (__aeabi_ldivmod, ~200
 * cycles). Here: magnitudes, then a 64/32 unsigned divide from two 32-bit divides (one hardware udiv
 * each on the Cortex-A7; Hacker's Delight, "divlu").
 */

#ifndef SH2DIVU_H
#define SH2DIVU_H

#include "core.h"

/* (u1:u0) / v with u1 < v (the quotient fits in 32 bits); v != 0. Returns the quotient, *r = remainder. */
static INLINE u32 SH2DivU64by32(u32 u1, u32 u0, u32 v, u32 *r)
{
   const u32 b = 0x10000;
   int s = __builtin_clz(v);
   u32 vn1, vn0, un32, un21, un10, un1, un0, q1, q0, rhat;

   /* normalise: top bit of v set; (u0 >> 1) >> (31 - s) is u0 >> (32 - s) without a shift by 32 */
   v <<= s;
   vn1 = v >> 16;
   vn0 = v & 0xFFFF;
   un32 = (u1 << s) | ((u0 >> 1) >> (31 - s));
   un10 = u0 << s;
   un1 = un10 >> 16;
   un0 = un10 & 0xFFFF;

   q1 = un32 / vn1;
   rhat = un32 - q1 * vn1;
   while (q1 >= b || q1 * vn0 > b * rhat + un1)
   {
      q1--;
      rhat += vn1;
      if (rhat >= b)
         break;
   }

   un21 = un32 * b + un1 - q1 * v;
   q0 = un21 / vn1;
   rhat = un21 - q0 * vn1;
   while (q0 >= b || q0 * vn0 > b * rhat + un0)
   {
      q0--;
      rhat += vn1;
      if (rhat >= b)
         break;
   }

   *r = (un21 * b + un0 - q0 * v) >> s;
   return q1 * b + q0;
}

/* DVDNTH:DVDNTL / DVSR (signed), dvsr != 0. Returns 1 on overflow, else 0. *l / *h get the new DVDNTL /
 * DVDNTH: quotient / remainder, or on overflow 0x7FFFFFFF (positive) or 0x80000000 (negative) / 0xFFFFFFFE.
 * Like the former code: a quotient from -1 down to -2^32 is not an overflow (DVDNTL = its low word), and
 * 0x8000000000000000 / -1 wraps to a negative quotient (a negative overflow). */
static INLINE int SH2DivS64by32(u32 dvsr, u32 h, u32 l, u32 *nl, u32 *nh)
{
   int nneg = (s32) h < 0;
   int qneg = nneg != ((s32) dvsr < 0);
   u32 v = (s32) dvsr < 0 ? 0 - dvsr : dvsr;
   u32 u1 = h, u0 = l, q, r;

   if (nneg)
   {
      u0 = 0 - l;
      u1 = ~h + (l == 0);
   }

   if (u1 >= v)
   {
      /* |quotient| >= 2^32 */
      if (qneg && u1 == v && u0 < v)
      {
         /* quotient = -2^32 exactly: not an overflow for the former code */
         *nl = 0;
         *nh = nneg ? 0 - u0 : u0;
         return 0;
      }
      if (h == 0x80000000 && l == 0 && dvsr == 0xFFFFFFFF)
         qneg = 1;
      *nl = qneg ? 0x80000000 : 0x7FFFFFFF;
      *nh = 0xFFFFFFFE;
      return 1;
   }

   q = SH2DivU64by32(u1, u0, v, &r);
   if (!qneg && q > 0x7FFFFFFF)
   {
      *nl = 0x7FFFFFFF;
      *nh = 0xFFFFFFFE;
      return 1;
   }
   *nl = qneg ? 0 - q : q;
   *nh = nneg ? 0 - r : r;
   return 0;
}

#endif
