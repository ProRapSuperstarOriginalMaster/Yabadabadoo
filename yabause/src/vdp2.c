/*  Copyright 2003-2005 Guillaume Duhamel
    Copyright 2004-2007 Theo Berkau

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

/*! \file vdp2.c
    \brief VDP2 emulation functions
*/

#include <stdlib.h>
#include "vdp2.h"
#ifdef SH2_DYNAREC
#include "sh2_dynarec/sh2_dynarec.h"
#else
#define SH2_DYNAREC_VRAM_WRITE(r, addr, size) do { } while (0)
#endif
#include "yabprof.h"
#include "debug.h"
#include "peripheral.h"
#include "scu.h"
#include "sh2core.h"
#include "smpc.h"
#include "vdp1.h"
#include "yabause.h"
#include "movie.h"
#include "osdcore.h"

u8 * Vdp2Ram;
u32 Vdp2RamDirty[4];
u8 * Vdp2ColorRam;
Vdp2 * Vdp2Regs;
Vdp2Internal_struct Vdp2Internal;
Vdp2External_struct Vdp2External;

struct CellScrollData cell_scroll_data[270];
Vdp2 Vdp2Lines[270];
/* the registers as the last display ended (VBlank-IN), see Vdp2LinesCatchUp */
static Vdp2 vdp2_vblank_regs;
static int vdp2_vblank_regs_valid;

static int autoframeskipenab=0;
static int throttlespeed=0;
/* Frontend-driven frameskip (Vdp2SetExternalFrameSkip): -1 = use the
   built-in logic above, otherwise one of VDP2_FRAMESKIP_*. */
static int externalframeskip=-1;
/* Frontend frameskip: is the next frame shown, and may skipped frames
   defer their VDP1 drawing (exact mode)? See VIDSoftVdp1LazyFrame() */
static int external_next_shown = 1, external_lazy = 0;
/* Set while a frame only runs VDP1 (VDP2_FRAMESKIP_VDP2_ONLY); the video
   core then skips VDP2 layer rendering/compositing but still swaps the VDP1
   framebuffers, so the next rendered frame has up-to-date sprites. */
int vdp2_skip_draw=0;
/* Is a VDP1 draw due? With plot trigger mode 2 VDP1 starts drawing at a
   frame change, so in manual change mode (FBCR.FCM = 1) it draws only after
   the game asked for a change. Games running at 30 fps or less change every
   second frame or so; drawing at every VBlank-OUT redrew the same list into
   a buffer that was erased and drawn again before it was shown.
   Set at VBlank-IN when a change happened, cleared when a real video core
   draws: under frameskip the dummy core's draws don't count, so the change
   is drawn in the next frame that renders. Starts at 1 so a loaded state
   draws. */
