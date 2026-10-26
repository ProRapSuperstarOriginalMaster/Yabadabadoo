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

#include "titan.h"
#include "../yabprof.h"
#include "../vidshared.h"
#include "../vidsoft.h"
#include "../threads.h"

#include <stdlib.h>
#include <string.h>

/* see TitanRenderInputsSame */
static u16 tt_render_regs[8];
static int tt_render_regs_valid;

/* private */
typedef u32 (*TitanBlendFunc)(u32 top, u32 bottom);
typedef int FASTCALL (*TitanTransFunc)(u32 pixel);
void TitanRenderLines(pixel_t * dispbuffer, int start_line, int end_line);
extern int vdp2_interlace;

int vidsoft_num_priority_threads = 0;

static struct TitanContext {
   int inited;
   struct PixelData * vdp2framebuffer[6];
   u32 * linescreen[4];
   u32 * dot_linecolor[2];   /* see TitanGetDotLineColors, allocated on first use */
   int vdp2width;
   int vdp2height;
   TitanBlendFunc blend;
   TitanTransFunc trans;
   struct PixelData * backscreen;
   int layer_priority[6];
   int layer_dirtied[6];
   /* layers drawn this frame, highest index first (TitanDigPixel order) */
   int active_layers[6];
   int num_active_layers;
   /* Layer lines are cleared lazily: a line is valid only if it was written
    * in the current frame (line_frame == frame); otherwise it reads as empty.
    * This avoids a full memset of every drawn layer each frame. */
   u32 frame;
   u32 line_frame[6][512];
   /* upper bound of the pixel priorities in each layer line (valid when
    * line_frame matches); 7 unless the writer said otherwise */
   u8 line_maxprio[6][512];
   int blend_mode;       /* TITAN_BLEND_*, selects blend/trans below */
   u16 line_ccctl;       /* see TitanSetLineColorCalc */
} tt_context = {
   0,
   { NULL, NULL, NULL, NULL, NULL, NULL },
   { NULL, NULL, NULL, NULL },
   { NULL, NULL },
   320,
   224,
   NULL,NULL,NULL,
   { 0, 0, 0, 0, 0, 0 },
   { 0, 0, 0, 0, 0, 0 },
   { 0, 0, 0, 0, 0, 0 },
   0,
   1,
   { { 0 } },
   { { 0 } },
   TITAN_BLEND_TOP
};

/* line colour of a top pixel with linescreen != 0; pos is its layer offset */
static INLINE u32 TitanLineColor(const struct PixelData *pd, int pos, int y)
{
   if (pd->linescreen < TITAN_LINESCREEN_DOT)
      return tt_context.linescreen[pd->linescreen][y];
   return tt_context.dot_linecolor[pd->linescreen - TITAN_LINESCREEN_DOT][pos];
}

/* stands in for layer lines not written this frame */
static struct PixelData titan_empty_line[704];

static INLINE struct PixelData *TitanReadLine(int layer, int layer_y)
{
   if (tt_context.line_frame[layer][layer_y] != tt_context.frame)
      return titan_empty_line;
   return &tt_context.vdp2framebuffer[layer][layer_y * tt_context.vdp2width];
}

struct
{
   volatile int need_draw[5];
   volatile int draw_finished[5];
   struct
   {
      volatile int start;
      volatile int end;
   }lines[5];

   pixel_t * dispbuffer;
   int use_simplified;
}priority_thread_context;

#if defined WORDS_BIGENDIAN
#ifdef USE_RGB_555
static INLINE u32 TitanFixAlpha(u32 pixel) { return (((pixel >> 27) & 0x1F) | ((pixel >> 14) & 0x7C0) | (pixel >> 1) & 0xF8); }
#elif USE_RGB_565
static INLINE u32 TitanFixAlpha(u32 pixel) { return (((pixel >> 27) & 0x1F) | ((pixel >> 13) & 0x7E0) | (pixel & 0xF8)); }
#else
static INLINE u32 TitanFixAlpha(u32 pixel) { return ((((pixel & 0x3F) << 2) + 0x03) | (pixel & 0xFFFFFF00)); }
#endif

static INLINE u8 TitanGetAlpha(u32 pixel) { return pixel & 0x3F; }
static INLINE u8 TitanGetRed(u32 pixel) { return (pixel >> 8) & 0xFF; }
static INLINE u8 TitanGetGreen(u32 pixel) { return (pixel >> 16) & 0xFF; }
static INLINE u8 TitanGetBlue(u32 pixel) { return (pixel >> 24) & 0xFF; }
static INLINE u32 TitanCreatePixel(u8 alpha, u8 red, u8 green, u8 blue) { return alpha | (red << 8) | (green << 16) | (blue << 24); }
#else
#ifdef USE_RGB_555
static INLINE u32 TitanFixAlpha(u32 pixel) { return (((pixel << 7) & 0x7C00) | ((pixel >> 6) & 0x3C0) | ((pixel >> 19) & 0x1F)); }
#elif USE_RGB_565
static INLINE u32 TitanFixAlpha(u32 pixel) { return (((pixel << 8) & 0xF800) | ((pixel >> 5) & 0x7C0) | ((pixel >> 19) & 0x1F)); }
#else
static INLINE u32 TitanFixAlpha(u32 pixel) { return ((((pixel & 0x3F000000) << 2) + 0x03000000) | (pixel & 0x00FFFFFF)); }
#endif

