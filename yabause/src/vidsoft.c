/*  Copyright 2003-2004 Guillaume Duhamel
    Copyright 2004-2008 Theo Berkau
    Copyright 2006 Fabien Coulon
    Copyright 2015 R. Danbrook

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

/*! \file vidsoft.c
    \brief Software video renderer interface.
*/

#include "vidsoft.h"
#include "yabprof.h"
#include "ygl.h"
#include "vidshared.h"
#include "debug.h"
#include "vdp2.h"
#include "titan/titan.h"

#ifdef HAVE_LIBGL
#define USE_OPENGL
#endif

#ifdef USE_OPENGL
#include "ygl.h"
#endif

#include "yui.h"
#include "threads.h"
#include <limits.h>
#include <time.h>

#include <stdlib.h>
#include <limits.h>

#if defined WORDS_BIGENDIAN
static INLINE u32 COLSAT2YAB16(int priority,u32 temp)            { return (priority | (temp & 0x7C00) << 1 | (temp & 0x3E0) << 14 | (temp & 0x1F) << 27); }
static INLINE u32 COLSAT2YAB32(int priority,u32 temp)            { return (((temp & 0xFF) << 24) | ((temp & 0xFF00) << 8) | ((temp & 0xFF0000) >> 8) | priority); }
static INLINE u32 COLSAT2YAB32_2(int priority,u32 temp1,u32 temp2)   { return (((temp2 & 0xFF) << 24) | ((temp2 & 0xFF00) << 8) | ((temp1 & 0xFF) << 8) | priority); }
static INLINE u32 COLSATSTRIPPRIORITY(u32 pixel)              { return (pixel | 0xFF); }
#else
static INLINE u32 COLSAT2YAB16(int priority,u32 temp) { return (priority << 24 | (temp & 0x1F) << 3 | (temp & 0x3E0) << 6 | (temp & 0x7C00) << 9); }
static INLINE u32 COLSAT2YAB32(int priority, u32 temp) { return (priority << 24 | (temp & 0xFF0000) | (temp & 0xFF00) | (temp & 0xFF)); }
static INLINE u32 COLSAT2YAB32_2(int priority,u32 temp1,u32 temp2)   { return (priority << 24 | ((temp1 & 0xFF) << 16) | (temp2 & 0xFF00) | (temp2 & 0xFF)); }
static INLINE u32 COLSATSTRIPPRIORITY(u32 pixel) { return (0xFF000000 | pixel); }
#endif

#define COLOR_ADDt(b)		(b>0xFF?0xFF:(b<0?0:b))
#define COLOR_ADDb(b1,b2)	COLOR_ADDt((signed) (b1) + (b2))
#ifdef WORDS_BIGENDIAN
#define COLOR_ADD(l,r,g,b)      (l & 0xFF) | \
                                (COLOR_ADDb((l >> 8) & 0xFF, b) << 8) | \
                                (COLOR_ADDb((l >> 16) & 0xFF, g) << 16) | \
                                (COLOR_ADDb((l >> 24), r) << 24)
#else
#define COLOR_ADD(l,r,g,b)	COLOR_ADDb((l & 0xFF), r) | \
                                (COLOR_ADDb((l >> 8) & 0xFF, g) << 8) | \
                                (COLOR_ADDb((l >> 16) & 0xFF, b) << 16) | \
				(l & 0xFF000000)
#endif


int VIDSoftInit(void);
int VIDSoftSetupGL(void);
void VIDSoftDeInit(void);
void VIDSoftResize(unsigned int, unsigned int, int);
int VIDSoftIsFullscreen(void);
int VIDSoftVdp1Reset(void);
void VIDSoftVdp1DrawStart(void);
void VIDSoftVdp1DrawEnd(void);
void VIDSoftVdp1NormalSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1ScaledSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1DistortedSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1PolygonDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1PolylineDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1LineDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
void VIDSoftVdp1UserClipping(u8 * ram, Vdp1 * regs);
void VIDSoftVdp1SystemClipping(u8 * ram, Vdp1 * regs);
void VIDSoftVdp1LocalCoordinate(u8 * ram, Vdp1 * regs);
void VIDSoftVdp1ReadFrameBuffer(u32 type, u32 addr, void * out);
void VIDSoftVdp1WriteFrameBuffer(u32 type, u32 addr, u32 val);
int VIDSoftVdp2Reset(void);
void VIDSoftVdp2DrawStart(void);
void VIDSoftVdp2DrawEnd(void);
void VIDSoftVdp2DrawScreens(void);
void VIDSoftVdp2SetResolution(u16 TVMD);
void VIDSoftGetGlSize(int *width, int *height);
void VIDSoftVdp1SwapFrameBuffer(void);
void VIDSoftVdp1EraseFrameBuffer(Vdp1* regs, u8 * back_framebuffer);
void VidsoftDrawSprite(Vdp2 * vdp2_regs, u8 * sprite_window_mask, u8* vdp1_front_framebuffer, u8 * vdp2_ram, Vdp1* vdp1_regs, Vdp2* vdp2_lines, u8*color_ram);
void VIDSoftGetNativeResolution(int *width, int *height, int*interlace);
void VIDSoftVdp2DispOff(void);

VideoInterface_struct VIDSoft = {
VIDCORE_SOFT,
"Software Video Interface",
VIDSoftInit,
VIDSoftDeInit,
VIDSoftResize,
VIDSoftIsFullscreen,
VIDSoftVdp1Reset,
VIDSoftVdp1DrawStart,
VIDSoftVdp1DrawEnd,
VIDSoftVdp1NormalSpriteDraw,
VIDSoftVdp1ScaledSpriteDraw,
VIDSoftVdp1DistortedSpriteDraw,
//for the actual hardware, polygons are essentially identical to distorted sprites
//the actual hardware draws using diagonal lines, which is why using half-transparent processing
//on distorted sprites and polygons is not recommended since the hardware overdraws to prevent gaps
//thus, with half-transparent processing some pixels will be processed more than once, producing moire patterns in the drawn shapes
VIDSoftVdp1DistortedSpriteDraw,
VIDSoftVdp1PolylineDraw,
VIDSoftVdp1LineDraw,
VIDSoftVdp1UserClipping,
VIDSoftVdp1SystemClipping,
VIDSoftVdp1LocalCoordinate,
VIDSoftVdp1ReadFrameBuffer,
VIDSoftVdp1WriteFrameBuffer,
VIDSoftVdp2Reset,
VIDSoftVdp2DrawStart,
VIDSoftVdp2DrawEnd,
VIDSoftVdp2DrawScreens,
VIDSoftGetGlSize,
VIDSoftGetNativeResolution,
VIDSoftVdp2DispOff
};

pixel_t *dispbuffer=NULL;
u8 *vdp1framebuffer[2]= { NULL, NULL };
u8 *vdp1frontframebuffer;
u8 *vdp1backframebuffer;
/* Exact skip (vdp2.c, where both are defined: this file sits at GCC's inlining
   limit). Bumped when the shown sprite picture may change: a frame change or
   a new framebuffer mode (how the buffer is read); a deferred draw into the
   shown buffer counts in vdp1_st_run. */
extern u32 vidsoft_vdp1_front_gen;
/* the field whose picture is on screen: drawn, or skipped as unchanged */
extern u32 vdp2_picture_frame;
u8 sprite_window_mask[704 * 512];

static int vdp1width;
static int vdp1height;
static int vdp1interlace;
static int vdp1pixelsize;
int vdp2width;
int rbg0width = 0;
int vdp2height;

#ifdef USE_OPENGL
static int outputwidth;
static int outputheight;
GLuint vao = 0;
GLuint vbo = 0;
GLuint vshader = 0;
GLuint fshader = 0;
GLuint gl_shader_prog = 0;
GLuint gl_texture_id = 0;
#endif
extern int VideoUseGL;
int vdp2_x_hires = 0;
int vdp2_interlace = 0;
static int rbg0height = 0;
int bilinear = 0;
int vidsoft_num_layer_threads = 0;

/* Bring a render thread's copy of VDP1 or VDP2 RAM up to date: only the
 * 4 KB pages written since the last copy (all VRAM writes go through the
 * Vdp1Ram/Vdp2Ram write handlers, which mark them; init and state loads
 * mark everything). Games change a few pages a frame, so this replaces two
 * 512 KB copies per drawn frame on the emulation thread. YAB_SNAPSHOT_VERIFY=1
 * compares the whole copy afterwards and reports pages that differ. */
static int snapshot_verify = -1;
static unsigned long snapshot_mismatches;

static void VidsoftSyncRamCopy(u8 *dst, const u8 *src, u32 *dirty, const char *what)
{
   int w;
   for (w = 0; w < 4; w++)
   {
      u32 bits = dirty[w];
      dirty[w] = 0;
      while (bits)
      {
         int first = __builtin_ctz(bits), n = 0;
         while (first + n < 32 && (bits >> (first + n)) & 1)
            n++;
         memcpy(dst + ((w * 32 + first) << 12), src + ((w * 32 + first) << 12), (size_t)n << 12);
         bits &= ~(n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1) << first);
      }
   }
   if (snapshot_verify < 0)
      snapshot_verify = getenv("YAB_SNAPSHOT_VERIFY") && getenv("YAB_SNAPSHOT_VERIFY")[0] == '1';
   if (snapshot_verify)
   {
      int page, bad = 0, firstbad = -1;
      for (page = 0; page < 128; page++)
         if (memcmp(dst + (page << 12), src + (page << 12), 4096))
         {
            if (firstbad < 0) firstbad = page;
            bad++;
         }
      if (bad)
      {
         snapshot_mismatches += bad;
         fprintf(stderr, "snapshot verify: %s copy stale in %d pages (first %d), %lu so far\n", what, bad, firstbad, snapshot_mismatches);
         memcpy(dst, src, 0x80000);
      }
   }
}
/* Rotation screens whose coefficient table holds line colour data and is
   read per dot (Panzer Dragoon's sea haze): take the line colour per pixel,
   as the hardware does, instead of once per line. Core option. */
int vidsoft_rotation_dot_linecolor = 1;
int bad_cycle_setting[6] = { 0 };

/* VDP1 drawing state. Two threads can draw the same command list into
 * different framebuffer rows (see VIDSoftVdp1DrawStart), so all of it is
 * per thread. The default TLS model for a shared library calls
 * __tls_get_addr() on every access, which the per-pixel code can't afford:
 * the few scalars use the initial-exec model (a read of the thread pointer
 * register; they fit easily in the static TLS space glibc keeps for dlopen),
 * and the arrays are plain statics with one copy per drawing thread,
 * picked by vdp1_tid. */
#if defined(__GNUC__) || defined(__clang__)
#define VDP1_TLS __thread __attribute__((tls_model("initial-exec")))
#else
#define VDP1_TLS
#endif

/* framebuffer rows this thread draws (all when not split) */
static VDP1_TLS int vdp1_row_lo = INT_MIN, vdp1_row_hi = INT_MAX;
/* which copy of the per-thread arrays this thread uses: 1 for the second
 * VDP1 thread, 0 everywhere else */
static VDP1_TLS int vdp1_tid;

#ifdef YAB_VDP1LOG
/* Renderer log (vdp1.h). Every framebuffer write goes through VDP1W(), which counts it for the
 * entry being drawn; VDP1RLOG_WHY names the rule when a shape is dropped whole, VDP1RLOG_L999
 * counts lines dropped by the 999-pixel guard. YAB_VDP1LOG_CHG=1 also counts the pixels each
 * entry changed (framebuffer compare per entry, slow): changed > written means a write that
 * doesn't go through VDP1W(). */
#include <pthread.h>
#include <stdarg.h>
int vdp1log_on = -1, vdp1log_frame;
static FILE *vdp1log_fp;
static int vdp1log_chg, vdp1log_lo, vdp1log_hi = INT_MAX;
static VDP1_TLS int vdp1log_act;
static pthread_mutex_t vdp1log_mx = PTHREAD_MUTEX_INITIALIZER;
static int vdp1log_seq, vdp1log_start_frame, vdp1log_copydiff = -1, vdp1log_copyram;
static VDP1_TLS int vdp1log_px, vdp1log_l999;
static VDP1_TLS const char *vdp1log_why;
static VDP1_TLS u8 *vdp1log_fbp;
static VDP1_TLS char *vdp1log_buf;
static VDP1_TLS size_t vdp1log_len, vdp1log_cap;
static u8 vdp1log_prev[2][0x40000];
#define VDP1W(l) (*(vdp1log_px++, &(l)))
#define VDP1RLOG_WHY(s) (vdp1log_why = (s))
#define VDP1RLOG_L999() (vdp1log_l999++)

static int Vdp1LogOpen(void)
{
   if (vdp1log_on < 0)
   {
      const char *f = getenv("YAB_VDP1LOG");
      vdp1log_fp = f && *f ? fopen(f, "w") : NULL;
      vdp1log_on = vdp1log_fp != NULL;
      vdp1log_chg = getenv("YAB_VDP1LOG_CHG") && getenv("YAB_VDP1LOG_CHG")[0] == '1';
      if (getenv("YAB_VDP1LOG_FRAMES"))   /* first-last harness frame */
         sscanf(getenv("YAB_VDP1LOG_FRAMES"), "%d-%d", &vdp1log_lo, &vdp1log_hi);
   }
   return vdp1log_on;
}

static void Vdp1LogPrintf(const char *fmt, ...)
{
   va_list ap;
   int n;
   if (vdp1log_cap - vdp1log_len < 512)
   {
      vdp1log_cap = vdp1log_cap ? vdp1log_cap * 2 : 1 << 16;
      vdp1log_buf = (char *)realloc(vdp1log_buf, vdp1log_cap);
   }
   va_start(ap, fmt);
   n = vsnprintf(vdp1log_buf + vdp1log_len, vdp1log_cap - vdp1log_len, fmt, ap);
   va_end(ap);
   if (n > 0)
      vdp1log_len += n;
}

/* main thread, before a draw starts: its number and frame */
static void Vdp1LogStart(const u8 *ram)
{
   vdp1log_seq++;
   vdp1log_start_frame = vdp1log_frame;
   vdp1log_copyram = ram == Vdp1Ram;
}

/* main thread, after the render copy was brought up to date: 4 KB pages that still differ */
static void Vdp1LogCopyCheck(const u8 *copy)
{
   int p, n = 0;
   if (!Vdp1LogOpen())
      return;
   for (p = 0; p < 0x80000; p += 0x1000)
      n += memcmp(copy + p, Vdp1Ram + p, 0x1000) != 0;
   vdp1log_copydiff = n;
}

static void Vdp1LogRows(int *lo, int *hi, int *unit)
{
   int rowbytes = vdp1width * (vdp1pixelsize ? vdp1pixelsize : 2);
   int rows = rowbytes > 0 ? 0x40000 / rowbytes : 0;
   *lo = vdp1_row_lo < 0 ? 0 : vdp1_row_lo;
   *hi = vdp1_row_hi > rows ? rows : vdp1_row_hi;
   *unit = rowbytes;
}

void Vdp1LogDrawBegin(u8 *ram, Vdp1 *regs, u8 *back_framebuffer)
{
   vdp1log_act = Vdp1LogOpen() && vdp1log_start_frame >= vdp1log_lo && vdp1log_start_frame <= vdp1log_hi;
   if (!vdp1log_act)
      return;
   vdp1log_len = 0;
   vdp1log_fbp = back_framebuffer;
   Vdp1LogPrintf("D %d %d t%d rows %d %d pix%d w%d h%d il%d copydiff %d%s sys %d,%d user %d,%d-%d,%d local %d,%d\n",
      vdp1log_start_frame, vdp1log_seq, vdp1_tid, vdp1_row_lo == INT_MIN ? -1 : vdp1_row_lo,
      vdp1_row_hi == INT_MAX ? -1 : vdp1_row_hi, vdp1pixelsize, vdp1width, vdp1height, vdp1interlace,
      vdp1log_copydiff, vdp1log_copyram ? " live" : "", regs->systemclipX2, regs->systemclipY2,
      regs->userclipX1, regs->userclipY1, regs->userclipX2, regs->userclipY2, regs->localX, regs->localY);
}

void Vdp1LogEntryBegin(void)
{
   if (!vdp1log_act)
      return;
   vdp1log_px = 0;
   vdp1log_l999 = 0;
   vdp1log_why = NULL;
   if (vdp1log_chg && vdp1log_fbp)
   {
      int lo, hi, unit;
      Vdp1LogRows(&lo, &hi, &unit);
      if (hi > lo)
         memcpy(vdp1log_prev[vdp1_tid] + lo * unit, vdp1log_fbp + lo * unit, (hi - lo) * unit);
   }
}

void Vdp1LogEntryEnd(u8 *ram, u32 addr, u32 idx)
{
   int i, chg = -1;
   if (!vdp1log_act)
      return;
   if (vdp1log_chg && vdp1log_fbp)
   {
      int lo, hi, unit, k;
      Vdp1LogRows(&lo, &hi, &unit);
      chg = 0;
      if (vdp1pixelsize == 2)
      {
         const u16 *a = (const u16 *)(vdp1log_prev[vdp1_tid] + lo * unit), *b = (const u16 *)(vdp1log_fbp + lo * unit);
         for (k = 0; k < (hi - lo) * unit / 2; k++)
            chg += a[k] != b[k];
      }
      else
      {
         const u8 *a = vdp1log_prev[vdp1_tid] + lo * unit, *b = vdp1log_fbp + lo * unit;
         for (k = 0; k < (hi - lo) * unit; k++)
            chg += a[k] != b[k];
      }
   }
   Vdp1LogPrintf("E %u %05x", idx, addr);
   for (i = 0; i < 32; i += 2)
      Vdp1LogPrintf(" %04x", T1ReadWord(ram, (addr + i) & 0x7FFFF));
   Vdp1LogPrintf(" px %d why %s l999 %d chg %d\n", vdp1log_px, vdp1log_why ? vdp1log_why : "-", vdp1log_l999, chg);
}

void Vdp1LogDrawEnd(const char *end, u32 visits)
{
   if (!vdp1log_act)
      return;
   Vdp1LogPrintf("X %s %u\n", end, visits);
   pthread_mutex_lock(&vdp1log_mx);
   fwrite(vdp1log_buf, 1, vdp1log_len, vdp1log_fp);
   fflush(vdp1log_fp);
   pthread_mutex_unlock(&vdp1log_mx);
   vdp1log_len = 0;
}

void Vdp1LogNoDraw(const char *why)
{
   if (!Vdp1LogOpen() || vdp1log_frame < vdp1log_lo || vdp1log_frame > vdp1log_hi)
      return;
   pthread_mutex_lock(&vdp1log_mx);
   fprintf(vdp1log_fp, "N %d %s\n", vdp1log_frame, why);
   pthread_mutex_unlock(&vdp1log_mx);
}
#else
#define VDP1W(l) l
#define VDP1RLOG_WHY(s) ((void)0)
#define VDP1RLOG_L999() ((void)0)
#endif
/* framebuffer row of VDP1 y coordinate y, as putpixel() computes it */
#define VDP1_ROW(y) (vdp1interlace == 2 ? (y) / 2 : (y))

struct VidsoftVdp1ThreadContext
{
   volatile int draw_finished;
   volatile int need_draw;
   Vdp1 regs;
   u8 ram[0x80000];
   // What the threads draw from: ram for a draw started at once, a
   // deferred job's own snapshot otherwise (see Vdp1Job)
   u8 *ramp;
   // Drawn in place: every other access to the back framebuffer (CPU
   // reads/writes, swaps) waits for the thread first
   u8 *back_framebuffer;
   // rows this thread draws, and its CPU time for the last frame (ns)
   int row_lo, row_hi;
   u64 cpu_ns;
}vidsoft_vdp1_thread_context;

/* The second VDP1 thread draws the same commands, from the first thread's
 * snapshot of VDP1 RAM, into the other rows of the framebuffer */
struct VidsoftVdp1SplitContext
{
   volatile int draw_finished;
   volatile int need_draw;
   Vdp1 regs;
   int row_lo, row_hi;
   u64 cpu_ns;
}vidsoft_vdp1b_context;

int vidsoft_vdp1_thread_enabled = 0;
static int vdp1_split_enabled = -1;  // YAB_VDP1_SPLIT: 0 off, 2 every frame (for tests), else when worth it
static int vdp1_split_row = 112;     // first row of the second thread, rebalanced each frame
static int vdp1_last_split;          // was the previous frame split?
static unsigned long vdp1_frames_split, vdp1_frames_single;
static void VidsoftVdp1Erase(const Vdp1* regs, u8 * back_framebuffer);

typedef struct
{
   int pending;      /* captured, not drawn */
   int erase;        /* erases before drawing (decided at capture) */
   Vdp1 regs;
   u8 *ram;          /* snapshot of VDP1 RAM at capture */
   u32 stale[4];     /* pages of ram older than Vdp1Ram */
} Vdp1Job;
static Vdp1Job vdp1_job[2];               /* per vdp1framebuffer[] index */
static u32 vdp1_ctx_stale[4];             /* the same for vidsoft_vdp1_thread_context.ram */
static int vdp1_lazy = -1;                /* YAB_VDP1_LAZY */
static int vdp1_lazy_active;              /* this frame: skipping with exact VDP1 */
static int vdp1_lazy_defer;               /* this frame's draws are deferred */
static int vdp1_lazy_next_shown;          /* the next frame is shown */
static int vdp1_swap_gap = 1, vdp1_since_swap; /* frames between frame changes */
static unsigned long vdp1_st_eager, vdp1_st_captured, vdp1_st_replaced, vdp1_st_sync, vdp1_st_backoffs;
unsigned long vdp1_st_run;   /* also read by vdp2.c (exact skip) */
static int vdp1_lazy_shown, vdp1_lazy_misses, vdp1_lazy_backoff;

/* With less VDP1 work than this per frame a second thread costs more than
 * it saves (Princess Crown's sprites take about 3.5 ms on the Miyoo Mini) */
#define VDP1_SPLIT_MIN_NS 5000000ull

static void VidsoftWaitForLayerThreads(void);

typedef struct { s16 x; s16 y; } vdp1vertex;

typedef struct
{
   int pagepixelwh, pagepixelwh_bits, pagepixelwh_mask;
   int planepixelwidth, planepixelwidth_bits, planepixelwidth_mask;
   int planepixelheight, planepixelheight_bits, planepixelheight_mask;
   int screenwidth;
   int screenheight;
   int oldcellx, oldcelly, oldcellcheck;
   int xmask, ymask;
   u32 planetbl[16];
} screeninfo_struct;

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 FASTCALL Vdp2ColorRamGetColorCM(int colormode, u32 addr, u8* vdp2_color_ram)
{
   switch(colormode)
   {
      case 0:
      {
         u32 tmp;
         addr <<= 1;
         tmp = T2ReadWord(vdp2_color_ram, addr & 0xFFF);
         /* we preserve MSB for special color calculation mode 3 (see Vdp2 user's manual 3.4 and 12.3) */
         return (((tmp & 0x1F) << 3) | ((tmp & 0x03E0) << 6) | ((tmp & 0x7C00) << 9)) | ((tmp & 0x8000) << 16);
      }
      case 1:
      {
         u32 tmp;
         addr <<= 1;
         tmp = T2ReadWord(vdp2_color_ram, addr & 0xFFF);
         /* we preserve MSB for special color calculation mode 3 (see Vdp2 user's manual 3.4 and 12.3) */
         return (((tmp & 0x1F) << 3) | ((tmp & 0x03E0) << 6) | ((tmp & 0x7C00) << 9)) | ((tmp & 0x8000) << 16);
      }
      case 2:
      {
         addr <<= 2;   
         return T2ReadLong(vdp2_color_ram, addr & 0xFFF);
      }
      default: break;
   }

   return 0;
}

static INLINE u32 FASTCALL Vdp2ColorRamGetColor(u32 addr, u8* vdp2_color_ram)
{
   return Vdp2ColorRamGetColorCM(Vdp2Internal.ColorMode, addr, vdp2_color_ram);
}

/* Vdp2ColorRamGetColorCM() for every address, built once per drawn frame
 * from the colour RAM the layers draw from (the threads' snapshot, or the
 * live one when there are no layer threads). The colour depends
 * only on addr & 0x7FF in modes 0/1 and addr & 0x3FF in mode 2, so
 * vidsoft_pal[addr & vidsoft_pal_mask] is the same colour, without decoding
 * RGB555 per pixel. Used only while the colour mode and colour RAM are the
 * ones it was built from. */
static u32 vidsoft_pal[2048];
static u32 vidsoft_pal_mask;
static int vidsoft_pal_mode = -1;
static const u8 *vidsoft_pal_src;

