/*  Copyright 2007 Guillaume Duhamel

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

/*! \file m68kmusashi.c
\brief Musashi 68000 interface.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68kmusashi.h"
#include "musashi/m68k.h"
#include "m68kcore.h"
#include "musashi/m68kcpu.h"   /* defines uint: after the system headers */

struct ReadWriteFuncs
{
   M68K_READ  *r_8;
   M68K_READ  *r_16;
   M68K_WRITE *w_8;
   M68K_WRITE *w_16;
}rw_funcs;

static int M68KMusashiInit(void) {

   m68k_init();
   m68k_set_reset_instr_callback(m68k_pulse_reset);
   m68k_set_cpu_type(M68K_CPU_TYPE_68000);

   return 0;
}

static void M68KMusashiDeInit(void) {
}

static void M68KMusashiReset(void) {
   m68k_pulse_reset();
   M68KMusashiIdleReset();
}

/* Idle-loop skipping.

   Sound drivers spend most of their time polling: the Sega drivers scan
   their command slots and poll the SCSP timer flag in a loop that changes
   nothing but a counter until the SH2 writes a command or the timer fires
   (NiGHTS: 60% of the 68K's instructions). Such a loop is learned by
   single-stepping now and then (a probe): a pass of code that comes back to
   the same instruction with all registers and SR equal, whose writes leave
   memory as they found it (stack slots, values stored again) or step a
   counter that only the writing instruction reads, and whose other reads
   (its inputs) are of sound RAM or side-effect-free SCSP registers.

   While the 68K is at an instruction of the pass with the registers it had
   there, and every input still holds the value it had, the time slice is
   skipped: running it would only go round the loop. The counters are
   stepped as the pass would have stepped them. When anything changes (an
   input, an interrupt being raised) the 68K is put where the skipped time
   has got it to in the pass, so it carries on exactly as if it had run:
   frames and audio are identical with and without skipping. Writes to the
   loop's code drop it. */

int m68k_idle_skip = 1;

#define IDLE_LOOPS      4
#define IDLE_READS      32
#define IDLE_COUNTERS   2
#define IDLE_STEPS      256     /* instructions per pass */
#define IDLE_WRITES     48      /* writes per pass that leave memory as they found it */
#define IDLE_RANGE      0x1000  /* bytes of code a loop may span */
#define PROBE_STEPS     768
#define PROBE_MEMOPS    2048
#define PROBE_CYCLES    3000    /* give up a probe after this many cycles */
#define PROBE_WAIT_CYCLES 30000 /* or, waiting to relearn a loop, after this many */
#define IDLE_REPROBE    4       /* slices between probes while in a loop whose values changed */

typedef struct
{
   u32 lo, hi;                           /* code range, hi exclusive */
   u8 pcmap[IDLE_RANGE / 16];            /* instruction starts in the loop, one bit per word */
   int nreads;
   u32 raddr[IDLE_READS], rval[IDLE_READS];
   u8 rsize[IDLE_READS];
   int ncounters;
   u32 caddr[IDLE_COUNTERS];
   s32 cdelta[IDLE_COUNTERS];
   u8 csize[IDLE_COUNTERS];
   u32 coff[IDLE_COUNTERS];              /* cycles into the pass when it steps */
   /* every instruction of the pass: where it is, the registers before it,
      and how many cycles into the pass it starts */
   int nsteps;
   u32 spc[IDLE_STEPS], ssr[IDLE_STEPS], soff[IDLE_STEPS];
   u32 sregs[IDLE_STEPS][16];
   /* the pass's other writes (stack slots, values stored again), to put
      memory in the state of any point of the pass */
   int nwrites;
   u32 waddr[IDLE_WRITES], wval[IDLE_WRITES], wold[IDLE_WRITES];
   u16 wstep[IDLE_WRITES];
   u8 wsize[IDLE_WRITES];
   u32 iter_cycles, lastuse;
   u32 vphase;                           /* where the skipped time has got to in the pass */
   int vvalid;
   int cand;                             /* the step the 68K is at (idle_match) */
} IdleLoop;

static IdleLoop idle_loops[IDLE_LOOPS];
static int idle_env = -2, idle_log;      /* YAB_68K_IDLE, YAB_68K_IDLE_LOG */
static u64 idle_skipped, idle_run;
static u32 stat_probes, stat_psteps, stat_classify, stat_checks, stat_found;
static double stat_tprobe, stat_trun, stat_tcheck;
static u32 stat_steps_found, stat_steps_fail, stat_fail_reason[4];
#include <time.h>
static double stat_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static int idle_nloops;
static u32 idle_clock;                   /* counts slices, for replacing the least recently used loop */
static u32 probe_wait, probe_interval = 16;
static u32 relearn_interval = IDLE_REPROBE;   /* doubles while relearning fails */
static u32 relearn_sig = 0xFFFFFFFF;           /* the loop's inputs at the previous slice */