static INLINE u8 TitanGetAlpha(u32 pixel) { return (pixel >> 24) & 0x3F; }
static INLINE u8 TitanGetRed(u32 pixel) { return (pixel >> 16) & 0xFF; }
static INLINE u8 TitanGetGreen(u32 pixel) { return (pixel >> 8) & 0xFF; }
static INLINE u8 TitanGetBlue(u32 pixel) { return pixel & 0xFF; }
static INLINE u32 TitanCreatePixel(u8 alpha, u8 red, u8 green, u8 blue) { return (alpha << 24) | (red << 16) | (green << 8) | blue; }
#endif


void set_layer_y(const int start_line, int * layer_y)
{
   if (vdp2_interlace)
      *layer_y = start_line / 2;
   else
      *layer_y = start_line;
}

void TitanRenderLinesSimplified(pixel_t * dispbuffer, int start_line, int end_line)
{
   int x, y, i, layer, j, layer_y;
   int line_increment, interlace_line;
   int sorted_layers[8] = { 0 };
   int num_layers = 0;

   if (!tt_context.inited || (!tt_context.trans))
   {
      return;
   }

   Vdp2GetInterlaceInfo(&interlace_line, &line_increment);

   //pre-sort the layers so it doesn't have to be done per-pixel
   for (i = 7; i >= 0; i--)
   {
      for (layer = TITAN_RBG0; layer >= 0; layer--)
      {
         if (tt_context.layer_priority[layer] > 0 && tt_context.layer_priority[layer] == i)
            sorted_layers[num_layers++] = layer;
      }
   }

   //last layer is always the back screen
   sorted_layers[num_layers++] = TITAN_BACK;

   set_layer_y(start_line, &layer_y);

   struct PixelData *back_buf = tt_context.backscreen;

   for (y = start_line + interlace_line; y < end_line; y += line_increment)
   {
      pixel_t *disp_row = dispbuffer + (y * tt_context.vdp2width);
      int layer_pos = layer_y * tt_context.vdp2width;
      struct PixelData *rows[6];

      /* The layers in priority order (srow), and for each sprite priority how
       * many of them come before the sprite (kfor): a layer at or below the
       * sprite's priority is never reached, even where the sprite pixel is
       * transparent (that pixel then goes straight to the back screen), so a
       * pixel takes the first opaque one of its first kfor[] layers, else the
       * sprite pixel if opaque, else the back screen; the same as walking
       * sorted_layers. The next layer's pixel is loaded before the current
       * one is tested (the in-order Cortex-A7 stalled on each test right
       * after its load); srow[nbg] is an empty row so that stays in bounds. */
      struct PixelData *srow[8];
      int kfor[8];
      const int nbg = num_layers - 1;
      struct PixelData *sprite_row;

      for (layer = 0; layer < 6; layer++)
         rows[layer] = TitanReadLine(layer, layer_y);
      for (j = 0; j < nbg; j++)
         srow[j] = rows[sorted_layers[j]];
      srow[nbg] = titan_empty_line;
      for (i = 0; i < 8; i++)
      {
         for (j = 0; j < nbg && i < tt_context.layer_priority[sorted_layers[j]]; j++)
            ;
         kfor[i] = j;
      }
      sprite_row = rows[TITAN_SPRITE];

      for (x = 0; x < tt_context.vdp2width; x++, layer_pos++)
      {
         u32 out_pixel = 0;
         const struct PixelData sprite = sprite_row[x];
         const int k = sprite.priority > 7 ? 0 : kfor[sprite.priority];
         u32 next = srow[0][x].pixel;

         for (j = 0; j < k; j++)
         {
            const u32 bg_px = next;
            next = srow[j + 1][x].pixel;
            if (bg_px)
            {
               out_pixel = TitanFixAlpha(bg_px);
               goto done;
            }
         }
         if (sprite.pixel)
            out_pixel = TitanFixAlpha(sprite.pixel);
         else
         {
            u32 back_px = back_buf[layer_pos].pixel ? back_buf[layer_pos].pixel : back_buf[y].pixel;
            if (back_px) out_pixel = TitanFixAlpha(back_px);
         }
done:
         disp_row[x] = out_pixel;
      }
      layer_y++;
   }
}