static int vdp1_draw_pending = 1;
static int vdp1_draw_every_frame = 0;  /* core option; YAB_VDP1_EVERYFRAME=0/1 overrides it for tests */
static int vdp1_draw_env_checked = 0;
/* The real video core while a skipped frame runs on VIDDummy */
static VideoInterface_struct *vdp2_saved_vidcore = NULL;
u64 lastticks=0;
static int fps;
int vdp2_is_odd_frame = 0;
u32 vdp2_frame_number = 0;  /* counts VBlank-OUTs */

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL Vdp2RamReadByte(u32 addr) {
   addr &= 0x7FFFF;
   return T1ReadByte(Vdp2Ram, addr);
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL Vdp2RamReadWord(u32 addr) {
   addr &= 0x7FFFF;
   return T1ReadWord(Vdp2Ram, addr);
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL Vdp2RamReadLong(u32 addr) {
   addr &= 0x7FFFF;
   return T1ReadLong(Vdp2Ram, addr);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2RamWriteByte(u32 addr, u8 val) {
   addr &= 0x7FFFF;
   VDP2_RAM_MARK(addr);
   T1WriteByte(Vdp2Ram, addr, val);
   SH2_DYNAREC_VRAM_WRITE(1, addr, 1);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2RamWriteWord(u32 addr, u16 val) {
   addr &= 0x7FFFF;
   VDP2_RAM_MARK(addr);
   T1WriteWord(Vdp2Ram, addr, val);
   SH2_DYNAREC_VRAM_WRITE(1, addr, 2);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2RamWriteLong(u32 addr, u32 val) {
   addr &= 0x7FFFF;
   VDP2_RAM_MARK(addr);
   T1WriteLong(Vdp2Ram, addr, val);
   SH2_DYNAREC_VRAM_WRITE(1, addr, 4);
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL Vdp2ColorRamReadByte(u32 addr) {
   addr &= 0xFFF;
   return T2ReadByte(Vdp2ColorRam, addr);
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL Vdp2ColorRamReadWord(u32 addr) {
   addr &= 0xFFF;
   return T2ReadWord(Vdp2ColorRam, addr);
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL Vdp2ColorRamReadLong(u32 addr) {
   addr &= 0xFFF;
   return T2ReadLong(Vdp2ColorRam, addr);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2ColorRamWriteByte(u32 addr, u8 val) {
   addr &= 0xFFF;
   T2WriteByte(Vdp2ColorRam, addr, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2ColorRamWriteWord(u32 addr, u16 val) {
   addr &= 0xFFF;
   T2WriteWord(Vdp2ColorRam, addr, val);
//   if (Vdp2Internal.ColorMode == 0)
//      T1WriteWord(Vdp2ColorRam, addr + 0x800, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2ColorRamWriteLong(u32 addr, u32 val) {
   addr &= 0xFFF;
   T2WriteLong(Vdp2ColorRam, addr, val);
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL Sh2Vdp2RamReadByte(SH2_struct *sh, u32 addr) {
   return Vdp2RamReadByte(addr);
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL Sh2Vdp2RamReadWord(SH2_struct *sh, u32 addr) {
   return Vdp2RamReadWord(addr);
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL Sh2Vdp2RamReadLong(SH2_struct *sh, u32 addr) {
   return Vdp2RamReadLong(addr);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2RamWriteByte(SH2_struct *sh, u32 addr, u8 val) {
   Vdp2RamWriteByte(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2RamWriteWord(SH2_struct *sh, u32 addr, u16 val) {
   Vdp2RamWriteWord(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2RamWriteLong(SH2_struct *sh, u32 addr, u32 val) {
   Vdp2RamWriteLong(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL Sh2Vdp2ColorRamReadByte(SH2_struct *sh, u32 addr) {
   return Vdp2ColorRamReadByte(addr);
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL Sh2Vdp2ColorRamReadWord(SH2_struct *sh, u32 addr) {
   return Vdp2ColorRamReadWord(addr);
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL Sh2Vdp2ColorRamReadLong(SH2_struct *sh, u32 addr) {
   return Vdp2ColorRamReadLong(addr);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2ColorRamWriteByte(SH2_struct *sh, u32 addr, u8 val) {
   Vdp2ColorRamWriteByte(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2ColorRamWriteWord(SH2_struct *sh, u32 addr, u16 val) {
   Vdp2ColorRamWriteWord(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2ColorRamWriteLong(SH2_struct *sh, u32 addr, u32 val) {
   Vdp2ColorRamWriteLong(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

int Vdp2Init(void) {
   if ((Vdp2Regs = (Vdp2 *) calloc(1, sizeof(Vdp2))) == NULL)
      return -1;

   if ((Vdp2Ram = T1MemoryInit(0x80000)) == NULL)
      return -1;
   VDP2_RAM_MARK_ALL();

   if ((Vdp2ColorRam = T2MemoryInit(0x1000)) == NULL)
      return -1;

   Vdp2Reset();
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2DeInit(void) {
   if (Vdp2Regs)
      free(Vdp2Regs);
   Vdp2Regs = NULL;

   if (Vdp2Ram)
      T1MemoryDeInit(Vdp2Ram);
   Vdp2Ram = NULL;

   if (Vdp2ColorRam)
      T2MemoryDeInit(Vdp2ColorRam);
   Vdp2ColorRam = NULL;
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2Reset(void) {
   Vdp2Regs->TVMD = 0x0000;
   Vdp2Regs->EXTEN = 0x0000;
   Vdp2Regs->TVSTAT = Vdp2Regs->TVSTAT & 0x1;
   Vdp2Regs->VRSIZE = 0x0000; // fix me(version should be set)
   Vdp2Regs->RAMCTL = 0x0000;
   Vdp2Regs->BGON = 0x0000;
   Vdp2Regs->CHCTLA = 0x0000;
   Vdp2Regs->CHCTLB = 0x0000;
   Vdp2Regs->BMPNA = 0x0000;
   Vdp2Regs->MPOFN = 0x0000;
   Vdp2Regs->MPABN2 = 0x0000;
   Vdp2Regs->MPCDN2 = 0x0000;
   Vdp2Regs->SCXIN0 = 0x0000;
   Vdp2Regs->SCXDN0 = 0x0000;
   Vdp2Regs->SCYIN0 = 0x0000;
   Vdp2Regs->SCYDN0 = 0x0000;
   Vdp2Regs->ZMXN0.all = 0x00000000;
   Vdp2Regs->ZMYN0.all = 0x00000000;
   Vdp2Regs->SCXIN1 = 0x0000;
   Vdp2Regs->SCXDN1 = 0x0000;
   Vdp2Regs->SCYIN1 = 0x0000;
   Vdp2Regs->SCYDN1 = 0x0000;
   Vdp2Regs->ZMXN1.all = 0x00000000;
   Vdp2Regs->ZMYN1.all = 0x00000000;
   Vdp2Regs->SCXN2 = 0x0000;
   Vdp2Regs->SCYN2 = 0x0000;
   Vdp2Regs->SCXN3 = 0x0000;
   Vdp2Regs->SCYN3 = 0x0000;
   Vdp2Regs->ZMCTL = 0x0000;
   Vdp2Regs->SCRCTL = 0x0000;
   Vdp2Regs->VCSTA.all = 0x00000000;
   Vdp2Regs->BKTAU = 0x0000;
   Vdp2Regs->BKTAL = 0x0000;
   Vdp2Regs->RPMD = 0x0000;
   Vdp2Regs->RPRCTL = 0x0000;
   Vdp2Regs->KTCTL = 0x0000;
   Vdp2Regs->KTAOF = 0x0000;
   Vdp2Regs->OVPNRA = 0x0000;
   Vdp2Regs->OVPNRB = 0x0000;
   Vdp2Regs->WPSX0 = 0x0000;
   Vdp2Regs->WPSY0 = 0x0000;
   Vdp2Regs->WPEX0 = 0x0000;
   Vdp2Regs->WPEY0 = 0x0000;
   Vdp2Regs->WPSX1 = 0x0000;
   Vdp2Regs->WPSY1 = 0x0000;
   Vdp2Regs->WPEX1 = 0x0000;
   Vdp2Regs->WPEY1 = 0x0000;
   Vdp2Regs->WCTLA = 0x0000;
   Vdp2Regs->WCTLB = 0x0000;
   Vdp2Regs->WCTLC = 0x0000;
   Vdp2Regs->WCTLD = 0x0000;
   Vdp2Regs->SPCTL = 0x0000;
   Vdp2Regs->SDCTL = 0x0000;
   Vdp2Regs->CRAOFA = 0x0000;
   Vdp2Regs->CRAOFB = 0x0000;
   Vdp2Regs->LNCLEN = 0x0000;
   Vdp2Regs->SFPRMD = 0x0000;
   Vdp2Regs->CCCTL = 0x0000;
   Vdp2Regs->SFCCMD = 0x0000;
   Vdp2Regs->PRISA = 0x0000;
   Vdp2Regs->PRISB = 0x0000;
   Vdp2Regs->PRISC = 0x0000;
   Vdp2Regs->PRISD = 0x0000;
   Vdp2Regs->PRINA = 0x0000;
   Vdp2Regs->PRINB = 0x0000;
   Vdp2Regs->PRIR = 0x0000;
   Vdp2Regs->CCRNA = 0x0000;
   Vdp2Regs->CCRNB = 0x0000;
   Vdp2Regs->CLOFEN = 0x0000;
   Vdp2Regs->CLOFSL = 0x0000;
   Vdp2Regs->COAR = 0x0000;
   Vdp2Regs->COAG = 0x0000;
   Vdp2Regs->COAB = 0x0000;
   Vdp2Regs->COBR = 0x0000;
   Vdp2Regs->COBG = 0x0000;
   Vdp2Regs->COBB = 0x0000;

   yabsys.VBlankLineCount = 225;
   Vdp2Internal.ColorMode = 0;
   vdp2_vblank_regs_valid = 0;

   Vdp2External.disptoggle = 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
void Vdp2VBlankINBody(void);
void Vdp2VBlankIN(void) { YAB_PROF_BEGIN(PROF_VBLANKIN); Vdp2VBlankINBody(); YAB_PROF_END(PROF_VBLANKIN); }
#define Vdp2VBlankIN Vdp2VBlankINBody
#endif
void Vdp2VBlankIN(void) {
#ifdef YAB_PROFILE
#undef Vdp2VBlankIN
#endif
   memcpy(&vdp2_vblank_regs, Vdp2Regs, sizeof(Vdp2));
   vdp2_vblank_regs_valid = 1;
   if (!(Vdp1Regs->FBCR & 2) || Vdp1External.changerequest)
      vdp1_draw_pending = 1;
   Vdp1External.changerequest = 0;
   VIDCore->Vdp2DrawEnd();
   /* this should be done after a frame change or a plot trigger */
   Vdp1Regs->COPR = 0;

   /* I'm not 100% sure about this, but it seems that when using manual change
   we should swap framebuffers in the "next field" and thus, clear the CEF...
   now we're lying a little here as we're not swapping the framebuffers. */
   if (Vdp1External.manualchange) Vdp1Regs->EDSR >>= 1;

   Vdp2Regs->TVSTAT |= 0x0008;
   ScuSendVBlankIN();

   if (yabsys.IsSSH2Running)
      SH2SendInterrupt(SSH2, 0x43, 0x6);
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
void Vdp2HBlankINBody(void);
void Vdp2HBlankIN(void) { YAB_PROF_BEGIN(PROF_HBLANK); Vdp2HBlankINBody(); YAB_PROF_END(PROF_HBLANK); }
#define Vdp2HBlankIN Vdp2HBlankINBody
#endif
void Vdp2HBlankIN(void) {
#ifdef YAB_PROFILE
#undef Vdp2HBlankIN
#endif
   Vdp2Regs->TVSTAT |= 0x0004;
   ScuSendHBlankIN();

   if (yabsys.IsSSH2Running)
      SH2SendInterrupt(SSH2, 0x41, 0x2);
}

//////////////////////////////////////////////////////////////////////////////

#ifdef YAB_PROFILE
void Vdp2HBlankOUTBody(void);
void Vdp2HBlankOUT(void) { YAB_PROF_BEGIN(PROF_HBLANK); Vdp2HBlankOUTBody(); YAB_PROF_END(PROF_HBLANK); }
#define Vdp2HBlankOUT Vdp2HBlankOUTBody
#endif
void Vdp2HBlankOUT(void) {
#ifdef YAB_PROFILE
#undef Vdp2HBlankOUT
#endif
   int i;
   Vdp2Regs->TVSTAT &= ~0x0004;

   if (yabsys.LineCount < 270)
   {
      u32 cell_scroll_table_start_addr = (Vdp2Regs->VCSTA.all & 0x7FFFE) << 1;
      memcpy(Vdp2Lines + yabsys.LineCount, Vdp2Regs, sizeof(Vdp2));

      for (i = 0; i < 88; i++)
      {
         cell_scroll_data[yabsys.LineCount].data[i] = T1ReadLong(Vdp2Ram, cell_scroll_table_start_addr + i * 4);
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

Vdp2 * Vdp2RestoreRegs(int line, Vdp2* lines) {
   return line > 270 ? NULL : lines + line;
}

//////////////////////////////////////////////////////////////////////////////

static void FPSDisplay(void)
{
   static int fpsframecount = 0;
   static u64 fpsticks;

   OSDPushMessage(OSDMSG_FPS, 1, "%02d/%02d FPS", fps, yabsys.IsPal ? 50 : 60);
   OSDPushMessage(OSDMSG_DEBUG, 1, "%d %d %s %s", framecounter, lagframecounter, MovieStatus, InputDisplayString);
   fpsframecount++;
   if(YabauseGetTicks() >= fpsticks + yabsys.tickfreq)
   {
      fps = fpsframecount;
      fpsframecount = 0;
      fpsticks = YabauseGetTicks();
   }
}

//////////////////////////////////////////////////////////////////////////////

void SpeedThrottleEnable(void) {
   throttlespeed = 1;
}

//////////////////////////////////////////////////////////////////////////////

void SpeedThrottleDisable(void) {
   throttlespeed = 0;
}

//////////////////////////////////////////////////////////////////////////////

/* The renderers draw the whole frame at VBlank-OUT, at the start of the
   display. A layer is drawn if BGON enables it now and, line by line, if
   BGON enabled it in Vdp2Lines, which were captured during the previous
   frame's display; other per-line registers (colour offsets, plane
   addresses) come from Vdp2Lines too. A layer switched on during the VBlank
   in between would be on for the frame but off on every line: not drawn.
   VF2's intro switches its FMV between NBG0 and NBG1 this way every third
   frame (one black frame each time; Beetle shows none).
   So VBlank-IN keeps a copy of the registers as the display ended, and when
   the game changed BGON since then (a scene switch), VBlank-OUT carries
   every register it changed into the lines, from line 0 up to the first line
   where that register changed during the display (mid-frame changes stay).
   All of them, not just BGON: a new scene's layers need their new plane
   addresses too (Princess Crown, loading: garbage tiles otherwise). Nothing
   is carried when BGON stayed: games pair fades (colour offsets) with VDP1
   pictures, which reach the screen a frame late here as well (Sega Rally:
   carrying fades over made its brightness flicker). */
static void Vdp2LinesCatchUp(void)
{
   const u16 *now = (const u16 *)Vdp2Regs;
   const u16 *before = (const u16 *)&vdp2_vblank_regs;
   int last = yabsys.VBlankLineCount < 270 ? yabsys.VBlankLineCount : 270;
   int i, line;

   if (!vdp2_vblank_regs_valid)
      return;
   vdp2_vblank_regs_valid = 0;

   if (Vdp2Regs->BGON == vdp2_vblank_regs.BGON || Vdp2Regs->BGON == Vdp2Lines[0].BGON)
      return;

   for (i = 0; i < (int)(sizeof(Vdp2) / sizeof(u16)); i++)
   {
      u16 old = ((u16 *)&Vdp2Lines[0])[i];
      if (now[i] == before[i] || now[i] == old)
         continue;
      for (line = 0; line < last && ((u16 *)&Vdp2Lines[line])[i] == old; line++)
         ((u16 *)&Vdp2Lines[line])[i] = now[i];
   }
}

#ifdef YAB_PROFILE
void Vdp2VBlankOUTBody(void);
void Vdp2VBlankOUT(void) { YAB_PROF_BEGIN(PROF_VBLANKOUT); Vdp2VBlankOUTBody(); YAB_PROF_END(PROF_VBLANKOUT); }
#define Vdp2VBlankOUT Vdp2VBlankOUTBody
#endif
void Vdp2VBlankOUT(void) {
#ifdef YAB_PROFILE
#undef Vdp2VBlankOUT
#endif
   static int framestoskip = 0;
   static int framesskipped = 0;
   static int skipnextframe = 0;
   static u64 curticks = 0;
   static u64 diffticks = 0;
   static u32 framecount = 0;
   static u64 onesecondticks = 0;

   YabProfFrame();

   if (vdp2_is_odd_frame)
      vdp2_is_odd_frame = 0;
   else
      vdp2_is_odd_frame = 1;
   vdp2_frame_number++;

   Vdp2Regs->TVSTAT = ((Vdp2Regs->TVSTAT & ~0x0008) & ~0x0002) | (vdp2_is_odd_frame << 1);

   if (externalframeskip >= 0)
   {
      skipnextframe = externalframeskip == VDP2_FRAMESKIP_ALL;
      vdp2_skip_draw = externalframeskip == VDP2_FRAMESKIP_VDP2_ONLY;
      framestoskip = 0;
   }
   else
      /* Frameskip was switched off: a VDP2_ONLY frame must not stay in
         effect, or VDP2 never draws again (black screen, sound running) */
      vdp2_skip_draw = 0;

   if (skipnextframe && (! vdp2_saved_vidcore))
   {
      vdp2_saved_vidcore = VIDCore;
      VIDCore = &VIDDummy;
   }
   else if (vdp2_saved_vidcore && (! skipnextframe))
   {
      VIDCore = vdp2_saved_vidcore;
      vdp2_saved_vidcore = NULL;
   }

   Vdp2LinesCatchUp();

   if (VIDCore->id == 2 /* VIDCORE_SOFT */)
   {
      extern void VIDSoftVdp1LazyFrame(int active, int shown, int next_shown, int draw_due);
      VIDSoftVdp1LazyFrame(externalframeskip >= 0 && external_lazy, externalframeskip <= VDP2_FRAMESKIP_NONE /* -1: frameskip off */,
                           external_next_shown, Vdp1Regs->PTMR == 2 && (vdp1_draw_pending || vdp1_draw_every_frame));
   }

   VIDCore->Vdp2DrawStart();

   if (!vdp1_draw_env_checked)
   {
      const char *env = getenv("YAB_VDP1_EVERYFRAME");
      if (env)
         vdp1_draw_every_frame = env[0] == '1';
      vdp1_draw_env_checked = 1;
   }

   {
      const int draw = Vdp1Regs->PTMR == 2 && (vdp1_draw_pending || vdp1_draw_every_frame);
      if (draw && VIDCore->id != VIDCORE_DUMMY)
         vdp1_draw_pending = 0;

      if (Vdp2Regs->TVMD & 0x8000) {
         VIDCore->Vdp2DrawScreens();
         if (draw) Vdp1Draw();
      }
      else
      {
         VIDCore->Vdp2DispOff();
         if (draw) Vdp1Draw();
      }
      /* the list wasn't finished yet: draw it again next frame */
      if (draw && Vdp1ListIncomplete && VIDCore->id != VIDCORE_DUMMY && !vdp1_draw_every_frame)
         vdp1_draw_pending = 1;
   }

   FPSDisplay();
   if ((Vdp1Regs->FBCR & 2) && (Vdp1Regs->TVMR & 8))
      Vdp1External.manualerase = 1;

   if (!skipnextframe)
   {
      framesskipped = 0;

      if (framestoskip > 0)
         skipnextframe = 1;
   }
   else
   {
      framestoskip--;

      if (framestoskip < 1)
         skipnextframe = 0;
      else
         skipnextframe = 1;

      framesskipped++;
   }

   // Do Frame Skip/Frame Limiting/Speed Throttling here
   if (throttlespeed)
   {
      // Should really depend on how fast we're rendering the frames
      if (framestoskip < 1)
         framestoskip = 6;
   }
   //when in frame advance, disable frame skipping
   else if (autoframeskipenab && FrameAdvanceVariable == 0)
   {
      framecount++;

      if (framecount > (yabsys.IsPal ? 50 : 60))
      {
         framecount = 1;
         onesecondticks = 0;
      }

      curticks = YabauseGetTicks();
      diffticks = curticks-lastticks;

      if ((onesecondticks+diffticks) > ((yabsys.OneFrameTime * (u64)framecount) + (yabsys.OneFrameTime / 2)) &&
          framesskipped < 9)
      {
         // Skip the next frame
         skipnextframe = 1;

         // How many frames should we skip?
         framestoskip = 1;
      }
      else if ((onesecondticks+diffticks) < ((yabsys.OneFrameTime * (u64)framecount) - (yabsys.OneFrameTime / 2)))
      {
         // Check to see if we need to limit speed at all
         for (;;)
         {
            curticks = YabauseGetTicks();
            diffticks = curticks-lastticks;
            if ((onesecondticks+diffticks) >= (yabsys.OneFrameTime * (u64)framecount))
               break;
         }
      }

      onesecondticks += diffticks;
      lastticks = curticks;
   }

   ScuSendVBlankOUT();
   
   if (Vdp2Regs->EXTEN & 0x200) // Should be revised for accuracy(should occur only occur on the line it happens at, etc.)
   {
      // Only Latch if EXLTEN is enabled
      if (SmpcRegs->EXLE & 0x1)
         Vdp2SendExternalLatch((PORTDATA1.data[3]<<8)|PORTDATA1.data[4], (PORTDATA1.data[5]<<8)|PORTDATA1.data[6]);
	}
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2SendExternalLatch(int hcnt, int vcnt)
{
   Vdp2Regs->HCNT = hcnt << 1;
   Vdp2Regs->VCNT = vcnt;
   Vdp2Regs->TVSTAT |= 0x200;
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL Vdp2ReadByte(u32 addr) {
   LOG("VDP2 register byte read = %08X\n", addr);
   addr &= 0x1FF;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL Vdp2ReadWord(u32 addr) {
   addr &= 0x1FF;

   switch (addr)
   {
      case 0x000:
         return Vdp2Regs->TVMD;
      case 0x002:
         if (!(Vdp2Regs->EXTEN & 0x200))
         {
            // Latch HV counter on read
            // Vdp2Regs->HCNT = ?;
            Vdp2Regs->VCNT = yabsys.LineCount;
            Vdp2Regs->TVSTAT |= 0x200;
         }

         return Vdp2Regs->EXTEN;
      case 0x004:
      {
         u16 tvstat = Vdp2Regs->TVSTAT;

         // Clear External latch and sync flags
         Vdp2Regs->TVSTAT &= 0xFCFF;

         // if TVMD's DISP bit is cleared, TVSTAT's VBLANK bit is always set
         if (Vdp2Regs->TVMD & 0x8000)
            return tvstat;
         else
            return (tvstat | 0x8);
      }
      case 0x006:         
         return Vdp2Regs->VRSIZE;
      case 0x008:
         return Vdp2Regs->HCNT;
      case 0x00A:
         return Vdp2Regs->VCNT;
      default:
      {
         LOG("Unhandled VDP2 word read: %08X\n", addr);
         break;
      }
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL Vdp2ReadLong(u32 addr) {
   LOG("VDP2 register long read = %08X\n", addr);
   addr &= 0x1FF;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2WriteByte(u32 addr, UNUSED u8 val) {
   LOG("VDP2 register byte write = %08X\n", addr);
   addr &= 0x1FF;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2WriteWord(u32 addr, u16 val) {
   addr &= 0x1FF;

   switch (addr)
   {
      case 0x000:
         Vdp2Regs->TVMD = val;
         yabsys.VBlankLineCount = 225+(val & 0x30);
         return;
      case 0x002:
         Vdp2Regs->EXTEN = val;
         return;
      case 0x004:
         // TVSTAT is read-only
         return;
      case 0x006:
         Vdp2Regs->VRSIZE = val;
         return;
      case 0x008:
         // HCNT is read-only
         return;
      case 0x00A:
         // VCNT is read-only
         return;
      case 0x00C:
         // Reserved
         return;
      case 0x00E:
         Vdp2Regs->RAMCTL = val;
         Vdp2Internal.ColorMode = (val >> 12) & 0x3;
         return;
      case 0x010:
         Vdp2Regs->CYCA0L = val;
         return;
      case 0x012:
         Vdp2Regs->CYCA0U = val;
         return;
      case 0x014:
         Vdp2Regs->CYCA1L = val;
         return;
      case 0x016:
         Vdp2Regs->CYCA1U = val;
         return;
      case 0x018:
         Vdp2Regs->CYCB0L = val;
         return;
      case 0x01A:
         Vdp2Regs->CYCB0U = val;
         return;
      case 0x01C:
         Vdp2Regs->CYCB1L = val;
         return;
      case 0x01E:
         Vdp2Regs->CYCB1U = val;
         return;
      case 0x020:
         Vdp2Regs->BGON = val;
         return;
      case 0x022:
         Vdp2Regs->MZCTL = val;
         return;
      case 0x024:
         Vdp2Regs->SFSEL = val;
         return;
      case 0x026:
         Vdp2Regs->SFCODE = val;
         return;
      case 0x028:
         Vdp2Regs->CHCTLA = val;
         return;
      case 0x02A:
         Vdp2Regs->CHCTLB = val;
         return;
      case 0x02C:
         Vdp2Regs->BMPNA = val;
         return;
      case 0x02E:
         Vdp2Regs->BMPNB = val;
         return;
      case 0x030:
         Vdp2Regs->PNCN0 = val;
         return;
      case 0x032:
         Vdp2Regs->PNCN1 = val;
         return;
      case 0x034:
         Vdp2Regs->PNCN2 = val;
         return;
      case 0x036:
         Vdp2Regs->PNCN3 = val;
         return;
      case 0x038:
         Vdp2Regs->PNCR = val;
         return;
      case 0x03A:
         Vdp2Regs->PLSZ = val;
         return;
      case 0x03C:
         Vdp2Regs->MPOFN = val;
         return;
      case 0x03E:
         Vdp2Regs->MPOFR = val;
         return;
      case 0x040:
         Vdp2Regs->MPABN0 = val;
         return;
      case 0x042:
         Vdp2Regs->MPCDN0 = val;
         return;
      case 0x044:
         Vdp2Regs->MPABN1 = val;
         return;
      case 0x046:
         Vdp2Regs->MPCDN1 = val;
         return;
      case 0x048:
         Vdp2Regs->MPABN2 = val;
         return;
      case 0x04A:
         Vdp2Regs->MPCDN2 = val;
         return;
      case 0x04C:
         Vdp2Regs->MPABN3 = val;
         return;
      case 0x04E:
         Vdp2Regs->MPCDN3 = val;
         return;
      case 0x050:
         Vdp2Regs->MPABRA = val;
         return;
      case 0x052:
         Vdp2Regs->MPCDRA = val;
         return;
      case 0x054:
         Vdp2Regs->MPEFRA = val;
         return;
      case 0x056:
         Vdp2Regs->MPGHRA = val;
         return;
      case 0x058:
         Vdp2Regs->MPIJRA = val;
         return;
      case 0x05A:
         Vdp2Regs->MPKLRA = val;
         return;
      case 0x05C:
         Vdp2Regs->MPMNRA = val;
         return;
      case 0x05E:
         Vdp2Regs->MPOPRA = val;
         return;
      case 0x060:
         Vdp2Regs->MPABRB = val;
         return;
      case 0x062:
         Vdp2Regs->MPCDRB = val;
         return;
      case 0x064:
         Vdp2Regs->MPEFRB = val;
         return;
      case 0x066:
         Vdp2Regs->MPGHRB = val;
         return;
      case 0x068:
         Vdp2Regs->MPIJRB = val;
         return;
      case 0x06A:
         Vdp2Regs->MPKLRB = val;
         return;
      case 0x06C:
         Vdp2Regs->MPMNRB = val;
         return;
      case 0x06E:
         Vdp2Regs->MPOPRB = val;
         return;
      case 0x070:
         Vdp2Regs->SCXIN0 = val;
         return;
      case 0x072:
         Vdp2Regs->SCXDN0 = val;
         return;
      case 0x074:
         Vdp2Regs->SCYIN0 = val;
         return;
      case 0x076:
         Vdp2Regs->SCYDN0 = val;
         return;
      case 0x078:
         Vdp2Regs->ZMXN0.part.I = val;
         return;
      case 0x07A:
         Vdp2Regs->ZMXN0.part.D = val;
         return;
      case 0x07C:
         Vdp2Regs->ZMYN0.part.I = val;
         return;
      case 0x07E:
         Vdp2Regs->ZMYN0.part.D = val;
         return;
      case 0x080:
         Vdp2Regs->SCXIN1 = val;
         return;
      case 0x082:
         Vdp2Regs->SCXDN1 = val;
         return;
      case 0x084:
         Vdp2Regs->SCYIN1 = val;
         return;
      case 0x086:
         Vdp2Regs->SCYDN1 = val;
         return;
      case 0x088:
         Vdp2Regs->ZMXN1.part.I = val;
         return;
      case 0x08A:
         Vdp2Regs->ZMXN1.part.D = val;
         return;
      case 0x08C:
         Vdp2Regs->ZMYN1.part.I = val;
         return;
      case 0x08E:
         Vdp2Regs->ZMYN1.part.D = val;
         return;
      case 0x090:
         Vdp2Regs->SCXN2 = val;
         return;
      case 0x092:
         Vdp2Regs->SCYN2 = val;
         return;
      case 0x094:
         Vdp2Regs->SCXN3 = val;
         return;
      case 0x096:
         Vdp2Regs->SCYN3 = val;
         return;
      case 0x098:
         Vdp2Regs->ZMCTL = val;
         return;
      case 0x09A:
         Vdp2Regs->SCRCTL = val;
         return;
      case 0x09C:
         Vdp2Regs->VCSTA.part.U = val;
         return;
      case 0x09E:
         Vdp2Regs->VCSTA.part.L = val;
         return;
      case 0x0A0:
         Vdp2Regs->LSTA0.part.U = val;
         return;
      case 0x0A2:
         Vdp2Regs->LSTA0.part.L = val;
         return;
      case 0x0A4:
         Vdp2Regs->LSTA1.part.U = val;
         return;
      case 0x0A6:
         Vdp2Regs->LSTA1.part.L = val;
         return;
      case 0x0A8:
         Vdp2Regs->LCTA.part.U = val;
         return;
      case 0x0AA:
         Vdp2Regs->LCTA.part.L = val;
         return;
      case 0x0AC:
         Vdp2Regs->BKTAU = val;
         return;
      case 0x0AE:
         Vdp2Regs->BKTAL = val;
         return;
      case 0x0B0:
         Vdp2Regs->RPMD = val;
         return;
      case 0x0B2:
         Vdp2Regs->RPRCTL = val;
         return;
      case 0x0B4:
         Vdp2Regs->KTCTL = val;
         return;
      case 0x0B6:
         Vdp2Regs->KTAOF = val;
         return;
      case 0x0B8:
         Vdp2Regs->OVPNRA = val;
         return;
      case 0x0BA:
         Vdp2Regs->OVPNRB = val;
         return;
      case 0x0BC:
         Vdp2Regs->RPTA.part.U = val;
         return;
      case 0x0BE:
         Vdp2Regs->RPTA.part.L = val;
         return;
      case 0x0C0:
         Vdp2Regs->WPSX0 = val;
         return;
      case 0x0C2:
         Vdp2Regs->WPSY0 = val;
         return;
      case 0x0C4:
         Vdp2Regs->WPEX0 = val;
         return;
      case 0x0C6:
         Vdp2Regs->WPEY0 = val;
         return;
      case 0x0C8:
         Vdp2Regs->WPSX1 = val;
         return;
      case 0x0CA:
         Vdp2Regs->WPSY1 = val;
         return;
      case 0x0CC:
         Vdp2Regs->WPEX1 = val;
         return;
      case 0x0CE:
         Vdp2Regs->WPEY1 = val;
         return;
      case 0x0D0:
         Vdp2Regs->WCTLA = val;
         return;
      case 0x0D2:
         Vdp2Regs->WCTLB = val;
         return;
      case 0x0D4:
         Vdp2Regs->WCTLC = val;
         return;
      case 0x0D6:
         Vdp2Regs->WCTLD = val;
         return;
      case 0x0D8:
         Vdp2Regs->LWTA0.part.U = val;
         return;
      case 0x0DA:
         Vdp2Regs->LWTA0.part.L = val;
         return;
      case 0x0DC:
         Vdp2Regs->LWTA1.part.U = val;
         return;
      case 0x0DE:
         Vdp2Regs->LWTA1.part.L = val;
         return;
      case 0x0E0:
         Vdp2Regs->SPCTL = val;
         return;
      case 0x0E2:
         Vdp2Regs->SDCTL = val;
         return;
      case 0x0E4:
         Vdp2Regs->CRAOFA = val;
         return;
      case 0x0E6:
         Vdp2Regs->CRAOFB = val;
         return;     
      case 0x0E8:
         Vdp2Regs->LNCLEN = val;
         return;
      case 0x0EA:
         Vdp2Regs->SFPRMD = val;
         return;
      case 0x0EC:
         Vdp2Regs->CCCTL = val;
         return;     
      case 0x0EE:
         Vdp2Regs->SFCCMD = val;
         return;
      case 0x0F0:
         Vdp2Regs->PRISA = val;
         return;
      case 0x0F2:
         Vdp2Regs->PRISB = val;
         return;
      case 0x0F4:
         Vdp2Regs->PRISC = val;
         return;
      case 0x0F6:
         Vdp2Regs->PRISD = val;
         return;
      case 0x0F8:
         Vdp2Regs->PRINA = val;
         return;
      case 0x0FA:
         Vdp2Regs->PRINB = val;
         return;
      case 0x0FC:
         Vdp2Regs->PRIR = val;
         return;
      case 0x0FE:
         // Reserved
         return;
      case 0x100:
         Vdp2Regs->CCRSA = val;
         return;
      case 0x102:
         Vdp2Regs->CCRSB = val;
         return;
      case 0x104:
         Vdp2Regs->CCRSC = val;
         return;
      case 0x106:
         Vdp2Regs->CCRSD = val;
         return;
      case 0x108:
         Vdp2Regs->CCRNA = val;
         return;
      case 0x10A:
         Vdp2Regs->CCRNB = val;
         return;
      case 0x10C:
         Vdp2Regs->CCRR = val;
         return;
      case 0x10E:
         Vdp2Regs->CCRLB = val;
         return;
      case 0x110:
         Vdp2Regs->CLOFEN = val;
         return;
      case 0x112:
         Vdp2Regs->CLOFSL = val;
         return;
      case 0x114:
         Vdp2Regs->COAR = val;
         return;
      case 0x116:
         Vdp2Regs->COAG = val;
         return;
      case 0x118:
         Vdp2Regs->COAB = val;
         return;
      case 0x11A:
         Vdp2Regs->COBR = val;
         return;
      case 0x11C:
         Vdp2Regs->COBG = val;
         return;
      case 0x11E:
         Vdp2Regs->COBB = val;
         return;
      default:
      {
         LOG("Unhandled VDP2 word write: %08X\n", addr);
         break;
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Vdp2WriteLong(u32 addr, u32 val) {
   
   Vdp2WriteWord(addr,val>>16);
   Vdp2WriteWord(addr+2,val&0xFFFF);
   return;
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL Sh2Vdp2ReadByte(SH2_struct *sh, u32 addr) {
   return Vdp2ReadByte(addr);
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL Sh2Vdp2ReadWord(SH2_struct *sh, u32 addr) {
   return Vdp2ReadWord(addr);
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL Sh2Vdp2ReadLong(SH2_struct *sh, u32 addr) {
   return Vdp2ReadLong(addr);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2WriteByte(SH2_struct *sh, u32 addr, UNUSED u8 val) {
   Vdp2WriteByte(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2WriteWord(SH2_struct *sh, u32 addr, u16 val) {
   Vdp2WriteWord(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Sh2Vdp2WriteLong(SH2_struct *sh, u32 addr, u32 val) {
   Vdp2WriteLong(addr, val);
}

//////////////////////////////////////////////////////////////////////////////

int Vdp2SaveState(void ** stream)
{
   int offset;
   
   offset = MemStateWriteHeader(stream, "VDP2", 1);

   // Write registers
   MemStateWrite((void *)Vdp2Regs, sizeof(Vdp2), 1, stream);

   // Write VDP2 ram
   MemStateWrite((void *)Vdp2Ram, 0x80000, 1, stream);

   // Write CRAM
   MemStateWrite((void *)Vdp2ColorRam, 0x1000, 1, stream);

   // Write internal variables
   MemStateWrite((void *)&Vdp2Internal, sizeof(Vdp2Internal_struct), 1, stream);

   return MemStateFinishHeader(stream, offset);
}

//////////////////////////////////////////////////////////////////////////////

int Vdp2LoadState(const void * stream, UNUSED int version, int size)
{
   // Read registers
   MemStateRead((void *)Vdp2Regs, sizeof(Vdp2), 1, stream);
   vdp1_draw_pending = 1;

   // Read VDP2 ram
   MemStateRead((void *)Vdp2Ram, 0x80000, 1, stream);
   VDP2_RAM_MARK_ALL();

   // Read CRAM
   MemStateRead((void *)Vdp2ColorRam, 0x1000, 1, stream);

   // Read internal variables
   MemStateRead((void *)&Vdp2Internal, sizeof(Vdp2Internal_struct), 1, stream);

   return size;
}

//////////////////////////////////////////////////////////////////////////////

void ToggleNBG0(void)
{
   Vdp2External.disptoggle ^= 0x1;
}

//////////////////////////////////////////////////////////////////////////////

void ToggleNBG1(void)
{
   Vdp2External.disptoggle ^= 0x2;
}

//////////////////////////////////////////////////////////////////////////////

void ToggleNBG2(void)
{
   Vdp2External.disptoggle ^= 0x4;
}

//////////////////////////////////////////////////////////////////////////////

void ToggleNBG3(void)
{
   Vdp2External.disptoggle ^= 0x8;
}

//////////////////////////////////////////////////////////////////////////////

void ToggleRBG0(void)
{
   Vdp2External.disptoggle ^= 0x10;
}

//////////////////////////////////////////////////////////////////////////////

void ToggleFullScreen(void)
{
   if (VIDCore->IsFullscreen())
   {
      VIDCore->Resize(320, 224, 0);
   }
   else
   {
      VIDCore->Resize(640, 480, 1);
   }
}

//////////////////////////////////////////////////////////////////////////////

void EnableAutoFrameSkip(void)
{
   autoframeskipenab = 1;
   lastticks = YabauseGetTicks();
}

//////////////////////////////////////////////////////////////////////////////

void DisableAutoFrameSkip(void)
{
   autoframeskipenab = 0;
}

//////////////////////////////////////////////////////////////////////////////

/* Put the real video core back if a skipped frame left VIDDummy in place,
   e.g. before shutting video down: otherwise the real core's DeInit (which
   stops its worker threads) would never run. */
void Vdp2RestoreVideoCore(void)
{
   if (vdp2_saved_vidcore)
   {
      VIDCore = vdp2_saved_vidcore;
      vdp2_saved_vidcore = NULL;
   }
}

/* Draw VDP1 at every VBlank-OUT in plot trigger mode 2 (the old behaviour)
   instead of only after a frame change. Core option. */
void Vdp2SetVdp1DrawEveryFrame(int every)
{
   if (!getenv("YAB_VDP1_EVERYFRAME"))
      vdp1_draw_every_frame = every;
}

void Vdp2SetExternalFrameSkip(int skip)
{
   externalframeskip = skip;
}

void Vdp2SetExternalFrameSkipNext(int next_shown, int lazy)
{
   external_next_shown = next_shown;
   external_lazy = lazy;
}

//////////////////////////////////////////////////////////////////////////////