/* probe log: registers before each instruction and the data accesses it made.
   A probe runs over as many time slices as it needs (an iteration can be
   longer than a slice); running ahead of the SCSP instead would change the
   sound (key-on/off pairs written within one burst are never heard). */
static int probe_active, probe_session, probe_found;
static int probe_step, probe_cur;         /* next step to record; step being executed */
static s32 probe_cycles;
int m68k_probe_hook_active;               /* Musashi calls m68k_probe_hook() before each instruction */
#define PROBE_NO_REM 0x7FFFFFFF
static s32 probe_last_rem = PROBE_NO_REM;  /* cycles left before the previous instruction */
static u16 probe_hash[1024];              /* PC -> last step there + 1 */
static u32 probe_head = 0xFFFFFFFF;       /* record from this PC on (relearning a known loop) */
static struct { u32 pc, sr, regs[16], memop, cycles; } probe_st[PROBE_STEPS];
static struct { u32 addr, val, old; u16 step; u8 size, write; } probe_mem[PROBE_MEMOPS];
static int probe_nmem, probe_overflow;

/* the address the 68K sees in sound RAM, or the SCSP register address */
static INLINE u32 idle_norm(u32 addr)
{
   addr &= 0xFFFFFF;
   return addr < 0x100000 ? (addr & 0x7FFFF) : addr;
}

static u32 idle_read(u32 addr, u32 size);

/* the value a write replaces; SCSP registers are not read (side effects),
   a write to one never counts as storing the same value */
static u32 probe_old(u32 addr, u32 size, u32 value)
{
   return idle_norm(addr) < 0x100000 ? idle_read(addr, size) : ~value;
}

static void probe_log(u32 addr, u32 size, u32 val, u32 old, int write)
{
   if (probe_nmem >= PROBE_MEMOPS) { probe_overflow = 1; return; }
   probe_mem[probe_nmem].addr = idle_norm(addr);
   probe_mem[probe_nmem].size = size;
   probe_mem[probe_nmem].val = val;
   probe_mem[probe_nmem].old = old;
   probe_mem[probe_nmem].write = write;
   probe_mem[probe_nmem].step = probe_cur;
   probe_nmem++;
}

static u32 idle_read(u32 addr, u32 size)
{
   if (size == 1) return rw_funcs.r_8(addr) & 0xFF;
   if (size == 2) return rw_funcs.r_16(addr) & 0xFFFF;
   return ((rw_funcs.r_16(addr) & 0xFFFF) << 16) | (rw_funcs.r_16(addr + 2) & 0xFFFF);
}

static void idle_write(u32 addr, u32 size, u32 val)
{
   if (size == 1) rw_funcs.w_8(addr, val & 0xFF);
   else if (size == 2) rw_funcs.w_16(addr, val & 0xFFFF);
   else { rw_funcs.w_16(addr, val >> 16); rw_funcs.w_16(addr + 2, val & 0xFFFF); }
}

/* reading these has no side effect: sound RAM and the SCSP registers other
   than the MIDI input */
static int idle_readable(u32 addr, u32 size)
{
   if (addr < 0x80000)
      return 1;
   if (addr >= 0x100000 && addr < 0x100EE4)
      return addr + size <= 0x100404 || addr >= 0x100408;
   return 0;
}

/* Steps first..last-1 ran from one visit of an instruction to the next with
   the same registers. Learns them as a loop if they qualify. */
static const char *probe_reject;
#define REJECT(r) do { probe_reject = (r); return 0; } while (0)