void TitanRenderSimplifiedCheck(pixel_t * buf, int start, int end, int can_use_simplified_rendering)
{
   if (can_use_simplified_rendering)
      TitanRenderLinesSimplified(buf, start, end);
   else
      TitanRenderLines(buf, start, end);
}

#define DECLARE_PRIORITY_THREAD(FUNC_NAME, THREAD_NUMBER) \
void FUNC_NAME(void* data) \
{ \
   for (;;) \
   { \
      if (YabFlagGet(priority_thread_context.need_draw[THREAD_NUMBER])) \
      { \
         priority_thread_context.need_draw[THREAD_NUMBER] = 0; \
         YAB_PROF_TBEGIN(PROF_T_PRIO); \
         TitanRenderSimplifiedCheck(priority_thread_context.dispbuffer, priority_thread_context.lines[THREAD_NUMBER].start, priority_thread_context.lines[THREAD_NUMBER].end, priority_thread_context.use_simplified); \
         YAB_PROF_TEND(PROF_T_PRIO); \
         YabFlagSet(priority_thread_context.draw_finished[THREAD_NUMBER], 1); \
         YabFlagNotify(); \
      } \
      YabThreadSleep(); \
   } \
}

DECLARE_PRIORITY_THREAD(VidsoftPriorityThread0, 0);
DECLARE_PRIORITY_THREAD(VidsoftPriorityThread1, 1);
DECLARE_PRIORITY_THREAD(VidsoftPriorityThread2, 2);
DECLARE_PRIORITY_THREAD(VidsoftPriorityThread3, 3);
DECLARE_PRIORITY_THREAD(VidsoftPriorityThread4, 4);

#define FAST_DIV255(val) (((val) * 257 + 257) >> 16)

static INLINE u32 TitanBlendPixelsTop(u32 top, u32 bottom)
{
   u32 alpha, ralpha, tr, tg, tb, br, bg, bb;

   alpha = (TitanGetAlpha(top) << 2) + 3;
   ralpha = 0xFF - alpha;

   tr = FAST_DIV255(TitanGetRed(top) * alpha);
   tg = FAST_DIV255(TitanGetGreen(top) * alpha);
   tb = FAST_DIV255(TitanGetBlue(top) * alpha);

   br = FAST_DIV255(TitanGetRed(bottom) * ralpha);
   bg = FAST_DIV255(TitanGetGreen(bottom) * ralpha);
   bb = FAST_DIV255(TitanGetBlue(bottom) * ralpha);

   return TitanCreatePixel(0x3F, tr + br, tg + bg, tb + bb);
}

static INLINE u32 TitanBlendPixelsBottom(u32 top, u32 bottom)
{
   u32 alpha, ralpha, tr, tg, tb, br, bg, bb;

   if ((top & 0x80000000) == 0) return top;

   alpha = (TitanGetAlpha(bottom) << 2) + 3;
   ralpha = 0xFF - alpha;

   tr = FAST_DIV255(TitanGetRed(top) * alpha);
   tg = FAST_DIV255(TitanGetGreen(top) * alpha);
   tb = FAST_DIV255(TitanGetBlue(top) * alpha);

   br = FAST_DIV255(TitanGetRed(bottom) * ralpha);
   bg = FAST_DIV255(TitanGetGreen(bottom) * ralpha);
   bb = FAST_DIV255(TitanGetBlue(bottom) * ralpha);

   return TitanCreatePixel(TitanGetAlpha(top), tr + br, tg + bg, tb + bb);
}

static INLINE u32 TitanBlendShadow(u32 bottom)
{
   u32 br = FAST_DIV255(TitanGetRed(bottom) * 124);
   u32 bg = FAST_DIV255(TitanGetGreen(bottom) * 124);
   u32 bb = FAST_DIV255(TitanGetBlue(bottom) * 124);
   return TitanCreatePixel(0x3F, br, bg, bb);
}

static INLINE u32 TitanBlendPixelsAdd(u32 top, u32 bottom)
{
#if defined(__arm__)
   u32 res;
   asm("uqadd8 %0, %1, %2" : "=r"(res) : "r"(top), "r"(bottom));
   return (res & 0x00FFFFFF) | 0x3F000000;
#else
   u32 r, g, b;

   r = TitanGetRed(top) + TitanGetRed(bottom);
   if (r > 0xFF) r = 0xFF;

   g = TitanGetGreen(top) + TitanGetGreen(bottom);
   if (g > 0xFF) g = 0xFF;

   b = TitanGetBlue(top) + TitanGetBlue(bottom);
   if (b > 0xFF) b = 0xFF;

   return TitanCreatePixel(0x3F, r, g, b);
#endif
}