static void VidsoftBuildPalette(u8 *color_ram)
{
   const int mode = Vdp2Internal.ColorMode;
   const int n = mode == 2 ? 1024 : 2048;
   int i;

   vidsoft_pal_mode = -1;
   if (mode > 2)
      return;
   for (i = 0; i < n; i++)
      vidsoft_pal[i] = Vdp2ColorRamGetColorCM(mode, i, color_ram);
   vidsoft_pal_mask = n - 1;
   vidsoft_pal_src = color_ram;
   vidsoft_pal_mode = mode;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void Vdp2PatternAddr(vdp2draw_struct *info, Vdp2* regs, u8* ram)
{
   switch(info->patterndatasize)
   {
      case 1:
      {
         u16 tmp = T1ReadWord(ram, info->addr);         

         info->addr += 2;
         info->specialfunction = (info->supplementdata >> 9) & 0x1;
         info->specialcolorfunction = (info->supplementdata >> 8) & 0x1;

         switch(info->colornumber)
         {
            case 0: // in 16 colors
               info->paladdr = ((tmp & 0xF000) >> 8) | ((info->supplementdata & 0xE0) << 3);
               break;
            default: // not in 16 colors
               info->paladdr = (tmp & 0x7000) >> 4;
               break;
         }

         switch(info->auxmode)
         {
            case 0:
               info->flipfunction = (tmp & 0xC00) >> 10;

               switch(info->patternwh)
               {
                  case 1:
                     info->charaddr = (tmp & 0x3FF) | ((info->supplementdata & 0x1F) << 10);
                     break;
                  case 2:
                     info->charaddr = ((tmp & 0x3FF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x1C) << 10);
                     break;
               }
               break;
            case 1:
               info->flipfunction = 0;

               switch(info->patternwh)
               {
                  case 1:
                     info->charaddr = (tmp & 0xFFF) | ((info->supplementdata & 0x1C) << 10);
                     break;
                  case 2:
                     info->charaddr = ((tmp & 0xFFF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x10) << 10);
                     break;
               }
               break;
         }

         break;
      }
      case 2: {
         u16 tmp1 = T1ReadWord(ram, info->addr);
         u16 tmp2 = T1ReadWord(ram, info->addr+2);
         info->addr += 4;
         info->charaddr = tmp2 & 0x7FFF;
         info->flipfunction = (tmp1 & 0xC000) >> 14;
         switch(info->colornumber) {
            case 0:
               info->paladdr = (tmp1 & 0x7F) << 4;
               break;
            default:
               info->paladdr = ((tmp1 & 0x70) << 4);
               break;
         }
         info->specialfunction = (tmp1 & 0x2000) >> 13;
         info->specialcolorfunction = (tmp1 & 0x1000) >> 12;
         break;
      }
   }

   if (!(regs->VRSIZE & 0x8000))
      info->charaddr &= 0x3FFF;

   info->charaddr *= 0x20; // selon Runik
   if (info->specialprimode == 1) {
      info->priority = (info->priority & 0xE) | (info->specialfunction & 1);
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 FASTCALL DoNothing(UNUSED void *info, u32 pixel)
{
   return pixel;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 FASTCALL DoColorOffset(void *info, u32 pixel)
{
    return COLOR_ADD(pixel, ((vdp2draw_struct *)info)->cor,
                     ((vdp2draw_struct *)info)->cog,
                     ((vdp2draw_struct *)info)->cob);
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void ReadVdp2ColorOffset(Vdp2 * regs, vdp2draw_struct *info, int clofmask, int ccmask)
{
   if (regs->CLOFEN & clofmask)
   {
      // color offset enable
      if (regs->CLOFSL & clofmask)
      {
         // color offset B
         info->cor = regs->COBR & 0xFF;
         if (regs->COBR & 0x100)
            info->cor |= 0xFFFFFF00;

         info->cog = regs->COBG & 0xFF;
         if (regs->COBG & 0x100)
            info->cog |= 0xFFFFFF00;

         info->cob = regs->COBB & 0xFF;
         if (regs->COBB & 0x100)
            info->cob |= 0xFFFFFF00;
      }
      else
      {
         // color offset A
         info->cor = regs->COAR & 0xFF;
         if (regs->COAR & 0x100)
            info->cor |= 0xFFFFFF00;

         info->cog = regs->COAG & 0xFF;
         if (regs->COAG & 0x100)
            info->cog |= 0xFFFFFF00;

         info->cob = regs->COAB & 0xFF;
         if (regs->COAB & 0x100)
            info->cob |= 0xFFFFFF00;
      }

      /* an offset of 0, 0, 0 leaves every pixel as it is (games often keep
       * the offset enabled at zero, e.g. NiGHTS and Sega Rally in play) */
      info->PostPixelFetchCalc = (info->cor | info->cog | info->cob) ? &DoColorOffset : &DoNothing;
   }
   else // color offset disable
      info->PostPixelFetchCalc = &DoNothing;

}

//////////////////////////////////////////////////////////////////////////////

static INLINE int Vdp2FetchPixelCN(int colornumber, int colormode, vdp2draw_struct *info, int x, int y, u32 *color, u32 *dot, u8 * ram, int charaddr, int paladdr, u8* vdp2_color_ram)
{
   switch(colornumber)
   {
      case 0: // 4 BPP
         *dot = T1ReadByte(ram, ((charaddr + ((y * info->cellw) + x) / 2) & 0x7FFFF));
         if (!(x & 0x1)) *dot >>= 4;
         if (!(*dot & 0xF) && info->transparencyenable) return 0;
         else
         {
            *color = Vdp2ColorRamGetColorCM(colormode, info->coloroffset + (paladdr | (*dot & 0xF)),vdp2_color_ram);
            return 1;
         }
      case 1: // 8 BPP
         *dot = T1ReadByte(ram, ((charaddr + (y * info->cellw) + x) & 0x7FFFF));
         if (!(*dot & 0xFF) && info->transparencyenable) return 0;
         else
         {
            *color = Vdp2ColorRamGetColorCM(colormode, info->coloroffset + (paladdr | (*dot & 0xFF)), vdp2_color_ram);
            return 1;
         }
      case 2: // 16 BPP(palette)
         *dot = T1ReadWord(ram, ((charaddr + ((y * info->cellw) + x) * 2) & 0x7FFFF));
         if ((*dot == 0) && info->transparencyenable) return 0;
         else
         {
            *color = Vdp2ColorRamGetColorCM(colormode, info->coloroffset + *dot, vdp2_color_ram);
            return 1;
         }
      case 3: // 16 BPP(RGB)      
         *dot = T1ReadWord(ram, ((charaddr + ((y * info->cellw) + x) * 2) & 0x7FFFF));
         if (!(*dot & 0x8000) && info->transparencyenable) return 0;
         else
         {
            *color = COLSAT2YAB16(0, *dot);
            return 1;
         }
      case 4: // 32 BPP
         *dot = T1ReadLong(ram, ((charaddr + ((y * info->cellw) + x) * 4) & 0x7FFFF));
         if (!(*dot & 0x80000000) && info->transparencyenable) return 0;
         else
         {
            *color = COLSAT2YAB32(0, *dot);
            return 1;
         }
      default:
         return 0;
   }
}

static INLINE int Vdp2FetchPixel(vdp2draw_struct *info, int x, int y, u32 *color, u32 *dot, u8 * ram, int charaddr, int paladdr, u8* vdp2_color_ram)
{
   return Vdp2FetchPixelCN(info->colornumber, Vdp2Internal.ColorMode, info, x, y, color, dot, ram, charaddr, paladdr, vdp2_color_ram);
}

//////////////////////////////////////////////////////////////////////////////

static INLINE int TestWindow(int wctl, int enablemask, int inoutmask, clipping_struct *clip, int x, int y)
{
   if (wctl & enablemask) 
   {
      if (wctl & inoutmask)
      {
         // Draw inside of window
         if (x < clip->xstart || x > clip->xend ||
             y < clip->ystart || y > clip->yend)
            return 0;
      }
      else
      {
         // Draw outside of window
         if (x >= clip->xstart && x <= clip->xend &&
             y >= clip->ystart && y <= clip->yend)
            return 0;

		 //it seems to overflow vertically on hardware
		 if(clip->yend > vdp2height && (x >= clip->xstart && x <= clip->xend ))
			 return 0;
      }
      return 1; // return inactive;
   }
   return 3; // return disabled | inactive;
}

//////////////////////////////////////////////////////////////////////////////

int TestSpriteWindow(int wctl, int x, int y)
{
   int mask;
   int addr = (y*vdp2width) + x;

   if (addr >= (704 * 512))
      return 0;

   mask = sprite_window_mask[addr];

   if (wctl & 0x20)//sprite window enabled on layer
   {
      if (wctl & 0x10)//inside or outside
      {
         if (mask == 0)
            return 0;
      }
      else
      {
         if (mask)
            return 0;
      }

      return 1;
   }
   return 3;
}

//////////////////////////////////////////////////////////////////////////////

int WindowLogic(int wctl, int w0, int w1)
{
   if (((wctl & 0x80) == 0x80))
      /* AND logic, returns 0 only if both the windows are active */
      return w0 || w1;
   else
      /* OR logic, returns 0 if one of the windows is active */
      return w0 && w1;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE int TestBothWindow(int wctl, clipping_struct *clip, int x, int y)
{
    int w0, w1, spr;

    //all windows disabled
    if ((wctl & 0x2a) == 0)
    {
       if ((wctl & 0x80) == 0x80)
          return 0;
       else
          return 1;
    }

    // Only evaluate the windows that are enabled; a disabled window reports 3
    // (disabled | inactive), same as TestWindow/TestSpriteWindow would.
    w0 = (wctl & 0x2) ? TestWindow(wctl, 0x2, 0x1, &clip[0], x, y) : 3;
    w1 = (wctl & 0x8) ? TestWindow(wctl, 0x8, 0x4, &clip[1], x, y) : 3;
    spr = (wctl & 0x20) ? TestSpriteWindow(wctl, x, y) : 3;

    //if only window 0 is enabled
    if ((w1 & 2) && (spr & 2)) return w0 & 1;
    //if only window 1 is enabled
    if ((w0 & 2) && (spr & 2)) return w1 & 1;

    //window 0 and 1, sprite disabled
    if ((spr & 2))
       return WindowLogic(wctl, w0, w1);

    //if only sprite window is enabled
    if ((w1 & 2) && (w0 & 2)) return spr & 1;

    //window 0 and sprite enabled
    if ((wctl & 0x2a) == 0x22)
       return WindowLogic(wctl, w0, spr);

    //window 1 and sprite enabled
    if ((wctl & 0x2a) == 0x28)
       return WindowLogic(wctl, w1, spr);

    //all three windows enabled
    if ((wctl & 0x2a) == 0x2a)
    {
       if ((wctl & 0x80) == 0x80)
          return w0 || w1 || spr;//and logic
       else
          return w0 && w1 && spr;//or logic
    }

    return 1;

}

//////////////////////////////////////////////////////////////////////////////

/* TestBothWindow(wctl, clip, x, y) for x = 0..width-1 of line y, into out[].
 * Without the sprite window the result depends on x only through
 * comparisons with the windows' xstart/xend, so it is constant between
 * those points: evaluate it once per run. */
static void VidsoftWindowLine(int wctl, clipping_struct *clip, int y, int width, u8 *out)
{
   int bp[6], n = 0, k, j;
   bp[n++] = 0;
   if (wctl & 0x2) { bp[n++] = clip[0].xstart; bp[n++] = clip[0].xend + 1; }
   if (wctl & 0x8) { bp[n++] = clip[1].xstart; bp[n++] = clip[1].xend + 1; }
   for (k = 1; k < n; k++)          /* sort */
      for (j = k; j > 0 && bp[j - 1] > bp[j]; j--)
      {
         int t = bp[j]; bp[j] = bp[j - 1]; bp[j - 1] = t;
      }
   for (k = 0; k < n; k++)
   {
      int a = bp[k] < 0 ? 0 : bp[k];
      int b = k + 1 < n ? bp[k + 1] : width;
      if (b > width) b = width;
      if (a < b)
         memset(out + a, TestBothWindow(wctl, clip, a, y) ? 1 : 0, b - a);
   }
}

/* The same as a lookup table when it can be one: NULL when the sprite
 * window takes part (its test reads a per-pixel mask), so callers fall back
 * to TestBothWindow() per pixel */
static const u8 *VidsoftWinTable(int wctl, clipping_struct *clip, int y, int width, u8 *buf)
{
   if (wctl & 0x20)
      return NULL;
   VidsoftWindowLine(wctl, clip, y, width, buf);
   return buf;
}
/* With no window enabled the answer is fixed: tested here, so it stays free
 * per pixel even where the compiler doesn't inline TestBothWindow() (an
 * out-of-line call per pixel made Sega Rally's RBG0, which has no window,
 * 7% slower) */
#define WIN_AT(tab, wctl, clip, x, y) ((tab) ? (tab)[x] : ((wctl) & 0x2a) == 0 ? !((wctl) & 0x80) : TestBothWindow(wctl, clip, x, y))
/* Their buffers, one pair per Titan layer (a layer is drawn by one thread
 * at a time). Not on the stack: 2 KB more locals in the big drawing
 * functions are best avoided. */
static u8 vidsoft_win_buf[6][2][1024];

//////////////////////////////////////////////////////////////////////////////

static INLINE void GeneratePlaneAddrTable(vdp2draw_struct *info, u32 *planetbl, void FASTCALL (* PlaneAddr)(void *, int, Vdp2* ), Vdp2* regs)
{
   int i;

   for (i = 0; i < (info->mapwh*info->mapwh); i++)
   {
      PlaneAddr(info, i, regs);
      planetbl[i] = info->addr;
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void FASTCALL Vdp2MapCalcXY(vdp2draw_struct *info, int *x, int *y,
                                 screeninfo_struct *sinfo, Vdp2* regs, u8 * ram, int bad_cycle)
{
   int planenum;
   int flipfunction;
   const int pagesize_bits=info->pagewh_bits*2;
   const int cellwh=(2 + info->patternwh);

   const int check = ((y[0] >> cellwh) << 16) | (x[0] >> cellwh);
   //if ((x[0] >> cellwh) != sinfo->oldcellx || (y[0] >> cellwh) != sinfo->oldcelly)
   if(check != sinfo->oldcellcheck)
   {
      sinfo->oldcellx = x[0] >> cellwh;
      sinfo->oldcelly = y[0] >> cellwh;
	  sinfo->oldcellcheck = (sinfo->oldcelly << 16) | sinfo->oldcellx;

      // Calculate which plane we're dealing with
      planenum = ((y[0] >> sinfo->planepixelheight_bits) * info->mapwh) + (x[0] >> sinfo->planepixelwidth_bits);
      x[0] = (x[0] & sinfo->planepixelwidth_mask);
      y[0] = (y[0] & sinfo->planepixelheight_mask);

      // Fetch and decode pattern name data
      info->addr = sinfo->planetbl[planenum];

      // Figure out which page it's on(if plane size is not 1x1)
      info->addr += ((  ((y[0] >> sinfo->pagepixelwh_bits) << pagesize_bits) << info->planew_bits) +
                     (   (x[0] >> sinfo->pagepixelwh_bits) << pagesize_bits) +
                     (((y[0] & sinfo->pagepixelwh_mask) >> cellwh) << info->pagewh_bits) +
                     ((x[0] & sinfo->pagepixelwh_mask) >> cellwh)) << (info->patterndatasize_bits+1);

      Vdp2PatternAddr(info, regs, ram); // Heh, this could be optimized

      //pipeline the tiles so that they shift over by 1
      info->pipe[0] = info->pipe[1];

      info->pipe[1].paladdr = info->paladdr;
      info->pipe[1].charaddr = info->charaddr;
      info->pipe[1].flipfunction = info->flipfunction;
   }

   if (bad_cycle)
   {
      flipfunction = info->pipe[0].flipfunction;
   }
   else
   {
      flipfunction = info->flipfunction;
   }

   // Figure out which pixel in the tile we want
   if (info->patternwh == 1)
   {
      x[0] &= 8-1;
      y[0] &= 8-1;

	  switch(flipfunction & 0x3)
	  {
	  case 0: //none
		  break;
	  case 1: //horizontal flip
		  x[0] = 8 - 1 - x[0];
		  break;
	  case 2: // vertical flip
         y[0] = 8 - 1 - y[0];
		 break;
	  case 3: //flip both
         x[0] = 8 - 1 - x[0];
		 y[0] = 8 - 1 - y[0];
		 break;
	  }
   }
   else
   {
      if (flipfunction)
      {
         y[0] &= 16 - 1;
         if (flipfunction & 0x2)
         {
            if (!(y[0] & 8))
               y[0] = 8 - 1 - y[0] + 16;
            else
               y[0] = 16 - 1 - y[0];
         }
         else if (y[0] & 8)
            y[0] += 8;

         if (flipfunction & 0x1)
         {
            if (!(x[0] & 8))
               y[0] += 8;

            x[0] &= 8-1;
            x[0] = 8 - 1 - x[0];
         }
         else if (x[0] & 8)
         {
            y[0] += 8;
            x[0] &= 8-1;
         }
         else
            x[0] &= 8-1;
      }
      else
      {
         y[0] &= 16 - 1;

         if (y[0] & 8)
            y[0] += 8;
         if (x[0] & 8)
            y[0] += 8;
         x[0] &= 8-1;
      }
   }
}

/* Vdp2MapCalcXY() for the fast path of Vdp2DrawScroll(), inlined into its
 * per-format line loops. That path is only taken without bad cycle
 * emulation, so the tile pipeline (info->pipe), which only bad cycle
 * emulation reads, is not maintained. */
static INLINE __attribute__((always_inline)) void Vdp2MapCalcXYFast(vdp2draw_struct *info, int *x, int *y,
                                 screeninfo_struct *sinfo, Vdp2* regs, u8 * ram)
{
   int planenum;
   int flipfunction;
   const int pagesize_bits=info->pagewh_bits*2;
   const int cellwh=(2 + info->patternwh);

   const int check = ((y[0] >> cellwh) << 16) | (x[0] >> cellwh);
   //if ((x[0] >> cellwh) != sinfo->oldcellx || (y[0] >> cellwh) != sinfo->oldcelly)
   if(check != sinfo->oldcellcheck)
   {
      sinfo->oldcellx = x[0] >> cellwh;
      sinfo->oldcelly = y[0] >> cellwh;
	  sinfo->oldcellcheck = (sinfo->oldcelly << 16) | sinfo->oldcellx;

      // Calculate which plane we're dealing with
      planenum = ((y[0] >> sinfo->planepixelheight_bits) * info->mapwh) + (x[0] >> sinfo->planepixelwidth_bits);
      x[0] = (x[0] & sinfo->planepixelwidth_mask);
      y[0] = (y[0] & sinfo->planepixelheight_mask);

      // Fetch and decode pattern name data
      info->addr = sinfo->planetbl[planenum];

      // Figure out which page it's on(if plane size is not 1x1)
      info->addr += ((  ((y[0] >> sinfo->pagepixelwh_bits) << pagesize_bits) << info->planew_bits) +
                     (   (x[0] >> sinfo->pagepixelwh_bits) << pagesize_bits) +
                     (((y[0] & sinfo->pagepixelwh_mask) >> cellwh) << info->pagewh_bits) +
                     ((x[0] & sinfo->pagepixelwh_mask) >> cellwh)) << (info->patterndatasize_bits+1);

      Vdp2PatternAddr(info, regs, ram); // Heh, this could be optimized

   }

   flipfunction = info->flipfunction;

   // Figure out which pixel in the tile we want
   if (info->patternwh == 1)
   {
      x[0] &= 8-1;
      y[0] &= 8-1;

	  switch(flipfunction & 0x3)
	  {
	  case 0: //none
		  break;
	  case 1: //horizontal flip
		  x[0] = 8 - 1 - x[0];
		  break;
	  case 2: // vertical flip
         y[0] = 8 - 1 - y[0];
		 break;
	  case 3: //flip both
         x[0] = 8 - 1 - x[0];
		 y[0] = 8 - 1 - y[0];
		 break;
	  }
   }
   else
   {
      if (flipfunction)
      {
         y[0] &= 16 - 1;
         if (flipfunction & 0x2)
         {
            if (!(y[0] & 8))
               y[0] = 8 - 1 - y[0] + 16;
            else
               y[0] = 16 - 1 - y[0];
         }
         else if (y[0] & 8)
            y[0] += 8;

         if (flipfunction & 0x1)
         {
            if (!(x[0] & 8))
               y[0] += 8;

            x[0] &= 8-1;
            x[0] = 8 - 1 - x[0];
         }
         else if (x[0] & 8)
         {
            y[0] += 8;
            x[0] &= 8-1;
         }
         else
            x[0] &= 8-1;
      }
      else
      {
         y[0] &= 16 - 1;

         if (y[0] & 8)
            y[0] += 8;
         if (x[0] & 8)
            y[0] += 8;
         x[0] &= 8-1;
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void SetupScreenVars(vdp2draw_struct *info, screeninfo_struct *sinfo, void FASTCALL (* PlaneAddr)(void *, int, Vdp2*), Vdp2* regs)
{
   if (!info->isbitmap)
   {
      sinfo->pagepixelwh=64*8;
	  sinfo->pagepixelwh_bits = 9;
	  sinfo->pagepixelwh_mask = 511;

      sinfo->planepixelwidth=info->planew*sinfo->pagepixelwh;
	  sinfo->planepixelwidth_bits = 8+info->planew;
	  sinfo->planepixelwidth_mask = (1<<(sinfo->planepixelwidth_bits))-1;

      sinfo->planepixelheight=info->planeh*sinfo->pagepixelwh;
	  sinfo->planepixelheight_bits = 8+info->planeh;
	  sinfo->planepixelheight_mask = (1<<(sinfo->planepixelheight_bits))-1;

      sinfo->screenwidth=info->mapwh*sinfo->planepixelwidth;
      sinfo->screenheight=info->mapwh*sinfo->planepixelheight;
      sinfo->oldcellx=-1;
      sinfo->oldcelly=-1;
      sinfo->oldcellcheck=-1;
      sinfo->xmask = sinfo->screenwidth-1;
      sinfo->ymask = sinfo->screenheight-1;
      GeneratePlaneAddrTable(info, sinfo->planetbl, PlaneAddr, regs);
   }
   else
   {
      sinfo->pagepixelwh = 0;
	  sinfo->pagepixelwh_bits = 0;
	  sinfo->pagepixelwh_mask = 0;
      sinfo->planepixelwidth=0;
	  sinfo->planepixelwidth_bits=0;
	  sinfo->planepixelwidth_mask=0;
      sinfo->planepixelheight=0;
	  sinfo->planepixelheight_bits=0;
	  sinfo->planepixelheight_mask=0;
      sinfo->screenwidth=0;
      sinfo->screenheight=0;
      sinfo->oldcellx=0;
      sinfo->oldcelly=0;
      sinfo->oldcellcheck=0;
      sinfo->xmask = info->cellw-1;
      sinfo->ymask = info->cellh-1;
   }
}

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL GetAlpha(vdp2draw_struct * info, u32 color, u32 dot)
{
   if (((info->specialcolormode == 1) || (info->specialcolormode == 2)) && ((info->specialcolorfunction & 1) == 0)) {
      /* special color calculation mode 1 and 2 enables color calculation only when special color function = 1 */
      return 0x3F;
   } else if (info->specialcolormode == 2) {
      /* special color calculation 2 enables color calculation according to lower bits of the color code */
      if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
         return 0x3F;
      }
   } else if ((info->specialcolormode == 3) && ((color & 0x80000000) == 0)) {
      /* special color calculation mode 3 enables color calculation only for dots with MSB = 1 */
      return 0x3F;
   }
   return info->alpha;
}

//////////////////////////////////////////////////////////////////////////////

int PixelIsSpecialPriority(int specialcode, int dot)
{
   dot &= 0xf;

   if (specialcode & 0x01)
   {
      if (dot == 0 || dot == 1)
         return 1;
   }
   if (specialcode & 0x02)
   {
      if (dot == 2 || dot == 3)
         return 1;
   }
   if (specialcode & 0x04)
   {
      if (dot == 4 || dot == 5)
         return 1;
   }
   if (specialcode & 0x08)
   {
      if (dot == 6 || dot == 7)
         return 1;
   }
   if (specialcode & 0x10)
   {
      if (dot == 8 || dot == 9)
         return 1;
   }
   if (specialcode & 0x20)
   {
      if (dot == 0xa || dot == 0xb)
         return 1;
   }
   if (specialcode & 0x40)
   {
      if (dot == 0xc || dot == 0xd)
         return 1;
   }
   if (specialcode & 0x80)
   {
      if (dot == 0xe || dot == 0xf)
         return 1;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2GetInterlaceInfo(int * start_line, int * line_increment)
{
   if (vdp2_interlace)
   {
      if (vdp2_is_odd_frame)
      {
         *start_line = 1;
      }
      else
      {
         *start_line = 0;
      }

      *line_increment = 2;
   }
   else
   {
      *start_line = 0;
      *line_increment = 1;
   }
}

//////////////////////////////////////////////////////////////////////////////

/* Draws a run of pixels that all lie in the same 8-pixel block of the same
 * tile row, so the tile data, tile row (yf) and flip are fixed and the in-tile
 * column steps by dir per pixel. Same per-pixel steps as the general loop in
 * Vdp2DrawScroll(); colornumber/colormode are constants at each call site so
 * the fetch is specialised per colour format. */
static INLINE __attribute__((always_inline)) void Vdp2DrawScrollRun(const int colornumber, const int colormode,
   vdp2draw_struct *info, Vdp2 *regs, u8 *ram, u8 *color_ram, struct PixelData *titan_line,
   int i, int run, int xf, int dir, int yf, int j, int fresh, u32 meta_base, int post_calc,
   int cc_win_fixed, int cc_win_pass, int cc_wctl, clipping_struct *colorcalcwindow, const u8 *cc_tab)
{
   const int charaddr = info->charaddr;
   const int paladdr = info->paladdr;
   int k;

   for (k = 0; k < run; k++, xf += dir)
   {
      const int px = i + k;
      u32 color, dot, pixel;
      int priority;

      if (!Vdp2FetchPixelCN(colornumber, colormode, info, xf, yf, &color, &dot, ram, charaddr, paladdr, color_ram))
         goto transparent;

      priority = info->priority;

      //per-pixel priority is on
      if (info->specialprimode == 2)
      {
         priority = info->priority & 0xE;

         if (info->specialfunction & 1)
         {
            if (PixelIsSpecialPriority(info->specialcode,dot))
               priority |= 1;
         }
      }

      if (priority == 0)
         goto transparent;

      {
         u8 alpha;
         if (cc_win_fixed ? !cc_win_pass : !WIN_AT(cc_tab, cc_wctl, colorcalcwindow, px, j))
            alpha = 0x3F;
         else
            alpha = GetAlpha(info, color, dot);

         pixel = COLSAT2YAB32(alpha, color);
         if (post_calc == 1)
            pixel = DoColorOffset(info, pixel);
         else if (post_calc == 2)
            pixel = info->PostPixelFetchCalc(info, pixel);

         titan_line[px].pixel = pixel;
         titan_line[px].meta = meta_base | TitanPackMeta(priority, 0, 0, 0);
         continue;
      }

transparent:
      if (fresh)
      {
         titan_line[px].pixel = 0;
         titan_line[px].meta = 0;
      }
   }
}

/* COLOR_ADD(pixel, cor, cog, cob) with the offset split into a positive and
 * a negative part, one byte per channel (red in bits 0-7, as in COLOR_ADD;
 * see ColorOffsetParts): a saturating byte add, then a saturating byte
 * subtract. Only one part is non-zero per channel, so each channel is
 * clamped to 0..255 as in COLOR_ADD (an offset of -256 subtracts 255 and
 * still gives 0); the top byte (alpha) has 0 in both parts. Two instructions
 * on ARMv6+ (UQADD8, UQSUB8) instead of ~20. */
static INLINE u32 ColorOffsetSat(u32 pixel, u32 pos, u32 neg)
{
#if defined(__arm__) && defined(__ARM_FEATURE_SIMD32)
   __asm__("uqadd8 %0, %0, %1" : "+r"(pixel) : "r"(pos));
   __asm__("uqsub8 %0, %0, %1" : "+r"(pixel) : "r"(neg));
   return pixel;
#else
   u32 out = 0;
   int b;
   for (b = 0; b < 32; b += 8)
   {
      int c = (int)((pixel >> b) & 0xFF) + (int)((pos >> b) & 0xFF);
      if (c > 0xFF) c = 0xFF;
      c -= (int)((neg >> b) & 0xFF);
      if (c < 0) c = 0;
      out |= (u32)c << b;
   }
   return out;
#endif
}

static INLINE void ColorOffsetParts(const s32 *co, u32 *pos, u32 *neg)
{
   int k;
   *pos = *neg = 0;
   for (k = 0; k < 3; k++)
   {
      if (co[k] > 0)
         *pos |= (u32)co[k] << (k * 8);
      else if (co[k] < 0)
         *neg |= (u32)(co[k] < -0xFF ? 0xFF : -co[k]) << (k * 8);
   }
}

/* One line (span) of a 4bpp or 8bpp scroll layer in the usual case: what
 * Vdp2DrawScrollLineFast() does with Vdp2DrawScrollRun() per run, in two
 * passes. Pass 1 (Vdp2PalLineRuns) walks the tiles and writes one record per
 * run: tile row address, first palette entry, alpha, priority (meta), flip.
 * Pass 2 (Vdp2PalDraw*) draws the records. Each pass fits in the registers;
 * together in one loop (or a call per run) they didn't, and the pixel loop
 * reloaded its values from the stack.
 * Same pixels as Vdp2DrawScrollRun(). The caller makes sure that: vidsoft_pal
 * was built from this colour RAM in this colour mode, cellw is 8, there is no
 * per-pixel priority (specialprimode != 2), post_calc is 0 or 1, the colour
 * calc window is the same for the whole line (cc_win_fixed) and the special
 * colour calculation mode is not 3. Per tile pass 1 checks the rest (else
 * that run is drawn by Vdp2DrawScrollRun() at once): the alpha doesn't depend
 * on the pixel (not mode 2 with the tile's special colour function on),
 * charaddr is a multiple of 0x20, paladdr has no bits under the dot (so
 * paladdr | dot == paladdr + dot) and the run's palette entries don't wrap.
 * A tile row is then 4 or 8 bytes, aligned to its size: masking the row's
 * address with 0x7FFFF once masks every pixel's, and a row that is all zero
 * (with transparency on) is transparent. */
struct PalRun
{
   const u8 *row;
   const u32 *palp;
   u32 alpha, meta;
   s16 i;
   s8 xf, dir;
   u8 run, kind;   /* kind: PALRUN_DRAW, or PALRUN_CLEAR (transparent) */
};
#define PALRUN_DRAW 1
#define PALRUN_CLEAR 2
/* one buffer per Titan layer (a layer is drawn by one thread at a time); a
 * line has at most 704 pixels: 704 / 8 + 1 runs, +1 where x wraps */
static struct PalRun vidsoft_palruns[6][96];

/* Vdp2DrawScrollRun() out of line, for the runs pass 1 leaves to it (colour
 * calc window fixed for the line) */
static __attribute__((noinline)) void Vdp2DrawScrollRunOld(int colornumber, int colormode,
   vdp2draw_struct *info, Vdp2 *regs, u8 *ram, u8 *color_ram, struct PixelData *titan_line,
   int i, int run, int xf, int dir, int yf, int j, int fresh, u32 meta_base, int post_calc, int cc_win_pass)
{
   if (colornumber == 0)
      Vdp2DrawScrollRun(0, colormode, info, regs, ram, color_ram, titan_line, i, run, xf, dir, yf, j, fresh, meta_base, post_calc, 1, cc_win_pass, 0, NULL, NULL);
   else
      Vdp2DrawScrollRun(1, colormode, info, regs, ram, color_ram, titan_line, i, run, xf, dir, yf, j, fresh, meta_base, post_calc, 1, cc_win_pass, 0, NULL, NULL);
}

/* Pass 1: returns the number of records written to r */
static INLINE __attribute__((always_inline)) int Vdp2PalLineRuns(const int colornumber, struct PalRun *r,
   vdp2draw_struct *info, screeninfo_struct *sinfo, Vdp2 *regs, u8 *ram, u8 *color_ram, int colormode,
   struct PixelData *titan_line, int linescrollx, int Y, int j, int fresh, u32 meta_base, int post_calc,
   int cc_win_pass, int start, int end)
{
   const u32 dotmask = colornumber == 0 ? 0xF : 0xFF;
   const u32 palmask = vidsoft_pal_mask;
   const int scm = info->specialcolormode;
   const int te = info->transparencyenable;
   /* GetAlpha() without the pixel, behind the colour calc window: 0x3F where
    * the window is closed, or where special colour calculation mode 1 or 2 is
    * off for the tile */
   const u32 alpha_on = cc_win_pass ? (u32)(u8)info->alpha << 24 : 0x3Fu << 24;
   const int scm12 = scm == 1 || scm == 2;
   const int xmask = sinfo->xmask;
   int n = 0, i = start;

   while (i < end)
   {
      const int base = (info->x + i) & xmask;
      int run, x, y;

      x = base;
      if (linescrollx)
         x = (x + linescrollx) & 0x3FF;

      run = 8 - (x & 7);
      if (run > xmask + 1 - base)
         run = xmask + 1 - base;
      if (run > end - i)
         run = end - i;

      y = Y;
      Vdp2MapCalcXYFast(info, &x, &y, sinfo, regs, ram);

      if ((scm == 2 && (info->specialcolorfunction & 1)) || (info->charaddr & 0x1F) ||
          (info->paladdr & dotmask) || info->coloroffset + info->paladdr + dotmask > palmask)
         Vdp2DrawScrollRunOld(colornumber, colormode, info, regs, ram, color_ram, titan_line, i, run, x,
                              (info->flipfunction & 1) ? -1 : 1, y, j, fresh, meta_base, post_calc, cc_win_pass);
      else
      {
         const u8 *row = ram + ((info->charaddr + y * (colornumber == 0 ? 4 : 8)) & 0x7FFFF);
         u32 w[2];

         memcpy(w, row, colornumber == 0 ? 4 : 8);
         /* nothing to draw: priority 0, or only zero dots with transparency on */
         if (info->priority == 0 || (te && (colornumber == 0 ? w[0] : w[0] | w[1]) == 0))
         {
            if (fresh)
            {
               r[n].i = i;
               r[n].run = run;
               r[n].kind = PALRUN_CLEAR;
               n++;
            }
         }
         else
         {
            r[n].row = row;
            r[n].palp = vidsoft_pal + info->coloroffset + info->paladdr;
            r[n].alpha = scm12 && !(info->specialcolorfunction & 1) ? 0x3Fu << 24 : alpha_on;
            r[n].meta = meta_base | TitanPackMeta(info->priority, 0, 0, 0);
            r[n].i = i;
            r[n].xf = x;
            r[n].dir = (info->flipfunction & 1) ? -1 : 1;
            r[n].run = run;
            r[n].kind = PALRUN_DRAW;
            n++;
         }
      }
      i += run;
   }
   return n;
}

/* Pass 2; ofs (colour offset on/off) is a constant at each call site.
 * flags: 1 = transparency enabled, 2 = fresh line (write transparent pixels
 * too); ofs_pos/ofs_neg: ColorOffsetParts() */
static INLINE __attribute__((always_inline)) void Vdp2PalDrawBody(const int colornumber, const int ofs,
   const struct PalRun *r, int n, struct PixelData *titan_line, int flags, u32 ofs_pos, u32 ofs_neg)
{
   /* dot | opaque_zero is 0 only for a transparent pixel */
   const u32 opaque_zero = !(flags & 1);
   const int fresh = flags & 2;
   const struct PalRun *rend = r + n;

   for (; r < rend; r++)
   {
      struct PixelData *out = titan_line + r->i;
      struct PixelData *end = out + r->run;

      if (r->kind == PALRUN_CLEAR)
      {
         for (; out < end; out++)
         {
            out->pixel = 0;
            out->meta = 0;
         }
         continue;
      }

      {
         const u8 *row = r->row;
         const u32 *palp = r->palp;
         const u32 alpha = r->alpha, meta = r->meta;
         const int dir = r->dir;
         int xf = r->xf;
         const u8 *p = row + xf;

         for (; out < end; out++, xf += dir, p += dir)
         {
            u32 dot, pixel;

            if (colornumber == 0)
            {
               dot = row[xf >> 1];
               dot = (xf & 1) ? dot & 0xF : dot >> 4;
            }
            else
               dot = *p;

            if ((dot | opaque_zero) == 0)
            {
               if (fresh)
               {
                  out->pixel = 0;
                  out->meta = 0;
               }
               continue;
            }

            pixel = alpha | (palp[dot] & 0xFFFFFF);
            if (ofs)
               pixel = ColorOffsetSat(pixel, ofs_pos, ofs_neg);
            out->pixel = pixel;
            out->meta = meta;
         }
      }
   }
}

#define PAL_DRAW_FN(NAME, CN, OFS) \
static __attribute__((noinline)) void NAME(const struct PalRun *r, int n, struct PixelData *titan_line, int flags, u32 ofs_pos, u32 ofs_neg) \
{ \
   Vdp2PalDrawBody(CN, OFS, r, n, titan_line, flags, ofs_pos, ofs_neg); \
}
PAL_DRAW_FN(Vdp2PalDraw4, 0, 0)
PAL_DRAW_FN(Vdp2PalDraw4Ofs, 0, 1)
PAL_DRAW_FN(Vdp2PalDraw8, 1, 0)
PAL_DRAW_FN(Vdp2PalDraw8Ofs, 1, 1)
#undef PAL_DRAW_FN

/* Both passes for one line (span) */
static INLINE __attribute__((always_inline)) void Vdp2DrawScrollLinePal(const int colornumber,
   vdp2draw_struct *info, screeninfo_struct *sinfo, Vdp2 *regs, u8 *ram, u8 *color_ram, int colormode,
   struct PixelData *titan_line, int linescrollx, int Y, int j, int fresh, u32 meta_base, int post_calc,
   int cc_win_pass, int start, int end)
{
   struct PalRun *r = vidsoft_palruns[info->titan_which_layer];
   const int flags = (info->transparencyenable ? 1 : 0) | (fresh ? 2 : 0);
   const int n = Vdp2PalLineRuns(colornumber, r, info, sinfo, regs, ram, color_ram, colormode, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_pass, start, end);
   u32 ofs_pos = 0, ofs_neg = 0;

   if (post_calc == 1)
   {
      ColorOffsetParts(&info->cor, &ofs_pos, &ofs_neg);
      (colornumber == 0 ? Vdp2PalDraw4Ofs : Vdp2PalDraw8Ofs)(r, n, titan_line, flags, ofs_pos, ofs_neg);
   }
   else
      (colornumber == 0 ? Vdp2PalDraw4 : Vdp2PalDraw8)(r, n, titan_line, flags, 0, 0);
}

/* One line of the fast path in Vdp2DrawScroll(), for one colour format: the
 * line splits into runs inside one 8-pixel block (split further where x
 * wraps) that share tile data, row and flip. Kept as one function per format
 * so the run body is inlined once instead of being called for every run. */
static INLINE __attribute__((always_inline)) void Vdp2DrawScrollLineFast(const int colornumber, const int colormode,
   vdp2draw_struct *info, screeninfo_struct *sinfo, Vdp2 *regs, u8 *ram, u8 *color_ram,
   struct PixelData *titan_line, int linescrollx, int Y, int j, int fresh, u32 meta_base, int post_calc,
   int cc_win_fixed, int cc_win_pass, int cc_wctl, clipping_struct *colorcalcwindow, const u8 *cc_tab, int start, int end)
{
   int i = start;

   /* the usual 4bpp/8bpp line: Vdp2DrawScrollLinePal() (its conditions that
    * hold for the whole line; it tests the per-tile ones) */
   if (colornumber <= 1 && colormode == vidsoft_pal_mode && color_ram == vidsoft_pal_src &&
       info->cellw == 8 && info->specialprimode != 2 && post_calc != 2 &&
       cc_win_fixed && info->specialcolormode != 3)
   {
      Vdp2DrawScrollLinePal(colornumber, info, sinfo, regs, ram, color_ram, colormode, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_pass, start, end);
      return;
   }

   while (i < end)
   {
      const int base = (info->x + i) & sinfo->xmask;
      int run, dir, x, y;

      x = base;
      if (linescrollx)
         x = (x + linescrollx) & 0x3FF;

      run = 8 - (x & 7);
      if (run > sinfo->xmask + 1 - base)
         run = sinfo->xmask + 1 - base;
      if (run > end - i)
         run = end - i;

      y = Y;
      Vdp2MapCalcXYFast(info, &x, &y, sinfo, regs, ram);
      dir = (info->flipfunction & 1) ? -1 : 1;

      Vdp2DrawScrollRun(colornumber, colormode, info, regs, ram, color_ram, titan_line, i, run, x, dir, y, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab);
      i += run;
   }
}

/* The fast path for a horizontally zoomed line (coordinate increment != 1, as in
 * Virtua Fighter 2's backgrounds): each pixel's x is computed with the same
 * expression as the per-pixel loop in Vdp2DrawScroll(), but the tile lookup
 * (Vdp2MapCalcXYFast) runs only when x enters a new 8-pixel block; inside a
 * block the in-tile column is linear in x, as in Vdp2DrawScrollLineFast(). */
static INLINE __attribute__((always_inline)) void Vdp2DrawScrollLineZoom(const int colornumber, const int colormode,
   vdp2draw_struct *info, screeninfo_struct *sinfo, Vdp2 *regs, u8 *ram, u8 *color_ram,
   struct PixelData *titan_line, int linescrollx, int Y, int j, int fresh, u32 meta_base, int post_calc,
   int cc_win_fixed, int cc_win_pass, int cc_wctl, clipping_struct *colorcalcwindow, const u8 *cc_tab, int start, int end,
   const int *mosaic_x)
{
   int i, block = -1, x0 = 0, xf0 = 0, yf = 0, dir = 1;
   for (i = start; i < end; i++)
   {
      int x = info->x + mosaic_x[i]*info->coordincx;
      x &= sinfo->xmask;
      if (linescrollx)
      {
         x += linescrollx;
         x &= 0x3FF;
      }
      if ((x >> 3) != block)
      {
         int xc = x, y = Y;
         Vdp2MapCalcXYFast(info, &xc, &y, sinfo, regs, ram);
         block = x >> 3;
         x0 = x;
         xf0 = xc;
         yf = y;
         dir = (info->flipfunction & 1) ? -1 : 1;
      }
      Vdp2DrawScrollRun(colornumber, colormode, info, regs, ram, color_ram, titan_line, i, 1, xf0 + dir * (x - x0), dir, yf, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab);
   }
}

/* one out-of-line copy of the line loop per colour format. Not noinline:
   vidsoft.c is at GCC's per-file inlining limit, and taking these out of
   Vdp2DrawScroll changed what got inlined in the rotation and sprite paths
   (Sega Rally's rotated RBG0 3% slower on the device). */
#define SCROLL_LINE_FN(NAME, CN) \
static void NAME(const int colormode, vdp2draw_struct *info, screeninfo_struct *sinfo, Vdp2 *regs, u8 *ram, u8 *color_ram, \
   struct PixelData *titan_line, int linescrollx, int Y, int j, int fresh, u32 meta_base, int post_calc, \
   int cc_win_fixed, int cc_win_pass, int cc_wctl, clipping_struct *colorcalcwindow, const u8 *cc_tab, int start, int end) \
{ \
   Vdp2DrawScrollLineFast(CN, colormode, info, sinfo, regs, ram, color_ram, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab, start, end); \
}
SCROLL_LINE_FN(Vdp2DrawScrollLine4bpp, 0)
SCROLL_LINE_FN(Vdp2DrawScrollLine8bpp, 1)
SCROLL_LINE_FN(Vdp2DrawScrollLine16bppPal, 2)
SCROLL_LINE_FN(Vdp2DrawScrollLine16bppRGB, 3)
SCROLL_LINE_FN(Vdp2DrawScrollLine32bpp, 4)
#undef SCROLL_LINE_FN

#define SCROLL_ZOOM_FN(NAME, CN) \
static void NAME(const int colormode, vdp2draw_struct *info, screeninfo_struct *sinfo, Vdp2 *regs, u8 *ram, u8 *color_ram, \
   struct PixelData *titan_line, int linescrollx, int Y, int j, int fresh, u32 meta_base, int post_calc, \
   int cc_win_fixed, int cc_win_pass, int cc_wctl, clipping_struct *colorcalcwindow, const u8 *cc_tab, int start, int end, \
   const int *mosaic_x) \
{ \
   Vdp2DrawScrollLineZoom(CN, colormode, info, sinfo, regs, ram, color_ram, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab, start, end, mosaic_x); \
}
SCROLL_ZOOM_FN(Vdp2DrawScrollZoom4bpp, 0)
SCROLL_ZOOM_FN(Vdp2DrawScrollZoom8bpp, 1)
SCROLL_ZOOM_FN(Vdp2DrawScrollZoom16bppPal, 2)
SCROLL_ZOOM_FN(Vdp2DrawScrollZoom16bppRGB, 3)
SCROLL_ZOOM_FN(Vdp2DrawScrollZoom32bpp, 4)
#undef SCROLL_ZOOM_FN

/* x and y after mosaic, per mosaic size: built once at init (it used to be
   built by the first layer thread to draw, which set its "built" flag first,
   so another layer thread could read a half-built table) */
static int mosaic_table[16][1024];

static void VidsoftInitMosaicTable(void)
{
   int i, j;
   for (i = 0; i < 16; i++)
      for (j = 0; j < 1024; j++)
         mosaic_table[i][j] = j / (i + 1) * (i + 1);
}

static void FASTCALL Vdp2DrawScroll(vdp2draw_struct *info, Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   int i, j;
   int x, y;
   clipping_struct clip[2];
   u32 linewnd0addr, linewnd1addr;
   u32 line_window_base[2] = { 0 };
   screeninfo_struct sinfo;
   int scrolly;
   int *mosaic_y, *mosaic_x;
   clipping_struct colorcalcwindow[2];
   int start_line = 0, line_increment = 0;
   int bad_cycle = bad_cycle_setting[info->titan_which_layer];
   int charaddr, paladdr;
   int output_y = 0;
   u32 linescrollx_table[512] = { 0 };
   u32 linescrolly_table[512] = { 0 };
   float lineszoom_table[512] = { 0 };
   int num_vertical_cell_scroll_enabled = 0;

   SetupScreenVars(info, &sinfo, info->PlaneAddr, regs);

   scrolly = info->y;

   clip[0].xstart = clip[0].ystart = clip[0].xend = clip[0].yend = 0;
   clip[1].xstart = clip[1].ystart = clip[1].xend = clip[1].yend = 0;
   ReadWindowData(info->wctl, clip, regs);
   linewnd0addr = linewnd1addr = 0;
   ReadLineWindowData(&info->islinewindow, info->wctl, &linewnd0addr, &linewnd1addr,regs);
   line_window_base[0] = linewnd0addr;
   line_window_base[1] = linewnd1addr;
   /* color calculation window: in => no color calc, out => color calc */
   ReadWindowData(regs->WCTLD >> 8, colorcalcwindow, regs);
   mosaic_x = mosaic_table[info->mosaicxmask-1];
   mosaic_y = mosaic_table[info->mosaicymask-1];

   Vdp2GetInterlaceInfo(&start_line, &line_increment);

   if (regs->SCRCTL & 1)
      num_vertical_cell_scroll_enabled++;
   if (regs->SCRCTL & 0x100)
      num_vertical_cell_scroll_enabled++;

   //pre-generate line scroll tables
   for (j = start_line; j < vdp2height; j++)
   {
      if (info->islinescroll)
      {
         //line scroll interval bit
         int need_increment = ((j != 0) && (((j + 1) % info->lineinc) == 0));

         //horizontal line scroll
         if (info->islinescroll & 0x1)
         {
            linescrollx_table[j] = (T1ReadLong(ram, info->linescrolltbl) >> 16) & 0x7FF;
            if (need_increment)
               info->linescrolltbl += 4;
         }

         //vertical line scroll
         if (info->islinescroll & 0x2)
         {
            linescrolly_table[j] = ((T1ReadWord(ram, info->linescrolltbl) & 0x7FF)) + scrolly;
            if (need_increment)
               info->linescrolltbl += 4;
            y = info->y;
         }

         //line zoom
         if (info->islinescroll & 0x4)
         {
            lineszoom_table[j] = (T1ReadLong(ram, info->linescrolltbl) & 0x7FF00) / (float)65536.0;
            if (need_increment)
               info->linescrolltbl += 4;
         }
      }
   }

   for (j = start_line; j < vdp2height; j += line_increment)
   {
      int Y;
      int linescrollx = 0;
      // precalculate the coordinate for the line(it's faster) and do line
      // scroll
      if (info->islinescroll)
      {
         //horizontal line scroll
         if (info->islinescroll & 0x1)
         {
            linescrollx = linescrollx_table[j];
         }

         //vertical line scroll
         if (info->islinescroll & 0x2)
         {
            info->y = linescrolly_table[j];
            y = info->y;
         }
         else
            //y = info->y+((int)(info->coordincy *(float)(info->mosaicymask > 1 ? (j / info->mosaicymask * info->mosaicymask) : j)));
			y = info->y + info->coordincy*mosaic_y[j];

         //line zoom
         if (info->islinescroll & 0x4)
         {
            info->coordincx = lineszoom_table[j];
         }
      }
      else
         //y = info->y+((int)(info->coordincy *(float)(info->mosaicymask > 1 ? (j / info->mosaicymask * info->mosaicymask) : j)));
		 y = info->y + info->coordincy*mosaic_y[j];

      if (vdp2_interlace)
      {
         linewnd0addr = line_window_base[0] + (j * 4);
         linewnd1addr = line_window_base[1] + (j * 4);
      }

      // if line window is enabled, adjust clipping values
      ReadLineWindowClip(info->islinewindow, clip, &linewnd0addr, &linewnd1addr, ram, regs);
      y &= sinfo.ymask;

      if (info->isverticalscroll && (!vdp2_x_hires))//seems to be ignored in hi res
      {
         // this is *wrong*, vertical scroll use a different value per cell
         // info->verticalscrolltbl should be incremented by info->verticalscrollinc
         // each time there's a cell change and reseted at the end of the line...
         // or something like that :)
         u32 scroll_value = 0;
         int y_value = 0;
         
         if (vdp2_interlace)
            y_value = j / 2;
         else
            y_value = j;

         if (num_vertical_cell_scroll_enabled == 1)
         {
            scroll_value = cell_data[y_value].data[0] >> 16;
         }
         else
         {
            if (info->titan_which_layer == TITAN_NBG0)
               scroll_value = cell_data[y_value].data[0] >> 16;//reload cell data per line for sonic 2, 2 player mode
            else if (info->titan_which_layer == TITAN_NBG1)
               scroll_value = cell_data[y_value].data[1] >> 16;
         }

         y += scroll_value;
         y &= 0x1FF;
      }

      Y=y;

      if (vdp2_interlace)
         info->LoadLineParams(info, &sinfo, j / 2, lines);
      else
         info->LoadLineParams(info, &sinfo, j, lines);

      if (!info->enable)
         continue;

      {
         /* Loop invariants for this line, hoisted out of the pixel loop. With
          * no window enabled TestBothWindow() gives the same answer for every
          * pixel, and with no horizontal zoom the float x step is exact. */
         const int layer_win_fixed = (info->wctl & 0x2a) == 0;
         const int layer_win_pass = !(info->wctl & 0x80);
         const int cc_wctl = regs->WCTLD >> 8;
         const int cc_win_fixed = (cc_wctl & 0x2a) == 0;
         const int cc_win_pass = !(cc_wctl & 0x80);
         /* window tests per line instead of per pixel (where they can be) */
         const u8 *win_tab = layer_win_fixed ? NULL : VidsoftWinTable(info->wctl, clip, j, vdp2width, vidsoft_win_buf[info->titan_which_layer][0]);
         const u8 *cc_tab = cc_win_fixed ? NULL : VidsoftWinTable(cc_wctl, colorcalcwindow, j, vdp2width, vidsoft_win_buf[info->titan_which_layer][1]);
         const int zoom_x = info->coordincx != 1.0f;
         const int post_calc = info->PostPixelFetchCalc == &DoNothing ? 0 :
                               info->PostPixelFetchCalc == &DoColorOffset ? 1 : 2;
         const int layer = info->titan_which_layer;
         const u32 meta_base = TitanPackMeta(0, info->linescreen, info->titan_shadow_type, info->titan_shadow_enabled);
         struct PixelData *titan_line = TitanGetLayerLine(layer, output_y);
         const int skip_line = layer_win_fixed && !layer_win_pass;
         /* A line that is still stale from an earlier frame gets every pixel
          * written (zero where nothing is drawn) instead of being cleared first */
         const int fresh = skip_line ? 0 : TitanClaimLayerLine(layer, output_y);
         /* Fast path: without mosaic or bad cycle emulation. Unzoomed, x advances
          * by one per pixel, so the line splits into runs inside one 8-pixel
          * block (split further where x wraps) that share tile data, row and
          * flip; zoomed, x is computed per pixel and the tile lookup is redone
          * only when x enters a new block. Vdp2MapCalcXY() is still called at
          * the start of each block, keeping its tile state exactly as before. A
          * layer window only selects which spans of the line are drawn. */
         const int fast = !info->isbitmap && info->mosaicxmask == 1 && !bad_cycle &&
                          ((sinfo.xmask & (sinfo.xmask + 1)) == 0);

         if (fast && !skip_line)
         {
            const int colormode = Vdp2Internal.ColorMode;
            int start = 0, end = vdp2width;
            for (;;)
            {
               if (!layer_win_fixed)
               {
                  /* the next span the window lets through; pixels it hides
                   * are transparent, as in the per-pixel path */
                  while (start < vdp2width && !WIN_AT(win_tab, info->wctl, clip, start, j))
                  {
                     if (fresh)
                     {
                        titan_line[start].pixel = 0;
                        titan_line[start].meta = 0;
                     }
                     start++;
                  }
                  if (start >= vdp2width)
                     break;
                  end = start + 1;
                  while (end < vdp2width && WIN_AT(win_tab, info->wctl, clip, end, j))
                     end++;
               }
#define SCROLL_LINE(FN) FN(colormode, info, &sinfo, regs, ram, color_ram, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab, start, end)
#define SCROLL_ZOOM(FN) FN(colormode, info, &sinfo, regs, ram, color_ram, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab, start, end, mosaic_x)
               if (zoom_x)
                  switch (info->colornumber)
                  {
                  case 0: SCROLL_ZOOM(Vdp2DrawScrollZoom4bpp); break;
                  case 1: SCROLL_ZOOM(Vdp2DrawScrollZoom8bpp); break;
                  case 2: SCROLL_ZOOM(Vdp2DrawScrollZoom16bppPal); break;
                  case 3: SCROLL_ZOOM(Vdp2DrawScrollZoom16bppRGB); break;
                  case 4: SCROLL_ZOOM(Vdp2DrawScrollZoom32bpp); break;
                  default:
                     Vdp2DrawScrollLineZoom(info->colornumber, colormode, info, &sinfo, regs, ram, color_ram, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab, start, end, mosaic_x);
                     break;
                  }
               else
               switch (info->colornumber)
               {
               case 0: SCROLL_LINE(Vdp2DrawScrollLine4bpp); break;
               case 1: SCROLL_LINE(Vdp2DrawScrollLine8bpp); break;
               case 2: SCROLL_LINE(Vdp2DrawScrollLine16bppPal); break;
               case 3: SCROLL_LINE(Vdp2DrawScrollLine16bppRGB); break;
               case 4: SCROLL_LINE(Vdp2DrawScrollLine32bpp); break;
               default: // other formats draw nothing, but the line still has to be walked
                  Vdp2DrawScrollLineFast(info->colornumber, colormode, info, &sinfo, regs, ram, color_ram, titan_line, linescrollx, Y, j, fresh, meta_base, post_calc, cc_win_fixed, cc_win_pass, cc_wctl, colorcalcwindow, cc_tab, start, end);
                  break;
               }
#undef SCROLL_LINE
#undef SCROLL_ZOOM
               if (end >= vdp2width)
                  break;
               start = end;
            }
         }
         else
         for (i = skip_line ? vdp2width : 0; i < vdp2width; i++)
         {
            u32 color, dot, pixel;
            int priority;

            // See if screen position is clipped, if it isn't, continue
            if (!layer_win_fixed && !WIN_AT(win_tab, info->wctl, clip, i, j))
               goto transparent;

            if (zoom_x)
               x = info->x + mosaic_x[i]*info->coordincx;
            else
               x = info->x + mosaic_x[i];
            x &= sinfo.xmask;

            if (linescrollx) {
               x += linescrollx;
               x &= 0x3FF;
            }

            // Fetch Pixel, if it isn't transparent, continue
            if (!info->isbitmap)
            {
               // Tile
               y=Y;
               Vdp2MapCalcXY(info, &x, &y, &sinfo, regs, ram, bad_cycle);
            }

            if (!bad_cycle)
            {
               charaddr = info->charaddr;
               paladdr = info->paladdr;
            }
            else
            {
               charaddr = info->pipe[0].charaddr;
               paladdr = info->pipe[0].paladdr;
            }

            if (!Vdp2FetchPixel(info, x, y, &color, &dot, ram, charaddr, paladdr,color_ram))
               goto transparent;

            priority = info->priority;

            //per-pixel priority is on
            if (info->specialprimode == 2)
            {
               priority = info->priority & 0xE;

               if (info->specialfunction & 1)
               {
                  if (PixelIsSpecialPriority(info->specialcode,dot))
                  {
                     priority |= 1;
                  }
               }
            }

            if (priority == 0)
               goto transparent;

            // Apply color offset and color calculation/special color calculation
            // and then continue.
            // We almost need to know well ahead of time what the top
            // and second pixel is in order to work this.
            {
               u8 alpha;
               /* if we're in the valid area of the color calculation window, don't do color calculation */
               if (cc_win_fixed ? !cc_win_pass : !WIN_AT(cc_tab, cc_wctl, colorcalcwindow, i, j))
                  alpha = 0x3F;
               else
                  alpha = GetAlpha(info, color, dot);

               pixel = COLSAT2YAB32(alpha, color);
               if (post_calc == 1)
                  pixel = DoColorOffset(info, pixel);
               else if (post_calc == 2)
                  pixel = info->PostPixelFetchCalc(info, pixel);

               // same as TitanPutPixel()
               titan_line[i].pixel = pixel;
               titan_line[i].meta = meta_base | TitanPackMeta(priority, 0, 0, 0);
               continue;
            }

transparent:
            if (fresh)
            {
               titan_line[i].pixel = 0;
               titan_line[i].meta = 0;
            }
         }

         /* This line holds only this layer's pixels, so tell the compositor
          * the highest priority it can contain: the layer priority, or with
          * special priority per tile/pixel, that with the low bit set */
         if (fresh)
            TitanSetLineMaxPriority(layer, output_y, info->specialprimode == 0 ? info->priority :
                                                     info->specialprimode <= 2 ? (info->priority | 1) : 7);
      }
      output_y++;
   }    
}

//////////////////////////////////////////////////////////////////////////////

void Rbg0PutHiresPixel(vdp2draw_struct *info, u32 color, u32 dot, int i, int j)
{
   u32 pixel = info->PostPixelFetchCalc(info, COLSAT2YAB32(GetAlpha(info, color, dot), color));
   int x_pos = i * 2;
   TitanPutPixel(info->priority, x_pos, j, pixel, info->linescreen, info);
   TitanPutPixel(info->priority, x_pos + 1, j, pixel, info->linescreen, info);
}

//////////////////////////////////////////////////////////////////////////////

void Rbg0PutPixel(vdp2draw_struct *info, u32 color, u32 dot, int i, int j)
{
   if (vdp2_x_hires)
   {
      Rbg0PutHiresPixel(info, color, dot, i, j);
   }
   else
      TitanPutPixel(info->priority, i, j, info->PostPixelFetchCalc(info, COLSAT2YAB32(GetAlpha(info, color, dot), color)), info->linescreen, info);
}

//////////////////////////////////////////////////////////////////////////////

int CheckBanks(Vdp2* regs, int compare_value)
{
   if (((regs->RAMCTL >> 0) & 3) == compare_value)//a0
      return 0;
   if (((regs->RAMCTL >> 2) & 3) == compare_value)//a1
      return 0;
   if (((regs->RAMCTL >> 4) & 3) == compare_value)//b0
      return 0;
   if (((regs->RAMCTL >> 6) & 3) == compare_value)//b1
      return 0;

   return 1;//no setting present
}

int Rbg0CheckRam(Vdp2* regs)
{
   if (((regs->RAMCTL >> 8) & 3) == 3)//both banks are divided
   {
      //ignore delta kax if the coefficient table
      //bank is unspecified
      if (CheckBanks(regs, 1))
         return 1;
   }

   return 0;
}

/* The coefficient read of Vdp2ReadCoefficientFP() (vidshared.h), inlined:
 * the rotation loop below does it for every dot */
static INLINE __attribute__((always_inline)) void Vdp2ReadCoefficientFPInline(vdp2rotationparameterfp_struct *parameter, u32 addr, u8 *ram)
{
   fixed32 k;
   if (parameter->coefmode == 3)
   {
      Vdp2ReadCoefficientFP(parameter, addr, ram);
      return;
   }
   if (parameter->coefdatasize == 2)
   {
      s32 i;
      addr &= 0x7FFFE;
      i = T1ReadWord(ram, addr);
      parameter->msb = (i >> 15) & 0x1;
      k = (signed) ((i & 0x7FFF) | (i & 0x4000 ? 0xFFFFC000 : 0x00000000)) * 64;
   }
   else
   {
      s32 i;
      addr &= 0x7FFFC;
      i = T1ReadLong(ram, addr);
      parameter->linescreen = (i >> 24) & 0x7F;
      parameter->msb = (i >> 31) & 0x1;
      k = (signed) ((i & 0x00FFFFFF) | (i & 0x00800000 ? 0xFF800000 : 0x00000000));
   }
   switch (parameter->coefmode)
   {
      case 0: parameter->kx = parameter->ky = k; break;
      case 1: parameter->kx = k; break;
      default: parameter->ky = k; break;
   }
}

/* State of one line of Vdp2DrawRotationFP()'s coefficient loop */
struct RotCoefLine
{
   vdp2draw_struct *info;
   vdp2rotationparameterfp_struct *p, *p2;
   screeninfo_struct *sinfo, *sinfo2;
   Vdp2 *regs;
   u8 *ram, *color_ram;
   clipping_struct *clip, *rpwindow;
   const u8 *wintab, *rptab;        /* per-line window tables, or NULL (see VidsoftWinTable) */
   int userpwindow;
   fixed32 Xsp, Ysp, Xsp2, Ysp2;   /* the per-line part of the rotated position */
   u32 coefy, rcoefy, coefy2, rcoefy2;
   u32 *dotlc;
   int dotlc_en, dotlc_en2;
   u32 dotlc_word, dotlc_last, dotlc_color;
   int j;
};

/* One line of the coefficient loop of Vdp2DrawRotationFP(), for one colour
 * format (not hi-res). Same steps and results as the generic loop, with the
 * per-line terms hoisted (A*xmul + B*ymul + C, and dX*i kept as a running
 * sum, which is the same value modulo 2^32), the coefficient read, tile
 * lookup and pixel fetch inlined, and the pixel written straight into the
 * Titan line, claimed once, instead of through Rbg0PutPixel(). */
static INLINE __attribute__((always_inline)) void Vdp2DrawRotationCoefLine(const int colornumber, struct RotCoefLine *L)
{
   vdp2draw_struct *const info = L->info;
   vdp2rotationparameterfp_struct *const p = L->p, *const p2 = L->p2;
   u8 *const ram = L->ram, *const color_ram = L->color_ram;
   const int j = L->j;
   const int colormode = Vdp2Internal.ColorMode;
   const int win_fixed = (info->wctl & 0x2a) == 0, win_pass = !(info->wctl & 0x80);
   const int p2_coef = p2 != NULL && p2->coefenab;
   const int priority = info->priority;
   const int post_calc = info->PostPixelFetchCalc == &DoNothing ? 0 :
                         info->PostPixelFetchCalc == &DoColorOffset ? 1 : 2;
   const int layer = info->titan_which_layer;
   const u32 meta = TitanPackMeta(priority, info->linescreen, info->titan_shadow_type, info->titan_shadow_enabled);
   struct PixelData *line = NULL;
   u32 coefx = 0, rcoefx = 0, coefx2 = 0, rcoefx2 = 0;
   u32 dxi = 0, dyi = 0, dxi2 = 0, dyi2 = 0;
   int i;

   for (i = 0; i < rbg0width; i++)
   {
      u32 color, dot;
      int x, y;
      const u32 cdx = dxi, cdy = dyi, cdx2 = dxi2, cdy2 = dyi2;
      dxi += (u32)p->dX;
      dyi += (u32)p->dY;
      if (p2 != NULL)
      {
         dxi2 += (u32)p2->dX;
         dyi2 += (u32)p2->dY;
      }

      if (p->deltaKAx != 0)
      {
         Vdp2ReadCoefficientFPInline(p, p->coeftbladdr + (L->coefy + coefx + toint(rcoefx + L->rcoefy)) * p->coefdatasize, ram);
         coefx += toint(p->deltaKAx);
         rcoefx += decipart(p->deltaKAx);
      }
      if (p2_coef && (p2->deltaKAx != 0))
      {
         Vdp2ReadCoefficientFPInline(p2, p2->coeftbladdr + (L->coefy2 + coefx2 + toint(rcoefx2 + L->rcoefy2)) * p2->coefdatasize, ram);
         coefx2 += toint(p2->deltaKAx);
         rcoefx2 += decipart(p2->deltaKAx);
      }

      if (win_fixed ? !win_pass : !WIN_AT(L->wintab, info->wctl, L->clip, i, j))
         continue;

      if (((! L->userpwindow) && p->msb) || (L->userpwindow && (! WIN_AT(L->rptab, L->regs->WCTLD, L->rpwindow, i, j))))
      {
         if ((p2 == NULL) || (p2->coefenab && p2->msb)) continue;

         x = touint(mulfixed(p2->kx, (fixed32)((u32)L->Xsp2 + cdx2)) + p2->Xp);
         y = touint(mulfixed(p2->ky, (fixed32)((u32)L->Ysp2 + cdy2)) + p2->Yp);

         switch(p2->screenover) {
            case 0:
            case 1:
               x &= L->sinfo2->xmask;
               y &= L->sinfo2->ymask;
               break;
            case 2:
               if ((x > L->sinfo2->xmask) || (y > L->sinfo2->ymask)) continue;
               break;
            case 3:
               if ((x > 512) || (y > 512)) continue;
         }

         if (!info->isbitmap)
            Vdp2MapCalcXYFast(info, &x, &y, L->sinfo2, L->regs, ram);
      }
      else if (p->msb) continue;
      else
      {
         x = touint(mulfixed(p->kx, (fixed32)((u32)L->Xsp + cdx)) + p->Xp);
         y = touint(mulfixed(p->ky, (fixed32)((u32)L->Ysp + cdy)) + p->Yp);

         switch(p->screenover) {
            case 0:
            case 1:
               x &= L->sinfo->xmask;
               y &= L->sinfo->ymask;
               break;
            case 2:
               if ((x > L->sinfo->xmask) || (y > L->sinfo->ymask)) continue;
               break;
            case 3:
               if ((x > 512) || (y > 512)) continue;
         }

         if (!info->isbitmap)
            Vdp2MapCalcXYFast(info, &x, &y, L->sinfo, L->regs, ram);
      }

      if (!Vdp2FetchPixelCN(colornumber, colormode, info, x, y, &color, &dot, ram, info->charaddr, info->paladdr, color_ram))
         continue;

      if (L->dotlc)
      {
         /* which parameter drew this pixel: the same test as above */
         const int use2 = ((! L->userpwindow) && p->msb) || (L->userpwindow && (! WIN_AT(L->rptab, L->regs->WCTLD, L->rpwindow, i, j)));
         u32 addr = L->dotlc_word;
         if (use2 ? L->dotlc_en2 : L->dotlc_en)
            addr = (L->dotlc_word & 0x780) | (use2 ? p2->linescreen : p->linescreen);
         if (addr != L->dotlc_last)
         {
            L->dotlc_last = addr;
            L->dotlc_color = COLSAT2YAB32(0x3F, Vdp2ColorRamGetColor(addr, color_ram));
         }
         L->dotlc[j * vdp2width + i] = L->dotlc_color;
      }

      if (priority == 0)
         continue;
      if (!line)
      {
         /* TitanPutPixel() clears a line that is stale from an earlier frame
          * on its first write */
         line = TitanGetLayerLine(layer, j);
         if (TitanClaimLayerLine(layer, j))
            memset(line, 0, sizeof(struct PixelData) * vdp2width);
      }
      {
         u32 pixel = COLSAT2YAB32(GetAlpha(info, color, dot), color);
         if (post_calc == 1)
            pixel = DoColorOffset(info, pixel);
         else if (post_calc == 2)
            pixel = info->PostPixelFetchCalc(info, pixel);
         line[i].pixel = pixel;
         line[i].meta = meta;
      }
   }
}

/* Vdp2ColorRamGetColorCM(), always inlined (the compiler kept it out of line
 * in the rotation loop, a call per pixel) */
static INLINE __attribute__((always_inline)) u32 Vdp2ColorRamGetColorCMI(const int colormode, u32 addr, const u8 *vdp2_color_ram)
{
   if (colormode == 0 || colormode == 1)
   {
      u32 tmp = T2ReadWord((u8 *)vdp2_color_ram, (addr << 1) & 0xFFF);
      return (((tmp & 0x1F) << 3) | ((tmp & 0x03E0) << 6) | ((tmp & 0x7C00) << 9)) | ((tmp & 0x8000) << 16);
   }
   if (colormode == 2)
      return T2ReadLong((u8 *)vdp2_color_ram, (addr << 2) & 0xFFF);
   return 0;
}

/* One line of the coefficient loop when every coefficient of the line is the
 * same (coefficients per line: deltaKAx == 0 for both parameters), without a
 * rotation parameter window or per-dot line colours: the line's parameter
 * (A, or B where A's coefficient MSB is set) is chosen once, as are the
 * screen-over rule and GetAlpha()'s rule, and the colour lookup is inlined.
 * Same steps and results as Vdp2DrawRotationCoefLine(). CN is the colour
 * format, BM 1 for a bitmap screen; colormode is passed through for the
 * colour lookup. Used by Panzer Dragoon, Shining Force III (bitmap) and
 * others for their rotating floors and backgrounds. */
static INLINE __attribute__((always_inline)) void Vdp2DrawRotationKLineBody(const int CN, const int BM, const int colormode,
   vdp2draw_struct *info, vdp2rotationparameterfp_struct *q, screeninfo_struct *qs, fixed32 Xs, fixed32 Ys,
   Vdp2 *regs, u8 *ram, u8 *color_ram, const u8 *wintab, clipping_struct *clip, int j)
{
   const int win_fixed = (info->wctl & 0x2a) == 0, win_pass = !(info->wctl & 0x80);
   const int wctl = info->wctl;
   const int post_calc = info->PostPixelFetchCalc == &DoNothing ? 0 :
                         info->PostPixelFetchCalc == &DoColorOffset ? 1 : 2;
   const int layer = info->titan_which_layer;
   const u32 meta = TitanPackMeta(info->priority, info->linescreen, info->titan_shadow_type, info->titan_shadow_enabled);
   /* GetAlpha(), inlined: the mode and code are per screen, the special colour
    * function bit is per tile (Vdp2PatternAddr() sets it) */
   const int scmode = info->specialcolormode;
   const u8 alpha = info->alpha;
   const int specialcode = info->specialcode;
   const fixed32 kx = q->kx, ky = q->ky, Xp = q->Xp, Yp = q->Yp;
   const u32 dX = (u32)q->dX, dY = (u32)q->dY;
   const int over = q->screenover, xmask = qs->xmask, ymask = qs->ymask;
   const int transparent = info->transparencyenable;
   const int cellw = info->cellw, coloroffset = info->coloroffset;
   struct PixelData *line = NULL;
   u32 ax = (u32)Xs, ay = (u32)Ys;
   int i;

   for (i = 0; i < rbg0width; i++)
   {
      u32 color, dot, pixel;
      int x, y, a;
      const u32 cx = ax, cy = ay;
      ax += dX;
      ay += dY;

      if (win_fixed ? !win_pass : !WIN_AT(wintab, wctl, clip, i, j))
         continue;

      x = touint(mulfixed(kx, (fixed32)cx) + Xp);
      y = touint(mulfixed(ky, (fixed32)cy) + Yp);
      if (over <= 1)
      {
         x &= xmask;
         y &= ymask;
      }
      else if (over == 2)
      {
         if ((x > xmask) || (y > ymask)) continue;
      }
      else if ((x > 512) || (y > 512))
         continue;

      if (!BM)
         Vdp2MapCalcXYFast(info, &x, &y, qs, regs, ram);

      {
         const int charaddr = info->charaddr, paladdr = info->paladdr;
         if (CN == 0)
         {
            dot = T1ReadByte(ram, ((charaddr + ((y * cellw) + x) / 2) & 0x7FFFF));
            if (!(x & 0x1)) dot >>= 4;
            if (!(dot & 0xF) && transparent) continue;
            color = Vdp2ColorRamGetColorCMI(colormode, coloroffset + (paladdr | (dot & 0xF)), color_ram);
         }
         else if (CN == 1)
         {
            dot = T1ReadByte(ram, ((charaddr + (y * cellw) + x) & 0x7FFFF));
            if (!(dot & 0xFF) && transparent) continue;
            color = Vdp2ColorRamGetColorCMI(colormode, coloroffset + (paladdr | (dot & 0xFF)), color_ram);
         }
         else if (CN == 2)
         {
            dot = T1ReadWord(ram, ((charaddr + ((y * cellw) + x) * 2) & 0x7FFFF));
            if ((dot == 0) && transparent) continue;
            color = Vdp2ColorRamGetColorCMI(colormode, coloroffset + dot, color_ram);
         }
         else if (CN == 3)
         {
            dot = T1ReadWord(ram, ((charaddr + ((y * cellw) + x) * 2) & 0x7FFFF));
            if (!(dot & 0x8000) && transparent) continue;
            color = COLSAT2YAB16(0, dot);
         }
         else /* CN == 4 */
         {
            dot = T1ReadLong(ram, ((charaddr + ((y * cellw) + x) * 4) & 0x7FFFF));
            if (!(dot & 0x80000000) && transparent) continue;
            color = COLSAT2YAB32(0, dot);
         }
      }

      if (!line)
      {
         /* TitanPutPixel() clears a line that is stale from an earlier frame
          * on its first write */
         line = TitanGetLayerLine(layer, j);
         if (TitanClaimLayerLine(layer, j))
            memset(line, 0, sizeof(struct PixelData) * vdp2width);
      }
      if ((scmode == 1 || scmode == 2) && !(info->specialcolorfunction & 1))
         a = 0x3F;
      else if (scmode == 2)
         a = (specialcode & (1 << ((dot & 0xF) >> 1))) ? alpha : 0x3F;
      else if (scmode == 3 && !(color & 0x80000000))
         a = 0x3F;
      else
         a = alpha;
      pixel = COLSAT2YAB32(a, color);
      if (post_calc == 1)
         pixel = DoColorOffset(info, pixel);
      else if (post_calc == 2)
         pixel = info->PostPixelFetchCalc(info, pixel);
      line[i].pixel = pixel;
      line[i].meta = meta;
   }
}

#define ROT_KLINE_FN(NAME, CN, BM) \
static void NAME(vdp2draw_struct *info, vdp2rotationparameterfp_struct *q, screeninfo_struct *qs, fixed32 Xs, fixed32 Ys, \
   Vdp2 *regs, u8 *ram, u8 *color_ram, const u8 *wintab, clipping_struct *clip, int j) \
{ \
   switch (Vdp2Internal.ColorMode) \
   { \
   case 0: Vdp2DrawRotationKLineBody(CN, BM, 0, info, q, qs, Xs, Ys, regs, ram, color_ram, wintab, clip, j); break; \
   case 1: Vdp2DrawRotationKLineBody(CN, BM, 1, info, q, qs, Xs, Ys, regs, ram, color_ram, wintab, clip, j); break; \
   case 2: Vdp2DrawRotationKLineBody(CN, BM, 2, info, q, qs, Xs, Ys, regs, ram, color_ram, wintab, clip, j); break; \
   default: Vdp2DrawRotationKLineBody(CN, BM, 3, info, q, qs, Xs, Ys, regs, ram, color_ram, wintab, clip, j); break; \
   } \
}
ROT_KLINE_FN(Vdp2RotKLine4bpp, 0, 0)
ROT_KLINE_FN(Vdp2RotKLine8bpp, 1, 0)
ROT_KLINE_FN(Vdp2RotKLine16bppPal, 2, 0)
ROT_KLINE_FN(Vdp2RotKLine16bppRGB, 3, 0)
ROT_KLINE_FN(Vdp2RotKLine32bpp, 4, 0)
ROT_KLINE_FN(Vdp2RotKLine4bppBM, 0, 1)
ROT_KLINE_FN(Vdp2RotKLine8bppBM, 1, 1)
ROT_KLINE_FN(Vdp2RotKLine16bppPalBM, 2, 1)
ROT_KLINE_FN(Vdp2RotKLine16bppRGBBM, 3, 1)
ROT_KLINE_FN(Vdp2RotKLine32bppBM, 4, 1)
#undef ROT_KLINE_FN

static void FASTCALL Vdp2DrawRotationFP(vdp2draw_struct *info, vdp2rotationparameterfp_struct *parameter, Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
{
   int i, j;
   u8 *const winbuf = vidsoft_win_buf[info->titan_which_layer][0], *const rpbuf = vidsoft_win_buf[info->titan_which_layer][1];
   const u8 *wintab = NULL, *rptab = NULL;
   int x, y;
   screeninfo_struct sinfo;
   vdp2rotationparameterfp_struct *p=&parameter[info->rotatenum];
   clipping_struct clip[2];
   u32 linewnd0addr, linewnd1addr;

   clip[0].xstart = clip[0].ystart = clip[0].xend = clip[0].yend = 0;
   clip[1].xstart = clip[1].ystart = clip[1].xend = clip[1].yend = 0;
   ReadWindowData(info->wctl, clip, regs);
   linewnd0addr = linewnd1addr = 0;
   ReadLineWindowData(&info->islinewindow, info->wctl, &linewnd0addr, &linewnd1addr, regs);

   Vdp2ReadRotationTableFP(info->rotatenum, p, regs, ram);

   if (!p->coefenab)
   {
      fixed32 xmul, ymul, C, F;

      // Since coefficients aren't being used, we can simplify the drawing process
      if (IsScreenRotatedFP(p))
      {
         // No rotation
         info->x = touint(mulfixed(p->kx, (p->Xst - p->Px)) + p->Px + p->Mx);
         info->y = touint(mulfixed(p->ky, (p->Yst - p->Py)) + p->Py + p->My);
         info->coordincx = tofloat(p->kx);
         info->coordincy = tofloat(p->ky);
      }
      else
      {
         GenerateRotatedVarFP(p, &xmul, &ymul, &C, &F);

         // Do simple rotation
         CalculateRotationValuesFP(p);

         SetupScreenVars(info, &sinfo, info->PlaneAddr, regs);

         for (j = 0; j < vdp2height; j++)
         {
            info->LoadLineParams(info, &sinfo, j, lines);
            ReadLineWindowClip(info->islinewindow, clip, &linewnd0addr, &linewnd1addr, ram, regs);
            wintab = (info->wctl & 0x2a) && rbg0width <= 1024 ? VidsoftWinTable(info->wctl, clip, j, rbg0width, winbuf) : NULL;

            for (i = 0; i < rbg0width; i++)
            {
               u32 color, dot;

               if (!WIN_AT(wintab, info->wctl, clip, i, j))
                  continue;

               x = GenerateRotatedXPosFP(p, i, xmul, ymul, C) & sinfo.xmask;
               y = GenerateRotatedYPosFP(p, i, xmul, ymul, F) & sinfo.ymask;

               // Convert coordinates into graphics
               if (!info->isbitmap)
               {
                  // Tile
                  Vdp2MapCalcXY(info, &x, &y, &sinfo, regs, ram,0);
               }
 
               // Fetch pixel
               if (!Vdp2FetchPixel(info, x, y, &color, &dot, ram, info->charaddr,info->paladdr, color_ram))
               {
                  continue;
               }

               Rbg0PutPixel(info, color, dot, i, j);
            }
            xmul += p->deltaXst;
            ymul += p->deltaYst;
         }

         return;
      }
   }
   else
   {
      fixed32 xmul, ymul, C, F;
      u32 coefx, coefy;
      u32 rcoefx, rcoefy;
      u32 lineAddr, lineColor, lineInc;
      u16 lineColorAddr;

      fixed32 xmul2, ymul2, C2, F2;
      u32 coefx2, coefy2;
      u32 rcoefx2, rcoefy2;
      screeninfo_struct sinfo2;
      vdp2rotationparameterfp_struct *p2 = NULL;

      clipping_struct rpwindow[2];
      int userpwindow = 0;
      u32 *dotlc = NULL;          /* per-pixel line colours, see above */
      int dotlc_en = 0, dotlc_en2 = 0;
      u32 dotlc_word = 0, dotlc_last = 0xFFFFFFFF, dotlc_color = 0;
      int isrplinewindow = 0;
      u32 rplinewnd0addr, rplinewnd1addr;

      if ((regs->RPMD & 3) == 2)
         p2 = &parameter[1 - info->rotatenum];
      else if ((regs->RPMD & 3) == 3)
      {
         ReadWindowData(regs->WCTLD, rpwindow, regs);
         rplinewnd0addr = rplinewnd1addr = 0;
         ReadLineWindowData(&isrplinewindow, regs->WCTLD, &rplinewnd0addr, &rplinewnd1addr, regs);
         userpwindow = 1;
         p2 = &parameter[1 - info->rotatenum];
      }

      GenerateRotatedVarFP(p, &xmul, &ymul, &C, &F);

      // Rotation using Coefficient Tables(now this stuff just gets wacky. It
      // has to be done in software, no exceptions)
      CalculateRotationValuesFP(p);

      SetupScreenVars(info, &sinfo, p->PlaneAddr, regs);
      coefx = coefy = 0;
      rcoefx = rcoefy = 0;

      if (p2 != NULL)
      {
         Vdp2ReadRotationTableFP(1 - info->rotatenum, p2, regs, ram);
         GenerateRotatedVarFP(p2, &xmul2, &ymul2, &C2, &F2);
         CalculateRotationValuesFP(p2);
         SetupScreenVars(info, &sinfo2, p2->PlaneAddr, regs);
         coefx2 = coefy2 = 0;
         rcoefx2 = rcoefy2 = 0;
      }

      if (Rbg0CheckRam(regs))//sonic r / all star baseball 97
      {
         if (p->coefenab && p->coefmode == 0)
         {
            p->deltaKAx = 0;
         }

         if (p2 && p2->coefenab && p2->coefmode == 0)
         {
            p2->deltaKAx = 0;
         }
      }

      if (info->linescreen)
      {
         if ((info->rotatenum == 0) && (regs->KTCTL & 0x10))
            info->linescreen = 2;
         else if (regs->KTCTL & 0x1000)
            info->linescreen = 3;
         if (regs->VRSIZE & 0x8000)
            lineAddr = (regs->LCTA.all & 0x7FFFF) << 1;
         else
            lineAddr = (regs->LCTA.all & 0x3FFFF) << 1;

         lineInc = regs->LCTA.part.U & 0x8000 ? 2 : 0;

         /* the coefficient's line colour bits replace the low 7 bits of the
            line colour table's address; with per-dot coefficients that is
            per pixel */
         if (info->linescreen > 1 && vidsoft_rotation_dot_linecolor &&
             (p->deltaKAx != 0 || (p2 != NULL && p2->coefenab && p2->deltaKAx != 0)))
         {
            const int a_is_p = p == &parameter[0];
            dotlc = TitanGetDotLineColors(info->rotatenum);
            if (dotlc)
            {
               dotlc_en = (regs->KTCTL & (a_is_p ? 0x10 : 0x1000)) != 0;
               dotlc_en2 = (regs->KTCTL & (a_is_p ? 0x1000 : 0x10)) != 0;
               info->linescreen = TITAN_LINESCREEN_DOT + info->rotatenum;
            }
         }
      }

      for (j = 0; j < rbg0height; j++)
      {
         if (p->deltaKAx == 0)
         {
            Vdp2ReadCoefficientFP(p,
                                  p->coeftbladdr +
                                  (coefy + touint(rcoefy)) *
                                  p->coefdatasize, ram);
         }
         if ((p2 != NULL) && p2->coefenab && (p2->deltaKAx == 0))
         {
            Vdp2ReadCoefficientFP(p2,
                                  p2->coeftbladdr +
                                  (coefy2 + touint(rcoefy2)) *
                                  p2->coefdatasize, ram);
         }

         if (dotlc)
         {
            dotlc_word = T1ReadWord(ram, lineAddr) & 0x7FF;
            lineAddr += lineInc;
         }
         else if (info->linescreen > 1)
         {
            lineColorAddr = (T1ReadWord(ram, lineAddr) & 0x780) | p->linescreen;
            lineColor = Vdp2ColorRamGetColor(lineColorAddr, color_ram);
            lineAddr += lineInc;
            TitanPutLineHLine(info->linescreen, j, COLSAT2YAB32(0x3F, lineColor));
         }

         info->LoadLineParams(info, &sinfo, j, lines);
         ReadLineWindowClip(info->islinewindow, clip, &linewnd0addr, &linewnd1addr, ram, regs);

         if (userpwindow)
            ReadLineWindowClip(isrplinewindow, rpwindow, &rplinewnd0addr, &rplinewnd1addr, ram, regs);

         wintab = (info->wctl & 0x2a) && rbg0width <= 1024 ? VidsoftWinTable(info->wctl, clip, j, rbg0width, winbuf) : NULL;
         rptab = userpwindow && rbg0width <= 1024 ? VidsoftWinTable(regs->WCTLD, rpwindow, j, rbg0width, rpbuf) : NULL;

         if (!vdp2_x_hires)
         {
            struct RotCoefLine L;
            L.info = info; L.p = p; L.p2 = p2; L.sinfo = &sinfo; L.sinfo2 = &sinfo2;
            L.regs = regs; L.ram = ram; L.color_ram = color_ram;
            L.clip = clip; L.rpwindow = rpwindow; L.userpwindow = userpwindow;
            L.wintab = wintab; L.rptab = rptab;
            L.Xsp = mulfixed(p->A, xmul) + mulfixed(p->B, ymul) + C;
            L.Ysp = mulfixed(p->D, xmul) + mulfixed(p->E, ymul) + F;
            if (p2 != NULL)
            {
               L.Xsp2 = mulfixed(p2->A, xmul2) + mulfixed(p2->B, ymul2) + C2;
               L.Ysp2 = mulfixed(p2->D, xmul2) + mulfixed(p2->E, ymul2) + F2;
            }
            L.coefy = coefy; L.rcoefy = rcoefy;
            if (p2 != NULL) { L.coefy2 = coefy2; L.rcoefy2 = rcoefy2; }
            L.dotlc = dotlc; L.dotlc_en = dotlc_en; L.dotlc_en2 = dotlc_en2;
            L.dotlc_word = dotlc_word; L.dotlc_last = dotlc_last; L.dotlc_color = dotlc_color;
            L.j = j;
            /* (priority 0 draws nothing but still walks the tiles, whose
             * lookup state carries over: left to the full loop) */
            if (p->deltaKAx == 0 && (p2 == NULL || !p2->coefenab || p2->deltaKAx == 0) && !dotlc && !userpwindow &&
                info->colornumber <= 4 && info->priority != 0)
            {
               /* coefficients per line: one parameter for the whole line */
               vdp2rotationparameterfp_struct *q = NULL;
               screeninfo_struct *qs = NULL;
               fixed32 Xs = 0, Ys = 0;
               if (!p->msb)
               {
                  q = p; qs = &sinfo; Xs = L.Xsp; Ys = L.Ysp;
               }
               else if (p2 != NULL && !(p2->coefenab && p2->msb))
               {
                  q = p2; qs = &sinfo2; Xs = L.Xsp2; Ys = L.Ysp2;
               }
               if (q != NULL)
               {
                  static void (*const kline[2][5])(vdp2draw_struct *, vdp2rotationparameterfp_struct *, screeninfo_struct *,
                                                    fixed32, fixed32, Vdp2 *, u8 *, u8 *, const u8 *, clipping_struct *, int) = {
                     { Vdp2RotKLine4bpp, Vdp2RotKLine8bpp, Vdp2RotKLine16bppPal, Vdp2RotKLine16bppRGB, Vdp2RotKLine32bpp },
                     { Vdp2RotKLine4bppBM, Vdp2RotKLine8bppBM, Vdp2RotKLine16bppPalBM, Vdp2RotKLine16bppRGBBM, Vdp2RotKLine32bppBM } };
                  kline[info->isbitmap ? 1 : 0][info->colornumber](info, q, qs, Xs, Ys, regs, ram, color_ram, wintab, clip, j);
               }
            }
            else
            switch (info->colornumber)
            {
               case 0: Vdp2DrawRotationCoefLine(0, &L); break;
               case 1: Vdp2DrawRotationCoefLine(1, &L); break;
               case 2: Vdp2DrawRotationCoefLine(2, &L); break;
               case 3: Vdp2DrawRotationCoefLine(3, &L); break;
               case 4: Vdp2DrawRotationCoefLine(4, &L); break;
               default: Vdp2DrawRotationCoefLine(info->colornumber, &L); break;
            }
            dotlc_last = L.dotlc_last; dotlc_color = L.dotlc_color;
         }
         else
         for (i = 0; i < rbg0width; i++)
         {
            u32 color, dot;

            if (p->deltaKAx != 0)
            {
               Vdp2ReadCoefficientFP(p,
                                     p->coeftbladdr +
                                     (coefy + coefx + toint(rcoefx + rcoefy)) *
                                     p->coefdatasize, ram);
               coefx += toint(p->deltaKAx);
               rcoefx += decipart(p->deltaKAx);
            }
            if ((p2 != NULL) && p2->coefenab && (p2->deltaKAx != 0))
            {
               Vdp2ReadCoefficientFP(p2,
                                     p2->coeftbladdr +
                                     (coefy2 + coefx2 + toint(rcoefx2 + rcoefy2)) *
                                     p2->coefdatasize, ram);
               coefx2 += toint(p2->deltaKAx);
               rcoefx2 += decipart(p2->deltaKAx);
            }

            if (!WIN_AT(wintab, info->wctl, clip, i, j))
               continue;

            if (((! userpwindow) && p->msb) || (userpwindow && (! WIN_AT(rptab, regs->WCTLD, rpwindow, i, j))))
            {
               if ((p2 == NULL) || (p2->coefenab && p2->msb)) continue;

               x = GenerateRotatedXPosFP(p2, i, xmul2, ymul2, C2);
               y = GenerateRotatedYPosFP(p2, i, xmul2, ymul2, F2);

               switch(p2->screenover) {
                  case 0:
                     x &= sinfo2.xmask;
                     y &= sinfo2.ymask;
                     break;
                  case 1:
                     VDP2LOG("Screen-over mode 1 not implemented");
                     x &= sinfo2.xmask;
                     y &= sinfo2.ymask;
                     break;
                  case 2:
                     if ((x > sinfo2.xmask) || (y > sinfo2.ymask)) continue;
                     break;
                  case 3:
                     if ((x > 512) || (y > 512)) continue;
               }

               // Convert coordinates into graphics
               if (!info->isbitmap)
               {
                  // Tile
                  Vdp2MapCalcXY(info, &x, &y, &sinfo2, regs, ram, 0);
               }
            }
            else if (p->msb) continue;
            else
            {
               x = GenerateRotatedXPosFP(p, i, xmul, ymul, C);
               y = GenerateRotatedYPosFP(p, i, xmul, ymul, F);

               switch(p->screenover) {
                  case 0:
                     x &= sinfo.xmask;
                     y &= sinfo.ymask;
                     break;
                  case 1:
                     VDP2LOG("Screen-over mode 1 not implemented");
                     x &= sinfo.xmask;
                     y &= sinfo.ymask;
                     break;
                  case 2:
                     if ((x > sinfo.xmask) || (y > sinfo.ymask)) continue;
                     break;
                  case 3:
                     if ((x > 512) || (y > 512)) continue;
               }

               // Convert coordinates into graphics
               if (!info->isbitmap)
               {
                  // Tile
                  Vdp2MapCalcXY(info, &x, &y, &sinfo, regs, ram, 0);
               }
            }

            // Fetch pixel
            if (!Vdp2FetchPixel(info, x, y, &color, &dot, ram, info->charaddr, info->paladdr, color_ram))
            {
               continue;
            }

            if (dotlc)
            {
               /* which parameter drew this pixel: the same test as above */
               const int use2 = ((! userpwindow) && p->msb) || (userpwindow && (! WIN_AT(rptab, regs->WCTLD, rpwindow, i, j)));
               u32 addr = dotlc_word;
               if (use2 ? dotlc_en2 : dotlc_en)
                  addr = (dotlc_word & 0x780) | (use2 ? p2->linescreen : p->linescreen);
               if (addr != dotlc_last)
               {
                  dotlc_last = addr;
                  dotlc_color = COLSAT2YAB32(0x3F, Vdp2ColorRamGetColor(addr, color_ram));
               }
               if (vdp2_x_hires)
                  dotlc[j * vdp2width + i * 2] = dotlc[j * vdp2width + i * 2 + 1] = dotlc_color;
               else
                  dotlc[j * vdp2width + i] = dotlc_color;
            }

            Rbg0PutPixel(info, color, dot, i, j);
         }
         xmul += p->deltaXst;
         ymul += p->deltaYst;
         coefx = 0;
         rcoefx = 0;
         coefy += toint(p->deltaKAst);
         rcoefy += decipart(p->deltaKAst);

         if (p2 != NULL)
         {
            xmul2 += p2->deltaXst;
            ymul2 += p2->deltaYst;
            if (p2->coefenab)
            {
               coefx2 = 0;
               rcoefx2 = 0;
               coefy2 += toint(p2->deltaKAst);
               rcoefy2 += decipart(p2->deltaKAst);
            }
         }
      }
      return;
   }

   Vdp2DrawScroll(info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawBackScreen(void)
{
   int i, j;

   // Only draw black if TVMD's DISP and BDCLMD bits are cleared
   if ((Vdp2Regs->TVMD & 0x8000) == 0 && (Vdp2Regs->TVMD & 0x100) == 0)
   {
      // Draw Black
      for (j = 0; j < vdp2height; j++)
         TitanPutBackHLine(j, COLSAT2YAB32(0x3F, 0));
   }
   else
   {
      // Draw Back Screen
      u32 scrAddr;
      u16 dot;
      vdp2draw_struct info = { 0 };

      ReadVdp2ColorOffset(Vdp2Regs, &info, (1 << 5), 0);

      if (Vdp2Regs->VRSIZE & 0x8000)
         scrAddr = (((Vdp2Regs->BKTAU & 0x7) << 16) | Vdp2Regs->BKTAL) * 2;
      else
         scrAddr = (((Vdp2Regs->BKTAU & 0x3) << 16) | Vdp2Regs->BKTAL) * 2;

      if (Vdp2Regs->BKTAU & 0x8000)
      {
         // Per Line
         for (i = 0; i < vdp2height; i++)
         {
            dot = T1ReadWord(Vdp2Ram, scrAddr);
            scrAddr += 2;

            TitanPutBackHLine(i, info.PostPixelFetchCalc(&info, COLSAT2YAB16(0x3f, dot)));
         }
      }
      else
      {
         // Single Color
         dot = T1ReadWord(Vdp2Ram, scrAddr);

         for (j = 0; j < vdp2height; j++)
            TitanPutBackHLine(j, info.PostPixelFetchCalc(&info, COLSAT2YAB16(0x3f, dot)));
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

static void Vdp2DrawLineScreen(void)
{
   u32 scrAddr;
   u16 color;
   u32 dot;
   int i;
	int alpha;

   /* no need to go further if no screen is using the line screen */
   if (Vdp2Regs->LNCLEN == 0)
      return;

   if (Vdp2Regs->VRSIZE & 0x8000)
      scrAddr = (Vdp2Regs->LCTA.all & 0x7FFFF) << 1;
   else
      scrAddr = (Vdp2Regs->LCTA.all & 0x3FFFF) << 1;

   alpha = (Vdp2Regs->CCRLB & 0x1f) << 1;

   if (Vdp2Regs->LCTA.part.U & 0x8000)
   {
      /* per line */
      for (i = 0; i < vdp2height; i++)
      {
         color = T1ReadWord(Vdp2Ram, scrAddr) & 0x7FF;
         dot = Vdp2ColorRamGetColor(color, Vdp2ColorRam);
         scrAddr += 2;

         TitanPutLineHLine(1, i, COLSAT2YAB32(alpha, dot));
      }
   }
   else
   {
      /* single color, implemented but not tested... */
      color = T1ReadWord(Vdp2Ram, scrAddr) & 0x7FF;
      dot = Vdp2ColorRamGetColor(color, Vdp2ColorRam);
      for (i = 0; i < vdp2height; i++)
         TitanPutLineHLine(1, i, COLSAT2YAB32(alpha, dot));
   }
}

//////////////////////////////////////////////////////////////////////////////

/* Colour calculation per line (U21, Die Hard Arcade): a game may switch CCCTL
   or change the ratios (CCRNA/CCRNB/CCRR) in the middle of the display, a
   raster split. The layer setup reads them once, as they are at VBlank-OUT;
   when they differ between the lines of the display just captured
   (Vdp2Lines), vidsoft_cc_lines is set and every layer line takes its enable
   and ratio from its own line (LoadLineParams*). Only then: the lines are a
   frame old, so a game that changes them during the VBlank (a fade) still
   sees the change in this frame, as before. Set by VidsoftCheckColorCalcLines
   before the layer threads are woken. */
static int vidsoft_cc_lines = 0;

/* info->alpha of a layer: bit = its CCCTL enable bit, ratio = its 5-bit
   colour calculation ratio */
static __attribute__((noinline)) int VidsoftLayerAlpha(const Vdp2 *regs, u16 bit, u16 ratio)
{
   int alpha;
   if (regs->CCCTL & (0x200 | bit))
      alpha = ((~ratio & 0x1F) << 1) + 1;
   else
      alpha = 0x3F;
   if ((regs->CCCTL & (0x200 | bit)) == (0x200 | bit)) alpha |= 0x80;
   else if ((regs->CCCTL & (0x100 | bit)) == (0x100 | bit)) alpha |= 0x80;
   return alpha;
}

static void LoadLineParamsNBG0(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   if (vidsoft_cc_lines)
      info->alpha = VidsoftLayerAlpha(regs, 0x1, regs->CCRNA);
   ReadVdp2ColorOffset(regs, info, 0x1, 0x1);
   info->specialprimode = regs->SFPRMD & 0x3;
   info->enable = regs->BGON & 0x1 || regs->BGON & 0x20;//nbg0 or rbg1
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);//sonic 2, 2 player mode
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
static void Vdp2DrawNBG0Body(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data);
static void Vdp2DrawNBG0(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data) { YAB_PROF_TBEGIN(PROF_L_NBG0); Vdp2DrawNBG0Body(lines, regs, ram, color_ram, cell_data); YAB_PROF_TEND(PROF_L_NBG0); }
#define Vdp2DrawNBG0 Vdp2DrawNBG0Body
#endif
static void Vdp2DrawNBG0(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
#ifdef YAB_PROFILE
#undef Vdp2DrawNBG0
#endif
{
   vdp2draw_struct info = { 0 };
   vdp2rotationparameterfp_struct parameter[2];

   info.titan_which_layer = TITAN_NBG0;
   info.titan_shadow_enabled = (regs->SDCTL >> 0) & 1;

   parameter[0].PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
   parameter[1].PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;

   if (regs->BGON & 0x20)
   {
      // RBG1 mode
      info.enable = regs->BGON & 0x20;

      // Read in Parameter B
      Vdp2ReadRotationTableFP(1, &parameter[1], regs, ram);

      if((info.isbitmap = regs->CHCTLA & 0x2) != 0)
      {
         // Bitmap Mode
         ReadBitmapSize(&info, regs->CHCTLA >> 2, 0x3);

         info.charaddr = (regs->MPOFR & 0x70) * 0x2000;
         info.paladdr = (regs->BMPNA & 0x7) << 8;
         info.flipfunction = 0;
         info.specialfunction = 0;
         info.specialcolorfunction = (regs->BMPNA & 0x10) >> 4;
      }
      else
      {
         // Tile Mode
         info.mapwh = 4;
         ReadPlaneSize(&info, regs->PLSZ >> 12);
         ReadPatternData(&info, regs->PNCN0, regs->CHCTLA & 0x1);
      }

      info.rotatenum = 1;
      info.rotatemode = 0;
      info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;
   }
   else if (regs->BGON & 0x1)
   {
      // NBG0 mode
      info.enable = regs->BGON & 0x1;

      if((info.isbitmap = regs->CHCTLA & 0x2) != 0)
      {
         // Bitmap Mode
         ReadBitmapSize(&info, regs->CHCTLA >> 2, 0x3);

         info.x = regs->SCXIN0 & 0x7FF;
         info.y = regs->SCYIN0 & 0x7FF;

         info.charaddr = (regs->MPOFN & 0x7) * 0x20000;
         info.paladdr = (regs->BMPNA & 0x7) << 8;
         info.flipfunction = 0;
         info.specialfunction = 0;
         info.specialcolorfunction = (regs->BMPNA & 0x10) >> 4;
      }
      else
      {
         // Tile Mode
         info.mapwh = 2;

         ReadPlaneSize(&info, regs->PLSZ);

         info.x = regs->SCXIN0 & 0x7FF;
         info.y = regs->SCYIN0 & 0x7FF;
         ReadPatternData(&info, regs->PNCN0, regs->CHCTLA & 0x1);
      }

      info.coordincx = (regs->ZMXN0.all & 0x7FF00) / (float) 65536;
      info.coordincy = (regs->ZMYN0.all & 0x7FF00) / (float) 65536;
      info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG0PlaneAddr;
   }

   info.transparencyenable = !(regs->BGON & 0x100);
   info.specialprimode = regs->SFPRMD & 0x3;

   info.colornumber = (regs->CHCTLA & 0x70) >> 4;

   info.alpha = VidsoftLayerAlpha(regs, 0x1, regs->CCRNA);
   info.specialcolormode = regs->SFCCMD & 0x3;
   if (regs->SFSEL & 0x1)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x1)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFA & 0x7) << 8;
   ReadVdp2ColorOffset(regs, &info, 0x1, 0x1);
   info.priority = regs->PRINA & 0x7;

   if (!(info.enable & Vdp2External.disptoggle))
      return;

   ReadMosaicData(&info, 0x1, regs);
   ReadLineScrollData(&info, regs->SCRCTL & 0xFF, regs->LSTA0.all);
   if (regs->SCRCTL & 1)
   {
      info.isverticalscroll = 1;
      info.verticalscrolltbl = (regs->VCSTA.all & 0x7FFFE) << 1;
      if (regs->SCRCTL & 0x100)
         info.verticalscrollinc = 8;
      else
         info.verticalscrollinc = 4;
   }
   else
      info.isverticalscroll = 0;
   info.wctl = regs->WCTLA;

   info.LoadLineParams = (void (*)(void *, void *,int ,Vdp2*)) LoadLineParamsNBG0;

   if (info.enable == 1)
   {
      // NBG0 draw
      Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
   }
   else
   {
      // RBG1 draw
      Vdp2DrawRotationFP(&info, parameter, lines, regs, ram, color_ram, cell_data);
   }
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsNBG1(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   if (vidsoft_cc_lines)
      info->alpha = VidsoftLayerAlpha(regs, 0x2, regs->CCRNA >> 8);
   ReadVdp2ColorOffset(regs, info, 0x2, 0x2);
   info->specialprimode = (regs->SFPRMD >> 2) & 0x3;
   info->enable = regs->BGON & 0x2;//f1 challenge map when zoomed out
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
static void Vdp2DrawNBG1Body(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data);
static void Vdp2DrawNBG1(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data) { YAB_PROF_TBEGIN(PROF_L_NBG1); Vdp2DrawNBG1Body(lines, regs, ram, color_ram, cell_data); YAB_PROF_TEND(PROF_L_NBG1); }
#define Vdp2DrawNBG1 Vdp2DrawNBG1Body
#endif
static void Vdp2DrawNBG1(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
#ifdef YAB_PROFILE
#undef Vdp2DrawNBG1
#endif
{
   vdp2draw_struct info = { 0 };

   info.titan_which_layer = TITAN_NBG1;
   info.titan_shadow_enabled = (regs->SDCTL >> 1) & 1;

   info.enable = regs->BGON & 0x2;
   info.transparencyenable = !(regs->BGON & 0x200);
   info.specialprimode = (regs->SFPRMD >> 2) & 0x3;

   info.colornumber = (regs->CHCTLA & 0x3000) >> 12;

   if((info.isbitmap = regs->CHCTLA & 0x200) != 0)
   {
      ReadBitmapSize(&info, regs->CHCTLA >> 10, 0x3);

      info.x = regs->SCXIN1 & 0x7FF;
      info.y = regs->SCYIN1 & 0x7FF;

      info.charaddr = ((regs->MPOFN & 0x70) >> 4) * 0x20000;
      info.paladdr = regs->BMPNA & 0x700;
      info.flipfunction = 0;
      info.specialfunction = 0;
      info.specialcolorfunction = (regs->BMPNA & 0x1000) >> 12;
   }
   else
   {
      info.mapwh = 2;

      ReadPlaneSize(&info, regs->PLSZ >> 2);

      info.x = regs->SCXIN1 & 0x7FF;
      info.y = regs->SCYIN1 & 0x7FF;

      ReadPatternData(&info, regs->PNCN1, regs->CHCTLA & 0x100);
   }

   info.alpha = VidsoftLayerAlpha(regs, 0x2, regs->CCRNA >> 8);
   info.specialcolormode = (regs->SFCCMD >> 2) & 0x3;
   if (regs->SFSEL & 0x2)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x2)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFA & 0x70) << 4;
   ReadVdp2ColorOffset(regs, &info, 0x2, 0x2);
   info.coordincx = (regs->ZMXN1.all & 0x7FF00) / (float) 65536;
   info.coordincy = (regs->ZMYN1.all & 0x7FF00) / (float) 65536;

   info.priority = (regs->PRINA >> 8) & 0x7;
   info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG1PlaneAddr;

   if (!(info.enable & Vdp2External.disptoggle) ||
       (regs->BGON & 0x1 && (regs->CHCTLA & 0x70) >> 4 == 4)) // If NBG0 16M mode is enabled, don't draw
      return;

   ReadMosaicData(&info, 0x2, regs);
   ReadLineScrollData(&info, regs->SCRCTL >> 8, regs->LSTA1.all);
   if (regs->SCRCTL & 0x100)
   {
      info.isverticalscroll = 1;
      if (regs->SCRCTL & 0x1)
      {
         info.verticalscrolltbl = 4 + ((regs->VCSTA.all & 0x7FFFE) << 1);
         info.verticalscrollinc = 8;
      }
      else
      {
         info.verticalscrolltbl = (regs->VCSTA.all & 0x7FFFE) << 1;
         info.verticalscrollinc = 4;
      }
   }
   else
      info.isverticalscroll = 0;
   info.wctl = regs->WCTLA >> 8;

   info.LoadLineParams = (void(*)(void *, void*, int, Vdp2*)) LoadLineParamsNBG1;

   Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsNBG2(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   if (vidsoft_cc_lines)
      info->alpha = VidsoftLayerAlpha(regs, 0x4, regs->CCRNB);
   ReadVdp2ColorOffset(regs, info, 0x4, 0x4);
   info->specialprimode = (regs->SFPRMD >> 4) & 0x3;
   info->enable = regs->BGON & 0x4;
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
static void Vdp2DrawNBG2Body(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data);
static void Vdp2DrawNBG2(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data) { YAB_PROF_TBEGIN(PROF_L_NBG2); Vdp2DrawNBG2Body(lines, regs, ram, color_ram, cell_data); YAB_PROF_TEND(PROF_L_NBG2); }
#define Vdp2DrawNBG2 Vdp2DrawNBG2Body
#endif
static void Vdp2DrawNBG2(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
#ifdef YAB_PROFILE
#undef Vdp2DrawNBG2
#endif
{
   vdp2draw_struct info = { 0 };

   info.titan_which_layer = TITAN_NBG2;
   info.titan_shadow_enabled = (regs->SDCTL >> 2) & 1;

   info.enable = regs->BGON & 0x4;
   info.transparencyenable = !(regs->BGON & 0x400);
   info.specialprimode = (regs->SFPRMD >> 4) & 0x3;

   info.colornumber = (regs->CHCTLB & 0x2) >> 1;	
   info.mapwh = 2;

   ReadPlaneSize(&info, regs->PLSZ >> 4);
   info.x = regs->SCXN2 & 0x7FF;
   info.y = regs->SCYN2 & 0x7FF;
   ReadPatternData(&info, regs->PNCN2, regs->CHCTLB & 0x1);
    
   info.alpha = VidsoftLayerAlpha(regs, 0x4, regs->CCRNB);
   info.specialcolormode = (regs->SFCCMD >> 4) & 0x3;
   if (regs->SFSEL & 0x4)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x4)
      info.linescreen = 1;

   info.coloroffset = regs->CRAOFA & 0x700;
   ReadVdp2ColorOffset(regs, &info, 0x4, 0x4);
   info.coordincx = info.coordincy = 1;

   info.priority = regs->PRINB & 0x7;
   info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG2PlaneAddr;

   if (!(info.enable & Vdp2External.disptoggle) ||
      (regs->BGON & 0x1 && (regs->CHCTLA & 0x70) >> 4 >= 2)) // If NBG0 2048/32786/16M mode is enabled, don't draw
      return;

   ReadMosaicData(&info, 0x4, regs);
   info.islinescroll = 0;
   info.isverticalscroll = 0;
   info.wctl = regs->WCTLB;
   info.isbitmap = 0;

   info.LoadLineParams = (void(*)(void *,void*, int, Vdp2*)) LoadLineParamsNBG2;

   Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsNBG3(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   if (vidsoft_cc_lines)
      info->alpha = VidsoftLayerAlpha(regs, 0x8, regs->CCRNB >> 8);
   ReadVdp2ColorOffset(regs, info, 0x8, 0x8);
   info->specialprimode = (regs->SFPRMD >> 6) & 0x3;
   info->enable = regs->BGON & 0x8;
   GeneratePlaneAddrTable(info, sinfo->planetbl, info->PlaneAddr, regs);
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
static void Vdp2DrawNBG3Body(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data);
static void Vdp2DrawNBG3(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data) { YAB_PROF_TBEGIN(PROF_L_NBG3); Vdp2DrawNBG3Body(lines, regs, ram, color_ram, cell_data); YAB_PROF_TEND(PROF_L_NBG3); }
#define Vdp2DrawNBG3 Vdp2DrawNBG3Body
#endif
static void Vdp2DrawNBG3(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
#ifdef YAB_PROFILE
#undef Vdp2DrawNBG3
#endif
{
   vdp2draw_struct info = { 0 };

   info.titan_which_layer = TITAN_NBG3;
   info.titan_shadow_enabled = (regs->SDCTL >> 3) & 1;

   info.enable = regs->BGON & 0x8;
   info.transparencyenable = !(regs->BGON & 0x800);
   info.specialprimode = (regs->SFPRMD >> 6) & 0x3;

   info.colornumber = (regs->CHCTLB & 0x20) >> 5;
	
   info.mapwh = 2;

   ReadPlaneSize(&info, regs->PLSZ >> 6);
   info.x = regs->SCXN3 & 0x7FF;
   info.y = regs->SCYN3 & 0x7FF;
   ReadPatternData(&info, regs->PNCN3, regs->CHCTLB & 0x10);

   info.alpha = VidsoftLayerAlpha(regs, 0x8, regs->CCRNB >> 8);
   info.specialcolormode = (regs->SFCCMD >> 6) & 0x3;
   if (regs->SFSEL & 0x8)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x8)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFA & 0x7000) >> 4;
   ReadVdp2ColorOffset(regs, &info, 0x8, 0x8);
   info.coordincx = info.coordincy = 1;

   info.priority = (regs->PRINB >> 8) & 0x7;
   info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2NBG3PlaneAddr;

   if (!(info.enable & Vdp2External.disptoggle) ||
      (regs->BGON & 0x1 && (regs->CHCTLA & 0x70) >> 4 == 4) || // If NBG0 16M mode is enabled, don't draw
      (regs->BGON & 0x2 && (regs->CHCTLA & 0x3000) >> 12 >= 2)) // If NBG1 2048/32786 is enabled, don't draw
      return;

   ReadMosaicData(&info, 0x8, regs);
   info.islinescroll = 0;
   info.isverticalscroll = 0;
   info.wctl = regs->WCTLB >> 8;
   info.isbitmap = 0;

   info.LoadLineParams = (void(*)(void *, void*, int, Vdp2*)) LoadLineParamsNBG3;

   Vdp2DrawScroll(&info, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsRBG0(vdp2draw_struct * info, screeninfo_struct * sinfo, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   if (vidsoft_cc_lines)
      info->alpha = VidsoftLayerAlpha(regs, 0x10, regs->CCRR);
   ReadVdp2ColorOffset(regs, info, 0x10, 0x10);
   info->specialprimode = (regs->SFPRMD >> 8) & 0x3;
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
static void Vdp2DrawRBG0Body(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data);
static void Vdp2DrawRBG0(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data) { YAB_PROF_TBEGIN(PROF_L_RBG0); Vdp2DrawRBG0Body(lines, regs, ram, color_ram, cell_data); YAB_PROF_TEND(PROF_L_RBG0); }
#define Vdp2DrawRBG0 Vdp2DrawRBG0Body
#endif
static void Vdp2DrawRBG0(Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data)
#ifdef YAB_PROFILE
#undef Vdp2DrawRBG0
#endif
{
   vdp2draw_struct info = { 0 };
   vdp2rotationparameterfp_struct parameter[2];

   info.titan_which_layer = TITAN_RBG0;
   info.titan_shadow_enabled = (regs->SDCTL >> 4) & 1;

   parameter[0].PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
   parameter[1].PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;

   info.enable = regs->BGON & 0x10;
   info.priority = regs->PRIR & 0x7;
   if (!(info.enable & Vdp2External.disptoggle))
      return;
   info.transparencyenable = !(regs->BGON & 0x1000);
   info.specialprimode = (regs->SFPRMD >> 8) & 0x3;

   info.colornumber = (regs->CHCTLB & 0x7000) >> 12;

   // Figure out which Rotation Parameter we're using
   switch (regs->RPMD & 0x3)
   {
      case 0:
         // Parameter A
         info.rotatenum = 0;
         info.rotatemode = 0;
         info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
         break;
      case 1:
         // Parameter B
         info.rotatenum = 1;
         info.rotatemode = 0;
         info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;
         break;
      case 2:
         // Parameter A+B switched via coefficients
      case 3:
         // Parameter A+B switched via rotation parameter window
      default:
         info.rotatenum = 0;
         info.rotatemode = 1 + (regs->RPMD & 0x1);
         info.PlaneAddr = (void FASTCALL(*)(void *, int, Vdp2*))&Vdp2ParameterAPlaneAddr;
         break;
   }

   Vdp2ReadRotationTableFP(info.rotatenum, &parameter[info.rotatenum], regs, ram);

   if((info.isbitmap = regs->CHCTLB & 0x200) != 0)
   {
      // Bitmap Mode
      ReadBitmapSize(&info, regs->CHCTLB >> 10, 0x1);

      if (info.rotatenum == 0)
         // Parameter A
         info.charaddr = (regs->MPOFR & 0x7) * 0x20000;
      else
         // Parameter B
         info.charaddr = (regs->MPOFR & 0x70) * 0x2000;

      info.paladdr = (regs->BMPNB & 0x7) << 8;
      info.flipfunction = 0;
      info.specialfunction = 0;
      info.specialcolorfunction = (regs->BMPNB & 0x10) >> 4;
   }
   else
   {
      // Tile Mode
      info.mapwh = 4;

      if (info.rotatenum == 0)
         // Parameter A
         ReadPlaneSize(&info, regs->PLSZ >> 8);
      else
         // Parameter B
         ReadPlaneSize(&info, regs->PLSZ >> 12);

      ReadPatternData(&info, regs->PNCR, regs->CHCTLB & 0x100);
   }

   info.alpha = VidsoftLayerAlpha(regs, 0x10, regs->CCRR);
   info.specialcolormode = (regs->SFCCMD >> 8) & 0x3;
   if (regs->SFSEL & 0x10)
      info.specialcode = regs->SFCODE >> 8;
   else
      info.specialcode = regs->SFCODE & 0xFF;
   info.linescreen = 0;
   if (regs->LNCLEN & 0x10)
      info.linescreen = 1;

   info.coloroffset = (regs->CRAOFB & 0x7) << 8;

   ReadVdp2ColorOffset(regs, &info, 0x10, 0x10);
   info.coordincx = info.coordincy = 1;

   ReadMosaicData(&info, 0x10, regs);
   info.islinescroll = 0;
   info.isverticalscroll = 0;
   info.wctl = regs->WCTLC;

   info.LoadLineParams = (void(*)(void *, void*, int, Vdp2*)) LoadLineParamsRBG0;

   Vdp2DrawRotationFP(&info, parameter, lines, regs, ram, color_ram, cell_data);
}

//////////////////////////////////////////////////////////////////////////////

static void LoadLineParamsSprite(vdp2draw_struct * info, int line, Vdp2* lines)
{
   Vdp2 * regs;

   regs = Vdp2RestoreRegs(line, lines);
   if (regs == NULL) return;
   ReadVdp2ColorOffset(regs, info, 0x40, 0x40);
}

//////////////////////////////////////////////////////////////////////////////

struct {
   volatile int need_draw[6];
   volatile int draw_finished[6];
   Vdp2 lines[270];
   Vdp2 regs;
   u8 ram[0x80000];
   u8 color_ram[0x1000];
   struct CellScrollData cell_scroll_data[270];
   Vdp1 vdp1regs; // the sprite thread's copy, taken when it starts
}vidsoft_thread_context;

#define DECLARE_THREAD(NAME, LAYER, FUNC) \
void NAME(void * data) \
{ \
   for (;;) \
   { \
      if (YabFlagGet(vidsoft_thread_context.need_draw[LAYER])) \
      { \
         vidsoft_thread_context.need_draw[LAYER] = 0; \
         YAB_PROF_TBEGIN(PROF_T_LAYERS); \
         FUNC(vidsoft_thread_context.lines, &vidsoft_thread_context.regs, vidsoft_thread_context.ram, vidsoft_thread_context.color_ram, vidsoft_thread_context.cell_scroll_data); \
         YAB_PROF_TEND(PROF_T_LAYERS); \
         YabFlagSet(vidsoft_thread_context.draw_finished[LAYER], 1); \
         YabFlagNotify(); \
      } \
      YabThreadSleep(); \
   } \
}

DECLARE_THREAD(VidsoftRbg0Thread, TITAN_RBG0, Vdp2DrawRBG0)
DECLARE_THREAD(VidsoftNbg0Thread, TITAN_NBG0, Vdp2DrawNBG0)
DECLARE_THREAD(VidsoftNbg1Thread, TITAN_NBG1, Vdp2DrawNBG1)
DECLARE_THREAD(VidsoftNbg2Thread, TITAN_NBG2, Vdp2DrawNBG2)
DECLARE_THREAD(VidsoftNbg3Thread, TITAN_NBG3, Vdp2DrawNBG3)

//////////////////////////////////////////////////////////////////////////////

void VIDSoftSetNumLayerThreads(int num)
{
   vidsoft_num_layer_threads = num;
}

//////////////////////////////////////////////////////////////////////////////

static u64 VidsoftThreadCpuNs(void)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
   struct timespec ts;
   if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
      return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
   return 0;
}

void VidsoftVdp1Thread(void* data)
{
   for (;;)
   {
      if (YabFlagGet(vidsoft_vdp1_thread_context.need_draw))
      {
         u64 t0 = VidsoftThreadCpuNs();
         vidsoft_vdp1_thread_context.need_draw = 0;
         vdp1_row_lo = vidsoft_vdp1_thread_context.row_lo;
         vdp1_row_hi = vidsoft_vdp1_thread_context.row_hi;
         YAB_PROF_TBEGIN(PROF_T_VDP1);
         Vdp1DrawCommands(vidsoft_vdp1_thread_context.ramp, &vidsoft_vdp1_thread_context.regs, vidsoft_vdp1_thread_context.back_framebuffer);
         YAB_PROF_TEND(PROF_T_VDP1);
         vidsoft_vdp1_thread_context.cpu_ns = VidsoftThreadCpuNs() - t0;
         YabFlagSet(vidsoft_vdp1_thread_context.draw_finished, 1);
         YabFlagNotify();
      }

      YabThreadSleep();
   }
}

void VidsoftVdp1SplitThread(void* data)
{
   for (;;)
   {
      if (YabFlagGet(vidsoft_vdp1b_context.need_draw))
      {
         u64 t0 = VidsoftThreadCpuNs();
         vidsoft_vdp1b_context.need_draw = 0;
         vdp1_tid = 1;
         vdp1_row_lo = vidsoft_vdp1b_context.row_lo;
         vdp1_row_hi = vidsoft_vdp1b_context.row_hi;
         YAB_PROF_TBEGIN(PROF_T_VDP1);
         Vdp1DrawCommands(vidsoft_vdp1_thread_context.ramp, &vidsoft_vdp1b_context.regs, vidsoft_vdp1_thread_context.back_framebuffer);
         YAB_PROF_TEND(PROF_T_VDP1);
         vidsoft_vdp1b_context.cpu_ns = VidsoftThreadCpuNs() - t0;
         YabFlagSet(vidsoft_vdp1b_context.draw_finished, 1);
         YabFlagNotify();
      }

      YabThreadSleep();
   }
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
void VidsoftWaitForVdp1ThreadBody(void);
void VidsoftWaitForVdp1Thread(void) { YAB_PROF_BEGIN(PROF_WAIT_VDP1); VidsoftWaitForVdp1ThreadBody(); YAB_PROF_END(PROF_WAIT_VDP1); }
#define VidsoftWaitForVdp1Thread VidsoftWaitForVdp1ThreadBody
#endif
void VidsoftWaitForVdp1Thread()
#ifdef YAB_PROFILE
#undef VidsoftWaitForVdp1Thread
#endif
{
   if (vidsoft_vdp1_thread_enabled)
   {
      YabFlagWait(&vidsoft_vdp1_thread_context.draw_finished);
      YabFlagWait(&vidsoft_vdp1b_context.draw_finished);
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftSetVdp1ThreadEnable(int b)
{
   vidsoft_vdp1_thread_enabled = b;

}

void VidsoftSpriteThread(void * data)
{
   for (;;)
   {
      if (YabFlagGet(vidsoft_thread_context.need_draw[TITAN_SPRITE]))
      {
         vidsoft_thread_context.need_draw[TITAN_SPRITE] = 0;
         YAB_PROF_TBEGIN(PROF_T_SPRITE);
         VidsoftDrawSprite(&vidsoft_thread_context.regs, sprite_window_mask, vdp1frontframebuffer, vidsoft_thread_context.ram, &vidsoft_thread_context.vdp1regs, vidsoft_thread_context.lines, vidsoft_thread_context.color_ram);
         YAB_PROF_TEND(PROF_T_SPRITE);
         YabFlagSet(vidsoft_thread_context.draw_finished[TITAN_SPRITE], 1);
         YabFlagNotify();
      }
      YabThreadSleep();
   }
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftInit(void)
{
   VidsoftInitMosaicTable();
   int i;

   /* the render threads' copies of VDP1/VDP2 RAM start out stale */
   VDP1_RAM_MARK_ALL();
   VDP2_RAM_MARK_ALL();

   if (TitanInit() == -1)
      return -1;

   if ((dispbuffer = (pixel_t *)calloc(sizeof(pixel_t), 704 * 512)) == NULL)
      return -1;

   // Initialize VDP1 framebuffer 1
   if ((vdp1framebuffer[0] = (u8 *)calloc(sizeof(u8), 0x40000)) == NULL)
      return -1;

   // Initialize VDP1 framebuffer 2
   if ((vdp1framebuffer[1] = (u8 *)calloc(sizeof(u8), 0x40000)) == NULL)
      return -1;

   vdp1backframebuffer = vdp1framebuffer[0];
   vdp1frontframebuffer = vdp1framebuffer[1];
   vidsoft_vdp1_front_gen++;
   {
      int i;
      for (i = 0; i < 2; i++)
      {
         if (!vdp1_job[i].ram && (vdp1_job[i].ram = (u8 *)malloc(0x80000)) == NULL)
            return -1;
         vdp1_job[i].pending = 0;
         memset(vdp1_job[i].stale, 0xFF, sizeof(vdp1_job[i].stale));
      }
      memset(vdp1_ctx_stale, 0xFF, sizeof(vdp1_ctx_stale));
      vidsoft_vdp1_thread_context.ramp = vidsoft_vdp1_thread_context.ram;
   }
   rbg0width = vdp2width = 320;
   vdp2height = 224;

#ifdef USE_OPENGL
   if (VideoUseGL)
      if (VIDSoftSetupGL() != 0)
         return -1;
#endif

   for (i = 0; i < 6; i++)
   {
      vidsoft_thread_context.draw_finished[i] = 1;
      vidsoft_thread_context.need_draw[i] = 0;
   }

   vidsoft_vdp1_thread_context.need_draw = 0;
   vidsoft_vdp1_thread_context.draw_finished = 1;
   YabThreadStart(YAB_THREAD_VIDSOFT_VDP1, VidsoftVdp1Thread, 0);
   vidsoft_vdp1b_context.need_draw = 0;
   vidsoft_vdp1b_context.draw_finished = 1;
   YabThreadStart(YAB_THREAD_VIDSOFT_VDP1B, VidsoftVdp1SplitThread, 0);

   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_RBG0, VidsoftRbg0Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG0, VidsoftNbg0Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG1, VidsoftNbg1Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG2, VidsoftNbg2Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_NBG3, VidsoftNbg3Thread, 0);
   YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_SPRITE, VidsoftSpriteThread, 0);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftSetBilinear(int b)
{
   bilinear = b;
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftSetupGL(void)
{
#ifdef USE_OPENGL
   GLint status;
   GLint texAttrib;
   GLint posAttrib;

   // Shader sources
   const GLchar* vshader_src =
      "#version 330 core\n"
      "in vec2 position;"
      "in vec2 texcoord;"
      "out vec2 outcoord;"
      "void main() {"
      "   outcoord = texcoord;"
      "   gl_Position = vec4(position, 0.0, 1.0);"
      "}";

   const GLchar* fshader_src =
      "#version 330 core\n"
      "in vec2 outcoord;"
      "out vec4 fragcolor;"
      "uniform sampler2D sattex;"
      "void main() {"
      "   fragcolor = texture(sattex, outcoord);"
      "}";

   const float vertices[16] = {
      -1.0f, -1.0f, // Vertex 1 (X, Y)
      -1.0f, 1.0f,  // Vertex 2 (X, Y)
      1.0f, -1.0f,  // Vertex 3 (X, Y)
      1.0f, 1.0f,   // Vertex 4 (X, Y)
      0.0, 1.0,     // Texture 1 (X, Y)
      0.0, 0.0,     // Texture 2 (X, Y)
      1.0, 1.0,     // Texture 3 (X, Y)
      1.0, 0.0      // Texture 4 (X, Y)
   };

   outputwidth = vdp2width;
   outputheight = vdp2height;

   glewInit();

   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);

   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

   vshader = glCreateShader(GL_VERTEX_SHADER);
   glShaderSource(vshader, 1, &vshader_src, NULL);
   glCompileShader(vshader);

   glGetShaderiv(vshader, GL_COMPILE_STATUS, &status);
   if (status == GL_FALSE)
   {
      YGLLOG("Failed to compile vertex shader\n");
      return -1;
   }

   fshader = glCreateShader(GL_FRAGMENT_SHADER);
   glShaderSource(fshader, 1, &fshader_src, NULL);
   glCompileShader(fshader);

   glGetShaderiv(fshader, GL_COMPILE_STATUS, &status);
   if (status == GL_FALSE)
   {
      YGLLOG("Failed to compile fragment shader\n");
      return -1;
   }
	
   gl_shader_prog = glCreateProgram();
   glAttachShader(gl_shader_prog, vshader);
   glAttachShader(gl_shader_prog, fshader);

   glLinkProgram(gl_shader_prog);

   glValidateProgram(gl_shader_prog);
   glGetProgramiv(gl_shader_prog, GL_LINK_STATUS, &status);
   if (status == GL_FALSE)
   {
      YGLLOG("Failed to link shader program\n");
      return -1;
   }

   glUseProgram(gl_shader_prog);
	
   posAttrib = glGetAttribLocation(gl_shader_prog, "position");
   glEnableVertexAttribArray(posAttrib);
   glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 0, 0);

   texAttrib = glGetAttribLocation(gl_shader_prog, "texcoord");
   glEnableVertexAttribArray(texAttrib);
   glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 0, (void*)(8 * sizeof(GLfloat)));

   glGenTextures(1, &gl_texture_id);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, gl_texture_id);

   if (bilinear) { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); }
   else { glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); }
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

   glViewport(0, 0, outputwidth, outputheight);

   glUniform1i(glGetUniformLocation(gl_shader_prog, "sattex"), 0);
#endif
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftDeInit(void)
{
   // The render threads may still be drawing the last frame into the
   // buffers freed below (e.g. when content is closed right after a frame)
   VidsoftWaitForVdp1Thread();
   VidsoftWaitForLayerThreads();

   if (getenv("YAB_VDP1_STATS"))
   {
      fprintf(stderr, "VDP1 frames: %lu split, %lu one thread, last split row %d\n",
              vdp1_frames_split, vdp1_frames_single, vdp1_split_row);
      fprintf(stderr, "VDP1 lazy: %lu drawn at once, %lu deferred, %lu deferred drawn (%lu at a shown frame), %lu replaced unseen, %lu backoffs\n",
              vdp1_st_eager, vdp1_st_captured, vdp1_st_run, vdp1_st_sync, vdp1_st_replaced, vdp1_st_backoffs);
   }
   vdp1_job[0].pending = vdp1_job[1].pending = 0;

   if (dispbuffer)
   {
      free(dispbuffer);
      dispbuffer = NULL;
   }

   if (vdp1framebuffer[0])
      free(vdp1framebuffer[0]);
   vdp1framebuffer[0] = NULL;

   if (vdp1framebuffer[1])
      free(vdp1framebuffer[1]);
   vdp1framebuffer[1] = NULL;
#ifdef USE_OPENGL
   if (VideoUseGL)
   {
      if (gl_texture_id) { glDeleteTextures(1, &gl_texture_id); }
      if (gl_shader_prog) { glDeleteProgram(gl_shader_prog); }
      if (vshader) { glDeleteShader(vshader); }
      if (fshader) { glDeleteShader(fshader); }
      if (vao) { glDeleteVertexArrays(1, &vao); }
      if (vbo) { glDeleteBuffers(1, &vbo); }
   }
#endif
}

//////////////////////////////////////////////////////////////////////////////

static int IsFullscreen = 0;

void VIDSoftResize(unsigned int w, unsigned int h, int on)
{
#ifdef USE_OPENGL
   if (VideoUseGL)
   {
      IsFullscreen = on;
      glClear(GL_COLOR_BUFFER_BIT);
      glViewport(0, 0, w, h);
      outputwidth = w;
      outputheight = h;
   }
#endif
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftIsFullscreen(void) {
   return IsFullscreen;
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftVdp1Reset(void)
{
   Vdp1Regs->userclipX1 = Vdp1Regs->systemclipX1 = 0;
   Vdp1Regs->userclipY1 = Vdp1Regs->systemclipY1 = 0;
   Vdp1Regs->userclipX2 = Vdp1Regs->systemclipX2 = 512;
   Vdp1Regs->userclipY2 = Vdp1Regs->systemclipY2 = 256;
   
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

/* The framebuffer mode a draw sets up (TVMR/FBCR): interlace, size, pixel
 * size. Also read by the sprite layer and the CPU framebuffer access. */
static void VidsoftVdp1SetMode(const Vdp1 *regs)
{
   int interlace = (regs->FBCR & 8) ? 2 : 1;
   int width, height, pixelsize;
   if (regs->TVMR & 0x1)
   {
      if (regs->TVMR & 0x2)
      {
         // Rotation 8-bit
         width = 512;
         height = 512;
      }
      else
      {
         // Normal 8-bit
         width = 1024;
         height = 256;
      }

      pixelsize = 1;
   }
   else
   {
      // Rotation/Normal 16-bit
      width = 512;
      height = 256;
      pixelsize = 2;
   }
   if (interlace == vdp1interlace && width == vdp1width && height == vdp1height && pixelsize == vdp1pixelsize)
      return;
   /* The sprite layer thread (started at VBlank-OUT) reads the mode while
      it composes the frame's sprites: let it finish first, so it sees what
      the single-thread path sees */
   YabFlagWait(&vidsoft_thread_context.draw_finished[TITAN_SPRITE]);
   vidsoft_vdp1_front_gen++;
   vdp1interlace = interlace;
   vdp1width = width;
   vdp1height = height;
   vdp1pixelsize = pixelsize;
}

void VIDSoftVdp1DrawStartBody(Vdp1* regs, u8 * back_framebuffer)
{
   VidsoftVdp1SetMode(regs);
   VIDSoftVdp1EraseFrameBuffer(regs, back_framebuffer);

   //night warriors doesn't set clipping most frames and uses
   //the last part of the vdp1 framebuffer as scratch ram
   //the previously set clipping values need to be reused
}
//////////////////////////////////////////////////////////////////////////////

/* Does the command list use colour mode 7? Its getpixel() keeps the
 * previous pixel's value, which depends on lines the other thread draws,
 * so such a frame isn't split. Walks the list like Vdp1DrawCommands(). */
static int Vdp1ListNeedsOneThread(u8 *ram, u32 addr)
{
   u16 command = T1ReadWord(ram, addr);
   u32 commandCounter = 0;
   u32 returnAddr = 0xffffffff;

   while (!(command & 0x8000) && commandCounter < 2000) {
      if (!(command & 0x4000)) {
         switch (command & 0x000F) {
         case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            if (((T1ReadWord(ram, addr + 4) >> 3) & 0x7) == 7)
               return 1;
            break;
         case 8: case 9: case 10: case 11:
            break;
         default:
            return 0;
         }
      }
      switch ((command & 0x3000) >> 12) {
      case 0:
         addr += 0x20;
         break;
      case 1:
         addr = T1ReadWord(ram, addr + 2) * 8;
         break;
      case 2:
         if (returnAddr == 0xFFFFFFFF)
            returnAddr = addr + 0x20;
         addr = T1ReadWord(ram, addr + 2) * 8;
         break;
      case 3:
         if (returnAddr != 0xFFFFFFFF) {
            addr = returnAddr;
            returnAddr = 0xFFFFFFFF;
         }
         else
            addr += 0x20;
         break;
      }
      command = T1ReadWord(ram, addr);
      commandCounter++;
   }
   return 0;
}

/* Deferred VDP1 drawing under frameskip (with "Skip Polygon Drawing" off;
 * YAB_VDP1_LAZY=0 draws every skipped frame's VDP1 at once instead, for
 * tests). Without it every skipped frame draws VDP1 so the
 * shown frames are exact, at the cost of drawing lists that are never
 * shown. Here a draw in a frame that isn't shown is only captured: its
 * command list (a snapshot of VDP1 RAM and registers) and whether it erases
 * are kept per framebuffer, and drawn only when that buffer is about to be
 * shown or another draw needs it as its base. The emulation side
 * (Vdp1FakeDrawCommands, swaps, draw-end timing) runs as before. */
static int VidsoftFbIndex(const u8 *fb) { return fb == vdp1framebuffer[1]; }

/* Pages written since the last sync go stale in every copy */
static void VidsoftVdp1FoldDirty(void)
{
   int w;
   for (w = 0; w < 4; w++)
   {
      vdp1_ctx_stale[w] |= Vdp1RamDirty[w];
      vdp1_job[0].stale[w] |= Vdp1RamDirty[w];
      vdp1_job[1].stale[w] |= Vdp1RamDirty[w];
      Vdp1RamDirty[w] = 0;
   }
}

/* Start the VDP1 threads on a list (worker idle, caller waited): erase if
 * asked, pick the split, wake them. */
static void VidsoftVdp1Start(u8 *ram, const Vdp1 *regs, int erase, u8 *fb)
{
   int split;

   vidsoft_vdp1_thread_context.ramp = ram;
   memcpy(&vidsoft_vdp1_thread_context.regs, regs, sizeof(Vdp1));
   vidsoft_vdp1_thread_context.back_framebuffer = fb;
   VDP1RLOG(Vdp1LogStart(ram));

   {
   YAB_PROF_BEGIN(PROF_VDP1_ERASE);
   VidsoftVdp1SetMode(&vidsoft_vdp1_thread_context.regs);
   if (erase)
      VidsoftVdp1Erase(&vidsoft_vdp1_thread_context.regs, fb);
   YAB_PROF_END(PROF_VDP1_ERASE);
   }

   if (vdp1_split_enabled < 0)
   {
      const char *env = getenv("YAB_VDP1_SPLIT");
      vdp1_split_enabled = !env ? 1 : env[0] == '0' ? 0 : env[0] == '2' ? 2 : 1;
   }
   {
      u64 last_ns = vidsoft_vdp1_thread_context.cpu_ns + (vdp1_last_split ? vidsoft_vdp1b_context.cpu_ns : 0);
      YAB_PROF_BEGIN(PROF_VDP1_SCAN);
      split = vdp1_split_enabled && (vdp1_split_enabled == 2 || last_ns >= VDP1_SPLIT_MIN_NS) &&
              !Vdp1ListNeedsOneThread(ram, vidsoft_vdp1_thread_context.regs.addr);
      YAB_PROF_END(PROF_VDP1_SCAN);
   }
   vdp1_last_split = split;

   if (split)
   {
      /* Move the boundary towards the thread that had less work last
       * frame. Where it lies doesn't change the picture. */
      u64 a = vidsoft_vdp1_thread_context.cpu_ns, b = vidsoft_vdp1b_context.cpu_ns;
      int rows = vdp1height > 0 ? vdp1height : 256;
      if (a > b + b / 8)
         vdp1_split_row -= 4;
      else if (b > a + a / 8)
         vdp1_split_row += 4;
      if (vdp1_split_row < 8) vdp1_split_row = 8;
      if (vdp1_split_row > rows - 8) vdp1_split_row = rows - 8;

      vidsoft_vdp1_thread_context.row_lo = INT_MIN;
      vidsoft_vdp1_thread_context.row_hi = vdp1_split_row;
      memcpy(&vidsoft_vdp1b_context.regs, &vidsoft_vdp1_thread_context.regs, sizeof(Vdp1));
      vidsoft_vdp1b_context.row_lo = vdp1_split_row;
      vidsoft_vdp1b_context.row_hi = INT_MAX;
      vdp1_frames_split++;
   }
   else
   {
      vidsoft_vdp1_thread_context.row_lo = INT_MIN;
      vidsoft_vdp1_thread_context.row_hi = INT_MAX;
      vdp1_frames_single++;
   }

   //start thread
   vidsoft_vdp1_thread_context.draw_finished = 0;
   YabFlagSet(vidsoft_vdp1_thread_context.need_draw, 1);
   YabThreadWake(YAB_THREAD_VIDSOFT_VDP1);
   if (split)
   {
      vidsoft_vdp1b_context.draw_finished = 0;
      YabFlagSet(vidsoft_vdp1b_context.need_draw, 1);
      YabThreadWake(YAB_THREAD_VIDSOFT_VDP1B);
   }
}

/* Draw a framebuffer's deferred job (asynchronously, like any draw) */
static void VidsoftVdp1RunJob(int idx)
{
   Vdp1Job *j = &vdp1_job[idx];
   if (!j->pending)
      return;
   VidsoftWaitForVdp1Thread();
   j->pending = 0;
   VidsoftVdp1Start(j->ram, &j->regs, j->erase, vdp1framebuffer[idx]);
   vdp1_st_run++;
}

static int VidsoftModeKey(const Vdp1 *regs)
{
   return (regs->TVMR & 3) | ((regs->FBCR & 8) << 4);
}

/* May a new erasing job replace the pending one without drawing it? Only
 * if its erase window covers everything the old job could have changed:
 * the old erase window and the old system clip rectangle. */
static int VidsoftEraseCovers(const Vdp1 *nw, const Vdp1 *old, int old_erase)
{
   int nx0 = (nw->EWLR >> 6) & 0x1F8, ny0 = nw->EWLR & 0x1FF;
   int nx1 = ((nw->EWRR >> 6) & 0x3F8) + 8, ny1 = (nw->EWRR & 0x1FF) + 1;
   if (nx0 > 0 || ny0 > 0)
      return 0;
   if (nx1 < old->systemclipX2 + 1 || ny1 < old->systemclipY2 + 1)
      return 0;
   if (old_erase && (nx1 < ((old->EWRR >> 6) & 0x3F8) + 8 || ny1 < (old->EWRR & 0x1FF) + 1))
      return 0;
   return 1;
}

static void VidsoftVdp1Capture(int erase)
{
   int idx = VidsoftFbIndex(vdp1backframebuffer), other = !idx;
   Vdp1Job *j = &vdp1_job[idx];

   /* A mode change: everything pending draws in the old mode first */
   if ((vdp1_job[other].pending && VidsoftModeKey(&vdp1_job[other].regs) != VidsoftModeKey(Vdp1Regs)))
      VidsoftVdp1RunJob(other);
   if (j->pending && (VidsoftModeKey(&j->regs) != VidsoftModeKey(Vdp1Regs) ||
                      !erase || !VidsoftEraseCovers(Vdp1Regs, &j->regs, j->erase)))
      VidsoftVdp1RunJob(idx);   /* the new draw goes on top of it */
   else if (j->pending)
      vdp1_st_replaced++;

   /* the job's snapshot may be what the threads are drawing from */
   if (vidsoft_vdp1_thread_context.ramp == j->ram)
      VidsoftWaitForVdp1Thread();

   YAB_PROF_BEGIN(PROF_VDP1_SNAPSHOT);
   VidsoftVdp1FoldDirty();
   VidsoftSyncRamCopy(j->ram, Vdp1Ram, j->stale, "VDP1 job");
   VDP1RLOG(Vdp1LogCopyCheck(j->ram));
   memcpy(&j->regs, Vdp1Regs, sizeof(Vdp1));
   YAB_PROF_END(PROF_VDP1_SNAPSHOT);
   j->erase = erase;
   j->pending = 1;
   VidsoftVdp1SetMode(Vdp1Regs);
   vdp1_st_captured++;
}

/* Called at every VBlank-OUT before the screens are drawn (vdp2.c): shown,
 * is this frame shown; next_shown, the next one; active, skipping with
 * exact VDP1 (lazy drawing applies). */
void VIDSoftVdp1LazyFrame(int active, int shown, int next_shown, int draw_due)
{
   int front = VidsoftFbIndex(vdp1frontframebuffer), back = !front;

   if (vdp1_lazy < 0)
      vdp1_lazy = !(getenv("YAB_VDP1_LAZY") && getenv("YAB_VDP1_LAZY")[0] == '0');
   active = active && vdp1_lazy && vidsoft_vdp1_thread_enabled;

   /* Where the frame-change prediction keeps missing (Panzer Dragoon's
    * changes come one or two frames after a draw, irregularly), each miss
    * makes a shown frame wait for a whole VDP1 draw: drawing everything at
    * once (exact mode) is cheaper there. More than 4 misses in 64 shown
    * frames turns deferring off for 600 frames, then it is tried again. */
   if (vdp1_lazy_backoff > 0)
   {
      vdp1_lazy_backoff--;
      active = 0;
   }
   if (active && shown && ++vdp1_lazy_shown >= 64)
   {
      if (vdp1_lazy_misses > 4)
      {
         vdp1_lazy_backoff = 600;
         vdp1_st_backoffs++;
      }
      vdp1_lazy_shown = vdp1_lazy_misses = 0;
   }
   vdp1_lazy_active = active;
   vdp1_lazy_next_shown = next_shown;

   if (!active)
   {
      VidsoftVdp1RunJob(back);
      VidsoftVdp1RunJob(front);
      vdp1_lazy_defer = 0;
   }
   else if (shown && vdp1_job[front].pending)
   {
      /* Safety net: the front buffer is about to be composited */
      VidsoftVdp1RunJob(front);
      vdp1_st_sync++;
      vdp1_lazy_misses++;
   }

   /* A job drawn into the front buffer has no frame change after it to
    * wait for it: the sprite layer reads that buffer from now on */
   if (shown && vidsoft_vdp1_thread_context.back_framebuffer == vdp1frontframebuffer)
      VidsoftWaitForVdp1Thread();

   if (!active)
      return;

   if (next_shown)
   {
      /* The frame before a shown one. Will its end change frames? Then
       * the shown frame gets the back buffer (this frame's draw, or what
       * is pending there), else the front. */
      int predict_swap = vdp1_since_swap + 1 >= vdp1_swap_gap;
      if (!predict_swap)
         VidsoftVdp1RunJob(front);
      else if (!draw_due)
         VidsoftVdp1RunJob(back);
      vdp1_lazy_defer = !predict_swap;
   }
   else
      vdp1_lazy_defer = 1;
}

/* At every frame change (VBlank-IN): keeps the change cadence for the
 * prediction above */
static void VidsoftVdp1NoteSwap(int swapped)
{
   if (swapped)
   {
      vdp1_swap_gap = vdp1_since_swap + 1;
      vdp1_since_swap = 0;
   }
   else
      vdp1_since_swap++;
}

/* A frame change asked for during the VBlank, taken at VBlank-OUT (vdp2.c):
 * it belongs to the VBlank-IN before it, which counted no change */
void VIDSoftVdp1SwapAtVBlankOut(void)
{
   if (vdp1_since_swap > 0)
      vdp1_since_swap--;
   VIDSoftVdp1SwapFrameBuffer();
}

void VIDSoftVdp1DrawStart()
{
   if (vidsoft_vdp1_thread_enabled)
   {
      int erase = ((Vdp1Regs->FBCR & 2) == 0) || Vdp1External.manualerase;

      /* Only a draw that erases first can later be dropped unseen (by the
       * next erasing draw): one that draws over the buffer's old contents
       * (Shining Force III) is needed by the next, so it is drawn now */
      if (vdp1_lazy_active && vdp1_lazy_defer && erase)
      {
         VidsoftVdp1Capture(erase);
      }
      else
      {
         int idx = VidsoftFbIndex(vdp1backframebuffer);
         /* a deferred draw the new one goes on top of */
         if (vdp1_job[idx].pending)
         {
            if (erase && VidsoftModeKey(&vdp1_job[idx].regs) == VidsoftModeKey(Vdp1Regs) &&
                VidsoftEraseCovers(Vdp1Regs, &vdp1_job[idx].regs, vdp1_job[idx].erase))
            {
               vdp1_job[idx].pending = 0;
               vdp1_st_replaced++;
            }
            else
               VidsoftVdp1RunJob(idx);
         }
         if (vdp1_job[!idx].pending && VidsoftModeKey(&vdp1_job[!idx].regs) != VidsoftModeKey(Vdp1Regs))
            VidsoftVdp1RunJob(!idx);

         VidsoftWaitForVdp1Thread();

         //take a snapshot of the vdp1 state, to be used by the thread
         {
         YAB_PROF_BEGIN(PROF_VDP1_SNAPSHOT);
         VidsoftVdp1FoldDirty();
         VidsoftSyncRamCopy(vidsoft_vdp1_thread_context.ram, Vdp1Ram, vdp1_ctx_stale, "VDP1");
         VDP1RLOG(Vdp1LogCopyCheck(vidsoft_vdp1_thread_context.ram));
         YAB_PROF_END(PROF_VDP1_SNAPSHOT);
         }
         VidsoftVdp1Start(vidsoft_vdp1_thread_context.ram, Vdp1Regs, erase, vdp1backframebuffer);
         vdp1_st_eager++;
      }
      if (erase)
         Vdp1External.manualerase = 0;   /* as the erase itself does */

      Vdp1FakeDrawCommands(Vdp1Ram, Vdp1Regs);
   }
   else
   {
      VIDSoftVdp1DrawStartBody(Vdp1Regs, vdp1backframebuffer);
      VDP1RLOG(Vdp1LogStart(Vdp1Ram));
      Vdp1DrawCommands(Vdp1Ram, Vdp1Regs, vdp1backframebuffer);
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1DrawEnd(void)
{
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u16  Vdp1ReadPattern16( u32 base, u32 offset , u8 * ram) {

   u16 dot = T1ReadByte(ram, (base + (offset >> 1)) & 0x7FFFF);
  if ((offset & 0x1) == 0) dot >>= 4; // Even pixel
  else dot &= 0xF; // Odd pixel
  return dot;
}

static INLINE u16  Vdp1ReadPattern64(u32 base, u32 offset, u8 * ram) {

   return T1ReadByte(ram, (base + offset) & 0x7FFFF) & 0x3F;
}

static INLINE u16  Vdp1ReadPattern128(u32 base, u32 offset, u8 * ram) {

   return T1ReadByte(ram, (base + offset) & 0x7FFFF) & 0x7F;
}

static INLINE u16  Vdp1ReadPattern256(u32 base, u32 offset, u8 * ram) {

   return T1ReadByte(ram, (base + offset) & 0x7FFFF) & 0xFF;
}

static INLINE u16  Vdp1ReadPattern64k(u32 base, u32 offset, u8 * ram) {

  return T1ReadWord(ram, ( base + 2*offset) & 0x7FFFF);
}

////////////////////////////////////////////////////////////////////////////////

static INLINE u32 alphablend16(u32 d, u32 s, u32 level)
{
	int r,g,b,sr,sg,sb,dr,dg,db;

	int invlevel = 256-level;
	sr = s & 0x001f; dr = d & 0x001f; 
	r = (sr*level + dr*invlevel)>>8; r&= 0x1f;
	sg = s & 0x03e0; dg = d & 0x03e0;
	g = (sg*level + dg*invlevel)>>8; g&= 0x03e0;
	sb = s & 0x7c00; db = d & 0x7c00;
	b = (sb*level + db*invlevel)>>8; b&= 0x7c00;
	return r|g|b;
}

typedef struct _COLOR_PARAMS
{
	double r,g,b;
} COLOR_PARAMS;

VDP1_TLS COLOR_PARAMS leftColumnColor;



VDP1_TLS int currentPixel;
VDP1_TLS int currentPixelIsVisible;
VDP1_TLS int characterWidth;
VDP1_TLS int characterHeight;

static int getpixel(int linenumber, int currentlineindex, vdp1cmd_struct *cmd, u8 * ram) {

	u32 characterAddress;
	u32 colorlut;
	u16 colorbank;
	u8 SPD;
	int endcode;
	int endcodesEnabled;
	int untexturedColor = 0;
	int isTextured = 1;
	int currentShape = cmd->CMDCTRL & 0x7;
	int flip;

   characterAddress = cmd->CMDSRCA << 3;
   colorbank = cmd->CMDCOLR;
	colorlut = (u32)colorbank << 3;
   SPD = ((cmd->CMDPMOD & 0x40) != 0);//show the actual color of transparent pixels if 1 (they won't be drawn transparent)
   endcodesEnabled = ((cmd->CMDPMOD & 0x80) == 0) ? 1 : 0;
   flip = (cmd->CMDCTRL & 0x30) >> 4;

	//4 polygon, 5 polyline or 6 line
	if(currentShape == 4 || currentShape == 5 || currentShape == 6) {
		isTextured = 0;
      untexturedColor = cmd->CMDCOLR;
	}

	switch( flip ) {
		case 1:
			// Horizontal flipping
			currentlineindex = characterWidth - currentlineindex-1;
			break;
		case 2:
			// Vertical flipping
			linenumber = characterHeight - linenumber-1;

			break;
		case 3:
			// Horizontal/Vertical flipping
			linenumber = characterHeight - linenumber-1;
			currentlineindex = characterWidth - currentlineindex-1;
			break;
	}

   switch ((cmd->CMDPMOD >> 3) & 0x7)
	{
		case 0x0: //4bpp bank
			endcode = 0xf;
			currentPixel = Vdp1ReadPattern16( characterAddress + (linenumber*(characterWidth>>1)), currentlineindex , ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank &0xfff0)| currentPixel;
			currentPixelIsVisible = 0xf;
			break;

		case 0x1://4bpp lut
			endcode = 0xf;
         currentPixel = Vdp1ReadPattern16(characterAddress + (linenumber*(characterWidth >> 1)), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			if (!(currentPixel == 0 && !SPD))
				currentPixel = T1ReadWord(ram, (currentPixel * 2 + colorlut) & 0x7FFFF);
			currentPixelIsVisible = 0xffff;
			break;
		case 0x2://8pp bank (64 color)
			//is there a hardware bug with endcodes in this color mode?
			//there are white lines around some characters in scud
			//using an endcode of 63 eliminates the white lines
			//but also causes some dropout due to endcodes being triggered that aren't triggered on hardware
			//the closest thing i can do to match the hardware is make all pixels with color index 63 transparent
			//this needs more hardware testing

			endcode = 63;
         currentPixel = Vdp1ReadPattern64(characterAddress + (linenumber*(characterWidth)), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				currentPixel = 0;
		//		return 1;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank&0xffc0) | currentPixel;
			currentPixelIsVisible = 0x3f;
			break;
		case 0x3://128 color
			endcode = 0xff;
         currentPixel = Vdp1ReadPattern128(characterAddress + (linenumber*characterWidth), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank&0xff80) | currentPixel;//dead or alive needs colorbank to be masked
			currentPixelIsVisible = 0x7f;
			break;
		case 0x4://256 color
			endcode = 0xff;
         currentPixel = Vdp1ReadPattern256(characterAddress + (linenumber*characterWidth), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;
			currentPixelIsVisible = 0xff;
			if (!((currentPixel == 0) && !SPD)) 
				currentPixel = (colorbank&0xff00) | currentPixel;
			break;
		case 0x5://16bpp bank
		case 0x6://prohibited, used by (at least) Beach de Reach and seems to behave like 0x5
			endcode = 0x7fff;
         currentPixel = Vdp1ReadPattern64k(characterAddress + (linenumber*characterWidth * 2), currentlineindex, ram);
			if(isTextured && endcodesEnabled && currentPixel == endcode)
				return 1;

			/* the transparent pixel in 16bpp is supposed to be 0x0000
			but some games use pixels with invalid values and expect
			them to be transparent (see vdp1 doc p. 92) */
			if (!(currentPixel & 0x8000) && !SPD)
				currentPixel = 0;

			currentPixelIsVisible = 0xffff;
			break;
	}

	if(!isTextured)
		currentPixel = untexturedColor;

	//force the MSB to be on if MSBON is set
	//currentPixel |= cmd.CMDPMOD & (1 << 15);

	return 0;
}

static int gouraudAdjust( int color, int tableValue )
{
	color += (tableValue - 0x10);

	if ( color < 0 ) color = 0;
	if ( color > 0x1f ) color = 0x1f;

	return color;
}


static int CheckDil(int y, Vdp1 * regs)
{
   int dil = (regs->FBCR >> 2) & 1;

   if (vdp1interlace == 2)
   {
      if (dil)
      {
         if ((y & 1) == 0)
            return 1;
      }
      else
      {
         if ((y & 1))
            return 1;
      }
   }

   return 0;
}

static INLINE int IsUserClipped(int x, int y, Vdp1* regs)
{
   return !(x >= regs->userclipX1 &&
      x <= regs->userclipX2 &&
      y >= regs->userclipY1 &&
      y <= regs->userclipY2);
}

static INLINE int IsSystemClipped(int x, int y, Vdp1* regs)
{
   return !(x >= 0 &&
      x <= regs->systemclipX2 &&
      y >= 0 &&
      y <= regs->systemclipY2);
}

int IsClipped(int x, int y, Vdp1* regs, vdp1cmd_struct * cmd)
{
   if (cmd->CMDPMOD & 0x0400)//user clipping enabled
   {
      int is_user_clipped = IsUserClipped(x, y, regs);

      if (((cmd->CMDPMOD >> 9) & 0x3) == 0x3)//outside clipping mode
         is_user_clipped = !is_user_clipped;

      return is_user_clipped || IsSystemClipped(x, y, regs);
   }
   else
   {
      return IsSystemClipped(x, y, regs);
   }
}

static void putpixel8(int x, int y, Vdp1 * regs, vdp1cmd_struct *cmd, u8 * back_framebuffer) {

    int y2 = y / vdp1interlace;
    u8 * iPix = &back_framebuffer[(y2 * vdp1width) + x];
    int mesh = cmd->CMDPMOD & 0x0100;
    int SPD = ((cmd->CMDPMOD & 0x40) != 0);//show the actual color of transparent pixels if 1 (they won't be drawn transparent)

    if (y2 < vdp1_row_lo || y2 >= vdp1_row_hi)
        return;

    if (iPix >= (back_framebuffer + 0x40000))
        return;

    if (CheckDil(y, regs))
       return;

    currentPixel &= 0xFF;

    if (mesh && ((x ^ y2) & 1)) {
       return;
    }

    if (IsClipped(x, y, regs, cmd))
       return;

    if ( SPD || (currentPixel & currentPixelIsVisible))
    {
        switch( cmd->CMDPMOD & 0x7 )//we want bits 0,1,2
        {
        default:
        case 0:	// replace
            if (!((currentPixel == 0) && !SPD))
                VDP1W(*(iPix)) = currentPixel;
            break;
        }
    }
}

static void putpixel(int x, int y, Vdp1* regs, vdp1cmd_struct * cmd, u8 * back_framebuffer) {

	u16* iPix;
	int mesh = cmd->CMDPMOD & 0x0100;
	int SPD = ((cmd->CMDPMOD & 0x40) != 0);//show the actual color of transparent pixels if 1 (they won't be drawn transparent)
   int original_y = y;

   if (CheckDil(y, regs))
      return;

	y /= vdp1interlace;
   if (y < vdp1_row_lo || y >= vdp1_row_hi)
      return;
   iPix = &((u16 *)back_framebuffer)[(y * vdp1width) + x];

   if (iPix >= (u16*)(back_framebuffer + 0x40000))
		return;

	if(mesh && (x^y)&1)
		return;

   if (IsClipped(x, original_y, regs, cmd))
      return;

	if (cmd->CMDPMOD & (1 << 15))
	{
		if (currentPixel) {
			VDP1W(*iPix) |= 0x8000;
			return;
		}
	}

	if ( SPD || (currentPixel & currentPixelIsVisible))
	{
		switch( cmd->CMDPMOD & 0x7 )//we want bits 0,1,2
		{
		case 0:	// replace
			if (!((currentPixel == 0) && !SPD)) 
				VDP1W(*(iPix)) = currentPixel;
			break;
		case 1: // shadow
			if (*(iPix) & (1 << 15)) // only if MSB of framebuffer data is set
				VDP1W(*(iPix)) = ((*iPix & 0x7BDE) >> 1) | 0x8000;
			break;
		case 2: // half luminance
			VDP1W(*(iPix)) = ((currentPixel & ~0x8421) >> 1) | (1 << 15);
			break;
		case 3: // half transparent
			if ( *(iPix) & (1 << 15) )//only if MSB of framebuffer data is set 
				VDP1W(*(iPix)) = (((*iPix & currentPixel & 0x7FFF) + (((*iPix ^ currentPixel) & 0x7BDE) >> 1))) | 0x8000;
			else
				VDP1W(*(iPix)) = currentPixel;
			break;
		case 4: //gouraud
			#define COLOR(r,g,b)    (((r)&0x1F)|(((g)&0x1F)<<5)|(((b)&0x1F)<<10) |0x8000 )

			//handle the special case demonstrated in the sgl chrome demo
			//if we are in a paletted bank mode and the other two colors are unused, adjust the index value instead of rgb
			if(
				(((cmd->CMDPMOD >> 3) & 0x7) != 5) &&
				(((cmd->CMDPMOD >> 3) & 0x7) != 1) && 
				(int)leftColumnColor.g == 16 && 
				(int)leftColumnColor.b == 16) 
			{
				int c = (int)(leftColumnColor.r-0x10);
				if(c < 0) c = 0;
				currentPixel = currentPixel+c;
				VDP1W(*(iPix)) = currentPixel;
				break;
			}
			VDP1W(*(iPix)) = COLOR(
				gouraudAdjust(
				currentPixel&0x001F,
				(int)leftColumnColor.r),

				gouraudAdjust(
				(currentPixel&0x03e0) >> 5,
				(int)leftColumnColor.g),

				gouraudAdjust(
				(currentPixel&0x7c00) >> 10,
				(int)leftColumnColor.b)
				);
			break;
		default:
			VDP1W(*(iPix)) = alphablend16( COLOR((int)leftColumnColor.r,(int)leftColumnColor.g, (int)leftColumnColor.b), currentPixel, (1 << 7) ) | (1 << 15);
			break;
		}
	}
}

static int iterateOverLine(int x1, int y1, int x2, int y2, int greedy, void *data,
   int(*line_callback)(int x, int y, int i, void *data, Vdp1* regs, vdp1cmd_struct * cmd, u8* ram, u8* back_framebuffer), Vdp1* regs, vdp1cmd_struct * cmd, u8 * ram, u8* back_framebuffer) {
	int i, a, ax, ay, dx, dy;

	a = i = 0;
	dx = x2 - x1;
	dy = y2 - y1;
	ax = (dx >= 0) ? 1 : -1;
	ay = (dy >= 0) ? 1 : -1;

	//burning rangers tries to draw huge shapes
	//this will at least let it run
	if(abs(dx) > 999 || abs(dy) > 999)
		return INT_MAX;

	if (abs(dx) > abs(dy)) {
		if (ax != ay) dx = -dx;

		for (; x1 != x2; x1 += ax, i++) {
         if (line_callback && line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0) return i + 1;

			a += dy;
			if (abs(a) >= abs(dx)) {
				a -= dx;
				y1 += ay;

				// Make sure we 'fill holes' the same as the Saturn
				if (greedy) {
					i ++;
					if (ax == ay) {
						if (line_callback &&
                     line_callback(x1 + ax, y1 - ay, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					} else {
						if (line_callback &&
                     line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					}
				}
			}
		}

		// If the line isn't greedy here, we end up with gaps that don't occur on the Saturn
		if (/*(i == 0) || (y1 != y2)*/1) {
         if (line_callback) line_callback(x2, y2, i, data, regs, cmd, ram, back_framebuffer);
			i ++;
		}
	} else {
		if (ax != ay) dy = -dy;

		for (; y1 != y2; y1 += ay, i++) {
         if (line_callback && line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0) return i + 1;

			a += dx;
			if (abs(a) >= abs(dy)) {
				a -= dy;
				x1 += ax;

				if (greedy) {
					i ++;
					if (ay == ax) {
						if (line_callback &&
                     line_callback(x1, y1, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					} else {
						if (line_callback &&
                     line_callback(x1 - ax, y1 + ay, i, data, regs, cmd, ram, back_framebuffer) != 0)
							return i + 1;
					}
				}
			}
		}

		if (/*(i == 0) || (y1 != y2)*/1) {
         if (line_callback) line_callback(x2, y2, i, data, regs, cmd, ram, back_framebuffer);
			i ++;
		}
	}

	return i;
}

typedef struct {
	double linenumber;
	double texturestep;
	double xredstep;
	double xgreenstep;
	double xbluestep;
	int endcodesdetected;
	int previousStep;
} DrawLineData;

static int DrawLineCallback(int x, int y, int i, void *data, Vdp1* regs, vdp1cmd_struct * cmd, u8* ram, u8* back_framebuffer)
{
	int currentStep;
	DrawLineData *linedata = data;

	leftColumnColor.r += linedata->xredstep;
	leftColumnColor.g += linedata->xgreenstep;
	leftColumnColor.b += linedata->xbluestep;

	currentStep = (int)i * linedata->texturestep;
	if (getpixel(linedata->linenumber, currentStep, cmd, ram)) {
		if (currentStep != linedata->previousStep) {
			linedata->previousStep = currentStep;
			linedata->endcodesdetected ++;
		}
	} else if (vdp1pixelsize == 2) {
		putpixel(x, y, regs, cmd, back_framebuffer);
	} else {
      putpixel8(x, y, regs, cmd, back_framebuffer);
    }

	if (linedata->endcodesdetected == 2) return -1;

	return 0;
}

/* not inlined: drawQuad() only falls back to it for rare modes */
static __attribute__((noinline)) int DrawLine(int x1, int y1, int x2, int y2, int greedy, double linenumber, double texturestep, double xredstep, double xgreenstep, double xbluestep, Vdp1* regs, vdp1cmd_struct *cmd, u8 * ram, u8* back_framebuffer)
{
	DrawLineData data;

	data.linenumber = linenumber;
	data.texturestep = texturestep;
	data.xredstep = xredstep;
	data.xgreenstep = xgreenstep;
	data.xbluestep = xbluestep;
	data.endcodesdetected = 0;
	data.previousStep = 123456789;

#ifdef YAB_VDP1LOG
   {
      int r = iterateOverLine(x1, y1, x2, y2, greedy, &data, DrawLineCallback, regs, cmd, ram, back_framebuffer);
      if (r == INT_MAX)
         VDP1RLOG_L999();
      return r;
   }
#else
   return iterateOverLine(x1, y1, x2, y2, greedy, &data, DrawLineCallback, regs, cmd, ram, back_framebuffer);
#endif
}

/* Quad drawing without the per-pixel overhead of DrawLine(): the command's
 * settings are decoded once (Vdp1QuadSetup), the line walk calls the pixel
 * code directly, and clipping is only tested per pixel when a line is not
 * entirely inside the clip area. Everything else follows getpixel() and
 * putpixel() exactly, including the floating point steps, so the output is
 * the same. For colour modes 0-6, on the 16-bit framebuffer and (since VF2,
 * which draws its polygons there in high resolution) the 8-bit one. */
typedef struct {
   u32 characterAddress;
   u16 colorbank;
   u32 colorlut;
   int SPD, endcodesEnabled, flip, colormode, isTextured, untexturedColor;
   int mesh, drawmode, msbon;
   int dil_skip;     /* y parity that CheckDil() rejects, or -1 */
   int user_clip;    /* user clipping: 0 off, 1 inside, 2 outside */
   int fb8;          /* 8-bit framebuffer (putpixel8() instead of putpixel()) */
} Vdp1QuadState;

static int Vdp1QuadSetup(Vdp1QuadState *st, Vdp1 *regs, vdp1cmd_struct *cmd)
{
   int shape = cmd->CMDCTRL & 0x7;

   st->colormode = (cmd->CMDPMOD >> 3) & 0x7;
   if (st->colormode == 7)
      return 0;
   st->fb8 = vdp1pixelsize != 2;
   st->characterAddress = cmd->CMDSRCA << 3;
   st->colorbank = cmd->CMDCOLR;
   st->colorlut = (u32)st->colorbank << 3;
   st->SPD = ((cmd->CMDPMOD & 0x40) != 0);
   st->endcodesEnabled = ((cmd->CMDPMOD & 0x80) == 0) ? 1 : 0;
   st->flip = (cmd->CMDCTRL & 0x30) >> 4;
   st->isTextured = !(shape == 4 || shape == 5 || shape == 6);
   st->untexturedColor = st->isTextured ? 0 : cmd->CMDCOLR;
   st->mesh = cmd->CMDPMOD & 0x0100;
   st->drawmode = cmd->CMDPMOD & 0x7;
   st->msbon = cmd->CMDPMOD & (1 << 15);
   st->dil_skip = -1;
   if (vdp1interlace == 2)
      st->dil_skip = ((regs->FBCR >> 2) & 1) ? 0 : 1;
   st->user_clip = 0;
   if (cmd->CMDPMOD & 0x0400)
      st->user_clip = (((cmd->CMDPMOD >> 9) & 0x3) == 0x3) ? 2 : 1;
   return 1;
}

/* the drawing modes of putpixel() */
static INLINE __attribute__((always_inline)) int Vdp1QuadPut(const int drawmode, const int colormode, const int SPD,
   u16 *iPix, int cp, double lr, double lg, double lb)
{
   switch (drawmode)
   {
   case 0: // replace
      if (!((cp == 0) && !SPD))
         VDP1W(*iPix) = cp;
      break;
   case 1: // shadow
      if (*iPix & (1 << 15))
         VDP1W(*iPix) = ((*iPix & 0x7BDE) >> 1) | 0x8000;
      break;
   case 2: // half luminance
      VDP1W(*iPix) = ((cp & ~0x8421) >> 1) | (1 << 15);
      break;
   case 3: // half transparent
      if (*iPix & (1 << 15))
         VDP1W(*iPix) = (((*iPix & cp & 0x7FFF) + (((*iPix ^ cp) & 0x7BDE) >> 1))) | 0x8000;
      else
         VDP1W(*iPix) = cp;
      break;
   case 4: // gouraud
      if (colormode != 5 && colormode != 1 && (int)lg == 16 && (int)lb == 16)
      {
         int c = (int)(lr - 0x10);
         if (c < 0) c = 0;
         cp = cp + c;
         VDP1W(*iPix) = cp;
         break;
      }
      VDP1W(*iPix) = COLOR(gouraudAdjust(cp & 0x001F, (int)lr),
                    gouraudAdjust((cp & 0x03e0) >> 5, (int)lg),
                    gouraudAdjust((cp & 0x7c00) >> 10, (int)lb));
      break;
   default:
      VDP1W(*iPix) = alphablend16(COLOR((int)lr, (int)lg, (int)lb), cp, (1 << 7)) | (1 << 15);
      break;
   }
   return cp;
}

/* points of the line being drawn, in iterateOverLine() order, packed as
 * x | y << 16 (both stay within +-2000) */
static u32 quad_pt_buf[2][2048];
#define QUAD_PT(X, Y) (((u32)(X) & 0xFFFF) | ((u32)(Y) << 16))

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
} QuadLine;

typedef struct {
   int cp, vis;
   double lr, lg, lb;
} QuadPen;

/* DrawLineCallback(), i.e. getpixel() then putpixel() (putpixel8() if F8),
 * for each point of a line. CM and DM are a constant colour and drawing mode
 * for a specialised copy, or -1 to read them from the line. */
static INLINE __attribute__((always_inline)) QuadPen QuadPlotPoints(const int CM, const int DM, const int F8, const QuadLine L, QuadPen pen)
{
   const int colormode = CM >= 0 ? CM : L.colormode;
   const int drawmode = DM >= 0 ? DM : L.drawmode;
   int k, cp = pen.cp, vis = pen.vis;
   double lr = pen.lr, lg = pen.lg, lb = pen.lb;
   int endcodesdetected = 0, previousStep = 123456789;
   int quot = 0, rem = 0;

   for (k = 0; k < L.n; k++)
   {
      const int px = (s16)L.pts[k], py = (s32)L.pts[k] >> 16;
      int currentStep, endcode = 0;
      u32 idx;

      lr += L.xredstep; lg += L.xgreenstep; lb += L.xbluestep;
      /* currentStep = (int)k * texturestep, see DrawLineQuad() */
      if (rem == 0 && L.step_r != 0)
         currentStep = (int)k * L.texturestep;
      else
         currentStep = quot;
      quot += L.step_q;
      rem += L.step_r;
      if (rem >= L.linelength)
      {
         rem -= L.linelength;
         quot++;
      }
      idx = (L.flip & 1) ? (u32)(L.cw - currentStep - 1) : (u32)currentStep;

      /* Without end codes the texel only matters if the point is plotted,
       * so check that first (the same tests as below, IsClipped() inlined) */
      if (!L.endcodes)
      {
         int yy, pos, clipped;
         if ((py & 1) == L.dil_skip)
            continue;
         yy = L.interlace == 2 ? py / 2 : py;
         pos = (yy * L.width) + px;
         if (!(yy >= L.row_lo && yy < L.row_hi && pos < (F8 ? 0x40000 : 0x20000)) || (L.mesh && ((px ^ yy) & 1)))
            continue;
         if (!L.noclip)
         {
            clipped = !(px >= 0 && px <= L.regs->systemclipX2 && py >= 0 && py <= L.regs->systemclipY2);
            if (L.user_clip)
            {
               int uc = !(px >= L.regs->userclipX1 && px <= L.regs->userclipX2 &&
                          py >= L.regs->userclipY1 && py <= L.regs->userclipY2);
               if (L.user_clip == 2)
                  uc = !uc;
               clipped = clipped || uc;
            }
            if (clipped)
               continue;
         }
      }

      switch (colormode)
      {
      case 0:
         cp = Vdp1ReadPattern16(L.rowaddr, idx, L.ram);
         if (L.endcodes && cp == 0xf) { endcode = 1; break; }
         if (!((cp == 0) && !L.SPD)) cp = (L.colorbank & 0xfff0) | cp;
         vis = 0xf;
         break;
      case 1:
         cp = Vdp1ReadPattern16(L.rowaddr, idx, L.ram);
         if (L.endcodes && cp == 0xf) { endcode = 1; break; }
         if (!(cp == 0 && !L.SPD)) cp = T1ReadWord(L.ram, (cp * 2 + L.colorlut) & 0x7FFFF);
         vis = 0xffff;
         break;
      case 2:
         cp = Vdp1ReadPattern64(L.rowaddr, idx, L.ram);
         if (L.endcodes && cp == 63) cp = 0;
         if (!((cp == 0) && !L.SPD)) cp = (L.colorbank & 0xffc0) | cp;
         vis = 0x3f;
         break;
      case 3:
         cp = Vdp1ReadPattern128(L.rowaddr, idx, L.ram);
         if (L.endcodes && cp == 0xff) { endcode = 1; break; }
         if (!((cp == 0) && !L.SPD)) cp = (L.colorbank & 0xff80) | cp;
         vis = 0x7f;
         break;
      case 4:
         cp = Vdp1ReadPattern256(L.rowaddr, idx, L.ram);
         if (L.endcodes && cp == 0xff) { endcode = 1; break; }
         vis = 0xff;
         if (!((cp == 0) && !L.SPD)) cp = (L.colorbank & 0xff00) | cp;
         break;
      default:
         cp = Vdp1ReadPattern64k(L.rowaddr, idx, L.ram);
         if (L.endcodes && cp == 0x7fff) { endcode = 1; break; }
         if (!(cp & 0x8000) && !L.SPD) cp = 0;
         vis = 0xffff;
         break;
      }

      if (endcode)
      {
         if (currentStep != previousStep)
         {
            previousStep = currentStep;
            endcodesdetected++;
         }
      }
      else
      {
         if (!L.isTextured) cp = L.untexturedColor;
         if (F8)
         {
            /* putpixel8(): rows, buffer end, CheckDil(), then the colour is
             * cut to 8 bits, then mesh and clipping; replace mode only */
            int yy = L.interlace == 2 ? py / 2 : py;
            int pos = (yy * L.width) + px;
            if (yy >= L.row_lo && yy < L.row_hi && pos < 0x40000 && (py & 1) != L.dil_skip)
            {
               cp &= 0xFF;
               if (!(L.mesh && ((px ^ yy) & 1)) && (L.noclip || !IsClipped(px, py, L.regs, L.cmd)) &&
                   (L.SPD || (cp & vis)) && !((cp == 0) && !L.SPD))
                  VDP1W(L.fb8[pos]) = cp;
            }
         }
         else if ((py & 1) != L.dil_skip) /* CheckDil() */
         {
            int yy = L.interlace == 2 ? py / 2 : py; /* py / vdp1interlace without a divide */
            int pos = (yy * L.width) + px;
            if (yy >= L.row_lo && yy < L.row_hi && pos < 0x20000 && !(L.mesh && ((px ^ yy) & 1)) &&
                (L.noclip || !IsClipped(px, py, L.regs, L.cmd)))
            {
               if (L.msbon && cp)
                  VDP1W(L.fb[pos]) |= 0x8000;
               else if (L.SPD || (cp & vis))
                  cp = Vdp1QuadPut(drawmode, colormode, L.SPD, &L.fb[pos], cp, lr, lg, lb);
            }
         }
      }
      /* the last point is plotted after iterateOverLine()'s loop, where
       * DrawLineCallback()'s result no longer matters */
      if (endcodesdetected == 2 && k < L.n - 1)
         break;
   }

   pen.cp = cp;
   pen.vis = vis;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
}

/* The common case of 3D games (NiGHTS: 92% of the points drawn): a 16-colour
 * texture through a colour lookup table, Gouraud shaded, the whole line
 * inside the clip window, no mesh, end codes, MSB-on or double interlace.
 * Gives exactly what QuadPlotPoints(1, 4, ...) gives; that copy keeps the
 * tests for all the other cases and runs out of registers (about 15 values
 * reloaded from the stack per point, ~130 cycles a point on the Miyoo). */
static QuadPen QuadPlotCM1Gouraud(const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
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
      /* currentStep = (int)k * texturestep, see DrawLineQuad() */
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
      idx = flip ? (u32)(cwm1 - cs) : (u32)cs;
      dots = ram[(rowaddr + (idx >> 1)) & 0x7FFFF];
      cp = (idx & 1) ? (dots & 0xF) : (dots >> 4);
      if (cp == 0 && !SPD)
         continue;                                     /* transparent */
      cp = T1ReadWord((u8 *)ram, (cp * 2 + colorlut) & 0x7FFFF);
      if (SPD || cp)
         VDP1W(fb[pos]) = COLOR(gouraudAdjust(cp & 0x001F, (int)lr),
                         gouraudAdjust((cp & 0x03e0) >> 5, (int)lg),
                         gouraudAdjust((cp & 0x7c00) >> 10, (int)lb));
   }

   pen.cp = cp;
   pen.vis = 0xffff;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
}

/* The points of a line that the lean loops plot: inside the system clip
 * rectangle, this thread's rows and, with user clipping "inside" (user_clip
 * 1), the user clip rectangle (IsClipped(): clipped if outside either). The
 * intersection is a rectangle and the walk is monotonic in x and y, so they
 * are one run [*k0, *k1] (*k1 < *k0 when there are none). */
static INLINE __attribute__((always_inline)) void QuadLeanRun(const QuadLine *Lp, int *k0p, int *k1p)
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

/* Replace mode (and CM1 Gouraud) without end codes, mesh, MSB-on, double
 * interlace or user clipping "outside", on lines that may cross the clip
 * window: the points to plot are one run [k0, k1] (QuadLeanRun): only those points
 * are plotted, with the texture step at k0 computed directly. Gives exactly what
 * QuadPlotPoints() gives; Panzer Dragoon's large 256-colour walls are
 * mostly such lines. */
static INLINE __attribute__((always_inline)) QuadPen QuadPlotLeanBody(const int CM, const int DM, const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   const u8 *const ram = Lp->ram;
   const u32 rowaddr = Lp->rowaddr, colorlut = Lp->colorlut, colorbank = Lp->colorbank;
   const int SPD = Lp->SPD, width = Lp->width, n = Lp->n;
   const int flip = Lp->flip & 1, cwm1 = Lp->cw - 1;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength;
   const double texturestep = Lp->texturestep;
   const double rs = Lp->xredstep, gs = Lp->xgreenstep, bs = Lp->xbluestep;
   double lr = pen.lr, lg = pen.lg, lb = pen.lb;
   int cp = pen.cp, k, k0, k1, quot, rem, read = 0;

   QuadLeanRun(Lp, &k0, &k1);   /* none: k1 < k0; the colours still advance */

   /* the texture step at k0, as the full loop would have counted it */
   quot = k0 * step_q + (k0 * step_r) / (linelength > 0 ? linelength : 1);
   rem = (k0 * step_r) % (linelength > 0 ? linelength : 1);

   /* Replace mode has no Gouraud steps (they are 0, the colours stay as
    * they are). With Gouraud the colours are summed in one loop over every
    * point, as QuadPlotPoints() does: the core is built with -ffast-math, and
    * split loops get summed in a different order (different low bits, and
    * (int)lr can land on the other side of an integer). */
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
      idx = flip ? (u32)(cwm1 - cs) : (u32)cs;
      read = 1;
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
         VDP1W(fb[pos]) = cp;
      else
         VDP1W(fb[pos]) = COLOR(gouraudAdjust(cp & 0x001F, (int)lr),
                         gouraudAdjust((cp & 0x03e0) >> 5, (int)lg),
                         gouraudAdjust((cp & 0x7c00) >> 10, (int)lb));
   }
   pen.cp = cp;
   if (read)  /* as QuadPlotPoints(): set by the texel reads only */
      pen.vis = CM == 0 ? 0xf : CM == 4 ? 0xff : 0xffff;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
}

static QuadPen QuadPlotLeanCM0(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanBody(0, 0, Lp, pen); }
static QuadPen QuadPlotLeanCM1(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanBody(1, 0, Lp, pen); }
static QuadPen QuadPlotLeanCM4(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanBody(4, 0, Lp, pen); }
static QuadPen QuadPlotLeanCM1G(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanBody(1, 4, Lp, pen); }

/* one big-endian VDP1 RAM word: a single REV16 on ARM (T1ReadWord() leaves
 * the compiler a shift/or pair) */
static INLINE __attribute__((always_inline)) u32 QuadRam16(const u8 *ram, u32 addr)
{
   return __builtin_bswap16(*(const u16 *)(ram + addr));
}

/* gouraudAdjust(color, tableValue) for 0 <= color <= 31: one USAT on ARM */
static INLINE __attribute__((always_inline)) int QuadGouraud5(int color, int tableValue)
{
   int v = color + (tableValue - 0x10);
#if defined(__arm__) && defined(__ARM_FEATURE_SAT)
   __asm__("usat %0, #5, %0" : "+r"(v));
   return v;
#else
   return v < 0 ? 0 : v > 0x1f ? 0x1f : v;
#endif
}

/* the texture step where the quotient/remainder count has a remainder of 0:
 * the double product, as DrawLine() takes it (see DrawLineQuad()) */
static __attribute__((noinline)) int QuadStepExact(int k, double texturestep)
{
   return (int)k * texturestep;
}

/* The lean loop for RGB textures (colour mode 5) without end codes, replace
 * or Gouraud; no mesh, MSB-on, double interlace or user clipping "outside".
 * As QuadPlotLeanBody(): only the run [k0, k1] (QuadLeanRun) is read and
 * plotted. Gives exactly what QuadPlotPoints(5, DM, 0, ...) gives, including
 * the colour and texel left in the pen. (With end codes, Shining Force III's
 * case, QuadPlotLeanRGBEnd() below is used; ENDC is 0 here.) */
static INLINE __attribute__((always_inline)) QuadPen QuadPlotLeanRGBPlain(const int ENDC, const int DM, const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   const u8 *const ram = Lp->ram;
   const u32 rowaddr = Lp->rowaddr;
   const int SPD = Lp->SPD, width = Lp->width, n = Lp->n;
   const int flip = Lp->flip & 1, cwm1 = Lp->cw - 1;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength;
   const double texturestep = Lp->texturestep;
   const double rs = Lp->xredstep, gs = Lp->xgreenstep, bs = Lp->xbluestep;
   double lr = pen.lr, lg = pen.lg, lb = pen.lb;
   int cp = pen.cp, vis = pen.vis, k, k0, k1, kfirst, klast, quot, rem;
   int endcodes = 0, previousStep = 123456789;

   QuadLeanRun(Lp, &k0, &k1);
   /* without end codes, points outside the run are skipped before anything
    * is read; with Gouraud the colours still advance on every point */
   kfirst = ENDC || DM == 4 ? 0 : k0;
   klast = ENDC || DM == 4 ? n - 1 : k1;
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
      if (!ENDC && (!inside || pos >= 0x20000))
         continue;
      idx = flip ? (u32)(cwm1 - cs) : (u32)cs;
      cp = T1ReadWord((u8 *)ram, (rowaddr + 2 * idx) & 0x7FFFF);
      if (ENDC && cp == 0x7fff)
      {
         if (cs != previousStep)
         {
            previousStep = cs;
            endcodes++;
         }
      }
      else
      {
         if (!(cp & 0x8000) && !SPD)
            cp = 0;
         vis = 0xffff;
         if ((!ENDC || (inside && pos < 0x20000)) && (SPD || cp))
         {
            if (DM == 0)
               VDP1W(fb[pos]) = cp;
            else
               VDP1W(fb[pos]) = COLOR(gouraudAdjust(cp & 0x001F, (int)lr),
                               gouraudAdjust((cp & 0x03e0) >> 5, (int)lg),
                               gouraudAdjust((cp & 0x7c00) >> 10, (int)lb));
         }
      }
      if (ENDC && endcodes == 2 && k < n - 1)
         break;
   }
   pen.cp = cp;
   pen.vis = vis;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
}

/* The lean loop for RGB textures with end codes (colour mode 5: Shining Force
 * III draws its 3D towns with them), replace or Gouraud; no mesh, MSB-on,
 * double interlace or user clipping "outside". Every point's texel is read
 * first, as QuadPlotPoints() reads it before any test: 0x7fff is an end code
 * (not plotted; the second distinct one ends the line), the others are
 * plotted inside the run [k0, k1] (QuadLeanRun). The walk stops at k1: later
 * points are never plotted, and what they would leave behind (the texel and
 * its visibility; Gouraud colours) is not read again: the next line starts
 * with getpixel(), which sets the texel and visibility, and every Gouraud
 * line (drawmode 4) starts from its own colours (drawQuad()). The same
 * plotted pixels as QuadPlotPoints(5, DM, 0, ...). */
static INLINE __attribute__((always_inline)) QuadPen QuadPlotLeanRGBEnd(const int DM, const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   const u8 *const ram = Lp->ram;
   const u32 rowaddr = Lp->rowaddr;
   const int SPD = Lp->SPD, width = Lp->width;
   /* cwm1 - cs, or cs: (cs ^ fmask) + fadd */
   const u32 fmask = 0u - (u32)(Lp->flip & 1), fadd = fmask & (u32)Lp->cw;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength;
   const double texturestep = Lp->texturestep;
   const double rs = Lp->xredstep, gs = Lp->xgreenstep, bs = Lp->xbluestep;
   double lr = pen.lr, lg = pen.lg, lb = pen.lb;
   int cp = pen.cp, k, k0, k1, quot, rem;

   QuadLeanRun(Lp, &k0, &k1);
   if (k1 < k0)
      return pen;   /* nothing is plotted */

#define QUAD_RGB_TEXEL(K) \
      if (__builtin_expect(rem == 0 && step_r != 0, 0)) \
         cs = QuadStepExact((K), texturestep); \
      else \
         cs = quot; \
      quot += step_q; \
      rem += step_r; \
      if (rem >= linelength) \
      { \
         rem -= linelength; \
         quot++; \
      } \
      idx = ((u32)cs ^ fmask) + fadd; \
      cp = QuadRam16(ram, (rowaddr + 2 * idx) & 0x7FFFF);

#define QUAD_RGB_PLOT \
         if (DM == 0) \
            VDP1W(fb[pos]) = cp; \
         else \
            VDP1W(fb[pos]) = COLOR(QuadGouraud5(cp & 0x001F, (int)lr), \
                            QuadGouraud5((cp & 0x03e0) >> 5, (int)lg), \
                            QuadGouraud5((cp & 0x7c00) >> 10, (int)lb));

   int endcodes = 0, previousStep = 123456789;

   quot = 0;
   rem = 0;
   /* the points before the run only read and count the end codes */
   for (k = 0; k <= k1; k++)
   {
      const u32 pt = pts[k];
      const int pos = ((s32)pt >> 16) * width + (s16)pt;
      int cs;
      u32 idx;

      if (DM == 4)
      {
         lr += rs; lg += gs; lb += bs;
      }
      QUAD_RGB_TEXEL(k)
      if (cp == 0x7fff)
      {
         if (cs != previousStep)
         {
            previousStep = cs;
            if (++endcodes == 2)
               return pen;
         }
         continue;
      }
      if ((SPD || (cp & 0x8000)) && k >= k0 && pos < 0x20000)
      {
         QUAD_RGB_PLOT
      }
   }
   pen.cp = cp;
   pen.vis = 0xffff;
   pen.lr = lr;
   pen.lg = lg;
   pen.lb = lb;
   return pen;
#undef QUAD_RGB_TEXEL
#undef QUAD_RGB_PLOT
}

static __attribute__((noinline)) QuadPen QuadPlotLeanRGB(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanRGBPlain(0, 0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadPlotLeanRGBG(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanRGBPlain(0, 4, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadPlotLeanRGBE(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanRGBEnd(0, Lp, pen); }
static __attribute__((noinline)) QuadPen QuadPlotLeanRGBEG(const QuadLine *Lp, QuadPen pen) { return QuadPlotLeanRGBEnd(4, Lp, pen); }

/* Untextured polygons in replace mode (Dragon Force's menu panels): what
 * QuadPlotPoints(-1, 0, 0, ...) gives for them. No end codes (untextured),
 * so the points outside the run (QuadLeanRun) are skipped before anything
 * else; each point of the run reads a texel that is then replaced by the
 * command's colour: the colour, its visibility mask (vis, by colour mode) and
 * whether it is plotted are the same for every point. No mesh, MSB-on,
 * double interlace or user clipping "outside"; no Gouraud steps in replace
 * mode. */
static __attribute__((noinline)) QuadPen QuadPlotFill(const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u16 *const fb = Lp->fb;
   const int width = Lp->width, SPD = Lp->SPD, cp = Lp->untexturedColor;
   const int vis = Lp->colormode == 0 ? 0xf : Lp->colormode == 2 ? 0x3f : Lp->colormode == 3 ? 0x7f :
                   Lp->colormode == 4 ? 0xff : 0xffff;
   const int plot = (SPD || (cp & vis)) && !(cp == 0 && !SPD);
   int k, k0, k1, any = 0;

   QuadLeanRun(Lp, &k0, &k1);
   for (k = k0; k <= k1; k++)
   {
      const u32 pt = Lp->pts[k];
      const int pos = ((s32)pt >> 16) * width + (s16)pt;
      if (pos >= 0x20000)
         continue;
      any = 1;
      if (plot)
         VDP1W(fb[pos]) = cp;
   }
   (void)pts;
   if (any)
   {
      pen.cp = cp;
      pen.vis = vis;
   }
   return pen;
}

/* The lean loop for the 8-bit framebuffer (high resolution: Virtua Fighter 2
 * draws 95% of its polygon area as 128-colour textures this way): no end codes
 * (a 128-colour texel can never equal its end code 0xFF), mesh or Gouraud
 * steps, user clipping only when the whole line is inside it. putpixel8()
 * only replaces, so what is written is the texel's colour cut to 8 bits:
 * for CM0 (bank & 0xf0) | t, CM1 the low byte of the table colour, CM3
 * (bank & 0x80) | t, CM4 t; nothing when t is 0 without SPD (and for CM1
 * when the table colour's low byte is 0). As in QuadPlotLeanBody() the points
 * inside the clip area and this thread's rows are one run [k0, k1]; double
 * interlace (one field) is tested per point. Gives exactly what
 * QuadPlotPoints(CM, -1, 1, ...) gives. */
static INLINE __attribute__((always_inline)) QuadPen QuadPlotLean8Body(const int CM, const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u8 *const fb8 = Lp->fb8;
   const u8 *const ram = Lp->ram;
   const u32 rowaddr = Lp->rowaddr, colorlut = Lp->colorlut, colorbank = Lp->colorbank;
   const int SPD = Lp->SPD, width = Lp->width, n = Lp->n;
   const int flip = Lp->flip & 1, cwm1 = Lp->cw - 1;
   const int step_q = Lp->step_q, step_r = Lp->step_r, linelength = Lp->linelength;
   const double texturestep = Lp->texturestep;
   const int il = Lp->interlace == 2, dil = Lp->dil_skip;
   const int xhi = Lp->regs->systemclipX2, yhi = Lp->regs->systemclipY2;
   const int rlo = Lp->row_lo, rhi = Lp->row_hi;
   int cp = pen.cp, k, k0 = 0, k1 = n - 1, quot, rem, read = 0;

#define LEAN8_IN(pt) ((s16)(pt) >= 0 && (s16)(pt) <= xhi && ((s32)(pt) >> 16) >= 0 && ((s32)(pt) >> 16) <= yhi && \
                      (il ? ((s32)(pt) >> 17) : ((s32)(pt) >> 16)) >= rlo && (il ? ((s32)(pt) >> 17) : ((s32)(pt) >> 16)) < rhi)
   while (k0 < n && !LEAN8_IN(pts[k0]))
      k0++;
   while (k1 >= k0 && !LEAN8_IN(pts[k1]))
      k1--;
#undef LEAN8_IN

   /* the texture step at k0, as the full loop would have counted it */
   quot = k0 * step_q + (k0 * step_r) / (linelength > 0 ? linelength : 1);
   rem = (k0 * step_r) % (linelength > 0 ? linelength : 1);

   for (k = k0; k <= k1; k++)
   {
      const u32 pt = pts[k];
      const int px = (s16)pt, py = (s32)pt >> 16;
      int cs, pos, yy;
      u32 idx, t;

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
      if ((py & 1) == dil)
         continue;
      yy = il ? py >> 1 : py;   /* py >= 0 inside [k0, k1] */
      pos = yy * width + px;
      if (pos >= 0x40000)
         continue;
      idx = flip ? (u32)(cwm1 - cs) : (u32)cs;
      read = 1;
      if (CM == 0 || CM == 1)
      {
         u32 dots = ram[(rowaddr + (idx >> 1)) & 0x7FFFF];
         t = (idx & 1) ? (dots & 0xF) : (dots >> 4);
      }
      else if (CM == 3)
         t = ram[(rowaddr + idx) & 0x7FFFF] & 0x7F;
      else /* CM == 4 */
         t = ram[(rowaddr + idx) & 0x7FFFF];
      if (t == 0 && !SPD)
      {
         cp = 0;
         continue;
      }
      if (CM == 0)
         cp = (colorbank & 0xf0) | t;
      else if (CM == 1)
      {
         cp = T1ReadWord((u8 *)ram, (t * 2 + colorlut) & 0x7FFFF) & 0xFF;
         if (cp == 0 && !SPD)
            continue;
      }
      else if (CM == 3)
         cp = (colorbank & 0x80) | t;
      else
         cp = t;
      VDP1W(fb8[pos]) = cp;
   }
   pen.cp = cp;
   if (read)
      pen.vis = CM == 0 ? 0xf : CM == 1 ? 0xffff : CM == 3 ? 0x7f : 0xff;
   return pen;
}

static QuadPen QuadPlotLean8CM0(const QuadLine *Lp, QuadPen pen) { return QuadPlotLean8Body(0, Lp, pen); }
static QuadPen QuadPlotLean8CM1(const QuadLine *Lp, QuadPen pen) { return QuadPlotLean8Body(1, Lp, pen); }
static QuadPen QuadPlotLean8CM3(const QuadLine *Lp, QuadPen pen) { return QuadPlotLean8Body(3, Lp, pen); }
static QuadPen QuadPlotLean8CM4(const QuadLine *Lp, QuadPen pen) { return QuadPlotLean8Body(4, Lp, pen); }

/* Untextured polygons on the 8-bit framebuffer (high resolution: Virtua
 * Fighter's fighters, Die Hard Arcade's large flat shapes): what
 * QuadPlotPoints(CM, -1, 1, ...) gives for them. Untextured means no end codes,
 * so the texel the general loop reads is replaced by the command's colour, cut
 * to 8 bits by putpixel8(): the colour, its visibility mask (by colour mode)
 * and whether it is plotted are the same for every point, so only the point
 * tests are left (clip area, double interlace, this thread's rows, buffer
 * end). No mesh, user clipping (unless the whole line is inside it) or
 * Gouraud steps. Kept small: vidsoft.c is at GCC's inlining limit. */
static __attribute__((noinline)) QuadPen QuadPlotFill8(const QuadLine *Lp, QuadPen pen)
{
   const u32 *const pts = Lp->pts;
   u8 *const fb8 = Lp->fb8;
   const int width = Lp->width, n = Lp->n, SPD = Lp->SPD, cm = Lp->colormode;
   const int il = Lp->interlace == 2, dil = Lp->dil_skip;
   const int xhi = Lp->regs->systemclipX2, yhi = Lp->regs->systemclipY2;
   const int rlo = Lp->row_lo, rhi = Lp->row_hi;
   const int vis = cm == 0 ? 0xf : cm == 2 ? 0x3f : cm == 3 ? 0x7f : cm == 4 ? 0xff : 0xffff;
   const int cp = Lp->untexturedColor & 0xFF;
   const int plot = (SPD || (cp & vis)) && !(cp == 0 && !SPD);
   int k, any = 0;

   for (k = 0; k < n; k++)
   {
      const u32 pt = pts[k];
      const int px = (s16)pt, py = (s32)pt >> 16;
      int yy, pos;

      if (px < 0 || px > xhi || py < 0 || py > yhi || (py & 1) == dil)
         continue;
      yy = il ? py >> 1 : py;
      pos = yy * width + px;
      if (yy < rlo || yy >= rhi || pos >= 0x40000)
         continue;
      any = 1;
      if (plot)
         VDP1W(fb8[pos]) = cp;
   }
   if (any)
   {
      pen.cp = cp;
      pen.vis = vis;
   }
   return pen;
}

/* The other 16-colour lines on the 8-bit framebuffer: untextured ones take
 * QuadPlotFill8(), the rest the general loop (out of line, like the fill) */
static __attribute__((noinline)) QuadPen QuadPlotOther8CM0(const QuadLine *Lp, QuadPen pen)
{
   if (!Lp->isTextured && !Lp->mesh && (Lp->noclip || !Lp->user_clip) && !(Lp->drawmode & 4))
      return QuadPlotFill8(Lp, pen);
   return QuadPlotPoints(0, -1, 1, *Lp, pen);
}

static void DrawLineQuad(const Vdp1QuadState *st, int x1, int y1, int x2, int y2, double linenumber, double texturestep,
   int linelength, double xredstep, double xgreenstep, double xbluestep, Vdp1 *regs, vdp1cmd_struct *cmd, u8 *ram, u8 *back_framebuffer)
{
   /* Everything used per pixel is a local: the texture reads go through a
    * u8 pointer, after which the compiler would otherwise have to reload
    * every global and struct field */
   u16 *const fb = (u16 *)back_framebuffer;
   u32 *const quad_pt = quad_pt_buf[vdp1_tid];
   const int colormode = st->colormode, flip = st->flip, SPD = st->SPD;
   const int endcodes = st->isTextured && st->endcodesEnabled;
   const int isTextured = st->isTextured, untexturedColor = st->untexturedColor;
   const int mesh = st->mesh, msbon = st->msbon, dil_skip = st->dil_skip;
   const u32 colorbank = st->colorbank, colorlut = st->colorlut;
   const int cw = characterWidth, interlace = vdp1interlace, width = vdp1width;
   int line = (int)linenumber;
   u32 rowaddr;
   int a, ax, ay, dx, dy, n;
   int noclip;
   /* currentStep = (int)k * texturestep without floating point: when
    * texturestep is cw / linelength, k * cw / linelength is tracked as
    * quotient and remainder. The double product can only land on the other
    * side of an integer when the exact value is one (remainder 0), so only
    * then is it computed as before. */
   const int int_step = linelength > 0 && texturestep == (double)cw / linelength;
   const int step_q = int_step ? cw / linelength : 0, step_r = int_step ? cw % linelength : 0;

   if (flip & 2)
      line = characterHeight - line - 1;
   switch (colormode)
   {
   case 0: case 1: rowaddr = st->characterAddress + (line * (cw >> 1)); break;
   case 2: case 3: case 4: rowaddr = st->characterAddress + (line * cw); break;
   default: rowaddr = st->characterAddress + (line * cw * 2); break;
   }

   {
      /* every point the walk visits lies within the endpoints' box */
      int minx = x1 < x2 ? x1 : x2, maxx = x1 < x2 ? x2 : x1;
      int miny = y1 < y2 ? y1 : y2, maxy = y1 < y2 ? y2 : y1;
      noclip = minx >= 0 && maxx <= regs->systemclipX2 && miny >= 0 && maxy <= regs->systemclipY2;
      if (st->user_clip == 1)
         noclip = noclip && minx >= regs->userclipX1 && maxx <= regs->userclipX2 &&
                  miny >= regs->userclipY1 && maxy <= regs->userclipY2;
      else if (st->user_clip == 2)
         noclip = 0;
   }

   /* the walk of iterateOverLine() with greedy set; point k is plotted with i == k */
   dx = x2 - x1;
   dy = y2 - y1;
   ax = (dx >= 0) ? 1 : -1;
   ay = (dy >= 0) ? 1 : -1;
   a = 0;
   n = 0;

   if (abs(dx) > 999 || abs(dy) > 999)
   {
      VDP1RLOG_L999();
      return;
   }

   if (abs(dx) > abs(dy)) {
      if (ax != ay) dx = -dx;
      for (; x1 != x2; x1 += ax) {
         quad_pt[n++] = QUAD_PT(x1, y1);
         a += dy;
         if (abs(a) >= abs(dx)) {
            a -= dx;
            y1 += ay;
            if (ax == ay) { quad_pt[n++] = QUAD_PT(x1 + ax, y1 - ay); }
            else { quad_pt[n++] = QUAD_PT(x1, y1); }
         }
      }
   } else {
      if (ax != ay) dy = -dy;
      for (; y1 != y2; y1 += ay) {
         quad_pt[n++] = QUAD_PT(x1, y1);
         a += dx;
         if (abs(a) >= abs(dy)) {
            a -= dy;
            x1 += ax;
            if (ay == ax) { quad_pt[n++] = QUAD_PT(x1, y1); }
            else { quad_pt[n++] = QUAD_PT(x1 - ax, y1 + ay); }
         }
      }
   }
   quad_pt[n++] = QUAD_PT(x2, y2);

   {
      QuadLine L;
      QuadPen pen;
      int lean;

      if (!int_step)
      {
         DrawLine(x1, y1, x2, y2, 1, linenumber, texturestep, xredstep, xgreenstep, xbluestep, regs, cmd, ram, back_framebuffer);
         return;
      }
      L.pts = quad_pt; L.fb = fb; L.fb8 = back_framebuffer; L.ram = ram; L.regs = regs; L.cmd = cmd;
      L.rowaddr = rowaddr; L.colorbank = colorbank; L.colorlut = colorlut;
      L.colormode = colormode; L.drawmode = st->drawmode; L.flip = flip; L.SPD = SPD;
      L.endcodes = endcodes; L.isTextured = isTextured; L.untexturedColor = untexturedColor;
      L.mesh = mesh; L.msbon = msbon; L.dil_skip = dil_skip; L.cw = cw;
      L.interlace = interlace; L.width = width; L.noclip = noclip; L.n = n; L.user_clip = st->user_clip;
      L.step_q = step_q; L.step_r = step_r; L.linelength = linelength;
      L.row_lo = vdp1_row_lo; L.row_hi = vdp1_row_hi;
      L.texturestep = texturestep; L.xredstep = xredstep; L.xgreenstep = xgreenstep; L.xbluestep = xbluestep;
      pen.cp = currentPixel; pen.vis = currentPixelIsVisible;
      pen.lr = leftColumnColor.r; pen.lg = leftColumnColor.g; pen.lb = leftColumnColor.b;

      if (st->fb8)
      {
         /* 8-bit framebuffer: MSB-on and the drawing mode don't apply
          * (putpixel8() only replaces), a 128-colour texel never matches its
          * end code, and without Gouraud the colour steps are 0 */
         const int lean8 = !(L.endcodes && colormode != 3) && !L.mesh && L.isTextured &&
                           (L.noclip || !L.user_clip) && !(L.drawmode & 4);
         switch (colormode)
         {
         case 0: pen = lean8 ? QuadPlotLean8CM0(&L, pen) : QuadPlotOther8CM0(&L, pen); break;
         case 1: pen = lean8 ? QuadPlotLean8CM1(&L, pen) : QuadPlotPoints(1, -1, 1, L, pen); break;
         case 3: pen = lean8 ? QuadPlotLean8CM3(&L, pen) : QuadPlotPoints(3, -1, 1, L, pen); break;
         case 4: pen = lean8 ? QuadPlotLean8CM4(&L, pen) : QuadPlotPoints(4, -1, 1, L, pen); break;
         default: pen = QuadPlotPoints(-1, -1, 1, L, pen); break;
         }
         currentPixel = pen.cp;
         currentPixelIsVisible = pen.vis;
         leftColumnColor.r = pen.lr;
         leftColumnColor.g = pen.lg;
         leftColumnColor.b = pen.lb;
         return;
      }

      /* the lean copies: no end codes, mesh, MSB-on or double interlace, and
       * no user clipping "outside" (QuadLeanRun) */
      lean = !L.endcodes && !L.mesh && !L.msbon && L.dil_skip < 0 && L.isTextured &&
             (L.noclip || L.user_clip != 2);

      /* RGB textures (end codes allowed) and untextured replace mode */
      if (!L.mesh && !L.msbon && L.dil_skip < 0 && (L.noclip || L.user_clip != 2) &&
          ((L.isTextured && colormode == 5 && (L.drawmode == 0 || L.drawmode == 4)) ||
           (!L.isTextured && L.drawmode == 0)))
      {
         if (!L.isTextured)
            pen = QuadPlotFill(&L, pen);
         else if (L.endcodes)
            pen = L.drawmode == 4 ? QuadPlotLeanRGBEG(&L, pen) : QuadPlotLeanRGBE(&L, pen);
         else
            pen = L.drawmode == 4 ? QuadPlotLeanRGBG(&L, pen) : QuadPlotLeanRGB(&L, pen);
         currentPixel = pen.cp;
         currentPixelIsVisible = pen.vis;
         leftColumnColor.r = pen.lr;
         leftColumnColor.g = pen.lg;
         leftColumnColor.b = pen.lb;
         return;
      }

      /* specialised copies for the common colour and drawing modes */
      switch (colormode * 8 + L.drawmode)
      {
      case 0 * 8 + 0:
         pen = lean ? QuadPlotLeanCM0(&L, pen) : QuadPlotPoints(0, 0, 0, L, pen);
         break;
      case 0 * 8 + 4: pen = QuadPlotPoints(0, 4, 0, L, pen); break;
      case 1 * 8 + 0:
         pen = lean ? QuadPlotLeanCM1(&L, pen) : QuadPlotPoints(1, 0, 0, L, pen);
         break;
      case 1 * 8 + 4:
         if (lean && L.noclip)
            pen = QuadPlotCM1Gouraud(&L, pen);
         else if (lean)
            pen = QuadPlotLeanCM1G(&L, pen);
         else
            pen = QuadPlotPoints(1, 4, 0, L, pen);
         break;
      case 4 * 8 + 0:
         pen = lean ? QuadPlotLeanCM4(&L, pen) : QuadPlotPoints(4, 0, 0, L, pen);
         break;
      case 5 * 8 + 0: pen = QuadPlotPoints(5, 0, 0, L, pen); break;
      case 5 * 8 + 4: pen = QuadPlotPoints(5, 4, 0, L, pen); break;
      default:        pen = QuadPlotPoints(-1, -1, 0, L, pen); break;
      }

      currentPixel = pen.cp;
      currentPixelIsVisible = pen.vis;
      leftColumnColor.r = pen.lr;
      leftColumnColor.g = pen.lg;
      leftColumnColor.b = pen.lb;
   }
}

static INLINE double interpolate(double start, double end, int numberofsteps) {

	double stepvalue = 0;

	if(numberofsteps == 0)
		return 1;

	stepvalue = (end - start) / numberofsteps;

	return stepvalue;
}

typedef union _COLOR { // xbgr x555
	struct {
#ifdef WORDS_BIGENDIAN
	u16 x:1;
	u16 b:5;
	u16 g:5;
	u16 r:5;
#else
     u16 r:5;
     u16 g:5;
     u16 b:5;
     u16 x:1;
#endif
	};
	u16 value;
} COLOR;

VDP1_TLS COLOR gouraudA;
VDP1_TLS COLOR gouraudB;
VDP1_TLS COLOR gouraudC;
VDP1_TLS COLOR gouraudD;

static void gouraudTable(u8* ram, Vdp1* regs, vdp1cmd_struct * cmd)
{
	int gouraudTableAddress;



	gouraudTableAddress = (((unsigned int)cmd->CMDGRDA) << 3);

   gouraudA.value = T1ReadWord(ram, gouraudTableAddress);
   gouraudB.value = T1ReadWord(ram, gouraudTableAddress + 2);
   gouraudC.value = T1ReadWord(ram, gouraudTableAddress + 4);
   gouraudD.value = T1ReadWord(ram, gouraudTableAddress + 6);
}

static int xleft_buf[2][1000], yleft_buf[2][1000], xright_buf[2][1000], yright_buf[2][1000];

static int
storeLineCoords(int x, int y, int i, void *arrays, Vdp1* regs, vdp1cmd_struct * cmd, u8* ram, u8* back_framebuffer) {
	int **intArrays = arrays;

	intArrays[0][i] = x;
	intArrays[1][i] = y;

	return 0;
}

//skip objects that are completely outside of system clipping
int is_pre_clipped(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y, Vdp1* regs)
{
   int y_val = regs->systemclipY2;

   if (vdp1interlace)
      y_val *= 2;

   //if all x values are to the left of the screen
   if ((tl_x < 0) &&
      (bl_x < 0) &&
      (tr_x < 0) &&
      (br_x < 0))
      return 1;

   //to the right
   if ((tl_x > regs->systemclipX2) &&
      (bl_x > regs->systemclipX2) &&
      (tr_x > regs->systemclipX2) &&
      (br_x > regs->systemclipX2))
      return 1;

   //above
   if ((tl_y < 0) &&
      (bl_y < 0) &&
      (tr_y < 0) &&
      (br_y < 0))
      return 1;

   //below
   if ((tl_y > y_val) &&
      (bl_y > y_val) &&
      (tr_y > y_val) &&
      (br_y > y_val))
      return 1;

   return 0;
}

//a real vdp1 draws with arbitrary lines
//this is why endcodes are possible
//this is also the reason why half-transparent shading causes moire patterns
//and the reason why gouraud shading can be applied to a single line draw command
static void drawQuad(s16 tl_x, s16 tl_y, s16 bl_x, s16 bl_y, s16 tr_x, s16 tr_y, s16 br_x, s16 br_y, u8 * ram, Vdp1* regs, vdp1cmd_struct * cmd, u8* back_framebuffer){

	int totalleft;
	int totalright;
	int total;
	int i;
	int *intarrays[2];
	int *const xleft = xleft_buf[vdp1_tid], *const yleft = yleft_buf[vdp1_tid];
	int *const xright = xright_buf[vdp1_tid], *const yright = yright_buf[vdp1_tid];

	COLOR_PARAMS topLeftToBottomLeftColorStep = {0,0,0}, topRightToBottomRightColorStep = {0,0,0};
		
	//how quickly we step through the line arrays
	double leftLineStep = 1;
	double rightLineStep = 1; 

	//a lookup table for the gouraud colors
	COLOR colors[4];

	Vdp1QuadState quad_state;
	int quad_fast;

   if (is_pre_clipped(tl_x, tl_y, bl_x, bl_y, tr_x, tr_y, br_x, br_y, regs))
   {
      VDP1RLOG_WHY("preclip");
      return;
   }

   {
      /* every point drawn lies within the corners' box: nothing to do if
       * that is all in rows another thread draws */
      int miny = tl_y, maxy = tl_y;
      if (bl_y < miny) miny = bl_y; if (bl_y > maxy) maxy = bl_y;
      if (tr_y < miny) miny = tr_y; if (tr_y > maxy) maxy = tr_y;
      if (br_y < miny) miny = br_y; if (br_y > maxy) maxy = br_y;
      if (VDP1_ROW(maxy) < vdp1_row_lo || VDP1_ROW(miny) >= vdp1_row_hi)
      {
         /* Line and polyline commands take their first Gouraud colours
          * from the table the previous command loaded (gouraudLineSetup()'s
          * arguments are read before it loads its own). Load it here too, as
          * a thread that draws all rows would, or the colours depend on
          * where the VDP1 split row lies (it moves with timing: Panzer
          * Dragoon's reticle lines changed colour now and then). */
         characterWidth = ((cmd->CMDSIZE >> 8) & 0x3F) * 8;
         characterHeight = cmd->CMDSIZE & 0xFF;
         if ((cmd->CMDPMOD & (1 << 2)) &&   /* drawQuad() returns before it for huge shapes */
             !(abs(bl_x - tl_x) > 999 || abs(bl_y - tl_y) > 999 || abs(br_x - tr_x) > 999 || abs(br_y - tr_y) > 999))
            gouraudTable(ram, regs, cmd);
         VDP1RLOG_WHY("other-thread-rows");
         return;
      }
   }

	characterWidth = ((cmd->CMDSIZE >> 8) & 0x3F) * 8;
   characterHeight = cmd->CMDSIZE & 0xFF;
   quad_fast = Vdp1QuadSetup(&quad_state, regs, cmd);

	intarrays[0] = xleft; intarrays[1] = yleft;
   totalleft = iterateOverLine(tl_x, tl_y, bl_x, bl_y, 0, intarrays, storeLineCoords, regs, cmd, ram, back_framebuffer);
	intarrays[0] = xright; intarrays[1] = yright;
   totalright = iterateOverLine(tr_x, tr_y, br_x, br_y, 0, intarrays, storeLineCoords, regs, cmd, ram, back_framebuffer);

	//just for now since burning rangers will freeze up trying to draw huge shapes
	if(totalleft == INT_MAX || totalright == INT_MAX)
	{
		VDP1RLOG_WHY("edge>999");
		return;
	}

	total = totalleft > totalright ? totalleft : totalright;


   if (cmd->CMDPMOD & (1 << 2)) {

		gouraudTable(ram, regs, cmd);

		{ colors[0] = gouraudA; colors[1] = gouraudD; colors[2] = gouraudB; colors[3] = gouraudC; }

		topLeftToBottomLeftColorStep.r = interpolate(colors[0].r,colors[1].r,total);
		topLeftToBottomLeftColorStep.g = interpolate(colors[0].g,colors[1].g,total);
		topLeftToBottomLeftColorStep.b = interpolate(colors[0].b,colors[1].b,total);

		topRightToBottomRightColorStep.r = interpolate(colors[2].r,colors[3].r,total);
		topRightToBottomRightColorStep.g = interpolate(colors[2].g,colors[3].g,total);
		topRightToBottomRightColorStep.b = interpolate(colors[2].b,colors[3].b,total);
	}

	//we have to step the equivalent of less than one pixel on the shorter side
	//to make sure textures stretch properly and the shape is correct
	if(total == totalleft && totalleft != totalright) {
		//left side is larger
		leftLineStep = 1;
		rightLineStep = (double)totalright / totalleft;
	}
	else if(totalleft != totalright){
		//right side is larger
		rightLineStep = 1;
		leftLineStep = (double)totalleft / totalright;
	}

	for(i = 0; i < total; i++) {

		int xlinelength;

		double xtexturestep;
		double ytexturestep;

		COLOR_PARAMS rightColumnColor;

		COLOR_PARAMS leftToRightStep = {0,0,0};

		{
			/* skip lines that are all in rows another thread draws */
			int ya = yleft[(int)(i*leftLineStep)], yb = yright[(int)(i*rightLineStep)];
			int miny = ya < yb ? ya : yb, maxy = ya < yb ? yb : ya;
			if (VDP1_ROW(maxy) < vdp1_row_lo || VDP1_ROW(miny) >= vdp1_row_hi)
				continue;
			/* and, on the fast path, lines that lie outside the system
			 * clipping area: every point of the walk is within the
			 * endpoints' box, so none would be plotted, and nothing a line
			 * computes carries over to the next one there */
			if (quad_fast)
			{
				int xa = xleft[(int)(i*leftLineStep)], xb = xright[(int)(i*rightLineStep)];
				int minx = xa < xb ? xa : xb, maxx = xa < xb ? xb : xa;
				if (maxx < 0 || minx > regs->systemclipX2 || maxy < 0 || miny > regs->systemclipY2)
					continue;
			}
		}

		/* the length of the line: the number of points iterateOverLine()
		 * visits with greedy set, which is |dx| + |dy| + 1 (it steps along
		 * the major axis and adds a point for every minor step) */
		{
			int ldx = abs(xright[(int)(i*rightLineStep)] - xleft[(int)(i*leftLineStep)]);
			int ldy = abs(yright[(int)(i*rightLineStep)] - yleft[(int)(i*leftLineStep)]);
			xlinelength = (ldx > 999 || ldy > 999) ? INT_MAX : ldx + ldy + 1;
		}

		//so from 0 to the width of the texture / the length of the line is how far we need to step
		xtexturestep=interpolate(0,characterWidth,xlinelength);

		//now we need to interpolate the y texture coordinate across multiple lines
		ytexturestep=interpolate(0,characterHeight,total);

		//gouraud interpolation
		if(cmd->CMDPMOD & (1 << 2)) {

			//for each new line we need to step once more through each column
			//and add the orignal color + the number of steps taken times the step value to the bottom of the shape
			//to get the current colors to use to interpolate across the line

			leftColumnColor.r = colors[0].r +(topLeftToBottomLeftColorStep.r*i);
			leftColumnColor.g = colors[0].g +(topLeftToBottomLeftColorStep.g*i);
			leftColumnColor.b = colors[0].b +(topLeftToBottomLeftColorStep.b*i);

			rightColumnColor.r = colors[2].r +(topRightToBottomRightColorStep.r*i);
			rightColumnColor.g = colors[2].g +(topRightToBottomRightColorStep.g*i);
			rightColumnColor.b = colors[2].b +(topRightToBottomRightColorStep.b*i);

			//interpolate colors across to get the right step values
			leftToRightStep.r = interpolate(leftColumnColor.r,rightColumnColor.r,xlinelength);
			leftToRightStep.g = interpolate(leftColumnColor.g,rightColumnColor.g,xlinelength);
			leftToRightStep.b = interpolate(leftColumnColor.b,rightColumnColor.b,xlinelength);
		}

		if (quad_fast)
			DrawLineQuad(&quad_state,
				xleft[(int)(i*leftLineStep)],
				yleft[(int)(i*leftLineStep)],
				xright[(int)(i*rightLineStep)],
				yright[(int)(i*rightLineStep)],
				ytexturestep*i,
				xtexturestep,
				xlinelength,
				leftToRightStep.r,
				leftToRightStep.g,
				leftToRightStep.b,
				regs, cmd, ram, back_framebuffer);
		else
		DrawLine(
			xleft[(int)(i*leftLineStep)],
			yleft[(int)(i*leftLineStep)],
			xright[(int)(i*rightLineStep)],
			yright[(int)(i*rightLineStep)],
			1,
			ytexturestep*i, 
			xtexturestep,
			leftToRightStep.r,
			leftToRightStep.g,
			leftToRightStep.b,
         regs,
         cmd,
         ram, back_framebuffer
			);
	}
}

void VIDSoftVdp1NormalSpriteDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer) {

	s16 topLeftx,topLefty,topRightx,topRighty,bottomRightx,bottomRighty,bottomLeftx,bottomLefty;
	int spriteWidth;
	int spriteHeight;
   vdp1cmd_struct cmd;
	Vdp1ReadCommand(&cmd, regs->addr, ram);

	topLeftx = cmd.CMDXA + regs->localX;
	topLefty = cmd.CMDYA + regs->localY;
	spriteWidth = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
	spriteHeight = cmd.CMDSIZE & 0xFF;

	topRightx = topLeftx + (spriteWidth - 1);
	topRighty = topLefty;
	bottomRightx = topLeftx + (spriteWidth - 1);
	bottomRighty = topLefty + (spriteHeight - 1);
	bottomLeftx = topLeftx;
	bottomLefty = topLefty + (spriteHeight - 1);

   drawQuad(topLeftx, topLefty, bottomLeftx, bottomLefty, topRightx, topRighty, bottomRightx, bottomRighty, ram, regs, &cmd, back_framebuffer);
}

void VIDSoftVdp1ScaledSpriteDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer){

	s32 topLeftx,topLefty,topRightx,topRighty,bottomRightx,bottomRighty,bottomLeftx,bottomLefty;
	int x0,y0,x1,y1;
   vdp1cmd_struct cmd;
   Vdp1ReadCommand(&cmd, regs->addr, ram);

	x0 = cmd.CMDXA + regs->localX;
	y0 = cmd.CMDYA + regs->localY;

	switch ((cmd.CMDCTRL >> 8) & 0xF)
	{
	case 0x0: // Only two coordinates
	default:
		x1 = ((int)cmd.CMDXC) - x0 + regs->localX + 1;
		y1 = ((int)cmd.CMDYC) - y0 + regs->localY + 1;
		break;
	case 0x5: // Upper-left
		x1 = ((int)cmd.CMDXB) + 1;
		y1 = ((int)cmd.CMDYB) + 1;
		break;
	case 0x6: // Upper-Center
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1/2;
		x1++;
		y1++;
		break;
	case 0x7: // Upper-Right
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1;
		x1++;
		y1++;
		break;
	case 0x9: // Center-left
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		y0 = y0 - y1/2;
		x1++;
		y1++;
		break;
	case 0xA: // Center-center
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1/2;
		y0 = y0 - y1/2;
		x1++;
		y1++;
		break;
	case 0xB: // Center-right
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1;
		y0 = y0 - y1/2;
		x1++;
		y1++;
		break;
	case 0xD: // Lower-left
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		y0 = y0 - y1;
		x1++;
		y1++;
		break;
	case 0xE: // Lower-center
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1/2;
		y0 = y0 - y1;
		x1++;
		y1++;
		break;
	case 0xF: // Lower-right
		x1 = ((int)cmd.CMDXB);
		y1 = ((int)cmd.CMDYB);
		x0 = x0 - x1;
		y0 = y0 - y1;
		x1++;
		y1++;
		break;
	}

	topLeftx = x0;
	topLefty = y0;

	topRightx = x1+x0 - 1;
	topRighty = topLefty;

	bottomRightx = x1+x0 - 1;
	bottomRighty = y1+y0 - 1;

	bottomLeftx = topLeftx;
	bottomLefty = y1+y0 - 1;

   drawQuad(topLeftx, topLefty, bottomLeftx, bottomLefty, topRightx, topRighty, bottomRightx, bottomRighty, ram, regs, &cmd, back_framebuffer);
}

void VIDSoftVdp1DistortedSpriteDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer) {

	s32 xa,ya,xb,yb,xc,yc,xd,yd;
   vdp1cmd_struct cmd;

   Vdp1ReadCommand(&cmd, regs->addr, ram);

    xa = (s32)(cmd.CMDXA + regs->localX);
    ya = (s32)(cmd.CMDYA + regs->localY);

    xb = (s32)(cmd.CMDXB + regs->localX);
    yb = (s32)(cmd.CMDYB + regs->localY);

    xc = (s32)(cmd.CMDXC + regs->localX);
    yc = (s32)(cmd.CMDYC + regs->localY);

    xd = (s32)(cmd.CMDXD + regs->localX);
    yd = (s32)(cmd.CMDYD + regs->localY);

    drawQuad(xa, ya, xd, yd, xb, yb, xc, yc, ram, regs, &cmd, back_framebuffer);
}

static void gouraudLineSetup(double * redstep, double * greenstep, double * bluestep, int length, COLOR table1, COLOR table2, u8* ram, Vdp1* regs, vdp1cmd_struct * cmd, u8 * back_framebuffer) {

	gouraudTable(ram ,regs, cmd);

	*redstep =interpolate(table1.r,table2.r,length);
	*greenstep =interpolate(table1.g,table2.g,length);
	*bluestep =interpolate(table1.b,table2.b,length);

	leftColumnColor.r = table1.r;
	leftColumnColor.g = table1.g;
	leftColumnColor.b = table1.b;
}

void VIDSoftVdp1PolylineDraw(u8* ram, Vdp1*regs, u8 * back_framebuffer)
{
	int X[4];
	int Y[4];
	double redstep = 0, greenstep = 0, bluestep = 0;
	int length;
   vdp1cmd_struct cmd;

   Vdp1ReadCommand(&cmd, regs->addr, ram);

	X[0] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x0C));
	Y[0] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x0E));
	X[1] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x10));
	Y[1] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x12));
	X[2] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x14));
	Y[2] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x16));
	X[3] = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x18));
	Y[3] = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x1A));

   length = iterateOverLine(X[0], Y[0], X[1], Y[1], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudA, gouraudB, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[0], Y[0], X[1], Y[1], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);

   length = iterateOverLine(X[1], Y[1], X[2], Y[2], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudB, gouraudC, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[1], Y[1], X[2], Y[2], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);

   length = iterateOverLine(X[2], Y[2], X[3], Y[3], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudD, gouraudC, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[3], Y[3], X[2], Y[2], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);

   length = iterateOverLine(X[3], Y[3], X[0], Y[0], 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &greenstep, &bluestep, length, gouraudA, gouraudD, ram, regs, &cmd, back_framebuffer);
   DrawLine(X[0], Y[0], X[3], Y[3], 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);
}

void VIDSoftVdp1LineDraw(u8* ram, Vdp1*regs, u8* back_framebuffer)
{
	int x1, y1, x2, y2;
	double redstep = 0, greenstep = 0, bluestep = 0;
	int length;
   vdp1cmd_struct cmd;

   Vdp1ReadCommand(&cmd, regs->addr, ram);

	x1 = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x0C));
	y1 = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x0E));
	x2 = (int)regs->localX + (int)((s16)T1ReadWord(ram, regs->addr + 0x10));
	y2 = (int)regs->localY + (int)((s16)T1ReadWord(ram, regs->addr + 0x12));

   length = iterateOverLine(x1, y1, x2, y2, 1, NULL, NULL, regs, &cmd, ram, back_framebuffer);
   gouraudLineSetup(&redstep, &bluestep, &greenstep, length, gouraudA, gouraudB, ram, regs, &cmd, back_framebuffer);
   DrawLine(x1, y1, x2, y2, 0, 0, 0, redstep, greenstep, bluestep, regs, &cmd, ram, back_framebuffer);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1UserClipping(u8* ram, Vdp1*regs)
{
   regs->userclipX1 = T1ReadWord(ram, regs->addr + 0xC);
   regs->userclipY1 = T1ReadWord(ram, regs->addr + 0xE);
   regs->userclipX2 = T1ReadWord(ram, regs->addr + 0x14);
   regs->userclipY2 = T1ReadWord(ram, regs->addr + 0x16);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1SystemClipping(u8* ram, Vdp1*regs)
{
   regs->systemclipX1 = 0;
   regs->systemclipY1 = 0;
   regs->systemclipX2 = T1ReadWord(ram, regs->addr + 0x14);
   regs->systemclipY2 = T1ReadWord(ram, regs->addr + 0x16);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1LocalCoordinate(u8* ram, Vdp1*regs)
{
   regs->localX = T1ReadWord(ram, regs->addr + 0xC);
   regs->localY = T1ReadWord(ram, regs->addr + 0xE);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1ReadFrameBuffer(u32 type, u32 addr, void * out)
{
   u32 val;

   /* 32-bit reads return 0 (see case 2): no need to draw the pending VDP1
    * job and wait for it. Burning Rangers DMAs a 176x112 picture out of the
    * framebuffer this way every third frame, and waiting made the emulation
    * idle for the whole draw. */
   if (type == 2)
   {
      *(u32*)out = 0;
      return;
   }

   VidsoftVdp1RunJob(VidsoftFbIndex(vdp1backframebuffer));
   VidsoftWaitForVdp1Thread();

   switch (type)
   {
   case 0:
      val = T1ReadByte(vdp1backframebuffer, addr);
      *(u8*)out = val;
      break;
   case 1:
      val = T1ReadWord(vdp1backframebuffer, addr);
#ifndef WORDS_BIGENDIAN
      val = BSWAP16L(val);
#endif
      *(u16*)out = val;
      break;
   case 2:
#if 0 //enable when burning rangers is fixed
      val = T1ReadLong(vdp1backframebuffer, addr);
#ifndef WORDS_BIGENDIAN
      val = BSWAP32(val);
#endif
      val = (val & 0xffff) << 16 | (val & 0xffff0000) >> 16;
      *(u32*)out = val;
#else
      *(u32*)out = 0;
#endif
      break;
   default:
      break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1WriteFrameBuffer(u32 type, u32 addr, u32 val)
{
   VidsoftVdp1RunJob(VidsoftFbIndex(vdp1backframebuffer));
   VidsoftWaitForVdp1Thread();

   switch (type)
   {
   case 0:
      T1WriteByte(vdp1backframebuffer, addr, val);
      break;
   case 1:
#ifndef WORDS_BIGENDIAN
      val = BSWAP16L(val);
#endif
      T1WriteWord(vdp1backframebuffer, addr, val);
      break;
   case 2:
#ifndef WORDS_BIGENDIAN
      val = BSWAP32(val);
#endif
      val = (val & 0xffff) << 16 | (val & 0xffff0000) >> 16;
      T1WriteLong(vdp1backframebuffer, addr, val);
      break;
   default:
      break;
   }
}

//////////////////////////////////////////////////////////////////////////////

int VIDSoftVdp2Reset(void)
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

/* Sets vidsoft_cc_lines (see there) from the displayed lines of Vdp2Lines and
   gives Titan their CCCTL; returns the CCCTL that selects the blend mode: the
   lines' when colour calculation changes inside the display and the lines agree
   on the mode (bits 8-9), else the register's. A mode that changes inside the
   display is not drawn per line. */
static u16 VidsoftCheckColorCalcLines(void)
{
   const int n = 224 + (Vdp2Regs->TVMD & 0x30) < 270 ? 224 + (Vdp2Regs->TVMD & 0x30) : 270;
   const Vdp2 *l = Vdp2Lines;
   u16 ctl_or = 0, ctl_and = 0xFFFF, diff = 0;
   int i;

   for (i = 0; i < n; i++)
   {
      ctl_or |= l[i].CCCTL;
      ctl_and &= l[i].CCCTL;
      diff |= (l[i].CCCTL ^ l[0].CCCTL) | (l[i].CCRNA ^ l[0].CCRNA) |
              (l[i].CCRNB ^ l[0].CCRNB) | (l[i].CCRR ^ l[0].CCRR);
   }
   vidsoft_cc_lines = diff != 0;
   TitanSetLineColorCalc(vidsoft_cc_lines ? ctl_or : 0);
   if (vidsoft_cc_lines && (ctl_or & 0x300) == (ctl_and & 0x300))
      return ctl_or;
   return Vdp2Regs->CCCTL;
}

void VIDSoftVdp2DrawStart(void)
{
   int titanblendmode = TITAN_BLEND_TOP;
   u16 ccctl;
   if (vdp2_skip_draw) return;
   ccctl = VidsoftCheckColorCalcLines();
   if (ccctl & 0x100) titanblendmode = TITAN_BLEND_ADD;
   else if (ccctl & 0x200) titanblendmode = TITAN_BLEND_BOTTOM;
   TitanSetBlendingMode(titanblendmode);

   Vdp2DrawBackScreen();
   Vdp2DrawLineScreen();

   //dracula x bad cycle setting
   if (Vdp2Regs->CYCA0L == 0x5566 &&
      Vdp2Regs->CYCA0U == 0x47ff &&
      Vdp2Regs->CYCA1L == 0xffff &&
      Vdp2Regs->CYCA1U == 0xffff &&
      Vdp2Regs->CYCB0L == 0x12ff &&
      Vdp2Regs->CYCB0U == 0x03ff &&
      Vdp2Regs->CYCB1L == 0xffff &&
      Vdp2Regs->CYCB1U == 0xffff)
   {
      bad_cycle_setting[TITAN_NBG3] = 1;
   }
   else
      bad_cycle_setting[TITAN_NBG3] = 0;
}

//////////////////////////////////////////////////////////////////////////////


/* TitanPutPixel() for the sprite layer, writing through a per-line pointer
 * that is claimed (and cleared if stale) on the first pixel of the line */
static INLINE void VidsoftSpritePut(struct PixelData **line, int priority, s32 x, s32 y, u32 color, int linescreen, vdp2draw_struct *info)
{
   if (priority == 0)
      return;

   if (!*line)
   {
      *line = TitanGetLayerLine(TITAN_SPRITE, y);
      if (TitanClaimLayerLine(TITAN_SPRITE, y))
         memset(*line, 0, sizeof(struct PixelData) * vdp2width);
   }

   (*line)[x].pixel = color;
   (*line)[x].meta = TitanPackMeta(priority, linescreen, info->titan_shadow_type, info->titan_shadow_enabled);
}

void VidsoftDrawSprite(Vdp2 * vdp2_regs, u8 * spr_window_mask, u8* vdp1_front_framebuffer, u8 * vdp2_ram, Vdp1* vdp1_regs, Vdp2* vdp2_lines, u8*color_ram)
{
   int i, i2;
   u16 pixel;
   u8 prioritytable[8];
   u32 vdp1coloroffset;
   int colormode = vdp2_regs->SPCTL & 0x20;
   vdp2draw_struct info = { 0 };
   int islinewindow;
   clipping_struct clip[2];
   u32 linewnd0addr, linewnd1addr;
   int wctl;
   clipping_struct colorcalcwindow[2];
   int framebuffer_readout_y = 0;
   int start_line = 0, line_increment = 0;
   int sprite_window_enabled = vdp2_regs->SPCTL & 0x10;
   int vdp1spritetype = 0;
   int post_calc = 0, clof_on = 0;
   int fast = 0, xmode = 0, ccwin_on = 0;
   u8 winline[704], ccwin[704], ccline[704];
#define SPRITE_POST(v) (post_calc == 0 ? (v) : post_calc == 1 ? DoColorOffset(&info, (v)) : info.PostPixelFetchCalc(&info, (v)))

   if (sprite_window_enabled)
   {
      memset(spr_window_mask, 0, 704 * 512);
   }

   // Figure out whether to draw vdp1 framebuffer or vdp2 framebuffer pixels
   // based on priority
   if (Vdp1External.disptoggle && (vdp2_regs->TVMD & 0x8000))
   {
      int SPCCCS = (vdp2_regs->SPCTL >> 12) & 0x3;
      int SPCCN = (vdp2_regs->SPCTL >> 8) & 0x7;
      u8 colorcalctable[8];
      vdp2rotationparameterfp_struct p;
      int x, y;
      int output_y = 0;

      prioritytable[0] = vdp2_regs->PRISA & 0x7;
      prioritytable[1] = (vdp2_regs->PRISA >> 8) & 0x7;
      prioritytable[2] = vdp2_regs->PRISB & 0x7;
      prioritytable[3] = (vdp2_regs->PRISB >> 8) & 0x7;
      prioritytable[4] = vdp2_regs->PRISC & 0x7;
      prioritytable[5] = (vdp2_regs->PRISC >> 8) & 0x7;
      prioritytable[6] = vdp2_regs->PRISD & 0x7;
      prioritytable[7] = (vdp2_regs->PRISD >> 8) & 0x7;
      colorcalctable[0] = ((~vdp2_regs->CCRSA & 0x1F) << 1) + 1;
      colorcalctable[1] = ((~vdp2_regs->CCRSA >> 7) & 0x3E) + 1;
      colorcalctable[2] = ((~vdp2_regs->CCRSB & 0x1F) << 1) + 1;
      colorcalctable[3] = ((~vdp2_regs->CCRSB >> 7) & 0x3E) + 1;
      colorcalctable[4] = ((~vdp2_regs->CCRSC & 0x1F) << 1) + 1;
      colorcalctable[5] = ((~vdp2_regs->CCRSC >> 7) & 0x3E) + 1;
      colorcalctable[6] = ((~vdp2_regs->CCRSD & 0x1F) << 1) + 1;
      colorcalctable[7] = ((~vdp2_regs->CCRSD >> 7) & 0x3E) + 1;

      vdp1coloroffset = (vdp2_regs->CRAOFB & 0x70) << 4;
      vdp1spritetype = vdp2_regs->SPCTL & 0xF;

      ReadVdp2ColorOffset(vdp2_regs, &info, 0x40, 0x40);
      /* the offset is on for the frame if enabled now; its value can change
       * per line (LoadLineParamsSprite), so a zero is skipped line by line */
      clof_on = (vdp2_regs->CLOFEN & 0x40) != 0;

      wctl = vdp2_regs->WCTLC >> 8;
      clip[0].xstart = clip[0].ystart = clip[0].xend = clip[0].yend = 0;
      clip[1].xstart = clip[1].ystart = clip[1].xend = clip[1].yend = 0;
      ReadWindowData(wctl, clip, vdp2_regs);
      linewnd0addr = linewnd1addr = 0;
      ReadLineWindowData(&islinewindow, wctl, &linewnd0addr, &linewnd1addr, vdp2_regs);

      /* color calculation window: in => no color calc, out => color calc */
      ReadWindowData(vdp2_regs->WCTLD >> 8, colorcalcwindow, vdp2_regs);

      if (vdp1_regs->TVMR & 2)
         Vdp2ReadRotationTableFP(0, &p, vdp2_regs, vdp2_ram);

      info.titan_which_layer = TITAN_SPRITE;

      info.linescreen = (vdp2_regs->LNCLEN >> 5) & 1;

      Vdp2GetInterlaceInfo(&start_line, &line_increment);

      /* The common case gets a lean loop (identical output): 16-bit
       * framebuffer, not rotated, no sprite window anywhere. The window
       * tests become per-line tables, the framebuffer readout step is fixed
       * per frame, and empty pixels cost a load and a compare. */
      fast = vdp1pixelsize == 2 && !(vdp1_regs->TVMR & 2) && !sprite_window_enabled &&
             !(wctl & 0x20) && !((vdp2_regs->WCTLD >> 8) & 0x20) && vdp2width <= 704;
      if (fast)
      {
         if (vdp1width == 512 && vdp2_x_hires)
            xmode = 1;                  /* x = i / 2 */
         else if (vdp1width == 1024 && !vdp2_x_hires)
            xmode = 2;                  /* x = i * 2 */
         else
            xmode = 0;                  /* x = i */
         /* except for a 512-wide framebuffer on a low-res screen (x = i),
            the generic loop advances its readout position only for pixels
            that pass the window test: with a window that isn't the same */
         if (!(vdp1width == 512 && !vdp2_x_hires) && (wctl & 0x2a))
            fast = 0;
         ccwin_on = (vdp2_regs->CCCTL & 0x40) != 0;
         VidsoftWindowLine(vdp2_regs->WCTLD >> 8, colorcalcwindow, 0, vdp2width, ccwin);
      }

      for (i2 = start_line; i2 < vdp2height; i2 += line_increment)
      {
         float framebuffer_readout_pos = 0;
         struct PixelData *sprite_line = NULL; // claimed on the first pixel written

         ReadLineWindowClip(islinewindow, clip, &linewnd0addr, &linewnd1addr, vdp2_ram, vdp2_regs);

         if (vdp2_interlace)
            LoadLineParamsSprite(&info, i2 / 2, vdp2_lines);
         else
            LoadLineParamsSprite(&info, i2, vdp2_lines);
         post_calc = clof_on && (info.cor | info.cog | info.cob);

         if (vdp2_interlace)
         {
            y = framebuffer_readout_y;
            framebuffer_readout_y += 1;
         }
         else
         {
            y = i2;
         }

         if (fast)
         {
            const u16 *row = (const u16 *)vdp1_front_framebuffer + y * vdp1width;
            const u8 *cc = ccwin;
            int wall = (wctl & 0x2a) == 0, wval = !((wctl & 0x80) == 0x80);
            if (!wall)
               VidsoftWindowLine(wctl, clip, i2, vdp2width, winline);
            /* the colour calc window: rectangles only (no line window), but its
               y range matters, so it is per line too when enabled */
            if ((vdp2_regs->WCTLD >> 8) & 0x0a)
            {
               VidsoftWindowLine(vdp2_regs->WCTLD >> 8, colorcalcwindow, i2, vdp2width, ccline);
               cc = ccline;
            }
            if (wall && !wval)
            {
               output_y++;
               continue;
            }
            for (i = 0; i < vdp2width; i++)
            {
               x = xmode == 0 ? i : xmode == 1 ? i >> 1 : i << 1;
               pixel = row[x];
               if (pixel == 0 || (!wall && !winline[i]))
                  continue;
               info.titan_shadow_type = 0;
               if (pixel & 0x8000 && colormode)
               {
                  u8 alpha = 0x3F;
                  if (cc[i] && ccwin_on)
                  {
                     switch (SPCCCS) {
                     case 0: if (prioritytable[0] <= SPCCN) { alpha = colorcalctable[0]; if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80; } break;
                     case 1: if (prioritytable[0] == SPCCN) { alpha = colorcalctable[0]; if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80; } break;
                     case 2: if (prioritytable[0] >= SPCCN) { alpha = colorcalctable[0]; if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80; } break;
                     case 3: alpha = colorcalctable[0]; if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80; break;
                     }
                  }
                  /* (sprite window off: 0x8000 is drawn for sprite types below 8) */
                  if (pixel != 0x8000 || vdp1spritetype < 8)
                     VidsoftSpritePut(&sprite_line, prioritytable[0], i, output_y, SPRITE_POST(COLSAT2YAB16(alpha, pixel)), info.linescreen, &info);
               }
               else
               {
                  spritepixelinfo_struct spi;
                  u8 alpha = 0x3F;
                  u32 dot;

                  Vdp1GetSpritePixelInfo(vdp1spritetype, &pixel, &spi);
                  if (spi.normalshadow)
                  {
                     info.titan_shadow_type = TITAN_NORMAL_SHADOW;
                     VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, COLSAT2YAB16(0x3f, 0), info.linescreen, &info);
                     continue;
                  }
                  dot = Vdp2ColorRamGetColor(vdp1coloroffset + pixel, color_ram);
                  if (cc[i] && ccwin_on)
                  {
                     int transparent = 0;
                     switch (SPCCCS) {
                     case 0: if (prioritytable[spi.priority] <= SPCCN) transparent = 1; break;
                     case 1: if (prioritytable[spi.priority] == SPCCN) transparent = 1; break;
                     case 2: if (prioritytable[spi.priority] >= SPCCN) transparent = 1; break;
                     case 3: if (dot & 0x80000000) transparent = 1; break;
                     }
                     if (vdp2_regs->CCCTL & 0x200) {
                        alpha = colorcalctable[spi.colorcalc];
                        if (transparent) alpha |= 0x80;
                     }
                     else if (transparent) {
                        alpha = colorcalctable[spi.colorcalc];
                        if (vdp2_regs->CCCTL & 0x100) alpha |= 0x80;
                     }
                  }
                  if (spi.msbshadow)
                  {
                     info.titan_shadow_type = TITAN_MSB_SHADOW;
                     if (pixel == 0)
                     {
                        VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, SPRITE_POST(COLSAT2YAB32(alpha, 0)), info.linescreen, &info);
                        continue;
                     }
                  }
                  VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, SPRITE_POST(COLSAT2YAB32(alpha, dot)), info.linescreen, &info);
               }
            }
            output_y++;
            continue;
         }

         for (i = 0; i < vdp2width; i++)
         {

            info.titan_shadow_type = 0;

            // See if screen position is clipped, if it isn't, continue
            if (!(vdp2_regs->SPCTL & 0x10))
            {
               if (!TestBothWindow(wctl, clip, i, i2))
               {
                  continue;
               }
            }

            if (vdp1_regs->TVMR & 2) {
               x = (touint(p.Xst + i * p.deltaX + i2 * p.deltaXst)) & (vdp1width - 1);
               y = (touint(p.Yst + i * p.deltaY + i2 * p.deltaYst)) & (vdp1height - 1);
            }
            else
            {
               if (vdp1width == 1024 && vdp2_x_hires)
               {
                  //hi res vdp1 and hi res vdp2
                  //pixels 1:1
                  x = (int)framebuffer_readout_pos;
                  framebuffer_readout_pos += 1;
               }
               else if (vdp1width == 512 && vdp2_x_hires)
               {
                  //low res vdp1,hi res vdp2
                  //vdp1 pixel doubling
                  x = (int)framebuffer_readout_pos;
                  framebuffer_readout_pos += .5;
               }
               else if (vdp1width == 1024 && (!vdp2_x_hires))
               {
                  //hi res vdp1, low res vdp2
                  //the vdp1 framebuffer is read out at half-res
                  x = (int)framebuffer_readout_pos;
                  framebuffer_readout_pos += 2;
               }
               else
                  x = i;
            }

            if (vdp1pixelsize == 2)
            {
               // 16-bit pixel size
               pixel = ((u16 *)vdp1_front_framebuffer)[(y * vdp1width) + x];

               if (pixel == 0)
                  ;
               else if (pixel & 0x8000 && colormode)
               {
                  // 16 BPP               
                  u8 alpha = 0x3F;
                  if (TestBothWindow(vdp2_regs->WCTLD >> 8, colorcalcwindow, i, i2) && (vdp2_regs->CCCTL & 0x40))
                  {
                     switch (SPCCCS) {
                     case 0:
                        if (prioritytable[0] <= SPCCN)
                        {
                           alpha = colorcalctable[0];
                           if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80;
                        }
                        break;
                     case 1:
                        if (prioritytable[0] == SPCCN)
                        {
                           alpha = colorcalctable[0];
                           if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80;
                        }
                        break;
                     case 2:
                        if (prioritytable[0] >= SPCCN)
                        {
                           alpha = colorcalctable[0];
                           if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80;
                        }
                        break;
                     case 3:
                        alpha = colorcalctable[0];
                        if (vdp2_regs->CCCTL & 0x300) alpha |= 0x80;
                        break;
                     }
                  }

                  // if pixel is 0x8000, only draw pixel if sprite window
                  // is disabled/sprite type 2-7. sprite types 0 and 1 are
                  // -always- drawn and sprite types 8-F are always
                  // transparent.
                  if (pixel != 0x8000 || vdp1spritetype < 2 || (vdp1spritetype < 8 && !(vdp2_regs->SPCTL & 0x10)))
                     VidsoftSpritePut(&sprite_line, prioritytable[0], i, output_y, SPRITE_POST(COLSAT2YAB16(alpha, pixel)), info.linescreen, &info);
               }
               else
               {
                  // Color bank
                  spritepixelinfo_struct spi;
                  u8 alpha = 0x3F;
                  u32 dot;

                  Vdp1GetSpritePixelInfo(vdp1spritetype, &pixel, &spi);
                  if (spi.normalshadow)
                  {
                     info.titan_shadow_type = TITAN_NORMAL_SHADOW;
                     VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, COLSAT2YAB16(0x3f, 0), info.linescreen, &info);
                     continue;
                  }

                  dot = Vdp2ColorRamGetColor(vdp1coloroffset + pixel,color_ram);

                  if (TestBothWindow(vdp2_regs->WCTLD >> 8, colorcalcwindow, i, i2) && (vdp2_regs->CCCTL & 0x40))
                  {
                     int transparent = 0;

                     /* Sprite color calculation */
                     switch (SPCCCS) {
                     case 0:
                        if (prioritytable[spi.priority] <= SPCCN)
                           transparent = 1;
                        break;
                     case 1:
                        if (prioritytable[spi.priority] == SPCCN)
                           transparent = 1;
                        break;
                     case 2:
                        if (prioritytable[spi.priority] >= SPCCN)
                           transparent = 1;
                        break;
                     case 3:
                        if (dot & 0x80000000)
                           transparent = 1;
                        break;
                     }

                     if (vdp2_regs->CCCTL & 0x200) {
                        /* "bottom" mode, the alpha channel will be used by another layer,
                        so we set it regardless of whether sprites are transparent or not.
                        The highest priority bit is only set if the sprite is transparent
                        (in this case, it's the alpha channel of the lower priority layer
                        that will be used. */
                        alpha = colorcalctable[spi.colorcalc];
                        if (transparent) alpha |= 0x80;
                     }
                     else if (transparent) {
                        alpha = colorcalctable[spi.colorcalc];
                        if (vdp2_regs->CCCTL & 0x100) alpha |= 0x80;
                     }
                  }
                  if (spi.msbshadow)
                  {
                     if (sprite_window_enabled) {
                        spr_window_mask[(y*vdp2width) + x] = 1;
                        info.titan_shadow_type = TITAN_MSB_SHADOW;
                     }
                     else
                     {
                        info.titan_shadow_type = TITAN_MSB_SHADOW;
                     }

                     if (pixel == 0)
                     {
                        VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, SPRITE_POST(COLSAT2YAB32(alpha, 0)), info.linescreen, &info);
                        continue;
                     }
                  }

                  if ((sprite_window_enabled))
                  {
                     if (!TestBothWindow(wctl, clip, i, i2))
                     {
                        continue;
                     }
                  }

                  VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, SPRITE_POST(COLSAT2YAB32(alpha, dot)), info.linescreen, &info);
               }
            }
            else
            {
               // 8-bit pixel size
               pixel = vdp1_front_framebuffer[(y * vdp1width) + x];

               if (pixel != 0)
               {
                  // Color bank(fix me)
                  spritepixelinfo_struct spi;
                  u8 alpha = 0x3F;
                  u32 dot;

                  Vdp1GetSpritePixelInfo(vdp1spritetype, &pixel, &spi);
                  if (spi.normalshadow)
                  {
                     info.titan_shadow_type = TITAN_NORMAL_SHADOW;
                     VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, COLSAT2YAB16(0x3f, 0), info.linescreen, &info);
                     continue;
                  }

                  dot = Vdp2ColorRamGetColor(vdp1coloroffset + pixel, color_ram);

                  if (TestBothWindow(vdp2_regs->WCTLD >> 8, colorcalcwindow, i, i2) && (vdp2_regs->CCCTL & 0x40))
                  {
                     int transparent = 0;

                     /* Sprite color calculation */
                     switch (SPCCCS) {
                     case 0:
                        if (prioritytable[spi.priority] <= SPCCN)
                           transparent = 1;
                        break;
                     case 1:
                        if (prioritytable[spi.priority] == SPCCN)
                           transparent = 1;
                        break;
                     case 2:
                        if (prioritytable[spi.priority] >= SPCCN)
                           transparent = 1;
                        break;
                     case 3:
                        if (dot & 0x80000000)
                           transparent = 1;
                        break;
                     }

                     if (vdp2_regs->CCCTL & 0x200) {
                        /* "bottom" mode, the alpha channel will be used by another layer,
                        so we set it regardless of whether sprites are transparent or not.
                        The highest priority bit is only set if the sprite is transparent
                        (in this case, it's the alpha channel of the lower priority layer
                        that will be used. */
                        alpha = colorcalctable[spi.colorcalc];
                        if (transparent) alpha |= 0x80;
                     }
                     else if (transparent) {
                        alpha = colorcalctable[spi.colorcalc];
                        if (vdp2_regs->CCCTL & 0x100) alpha |= 0x80;
                     }
                  }

                  VidsoftSpritePut(&sprite_line, prioritytable[spi.priority], i, output_y, SPRITE_POST(COLSAT2YAB32(alpha, dot)), info.linescreen, &info);
               }
            }
         }

         output_y++;
      }
   }
}
#undef SPRITE_POST


static void VidsoftWaitForLayerThreads(void)
{
   YAB_PROF_BEGIN(PROF_WAIT_LAYERS);
   if (vidsoft_num_layer_threads > 0)
   {
      int layer;
      for (layer = 0; layer < 6; layer++)
         YabFlagWait(&vidsoft_thread_context.draw_finished[layer]);
   }
   YAB_PROF_END(PROF_WAIT_LAYERS);
}

void VIDSoftVdp2DrawEnd(void)
{
   if (vdp2_skip_draw)
   {
      // frameskip: VDP1 still ran this frame, only publish its framebuffer
      VIDSoftVdp1SwapFrameBuffer();
      return;
   }

   VidsoftWaitForLayerThreads();

   TitanRender(dispbuffer);

   /* An interlaced frame only draws its own field's lines; the other field
    * keeps the previous frame's. When frameskip skipped that frame, those
    * lines are older (with every other frame skipped, they are never drawn
    * again: half the picture was frozen or black). Repeat this field's
    * lines over them instead. */
   {
      if (vdp2_interlace && vdp2_frame_number - vdp2_picture_frame != 1)
      {
         int start_line, line_increment, y;
         Vdp2GetInterlaceInfo(&start_line, &line_increment);
         for (y = start_line; y < vdp2height; y += 2)
         {
            const int other = y ^ 1;
            if (other < vdp2height)
               memcpy(dispbuffer + other * vdp2width, dispbuffer + y * vdp2width,
                      vdp2width * sizeof(pixel_t));
         }
      }
   }

   VIDSoftVdp1SwapFrameBuffer();

   if (OSDUseBuffer())
      OSDDisplayMessages(dispbuffer, vdp2width, vdp2height);

#ifdef USE_OPENGL	
   if (VideoUseGL)
   {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vdp2width, vdp2height, 0, GL_RGBA, GL_UNSIGNED_BYTE, dispbuffer);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      if (! OSDUseBuffer())
         OSDDisplayMessages(NULL, -1, -1);
   }
#endif

   YuiSwapBuffers();
}

//////////////////////////////////////////////////////////////////////////////

void VidsoftStartLayerThread(int * layer_priority, int * draw_priority_0, int * num_threads_dispatched, int which_layer, void(*layer_func) (Vdp2* lines, Vdp2* regs, u8* ram, u8* color_ram, struct CellScrollData * cell_data))
{
   if (layer_priority[which_layer] > 0 || draw_priority_0[which_layer])
   {
      if (*num_threads_dispatched < vidsoft_num_layer_threads)
      {
         vidsoft_thread_context.draw_finished[which_layer] = 0;
         YabFlagSet(vidsoft_thread_context.need_draw[which_layer], 1);
         YabThreadWake(YAB_THREAD_VIDSOFT_LAYER_NBG3 + which_layer);
         *num_threads_dispatched = *num_threads_dispatched + 1;
      }
      else
      {
        /* the colour RAM snapshot was just taken from Vdp2ColorRam (same
           bytes); drawing from it lets the layer use vidsoft_pal */
        (*layer_func) (Vdp2Lines, Vdp2Regs, Vdp2Ram, vidsoft_thread_context.color_ram, cell_scroll_data);
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

int IsSpriteWindowEnabled(u16 wtcl)
{
   if (((wtcl& (1 << 13)) == 0) &&
      ((wtcl & (1 << 5)) == 0))
      return 0;

   return 1;
}

int CanUseSpriteThread()
{
   //check if sprite window is enabled
   if ((Vdp2Regs->SPCTL & (1 << 4)) == 0)
      return 1;

   //check if any layers are using it
   if (IsSpriteWindowEnabled(Vdp2Regs->WCTLA) ||
      IsSpriteWindowEnabled(Vdp2Regs->WCTLB) ||
      IsSpriteWindowEnabled(Vdp2Regs->WCTLC) ||
      IsSpriteWindowEnabled(Vdp2Regs->WCTLD))
   {
      //thread cannot be used
      return 0;
   }
   
   return 1;
}

#ifdef YAB_PROFILE
void VIDSoftVdp2DrawScreensBody(void);
void VIDSoftVdp2DrawScreens(void) { YAB_PROF_BEGIN(PROF_DRAWSCREENS); VIDSoftVdp2DrawScreensBody(); YAB_PROF_END(PROF_DRAWSCREENS); }
#define VIDSoftVdp2DrawScreens VIDSoftVdp2DrawScreensBody
#endif
void VIDSoftVdp2DrawScreens(void)
#ifdef YAB_PROFILE
#undef VIDSoftVdp2DrawScreens
#endif
{
   int draw_priority_0[6] = { 0 };
   int layer_priority[6] = { 0 };
   int num_threads_dispatched = 0;

   if (vdp2_skip_draw) return;

   VIDSoftVdp2SetResolution(Vdp2Regs->TVMD);
   layer_priority[TITAN_NBG0] = Vdp2Regs->PRINA & 0x7;
   layer_priority[TITAN_NBG1] = ((Vdp2Regs->PRINA >> 8) & 0x7);
   layer_priority[TITAN_NBG2] = (Vdp2Regs->PRINB & 0x7);
   layer_priority[TITAN_NBG3] = ((Vdp2Regs->PRINB >> 8) & 0x7);
   layer_priority[TITAN_RBG0] = (Vdp2Regs->PRIR & 0x7);

   TitanErase();

   if (Vdp2Regs->SFPRMD & 0x3FF)
   {
      draw_priority_0[TITAN_NBG0] = (Vdp2Regs->SFPRMD >> 0) & 0x3;
      draw_priority_0[TITAN_NBG1] = (Vdp2Regs->SFPRMD >> 2) & 0x3;
      draw_priority_0[TITAN_NBG2] = (Vdp2Regs->SFPRMD >> 4) & 0x3;
      draw_priority_0[TITAN_NBG3] = (Vdp2Regs->SFPRMD >> 6) & 0x3;
      draw_priority_0[TITAN_RBG0] = (Vdp2Regs->SFPRMD >> 8) & 0x3;
   }

   if (vidsoft_num_layer_threads > 0)
   {
      YAB_PROF_BEGIN(PROF_SNAPSHOT);
      memcpy(vidsoft_thread_context.lines, Vdp2Lines, sizeof(Vdp2) * 270);
      memcpy(&vidsoft_thread_context.regs, Vdp2Regs, sizeof(Vdp2));
      VidsoftSyncRamCopy(vidsoft_thread_context.ram, Vdp2Ram, Vdp2RamDirty, "VDP2");
      memcpy(vidsoft_thread_context.color_ram, Vdp2ColorRam, 0x1000);
      memcpy(vidsoft_thread_context.cell_scroll_data, cell_scroll_data, sizeof(struct CellScrollData) * 270);
      VidsoftBuildPalette(vidsoft_thread_context.color_ram);
      YAB_PROF_END(PROF_SNAPSHOT);
   }
   else
      VidsoftBuildPalette(Vdp2ColorRam);

   //draw vdp2 sprite layer on a thread if sprite window is not enabled
   if (CanUseSpriteThread() && vidsoft_num_layer_threads > 0)
   {
      vidsoft_thread_context.vdp1regs = *Vdp1Regs;
      vidsoft_thread_context.draw_finished[TITAN_SPRITE] = 0;
      YabFlagSet(vidsoft_thread_context.need_draw[TITAN_SPRITE], 1);
      YabThreadWake(YAB_THREAD_VIDSOFT_LAYER_SPRITE);
      num_threads_dispatched++;
   }
   else
   {
      VidsoftDrawSprite(Vdp2Regs, sprite_window_mask, vdp1frontframebuffer, Vdp2Ram, Vdp1Regs, Vdp2Lines, Vdp2ColorRam);
   }

   if (vidsoft_num_layer_threads > 0)
   {
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG0, Vdp2DrawNBG0);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_RBG0, Vdp2DrawRBG0);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG1, Vdp2DrawNBG1);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG2, Vdp2DrawNBG2);
      VidsoftStartLayerThread(layer_priority, draw_priority_0, &num_threads_dispatched, TITAN_NBG3, Vdp2DrawNBG3);
   }
   else
   {
      Vdp2DrawNBG0(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
      Vdp2DrawNBG1(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
      Vdp2DrawNBG2(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
      Vdp2DrawNBG3(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
      Vdp2DrawRBG0(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp2DrawScreen(int screen)
{
   VIDSoftVdp2SetResolution(Vdp2Regs->TVMD);
   VidsoftWaitForLayerThreads(); /* they may still read vidsoft_pal */
   VidsoftBuildPalette(Vdp2ColorRam);

   switch(screen)
   {
      case 0:
         Vdp2DrawNBG0(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 1:
         Vdp2DrawNBG1(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 2:
         Vdp2DrawNBG2(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 3:
         Vdp2DrawNBG3(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
      case 4:
         Vdp2DrawRBG0(Vdp2Lines, Vdp2Regs, Vdp2Ram, Vdp2ColorRam, cell_scroll_data);
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp2SetResolution(u16 TVMD)
{
   // This needs some work

   // Horizontal Resolution
   switch (TVMD & 0x7)
   {
      case 0:
         rbg0width = vdp2width = 320;
         break;
      case 1:
         rbg0width = vdp2width = 352;
         break;
      case 2:
         vdp2width = 640;
         rbg0width = 320;
         break;
      case 3:
         vdp2width = 704;
         rbg0width = 352;
         break;
      case 4:
         rbg0width = vdp2width = 320;
         break;
      case 5:
         rbg0width = vdp2width = 352;
         break;
      case 6:
         vdp2width = 640;
         rbg0width = 320;
         break;
      case 7:
         vdp2width = 704;
         rbg0width = 352;
         break;
   }

   if ((vdp2width == 704) || (vdp2width == 640))
      vdp2_x_hires = 1;
   else
      vdp2_x_hires = 0;

   // Vertical Resolution
   switch ((TVMD >> 4) & 0x3)
   {
      case 0:
         rbg0height = vdp2height = 224;
         break;
      case 1:
         rbg0height = vdp2height = 240;
         break;
      case 2:
         rbg0height = vdp2height = 256;
         break;
      default: break;
   }

   // Check for interlace
   switch ((TVMD >> 6) & 0x3)
   {
      case 3: // Double-density Interlace
         vdp2height *= 2;
         vdp2_interlace=1;
         break;
      case 2: // Single-density Interlace
      case 0: // Non-interlace
      default: 
         vdp2_interlace = 0;
         break;
   }

   TitanSetResolution(vdp2width, vdp2height);
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1SwapFrameBuffer(void)
{
   VidsoftVdp1NoteSwap(((Vdp1Regs->FBCR & 2) == 0) || Vdp1External.manualchange);
   if (((Vdp1Regs->FBCR & 2) == 0) || Vdp1External.manualchange)
   {
		u8 *temp;
      if (vidsoft_vdp1_thread_enabled)
      {
         VidsoftWaitForVdp1Thread();
      }

      temp = vdp1frontframebuffer;
      vdp1frontframebuffer = vdp1backframebuffer;
      vdp1backframebuffer = temp;
      vidsoft_vdp1_front_gen++;
      Vdp1External.manualchange = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftVdp1EraseFrameBuffer(Vdp1* regs, u8 * back_framebuffer)
{
   if (((regs->FBCR & 2) == 0) || Vdp1External.manualerase)
   {
      VidsoftVdp1Erase(regs, back_framebuffer);
      Vdp1External.manualerase = 0;
   }
}

static void VidsoftVdp1Erase(const Vdp1* regs, u8 * back_framebuffer)
{
   int i,i2;
   int w,h;

   {
      h = (regs->EWRR & 0x1FF) + 1;
      if (h > vdp1height) h = vdp1height;
      w = ((regs->EWRR >> 6) & 0x3F8) + 8;
      if (w > vdp1width) w = vdp1width;

      if (vdp1pixelsize == 2)
      {
         for (i2 = (regs->EWLR & 0x1FF); i2 < h; i2++)
         {
            for (i = ((regs->EWLR >> 6) & 0x1F8); i < w; i++)
               ((u16 *)back_framebuffer)[(i2 * vdp1width) + i] = regs->EWDR;
         }
      }
      else
      {
         w = regs->EWRR >> 9;
         w *= 16;

         for (i2 = (regs->EWLR & 0x1FF); i2 < h; i2++)
         {
            for (i = ((regs->EWLR >> 6) & 0x1F8); i < w; i++)
            {
               int pos = (i2 * vdp1width) + i;

               if (pos < 0x3FFFF)
                  back_framebuffer[pos] = regs->EWDR & 0xFF;
            }
         }
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

void VIDSoftGetGlSize(int *width, int *height)
{
#ifdef USE_OPENGL
   int usegl = 0;

   if (VideoUseGL)
      usegl = 1;

   if (usegl)
   {
      *width = outputwidth;
      *height = outputheight;
   }
   else
#endif
   {
      *width = vdp2width;
      *height = vdp2height;
   }
}

void VIDSoftGetNativeResolution(int *width, int *height, int* interlace)
{
   *width = vdp2width;
   *height = vdp2height;
   *interlace = vdp2_interlace;
}

void VIDSoftVdp2DispOff()
{
   if (vdp2_skip_draw) return;
   TitanErase();
}