static int probe_classify(int first, int last)
{
   IdleLoop l;
   int i, j, k, m0 = probe_st[first].memop, m1 = probe_st[last].memop;
   u32 lo = 0xFFFFFFFF, hi = 0;

   memset(&l, 0, sizeof(l));
   for (i = first; i < last; i++)
   {
      if (probe_st[i].pc < lo) lo = probe_st[i].pc;
      if (probe_st[i].pc + 2 > hi) hi = probe_st[i].pc + 2;
      l.iter_cycles += probe_st[i].cycles;
   }
   if (hi - lo > IDLE_RANGE || l.iter_cycles == 0 || last - first > IDLE_STEPS)
      REJECT("range");
   l.nsteps = last - first;
   for (i = first, j = 0; i < last; i++)
   {
      l.spc[i - first] = probe_st[i].pc;
      l.ssr[i - first] = probe_st[i].sr;
      memcpy(l.sregs[i - first], probe_st[i].regs, sizeof(l.sregs[0]));
      l.soff[i - first] = j;
      j += probe_st[i].cycles;
   }
   l.lo = lo; l.hi = hi + 8;   /* the last instruction's extension words */
   if (l.hi - l.lo > IDLE_RANGE) l.hi = l.lo + IDLE_RANGE;
   for (i = first; i < last; i++)
   {
      u32 w = (probe_st[i].pc - lo) >> 1;
      l.pcmap[w >> 3] |= 1 << (w & 7);
   }

   /* Writes, per address: either the pass leaves the value as it found it
      (values stored again, stack slots, temporaries), or it is a counter:
      written once per pass, read only by the instruction that writes it. */
   for (k = m0; k < m1; k++)
   {
      const u32 a = probe_mem[k].addr, sz = probe_mem[k].size;
      int first = -1, lastw = -1, nw = 0;
      if (!probe_mem[k].write)
         continue;
      for (j = m0; j < m1; j++)
         if (probe_mem[j].write && probe_mem[j].addr < a + sz && a < probe_mem[j].addr + probe_mem[j].size)
         {
            if (probe_mem[j].addr != a || probe_mem[j].size != sz)
               REJECT("write-overlap");
            if (first < 0) first = j;
            lastw = j;
            nw++;
         }
      if (first != k)
         continue;                          /* handled at its first write */
      if (probe_mem[lastw].val == probe_mem[first].old)
         continue;                          /* the pass leaves it as it was */
      if (nw != 1 || l.ncounters >= IDLE_COUNTERS || a >= 0x80000)
         REJECT("changes-memory");
      for (j = m0; j < m1; j++)
         if (j != k && !probe_mem[j].write && probe_mem[j].addr < a + sz && a < probe_mem[j].addr + probe_mem[j].size &&
             (probe_mem[j].addr != a || probe_mem[j].size != sz ||
              probe_st[probe_mem[j].step].pc != probe_st[probe_mem[k].step].pc))
            REJECT("counter-shared");
      {
         const u32 mask = sz == 1 ? 0xFF : sz == 2 ? 0xFFFF : 0xFFFFFFFF;
         s32 d = (s32)((probe_mem[k].val - probe_mem[k].old) & mask);
         if (sz == 1) d = (s8)d; else if (sz == 2) d = (s16)d;
         if (d < -16 || d > 16)
            REJECT("counter-delta");
         l.caddr[l.ncounters] = a;
         l.csize[l.ncounters] = sz;
         l.cdelta[l.ncounters] = d;
         l.coff[l.ncounters] = l.soff[probe_mem[k].step - first] + probe_st[probe_mem[k].step].cycles;
         l.ncounters++;
      }
   }

   for (k = m0; k < m1; k++)
   {
      if (!probe_mem[k].write)
         continue;
      for (j = 0; j < l.ncounters; j++)
         if (l.caddr[j] == probe_mem[k].addr)
            break;
      if (j < l.ncounters)
         continue;
      if (probe_mem[k].addr >= 0x80000 || l.nwrites >= IDLE_WRITES)
         REJECT("writes-limit");
      l.waddr[l.nwrites] = probe_mem[k].addr;
      l.wsize[l.nwrites] = probe_mem[k].size;
      l.wval[l.nwrites] = probe_mem[k].val;
      l.wold[l.nwrites] = probe_mem[k].old;
      l.wstep[l.nwrites] = probe_mem[k].step - first;
      l.nwrites++;
   }

   /* Reads: of a counter, or of what the pass itself wrote earlier, are
      internal; the rest are the loop's inputs, which must not change
      during the pass */
   for (k = m0; k < m1; k++)
   {
      const u32 a = probe_mem[k].addr, sz = probe_mem[k].size;
      int internal = 0;
      if (probe_mem[k].write)
         continue;
      for (j = 0; j < l.ncounters; j++)
         if (l.caddr[j] == a)
            internal = 1;
      for (j = m0; j < k && !internal; j++)
         if (probe_mem[j].write && probe_mem[j].addr < a + sz && a < probe_mem[j].addr + probe_mem[j].size)
            internal = 1;
      if (internal)
         continue;
      if (!idle_readable(a, sz))
         REJECT("unreadable");
      for (j = 0; j < l.nreads; j++)
         if (l.raddr[j] == a && l.rsize[j] == sz)
            break;
      if (j < l.nreads)
      {
         if (l.rval[j] != probe_mem[k].val)
            REJECT("value-changed");
         continue;
      }
      if (l.nreads >= IDLE_READS)
         REJECT("reads-limit");
      l.raddr[l.nreads] = a;
      l.rsize[l.nreads] = sz;
      l.rval[l.nreads] = probe_mem[k].val;
      l.nreads++;
   }

   /* keep it, replacing an overlapping or the least recently used loop */
   for (i = 0; i < idle_nloops; i++)
      if (idle_loops[i].lo < l.hi && l.lo < idle_loops[i].hi)
         break;
   if (i == idle_nloops)
   {
      if (idle_nloops < IDLE_LOOPS)
         idle_nloops++;
      else
         for (i = 0, j = 1; j < IDLE_LOOPS; j++)
            if (idle_loops[j].lastuse < idle_loops[i].lastuse)
               i = j;
   }
   l.lastuse = idle_clock;
   idle_loops[i] = l;
   return 1;
}