static INLINE int FASTCALL TitanTransAlpha(u32 pixel)
{
   return TitanGetAlpha(pixel) < 0x3F;
}

static INLINE int FASTCALL TitanTransBit(u32 pixel)
{
   return pixel & 0x80000000;
}

/* tt_context.blend / tt_context.trans for a blend mode known at compile time
 * (same pairs as TitanSetBlendingMode) */
static INLINE u32 TitanBlendM(const int mode, u32 top, u32 bottom)
{
   if (mode == TITAN_BLEND_BOTTOM)
      return TitanBlendPixelsBottom(top, bottom);
   if (mode == TITAN_BLEND_ADD)
      return TitanBlendPixelsAdd(top, bottom);
   return TitanBlendPixelsTop(top, bottom);
}

static INLINE int TitanTransM(const int mode, u32 pixel)
{
   if (mode == TITAN_BLEND_BOTTOM || mode == TITAN_BLEND_ADD)
      return TitanTransBit(pixel);
   return TitanTransAlpha(pixel);
}

static INLINE u32 TitanDigPixel(const int mode, int sprite_window, struct PixelData **rows, int num_rows, int x, int pos, int y)
{
   struct PixelData *p1 = NULL;
   struct PixelData *p2 = NULL;
   int prio1 = 0;
   int prio2 = 0;
   int i;

   /* Layers/lines not drawn this frame read as empty (priority 0) and can
    * never be picked, so only the ones written this frame are visited */
   for (i = 0; i < num_rows; i++)
   {
      struct PixelData *pd = &rows[i][x];
      int p = pd->priority;
      if (p > prio1)
      {
         p2 = p1;
         prio2 = prio1;
         p1 = pd;
         prio1 = p;
      }
      else if (p > prio2)
      {
         p2 = pd;
         prio2 = p;
      }
   }

   struct PixelData *back = &tt_context.backscreen[pos];
   if (!p1) p1 = back;
   if (!p2) p2 = back;

   u32 pixel = p1->pixel;

   if (p1->linescreen)
   {
      pixel = TitanBlendM(mode, pixel, TitanLineColor(p1, pos, y));
   }

   if ((p1->shadow_type == TITAN_MSB_SHADOW) && ((pixel & 0xFFFFFF) == 0))
   {
      //transparent sprite shadow
      if (p2->shadow_enabled)
      {
         pixel = TitanBlendShadow(p2->pixel);
      }
      else
      {
         pixel = p2->pixel;
      }
   }
   else if (p1->shadow_type == TITAN_MSB_SHADOW && ((pixel & 0xFFFFFF) != 0))
   {
      if (TitanTransM(mode, pixel))
      {
         pixel = TitanBlendM(mode, pixel, p2->pixel);
      }

      //sprite self-shadowing, only if sprite window is not enabled
      if (!sprite_window)
         pixel = TitanBlendShadow(pixel);
   }
   else if (p1->shadow_type == TITAN_NORMAL_SHADOW)
   {
      if (p2->shadow_enabled)
      {
         pixel = TitanBlendShadow(p2->pixel);
      }
      else
      {
         pixel = p2->pixel;
      }
   }
   else
   {
      if (TitanTransM(mode, pixel))
      {
         pixel = TitanBlendM(mode, pixel, p2->pixel);
      }
   }

   return pixel;
}

/* public */
int TitanInit()
{
   int i;
   const int reinit = tt_context.inited;

   tt_render_regs_valid = 0;

   if (! tt_context.inited)
   {
      for(i = 0;i < 6;i++)
      {
         if ((tt_context.vdp2framebuffer[i] = (struct PixelData *)calloc(sizeof(struct PixelData), 704 * 256)) == NULL)
            return -1;
      }

      /* linescreen 0 is not initialized as it's not used... */
      for(i = 1;i < 4;i++)
      {
         if ((tt_context.linescreen[i] = (u32 *)calloc(sizeof(u32), 512)) == NULL)
            return -1;
      }

      if ((tt_context.backscreen = (struct PixelData  *)calloc(sizeof(struct PixelData), 704 * 512)) == NULL)
         return -1;

      for (i = 0; i < 5; i++)
      {
         priority_thread_context.draw_finished[i] = 1;
         priority_thread_context.need_draw[i] = 0;
      }

      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_0, VidsoftPriorityThread0, NULL);
      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_1, VidsoftPriorityThread1, NULL);
      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_2, VidsoftPriorityThread2, NULL);
      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_3, VidsoftPriorityThread3, NULL);
      YabThreadStart(YAB_THREAD_VIDSOFT_PRIORITY_4, VidsoftPriorityThread4, NULL);

      tt_context.inited = 1;
   }

   /* Freshly calloc()ed buffers are already zero; clearing them again would
    * make all 6 x 1.4 MB resident, while a normal-resolution game only ever
    * touches the first half of each. Re-inits clear the old contents. */
   for(i = 0;i < 6;i++)
   {
      if (reinit)
         memset(tt_context.vdp2framebuffer[i], 0, sizeof(struct PixelData) * 704 * 256);
      tt_context.layer_dirtied[i] = 0;
   }

   for(i = 1;i < 4;i++)
      memset(tt_context.linescreen[i], 0, sizeof(u32) * 512);

   return 0;
}