static int probe_same_regs(int a, int b)
{
   return probe_st[a].sr == probe_st[b].sr &&
          memcmp(probe_st[a].regs, probe_st[b].regs, sizeof(probe_st[a].regs)) == 0;
}

static void probe_stop(void)
{
   m68k_probe_hook_active = 0;
   probe_active = 0;
}

/* before each instruction while a probe records: the registers, and
   whether this instruction was last reached with the same ones */
void m68k_probe_hook(void)
{
   const int i = probe_step;
   const u32 pc = REG_PC;
   const s32 rem = GET_CYCLES();
   u16 *h = &probe_hash[(pc >> 1) & 1023];
   int j;

   if (probe_head != 0xFFFFFFFF)
   {
      /* relearning a loop: one pass from its first instruction is enough,
         so the busy work before it is not recorded */
      if (pc != probe_head)
         return;
      probe_head = 0xFFFFFFFF;
      probe_active = 1;
      probe_last_rem = PROBE_NO_REM;
   }
   if (i > 0 && probe_last_rem != PROBE_NO_REM)
      probe_st[i - 1].cycles = probe_last_rem - rem;
   probe_last_rem = rem;
   if (i >= PROBE_STEPS || probe_overflow)
   {
      probe_stop();
      return;
   }
   probe_st[i].pc = pc;
   probe_st[i].sr = m68ki_get_sr();
   memcpy(probe_st[i].regs, REG_DA, sizeof(probe_st[i].regs));
   probe_st[i].memop = probe_nmem;
   j = (int)*h - 1;
   if (j >= 0 && probe_st[j].pc == pc && probe_same_regs(i, j))
   {
      stat_classify++;
      if (probe_classify(j, i))
      {
         probe_found = 1;
         stat_found++;
         probe_stop();
         return;
      }
   }
   *h = i + 1;
   probe_cur = i;
   probe_step = i + 1;
   stat_psteps++;
}

/* runs the slice recording each instruction, looking for an idle loop;
   head: the first instruction of a known loop to start recording at, or ~0 */
static s32 idle_probe(s32 cycles, u32 head)
{
   s32 done;

   if (!probe_session)
   {
      probe_head = head;
      stat_probes++;
      probe_session = 1;
      probe_found = 0;
      probe_nmem = 0;
      probe_overflow = 0;
      probe_step = 0;
      probe_cycles = 0;
      memset(probe_hash, 0, sizeof(probe_hash));
   }
   probe_last_rem = PROBE_NO_REM;
   probe_active = probe_head == 0xFFFFFFFF;
   m68k_probe_hook_active = 1;
   done = m68k_execute(cycles);
   if (m68k_probe_hook_active && probe_step > 0 && probe_last_rem != PROBE_NO_REM)
      probe_st[probe_step - 1].cycles = probe_last_rem - GET_CYCLES();
   probe_stop();
   probe_cycles += done;
   if (probe_found || probe_step >= PROBE_STEPS || probe_overflow || CPU_STOPPED ||
       probe_cycles >= (probe_head != 0xFFFFFFFF ? PROBE_WAIT_CYCLES : PROBE_CYCLES))
   {
      if (probe_found) stat_steps_found += probe_step; else { stat_steps_fail += probe_step;
         stat_fail_reason[probe_step >= PROBE_STEPS ? 0 : probe_overflow ? 1 : CPU_STOPPED ? 2 : 3]++; }
      probe_session = 0;
      probe_head = 0xFFFFFFFF;
      if (probe_found)
      {
         probe_interval = 16;
         relearn_interval = IDLE_REPROBE;
      }
      else
      {
         /* busy or no loop: probe less often */
         if (probe_interval < 1024)
            probe_interval <<= 1;
         if (relearn_interval < 256)
            relearn_interval <<= 1;
      }
   }
   return done;
}

static u32 idle_miss[IDLE_READS + 1];

/* 1: idle in the loop; -1: in the loop's code but in another state or with
   other input values (busy now, or idle again with other values, e.g. queue
   pointers that moved on: worth learning it again); 0: elsewhere */
static int idle_match(IdleLoop *l, u32 pc)
{
   int i;
   u32 w;
   if (pc < l->lo || pc >= l->hi)
      return 0;
   w = (pc - l->lo) >> 1;
   if (!(l->pcmap[w >> 3] & (1 << (w & 7))))
      return 0;
   if (!l->vvalid)
   {
      const u32 sr = m68ki_get_sr();
      for (i = 0; i < l->nsteps; i++)
         if (l->spc[i] == pc && l->ssr[i] == sr && memcmp(l->sregs[i], REG_DA, sizeof(l->sregs[i])) == 0)
            break;
      if (i == l->nsteps)
         return -1;
      l->cand = i;
   }
   for (i = 0; i < l->nreads; i++)
      if (idle_read(l->raddr[i], l->rsize[i]) != l->rval[i])
      {
         if (UNLIKELY(idle_log)) idle_miss[i]++;
         return -1;
      }
   return 1;
}

/* Skips the slice (idle_match() found the 68K at step cand of the pass).
   The counters step as often as the pass would have reached them in the
   skipped time, counted from that point of the pass: drivers use them as a
   load meter (Shining Force III reports "busy" to the SH2 when only one
   pass ran between two timer ticks). */
static int idle_skip(IdleLoop *l, s32 cycles)
{
   int i;
   if (!l->vvalid)
   {
      l->vphase = l->soff[l->cand];
      l->vvalid = 1;
   }
   for (i = 0; i < l->ncounters; i++)
   {
      /* steps at pass times coff, coff + iter, ... in (vphase, vphase + cycles] */
      u32 first = (l->coff[i] + l->iter_cycles - l->vphase) % l->iter_cycles, n;
      if (first == 0)
         first = l->iter_cycles;
      n = (u32)cycles >= first ? 1 + ((u32)cycles - first) / l->iter_cycles : 0;
      if (n)
         idle_write(l->caddr[i], l->csize[i],
                    idle_read(l->caddr[i], l->csize[i]) + l->cdelta[i] * (s32)n);
   }
   l->vphase = (l->vphase + cycles) % l->iter_cycles;
   l->lastuse = idle_clock;
   return 1;
}

/* Skipping leaves the 68K where it was when it started, but had it run, it
   would have gone round the loop: put it (registers, PC, and the memory the
   pass changes and restores) at the instruction of the pass that the
   skipped time has reached. Then it notices a timer flag, a command or an
   interrupt at the same point of its loop as without skipping; resuming
   where it stopped instead shifted the driver's timer reloads, and so its
   ticks, by up to a pass. */
static s32 idle_debt;   /* cycles the resumed 68K has run ahead of the slice boundary */

/* Skipping leaves the 68K where it was when it started, but had it run, it
   would have gone round the loop: put it (registers, PC, and the memory the
   pass changes and restores) where the skipped time has got it to. Like
   m68k_execute(), which finishes the instruction under way when a slice
   ends, that is the first instruction boundary at or after that time; the
   cycles it is ahead count against the next slice. Then it notices a timer
   flag, a command or an interrupt at the same point as without skipping.
   (Resuming where it stopped shifted the driver's timer reloads by up to a
   pass; resuming at the start of the instruction under way let an interrupt
   in before it: Shining Force III's timer-B handler then cleared the timer-A
   flag before the loop polled it, and its music lost ticks.) */
static void idle_resume(IdleLoop *l)
{
   int s, i, j;
   u32 t;
   for (s = 0; s < l->nsteps && l->soff[s] < l->vphase; s++)
      ;
   t = s < l->nsteps ? l->soff[s] : l->iter_cycles;
   if (s == l->nsteps)
      s = 0;                                /* the end of the pass is the start of the next */
   for (i = 0; i < l->ncounters; i++)
      if (l->coff[i] > l->vphase && l->coff[i] <= t)
         idle_write(l->caddr[i], l->csize[i], idle_read(l->caddr[i], l->csize[i]) + l->cdelta[i]);
   for (i = 0; i < l->nwrites; i++)
   {
      u32 v;
      int earlier = 0;
      for (j = 0; j < i; j++)
         if (l->waddr[j] == l->waddr[i])
            earlier = 1;
      if (earlier)
         continue;                          /* the first write of each address decides */
      v = l->wold[i];
      for (j = i; j < l->nwrites; j++)
         if (l->waddr[j] == l->waddr[i] && l->wstep[j] < s)
            v = l->wval[j];
      idle_write(l->waddr[i], l->wsize[i], v);
   }
   idle_debt -= t - l->vphase;
   memcpy(REG_DA, l->sregs[s], sizeof(l->sregs[s]));
   m68k_set_reg(M68K_REG_SR, l->ssr[s]);
   REG_PC = REG_PPC = l->spc[s];
   l->vvalid = 0;
}