void TitanErase()
{
   int i = 0;

   int height = tt_context.vdp2height;

   if (vdp2_interlace)
      height /= 2;

   (void)height;
   // lines are cleared on first write (see TitanClaimLayerLine)
   tt_context.frame++;
   for (i = 0; i < 6; i++)
      tt_context.layer_dirtied[i] = 0;
}

int TitanDeInit()
{
   int i;

   for(i = 0;i < 6;i++)
      free(tt_context.vdp2framebuffer[i]);

   for(i = 1;i < 4;i++)
      free(tt_context.linescreen[i]);

   for (i = 0; i < 2; i++)
   {
      free(tt_context.dot_linecolor[i]);
      tt_context.dot_linecolor[i] = NULL;
   }

   return 0;
}

void TitanSetResolution(int width, int height)
{
   int i;
   tt_context.vdp2width = width;
   tt_context.vdp2height = height;
   for (i = 0; i < 6; i++)
      tt_context.layer_dirtied[i] = 1;
}

void TitanGetResolution(int * width, int * height)
{
   *width = tt_context.vdp2width;
   *height = tt_context.vdp2height;
}

void TitanSetLineColorCalc(u16 ccctl)
{
   tt_context.line_ccctl = ccctl;
}

void TitanSetBlendingMode(int blend_mode)
{
   tt_context.blend_mode = (blend_mode == TITAN_BLEND_BOTTOM || blend_mode == TITAN_BLEND_ADD) ? blend_mode : TITAN_BLEND_TOP;
   if (blend_mode == TITAN_BLEND_BOTTOM)
   {
      tt_context.blend = TitanBlendPixelsBottom;
      tt_context.trans = TitanTransBit;
   }
   else if (blend_mode == TITAN_BLEND_ADD)
   {
      tt_context.blend = TitanBlendPixelsAdd;
      tt_context.trans = TitanTransBit;
   }
   else
   {
      tt_context.blend = TitanBlendPixelsTop;
      tt_context.trans = TitanTransAlpha;
   }
}

/* The colour each back screen line was last filled with, at width
 * back_fill_width: refilling a line with the same colour is skipped. Only
 * TitanPutBackHLine writes the back screen, and most games keep the same
 * back colour frame after frame; the fill (8 bytes a pixel, 675 KB at
 * 352x240) took ~11% of the emulation thread on the Miyoo. */
static u32 back_fill_color[512];
static u8 back_fill_valid[512];
static int back_fill_width = -1;

void TitanPutBackHLine(s32 y, u32 color)
{
   struct PixelData* buffer = &tt_context.backscreen[(y * tt_context.vdp2width)];
   int i;

   if (back_fill_width != tt_context.vdp2width)
   {
      memset(back_fill_valid, 0, sizeof(back_fill_valid));
      back_fill_width = tt_context.vdp2width;
   }
   if (y >= 0 && y < 512)
   {
      if (back_fill_valid[y] && back_fill_color[y] == color)
         return;
      back_fill_valid[y] = 1;
      back_fill_color[y] = color;
   }

   for (i = 0; i < tt_context.vdp2width; i++)
      buffer[i].pixel = color;
}

void TitanPutLineHLine(int linescreen, s32 y, u32 color)
{
   if (linescreen == 0) return;

   {
      u32 * buffer = tt_context.linescreen[linescreen] + y;
      *buffer = color;
   }
}

u32 *TitanGetDotLineColors(int n)
{
   if (!tt_context.dot_linecolor[n])
      tt_context.dot_linecolor[n] = (u32 *)calloc(sizeof(u32), 704 * 256);
   return tt_context.dot_linecolor[n];
}

struct PixelData *TitanGetLayerLine(int layer, int y)
{
   return &tt_context.vdp2framebuffer[layer][y * tt_context.vdp2width];
}

int TitanClaimLayerLine(int layer, int y)
{
   tt_context.layer_dirtied[layer] = 1;
   tt_context.line_maxprio[layer][y] = 7;
   if (tt_context.line_frame[layer][y] == tt_context.frame)
      return 0;
   tt_context.line_frame[layer][y] = tt_context.frame;
   return 1;
}

void TitanSetLineMaxPriority(int layer, int y, int maxprio)
{
   tt_context.line_maxprio[layer][y] = maxprio;
}

void TitanSetLayerDirty(int layer)
{
   tt_context.layer_dirtied[layer] = 1;
}