static void idle_resume_all(void)
{
   int i;
   for (i = 0; i < idle_nloops; i++)
      if (idle_loops[i].vvalid)
         idle_resume(&idle_loops[i]);
}

/* before the 68K's state is saved: from where skipped time has got it to */
void M68KMusashiIdleSync(void)
{
   idle_resume_all();
}

/* YAB_68K_IDLE=0/1 overrides the core option; YAB_68K_IDLE_LOG=1 reports
   learned loops and the share of skipped cycles */

static void idle_report(void)
{
   int i;
   fprintf(stderr, "68K idle: %.1f%% of cycles skipped, %d loops:", 
           idle_skipped + idle_run ? 100.0 * idle_skipped / (idle_skipped + idle_run) : 0.0, idle_nloops);
   for (i = 0; i < idle_nloops; i++)
      fprintf(stderr, " [%05x-%05x reads %d counters %d iter %u]", idle_loops[i].lo, idle_loops[i].hi,
              idle_loops[i].nreads, idle_loops[i].ncounters, idle_loops[i].iter_cycles);
   fprintf(stderr, " last reject: %s; probes %u (found %u) steps %u classify %u checks %u\n", probe_reject ? probe_reject : "-",
           stat_probes, stat_found, stat_psteps, stat_classify, stat_checks);
   fprintf(stderr, "  time: probe %.1f ms, run %.1f ms, check %.1f ms\n", stat_tprobe * 1e3, stat_trun * 1e3, stat_tcheck * 1e3);
   fprintf(stderr, "  steps in found probes %u, failed %u; fails: steps %u overflow %u stopped %u cycles %u\n", stat_steps_found, stat_steps_fail,
           stat_fail_reason[0], stat_fail_reason[1], stat_fail_reason[2], stat_fail_reason[3]);
   stat_steps_found = stat_steps_fail = 0; memset(stat_fail_reason, 0, sizeof(stat_fail_reason));
   stat_tprobe = stat_trun = stat_tcheck = 0;
   stat_probes = stat_psteps = stat_classify = stat_checks = stat_found = 0;
   if (idle_nloops)
   {
      fprintf(stderr, "  reads:");
      for (i = 0; i < idle_loops[0].nreads; i++)
         fprintf(stderr, " %x/%d=%x miss %u", idle_loops[0].raddr[i], idle_loops[0].rsize[i], idle_loops[0].rval[i], idle_miss[i]);
      fprintf(stderr, "\n");
   }
   memset(idle_miss, 0, sizeof(idle_miss));
   idle_skipped = idle_run = 0;
}

void M68KMusashiIdleReset(void)
{
   probe_session = 0;
   relearn_interval = IDLE_REPROBE;
   idle_nloops = 0;
   probe_wait = 0;
   probe_interval = 16;
}