void TitanPutPixel(int priority, s32 x, s32 y, u32 color, int linescreen, vdp2draw_struct* info)
{
   if (priority == 0) return;

   {
      int pos = (y * tt_context.vdp2width) + x;
      int layer = info->titan_which_layer;
      struct PixelData *pd = &tt_context.vdp2framebuffer[layer][pos];
      if (TitanClaimLayerLine(layer, y))
         memset(&tt_context.vdp2framebuffer[layer][y * tt_context.vdp2width], 0, sizeof(struct PixelData) * tt_context.vdp2width);
      pd->pixel = color;
      pd->meta = TitanPackMeta(priority, linescreen, info->titan_shadow_type, info->titan_shadow_enabled);
   }
}

void TitanPutHLine(int priority, s32 x, s32 y, s32 width, u32 color)
{
   if (priority == 0) return;

   {
      struct PixelData * buffer = &tt_context.vdp2framebuffer[priority][ (y * tt_context.vdp2width) + x];
      int i;

      for (i = 0; i < width; i++)
         buffer[i].pixel = color;
   }
}

/* How many pixels ahead the compositing loop prefetches each row (0 = off).
 * Device, NiGHTS: 16..128 all alike, 32 chosen. */
#ifndef TITAN_PLD
#define TITAN_PLD 32
#endif

/* One line of TitanRenderLinesM() for N rows (N constant per copy): find the
 * top pixel, stopping once no remaining row can beat it, with the next row's
 * priority loaded before the current one is compared. On the Miyoo's
 * in-order Cortex-A7 a compare right after its load stalled for the load;
 * now the load overlaps the previous compare. sorted[N] is an empty row, so
 * reading one row ahead is always valid. */
static INLINE __attribute__((always_inline)) void TitanLineM(const int mode, const int N, pixel_t *disp_row,
   struct PixelData *const *sorted_in, const int *sorted_tie_in, const int *sorted_max_in, struct PixelData **rows, int num_rows,
   int layer_pos, int y, int sprite_window, int width)
{
   struct PixelData *sorted[7];
   int tie[6], smax[6];
   int i, x;

   for (i = 0; i < N; i++)
   {
      sorted[i] = sorted_in[i];
      tie[i] = sorted_tie_in[i];
      smax[i] = sorted_max_in[i];
   }
   sorted[N] = titan_empty_line;

   for (x = 0; x < width; x++, layer_pos++)
   {
      struct PixelData *top = NULL;
      int topi = 0;
      u32 dot;

#if TITAN_PLD
      /* Once per cache line (8 pixels) of each row: the rows are written by
       * other threads a frame's work earlier and are out of the cache, and
       * the in-order Cortex-A7 stalled on every line it first read. */
      if ((x & 7) == 0)
         for (i = 0; i < N; i++)
            __builtin_prefetch(&sorted[i][x + TITAN_PLD]);
#endif

      if (N > 0)
      {
         int best = 0;
         int pn = sorted[0][x].priority;
         for (i = 0; i < N && best < smax[i]; i++)
         {
            const int p = pn;
            pn = sorted[i + 1][x].priority;
            if (p)
            {
               const int key = p * 8 + tie[i];
               if (key > best)
               {
                  best = key;
                  top = &sorted[i][x];
                  topi = i;
               }
            }
         }
      }

      /* If the top pixel is opaque and has no shadow, nothing below it
       * matters. Otherwise TitanDigPixel does the full job. */
      if (top && !top->shadow_type)
      {
         u32 pixel = top->pixel;
         if (top->linescreen)
            pixel = TitanBlendM(mode, pixel, TitanLineColor(top, layer_pos, y));
         if (!TitanTransM(mode, pixel))
         {
            disp_row[x] = pixel ? TitanFixAlpha(pixel) : 0;
            continue;
         }

         /* The pixel is blended with the one below it: the best of the other
          * rows (what TitanDigPixel would pick as p2), found the same way as
          * the top one, stopping once no remaining row can beat it, else the
          * back screen. */
         {
            struct PixelData *p2 = NULL;
            int best2 = 0;
            for (i = 0; i < N && best2 < smax[i]; i++)
            {
               const int p = sorted[i][x].priority;
               if (p && i != topi)
               {
                  const int key = p * 8 + tie[i];
                  if (key > best2)
                  {
                     best2 = key;
                     p2 = &sorted[i][x];
                  }
               }
            }
            if (!p2) p2 = &tt_context.backscreen[layer_pos];
            pixel = TitanBlendM(mode, pixel, p2->pixel);
            disp_row[x] = pixel ? TitanFixAlpha(pixel) : 0;
            continue;
         }
      }

      dot = TitanDigPixel(mode, sprite_window, rows, num_rows, x, layer_pos, y);
      disp_row[x] = dot ? TitanFixAlpha(dot) : 0;
   }
}