static s32 FASTCALL M68KMusashiExec(s32 cycle) {
   s32 done;
   if (UNLIKELY(idle_env == -2))
   {
      idle_env = getenv("YAB_68K_IDLE") ? atoi(getenv("YAB_68K_IDLE")) : -1;
      idle_log = getenv("YAB_68K_IDLE_LOG") != NULL;
   }
   if ((idle_env >= 0 ? idle_env : m68k_idle_skip) && !CPU_STOPPED)
   {
      const u32 pc = REG_PC;
      int i;
      if (probe_session)
      {
         { double t0 = idle_log ? stat_now() : 0; done = idle_probe(cycle, 0xFFFFFFFF); if (idle_log) stat_tprobe += stat_now() - t0; }
         idle_run += done;
         return done;
      }
      if (UNLIKELY(idle_log) && (++idle_clock % 20000) == 0)
         idle_report();
      else if (!idle_log)
         idle_clock++;
      IdleLoop *relearn = NULL;
      stat_checks++;
      for (i = 0; i < idle_nloops; i++)
      {
         const int m = idle_match(&idle_loops[i], pc);
         if (m > 0 && idle_env != 2 && idle_skip(&idle_loops[i], cycle))   /* 2: learn only (tests) */
         {
            idle_skipped += cycle;
            return cycle;
         }
         if (m)
            relearn = &idle_loops[i];
      }
      /* it runs, from where the skipped time has got to */
      idle_resume_all();
      ++probe_wait;
      if (relearn)
      {
         /* relearn once its inputs have stayed the same for a slice: while
            they change, the driver is busy and a probe would not find it
            idle */
         u32 sig = 0x9E3779B9;
         for (i = 0; i < relearn->nreads; i++)
            sig = (sig ^ idle_read(relearn->raddr[i], relearn->rsize[i])) * 0x01000193;
         if (sig != relearn_sig)
         {
            relearn_sig = sig;
            relearn = NULL;
         }
      }
      if (probe_wait >= probe_interval || (relearn && probe_wait >= relearn_interval))
      {
         probe_wait = 0;
         if (cycle + idle_debt > 0)
         {
            { double t0 = idle_log ? stat_now() : 0; done = idle_probe(cycle + idle_debt, relearn ? relearn->spc[0] : 0xFFFFFFFF) - idle_debt; if (idle_log) stat_tprobe += stat_now() - t0; }
            idle_debt = 0;
         }
         else
         {
            idle_debt += cycle;
            done = cycle;
         }
         idle_run += done;
         return done;
      }
   }
   else
   {
      idle_resume_all();
      if (UNLIKELY(idle_log) && (++idle_clock % 20000) == 0)
         idle_report();
   }
   if (UNLIKELY(cycle + idle_debt <= 0))
   {
      idle_debt += cycle;                   /* still ahead after this slice */
      return cycle;
   }
   { double t0 = idle_log ? stat_now() : 0; done = m68k_execute(cycle + idle_debt) - idle_debt; if (idle_log) stat_trun += stat_now() - t0; }
   idle_debt = 0;
   idle_run += done;
   return done;
}

static void M68KMusashiSync(void) {
}

static u32 M68KMusashiGetDReg(u32 num) {
   return m68k_get_reg(NULL, M68K_REG_D0 + num);
}

static u32 M68KMusashiGetAReg(u32 num) {
   return m68k_get_reg(NULL, M68K_REG_A0 + num);
}

static u32 M68KMusashiGetPC(void) {
   return m68k_get_reg(NULL, M68K_REG_PC);
}

static u32 M68KMusashiGetSR(void) {
   return m68k_get_reg(NULL, M68K_REG_SR);
}

static u32 M68KMusashiGetUSP(void) {
   return m68k_get_reg(NULL, M68K_REG_USP);
}

static u32 M68KMusashiGetMSP(void) {
   return m68k_get_reg(NULL, M68K_REG_MSP);
}

static void M68KMusashiSetDReg(u32 num, u32 val) {
   m68k_set_reg(M68K_REG_D0 + num, val);
}

static void M68KMusashiSetAReg(u32 num, u32 val) {
   m68k_set_reg(M68K_REG_A0 + num, val);
}

static void M68KMusashiSetPC(u32 val) {
   m68k_set_reg(M68K_REG_PC, val);
}

static void M68KMusashiSetSR(u32 val) {
   m68k_set_reg(M68K_REG_SR, val);
}

static void M68KMusashiSetUSP(u32 val) {
   m68k_set_reg(M68K_REG_USP, val);
}

static void M68KMusashiSetMSP(u32 val) {
   m68k_set_reg(M68K_REG_MSP, val);
}

static void M68KMusashiSetFetch(u32 low_adr, u32 high_adr, pointer fetch_adr) {
}

/* level 0: no interrupt pending any more (the SCSP drops its lines) */
static void FASTCALL M68KMusashiSetIRQ(s32 level) {
   if (level > 0)
   {
      idle_resume_all();   /* the interrupt is taken now, from where it would be */
      m68k_set_irq(level);
   }
   else
      m68k_set_irq(0);
}

/* the SH2 wrote to sound RAM: forget the loops whose code it changed */
static void FASTCALL M68KMusashiWriteNotify(u32 address, u32 size) {
   int i;
   address &= 0x7FFFF;
   for (i = 0; i < idle_nloops; i++)
      if (address < idle_loops[i].hi && idle_loops[i].lo < address + size)
      {
         idle_loops[i] = idle_loops[--idle_nloops];
         i--;
      }
}

/* Data accesses; with M68K_SEPARATE_READS instruction fetches go through
   m68k_read_immediate_*. During a probe they are logged. */