static INLINE void TitanRenderLinesM(const int mode, pixel_t * dispbuffer, int start_line, int end_line)
{
   int y, layer_y;
   int line_increment, interlace_line;
   const int sprite_window = (Vdp2Regs->SPCTL & 0x10) != 0;

   Vdp2GetInterlaceInfo(&interlace_line, &line_increment);

   set_layer_y(start_line, &layer_y);
   
   for (y = start_line + interlace_line; y < end_line; y += line_increment)
   {
      pixel_t *disp_row = dispbuffer + (y * tt_context.vdp2width);
      int layer_pos = layer_y * tt_context.vdp2width;
      struct PixelData *rows[6];
      /* The same rows ordered by the highest key any of their pixels can
       * have, where key = priority * 8 + tie and tie prefers earlier rows,
       * exactly as TitanDigPixel picks the top pixel */
      struct PixelData *sorted[6];
      int sorted_tie[6], sorted_max[6];
      int i, j, num_rows = 0;

      for (i = 0; i < tt_context.num_active_layers; i++)
      {
         int layer = tt_context.active_layers[i];
         if (tt_context.line_frame[layer][layer_y] == tt_context.frame)
         {
            int tie = 7 - num_rows;
            int max = tt_context.line_maxprio[layer][layer_y] * 8 + tie;
            for (j = num_rows; j > 0 && sorted_max[j - 1] < max; j--)
            {
               sorted[j] = sorted[j - 1];
               sorted_tie[j] = sorted_tie[j - 1];
               sorted_max[j] = sorted_max[j - 1];
            }
            sorted[j] = &tt_context.vdp2framebuffer[layer][layer_y * tt_context.vdp2width];
            sorted_tie[j] = tie;
            sorted_max[j] = max;
            rows[num_rows++] = &tt_context.vdp2framebuffer[layer][layer_pos];
         }
      }

      switch (num_rows)
      {
#define LINE_N(N) case N: TitanLineM(mode, N, disp_row, sorted, sorted_tie, sorted_max, rows, num_rows, layer_pos, y, sprite_window, tt_context.vdp2width); break;
      LINE_N(0) LINE_N(1) LINE_N(2) LINE_N(3) LINE_N(4) LINE_N(5) LINE_N(6)
#undef LINE_N
      }

      layer_y++;
   }
}

void TitanRenderLines(pixel_t * dispbuffer, int start_line, int end_line)
{
   if (!tt_context.inited || (!tt_context.trans))
   {
      return;
   }

   switch (tt_context.blend_mode)
   {
   case TITAN_BLEND_BOTTOM: TitanRenderLinesM(TITAN_BLEND_BOTTOM, dispbuffer, start_line, end_line); break;
   case TITAN_BLEND_ADD:    TitanRenderLinesM(TITAN_BLEND_ADD, dispbuffer, start_line, end_line); break;
   default:                 TitanRenderLinesM(TITAN_BLEND_TOP, dispbuffer, start_line, end_line); break;
   }
}

//num + 1 needs to be an even number to avoid issues with interlace modes
void VIDSoftSetNumPriorityThreads(int num)
{
   vidsoft_num_priority_threads = num > 5 ? 5 : num;

   if (num == 2)
      vidsoft_num_priority_threads = 1;

   if (num == 4)
      vidsoft_num_priority_threads = 3;
}

void TitanStartPriorityThread(int which)
{
   priority_thread_context.draw_finished[which] = 0;
   YabFlagSet(priority_thread_context.need_draw[which], 1);
   YabThreadWake(YAB_THREAD_VIDSOFT_PRIORITY_0 + which);
}

void TitanWaitForPriorityThread(int which)
{
   YabFlagWait(&priority_thread_context.draw_finished[which]);
}

void TitanRenderThreads(pixel_t * dispbuffer, int can_use_simplified)
{
   int i;
   int total_jobs = vidsoft_num_priority_threads + 1;//main thread runs a job
   int num_lines_per_job = tt_context.vdp2height / total_jobs;
   int remainder = tt_context.vdp2height % total_jobs;
   int starts[6] = { 0 }; 
   int ends[6] = { 0 };

   priority_thread_context.dispbuffer = dispbuffer;
   priority_thread_context.use_simplified = can_use_simplified;

   for (i = 0; i < total_jobs; i++)
   {
      starts[i] = num_lines_per_job * i;
      ends[i] = ((i + 1) * num_lines_per_job);
   }

   for (i = 0; i < vidsoft_num_priority_threads; i++)
   {
      priority_thread_context.lines[i].start = starts[i+1];
      priority_thread_context.lines[i].end = ends[i+1];
   }

   //put any remaining lines on the last thread
   priority_thread_context.lines[vidsoft_num_priority_threads - 1].end += remainder;

   for (i = 0; i < vidsoft_num_priority_threads; i++)
   {
      TitanStartPriorityThread(i);
   }

   TitanRenderSimplifiedCheck(dispbuffer, starts[0], ends[0], can_use_simplified);

   for (i = 0; i < vidsoft_num_priority_threads; i++)
   {
      TitanWaitForPriorityThread(i);
   }
}