unsigned int  m68k_read_memory_8(unsigned int address)
{
   u32 v = rw_funcs.r_8(address);
   if (UNLIKELY(probe_active)) probe_log(address, 1, v & 0xFF, 0, 0);
   return v;
}

unsigned int  m68k_read_memory_16(unsigned int address)
{
   u32 v = rw_funcs.r_16(address);
   if (UNLIKELY(probe_active)) probe_log(address, 2, v & 0xFFFF, 0, 0);
   return v;
}

unsigned int  m68k_read_memory_32(unsigned int address)
{
   u16 val1 = rw_funcs.r_16(address);
   u32 v = (val1 << 16 | rw_funcs.r_16(address + 2));
   if (UNLIKELY(probe_active)) probe_log(address, 4, v, 0, 0);
   return v;
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
   if (UNLIKELY(probe_active)) probe_log(address, 1, value & 0xFF, probe_old(address, 1, value & 0xFF), 1);
   rw_funcs.w_8(address, value);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
   if (UNLIKELY(probe_active)) probe_log(address, 2, value & 0xFFFF, probe_old(address, 2, value & 0xFFFF), 1);
   rw_funcs.w_16(address, value);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
   if (UNLIKELY(probe_active)) probe_log(address, 4, value, probe_old(address, 4, value), 1);
   rw_funcs.w_16(address, value >> 16 );
   rw_funcs.w_16(address + 2, value & 0xffff);
}

unsigned int m68k_read_immediate_16(unsigned int address)
{
   return rw_funcs.r_16(address);
}

unsigned int m68k_read_immediate_32(unsigned int address)
{
   return (rw_funcs.r_16(address) << 16) | rw_funcs.r_16(address + 2);
}

unsigned int m68k_read_pcrelative_8(unsigned int address)
{
   return m68k_read_memory_8(address);
}

unsigned int m68k_read_pcrelative_16(unsigned int address)
{
   return m68k_read_memory_16(address);
}

unsigned int m68k_read_pcrelative_32(unsigned int address)
{
   return m68k_read_memory_32(address);
}

static void M68KMusashiSetReadB(M68K_READ *Func) {
   rw_funcs.r_8 = Func;
}

static void M68KMusashiSetReadW(M68K_READ *Func) {
   rw_funcs.r_16 = Func;
}

static void M68KMusashiSetWriteB(M68K_WRITE *Func) {
   rw_funcs.w_8 = Func;
}

static void M68KMusashiSetWriteW(M68K_WRITE *Func) {
   rw_funcs.w_16 = Func;
}

static void M68KMusashiSaveState(void ** stream) {
}

static void M68KMusashiGetExtraState(u32 extra[4]) {
   extra[0] = m68k_get_reg(NULL, M68K_REG_USP);
   extra[1] = m68k_get_reg(NULL, M68K_REG_ISP);
   extra[2] = m68ki_cpu.stopped;
   extra[3] = m68ki_cpu.int_level;
}

/* after SR and the other registers: the stack pointer of the current mode is
   A7, these set the other one */
static void M68KMusashiSetExtraState(const u32 extra[4]) {
   m68k_set_reg(M68K_REG_USP, extra[0]);
   m68k_set_reg(M68K_REG_ISP, extra[1]);
   m68ki_cpu.stopped = extra[2];
   m68ki_cpu.int_level = extra[3];
   M68KMusashiIdleReset();
}

static void M68KMusashiLoadState(const void * stream) {
}

M68K_struct M68KMusashi = {
   3,
   "Musashi Interface",
   M68KMusashiInit,
   M68KMusashiDeInit,
   M68KMusashiReset,
   M68KMusashiExec,
   M68KMusashiSync,
   M68KMusashiGetDReg,
   M68KMusashiGetAReg,
   M68KMusashiGetPC,
   M68KMusashiGetSR,
   M68KMusashiGetUSP,
   M68KMusashiGetMSP,
   M68KMusashiSetDReg,
   M68KMusashiSetAReg,
   M68KMusashiSetPC,
   M68KMusashiSetSR,
   M68KMusashiSetUSP,
   M68KMusashiSetMSP,
   M68KMusashiSetFetch,
   M68KMusashiSetIRQ,
   M68KMusashiWriteNotify,
   M68KMusashiSetReadB,
   M68KMusashiSetReadW,
   M68KMusashiSetWriteB,
   M68KMusashiSetWriteW,
   M68KMusashiSaveState,
   M68KMusashiLoadState,
   M68KMusashiGetExtraState,
   M68KMusashiSetExtraState
};