/* What TitanRender and the line renderers read from the live registers (at
   VBlank-IN), as they were at its last run: the exact skip (vdp2.c) asks
   whether compositing the kept layers again would read the same. Anything
   they come to read from Vdp2Regs must be added here, as it is read. */
static void TitanReadRenderRegs(u16 *r)
{
   r[0] = Vdp2Regs->SPCTL & 0x10;
   r[1] = ((Vdp2Regs->CCCTL | tt_context.line_ccctl) & 0x807f) != 0;
   r[2] = (Vdp2Regs->SFPRMD & 0x3ff) != 0;
   r[3] = (Vdp2Regs->LNCLEN & 0x1f) != 0;
   r[4] = (Vdp2Regs->SDCTL & 0x13F) != 0;
   r[5] = Vdp2Regs->PRINA & 0x707;
   r[6] = Vdp2Regs->PRINB & 0x707;
   r[7] = Vdp2Regs->PRIR & 0x7;
}

int TitanRenderInputsSame(void)
{
   u16 r[8];
   if (!tt_render_regs_valid)
      return 0;
   TitanReadRenderRegs(r);
   return !memcmp(r, tt_render_regs, sizeof(r));
}

#ifdef YAB_PROFILE
void TitanRenderBody(pixel_t * dispbuffer);
void TitanRender(pixel_t * dispbuffer) { YAB_PROF_BEGIN(PROF_TITAN); TitanRenderBody(dispbuffer); YAB_PROF_END(PROF_TITAN); }
#define TitanRender TitanRenderBody
#endif
void TitanRender(pixel_t * dispbuffer)
#ifdef YAB_PROFILE
#undef TitanRender
#endif
{
   int can_use_simplified_rendering = 1;
   int i;

   if (!tt_context.inited || (!tt_context.trans))
   {
      return;
   }
   TitanReadRenderRegs(tt_render_regs);
   tt_render_regs_valid = 1;

   //using color calculation (on any line, when it changes inside the display)
   if (((Vdp2Regs->CCCTL | tt_context.line_ccctl) & 0x807f) != 0)
      can_use_simplified_rendering = 0;

   //using special priority
   if ((Vdp2Regs->SFPRMD & 0x3ff) != 0)
      can_use_simplified_rendering = 0;

   //using line screen
   if ((Vdp2Regs->LNCLEN & 0x1f) != 0)
      can_use_simplified_rendering = 0;

   //using shadow
   if ((Vdp2Regs->SDCTL & 0x13F) != 0)
      can_use_simplified_rendering = 0;

   tt_context.layer_priority[TITAN_NBG0] = Vdp2Regs->PRINA & 0x7;
   tt_context.layer_priority[TITAN_NBG1] = ((Vdp2Regs->PRINA >> 8) & 0x7);
   tt_context.layer_priority[TITAN_NBG2] = (Vdp2Regs->PRINB & 0x7);
   tt_context.layer_priority[TITAN_NBG3] = ((Vdp2Regs->PRINB >> 8) & 0x7);
   tt_context.layer_priority[TITAN_RBG0] = (Vdp2Regs->PRIR & 0x7);

   tt_context.num_active_layers = 0;
   for (i = TITAN_SPRITE; i >= 0; i--)
      if (tt_context.layer_dirtied[i])
         tt_context.active_layers[tt_context.num_active_layers++] = i;

   if (vidsoft_num_priority_threads > 0)
   {
      TitanRenderThreads(dispbuffer, can_use_simplified_rendering);
   }
   else
   {
      TitanRenderSimplifiedCheck(dispbuffer, 0, tt_context.vdp2height, can_use_simplified_rendering);
   }
}

#ifdef WORDS_BIGENDIAN
void TitanWriteColor(pixel_t * dispbuffer, s32 bufwidth, s32 x, s32 y, u32 color)
{
   int pos = (y * bufwidth) + x;
   pixel_t * buffer = dispbuffer + pos;
   *buffer = ((color >> 24) & 0xFF) | ((color >> 8) & 0xFF00) | ((color & 0xFF00) << 8) | ((color & 0xFF) << 24);
}
#else
void TitanWriteColor(pixel_t * dispbuffer, s32 bufwidth, s32 x, s32 y, u32 color)
{
   int pos = (y * bufwidth) + x;
   pixel_t * buffer = dispbuffer + pos;
   *buffer = color;
}
#endif
