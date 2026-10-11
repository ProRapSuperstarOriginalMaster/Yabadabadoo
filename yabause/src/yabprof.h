/*  Lightweight per-subsystem profiler (compiled in only with -DYAB_PROFILE).

    Each YAB_PROF_BEGIN/END pair adds the elapsed time to a counter; the
    report printed by YabProfReport() shows milliseconds per emulated frame.
    On 32-bit ARM the timer is the architectural counter (CNTVCT, readable
    from user space, ~40 ns per read on a Cortex-A7), elsewhere
    clock_gettime(). Counts are converted to time using the wall clock
    measured over the same window, so the counter frequency need not be known.
*/

#ifndef YABPROF_H
#define YABPROF_H

#ifdef YAB_PROFILE

#include "core.h"
#include <time.h>

enum {
   PROF_FRAME,        // all of YabauseEmulate()
   PROF_SH2,          // SH2Exec() calls (interpreter only)
   PROF_SCU,
   PROF_M68K,
   PROF_SCSP,
   PROF_SMPC,
   PROF_CS2,
   PROF_HBLANK,       // Vdp2HBlankIN/OUT (per-line register capture)
   PROF_VBLANKOUT,    // Vdp2VBlankOUT(): starts the frame's rendering
   PROF_DRAWSCREENS,  //   of which VIDSoftVdp2DrawScreens()
   PROF_VDP1DRAW,     //   Vdp1Draw() (anywhere)
   PROF_VBLANKIN,     // Vdp2VBlankIN(): finishes the frame's rendering
   PROF_WAIT_LAYERS,  //   of which waiting for layer threads
   PROF_TITAN,        //   of which TitanRender() on the main thread
   PROF_WAIT_VDP1,    // main thread waiting for the VDP1 thread (anywhere)
   PROF_OUTPUT,       // YuiSwapBuffers() -> frontend video callback
   PROF_SNAPSHOT,     // copying VDP2 state/VRAM for the layer threads
   PROF_VDP1_SNAPSHOT, // copying VDP1 RAM for the VDP1 thread
   PROF_VDP1_ERASE,  // VDP1 framebuffer erase at draw start
   PROF_VDP1_SCAN,   // pre-scan of the command list (split decision)
   PROF_SH2_COMPILE,  // dynarec: sh2_recompile_block()
   PROF_SH2_INVAL,    // dynarec: invalidations (count only)
   PROF_SH2_GETADDR,  // dynarec: get_addr() lookups of unlinked targets (count only)
   PROF_T_VDP1,       // worker CPU time: VDP1 thread
   PROF_T_LAYERS,     //                   VDP2 layer threads (sum)
   PROF_T_SPRITE,     //                   sprite layer thread
   PROF_T_PRIO,       //                   Titan compositing threads (sum)
   PROF_L_NBG0,       // CPU time per VDP2 layer, on whichever thread draws it
   PROF_L_NBG1,
   PROF_L_NBG2,
   PROF_L_NBG3,
   PROF_L_RBG0,
   PROF_W_M68K,       // emulation thread CPU: draining the queued sound work
   PROF_T_M68K,       // CPU time of the queued sound work, on whichever thread runs it
   PROF_COUNT
};

extern u64 yab_prof_ticks[PROF_COUNT];
extern u32 yab_prof_calls[PROF_COUNT];

#if defined(__arm__)
static INLINE u64 YabProfNow(void)
{
   u32 lo, hi;
   __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
   return ((u64)hi << 32) | lo;
}
#else
static INLINE u64 YabProfNow(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
#endif

/* Sections entered at most a few times per frame also record the calling
   thread's CPU time, so the report can show time spent off-CPU inside them */
#define PROF_CPU_MASK ((1u << PROF_FRAME) | (1u << PROF_VBLANKOUT) | (1u << PROF_DRAWSCREENS) | \
   (1u << PROF_VDP1DRAW) | (1u << PROF_VBLANKIN) | (1u << PROF_WAIT_LAYERS) | (1u << PROF_TITAN) | \
   (1u << PROF_WAIT_VDP1) | (1u << PROF_OUTPUT))
extern u64 yab_prof_cpu_ns[PROF_COUNT];
static INLINE u64 YabProfThreadNs(void);
/* Timeline trace (YAB_PROF_TRACE=file:first_frame:frames): sections in this
   mask are also logged with start and end time and thread, and written as a
   Chrome/Perfetto trace (open in ui.perfetto.dev) */
#define PROF_TRACE_MASK ((1ull << PROF_FRAME) | (1ull << PROF_VBLANKOUT) | (1ull << PROF_DRAWSCREENS) | \
   (1ull << PROF_VDP1DRAW) | (1ull << PROF_VBLANKIN) | (1ull << PROF_WAIT_LAYERS) | (1ull << PROF_TITAN) | \
   (1ull << PROF_WAIT_VDP1) | (1ull << PROF_OUTPUT) | (1ull << PROF_SNAPSHOT) | (1ull << PROF_VDP1_SNAPSHOT) | \
   (1ull << PROF_VDP1_ERASE) | (1ull << PROF_SH2_COMPILE) | (1ull << PROF_T_VDP1) | (1ull << PROF_T_LAYERS) | \
   (1ull << PROF_T_SPRITE) | (1ull << PROF_T_PRIO) | (1ull << PROF_L_NBG0) | (1ull << PROF_L_NBG1) | \
   (1ull << PROF_L_NBG2) | (1ull << PROF_L_NBG3) | (1ull << PROF_L_RBG0) | (1ull << PROF_W_M68K) | \
   (1ull << PROF_T_M68K))
extern volatile int yab_prof_tracing;
void YabProfTraceEvent(int id, u64 t0, u64 t1);
#define YAB_PROF_TRACE(id, t0, t1) do { \
      if (yab_prof_tracing && ((1ull << (id)) & PROF_TRACE_MASK)) \
         YabProfTraceEvent(id, t0, t1); \
   } while (0)
/* CPU event counters (YAB_PMU=preset or list, Linux): sections in this mask
   also add the calling thread's counter deltas (cycles + up to 4 events, user
   space only) to yab_pmu_acc; the report shows them per call. Low-rate
   sections only: one read() system call at each end. */
#define PROF_PMU_MASK ((1ull << PROF_FRAME) | (1ull << PROF_DRAWSCREENS) | (1ull << PROF_TITAN) | \
   (1ull << PROF_OUTPUT) | (1ull << PROF_T_VDP1) | (1ull << PROF_T_LAYERS) | (1ull << PROF_T_SPRITE) | \
   (1ull << PROF_T_PRIO) | (1ull << PROF_L_NBG0) | (1ull << PROF_L_NBG1) | (1ull << PROF_L_NBG2) | \
   (1ull << PROF_L_NBG3) | (1ull << PROF_L_RBG0))
#define YAB_PMU_MAX 5
extern volatile int yab_pmu_on;
int YabPmuRead(u64 *v);
void YabPmuAdd(int id, const u64 *v0);
#define YAB_PMU_BEGIN(id) u64 yab_prof_pmu_##id[YAB_PMU_MAX]; \
   const int yab_prof_pmuon_##id = yab_pmu_on && ((1ull << (id)) & PROF_PMU_MASK) && YabPmuRead(yab_prof_pmu_##id)
#define YAB_PMU_END(id) do { if (yab_prof_pmuon_##id) YabPmuAdd(id, yab_prof_pmu_##id); } while (0)
#define YAB_PROF_BEGIN(id) u64 yab_prof_start_##id = YabProfNow(); \
   u64 yab_prof_cstart_##id = ((1ull << (id)) & PROF_CPU_MASK) ? YabProfThreadNs() : 0; \
   YAB_PMU_BEGIN(id)
#define YAB_PROF_COUNT(id) (yab_prof_calls[id]++)
#define YAB_PROF_END(id) do { \
      YAB_PMU_END(id); \
      u64 yab_prof_now_ = YabProfNow(); \
      yab_prof_ticks[id] += yab_prof_now_ - yab_prof_start_##id; \
      yab_prof_calls[id]++; \
      YAB_PROF_TRACE(id, yab_prof_start_##id, yab_prof_now_); \
      if ((1ull << (id)) & PROF_CPU_MASK) \
         yab_prof_cpu_ns[id] += YabProfThreadNs() - yab_prof_cstart_##id; \
   } while (0)
/* Worker threads: CPU time of the calling thread (in ns), so time spent
   waiting for a core while other threads run is not counted as busy */
extern u64 yab_prof_thread_ns[PROF_COUNT];
static INLINE u64 YabProfThreadNs(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
   return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
#define YAB_PROF_TBEGIN(id) u64 yab_prof_tstart_##id = YabProfThreadNs(); \
   u64 yab_prof_twall_##id = yab_prof_tracing ? YabProfNow() : 0; \
   YAB_PMU_BEGIN(id)
#define YAB_PROF_TEND(id) do { \
      YAB_PMU_END(id); \
      __atomic_add_fetch(&yab_prof_thread_ns[id], YabProfThreadNs() - yab_prof_tstart_##id, __ATOMIC_RELAXED); \
      __atomic_add_fetch(&yab_prof_calls[id], 1, __ATOMIC_RELAXED); \
      if (yab_prof_twall_##id) \
         YAB_PROF_TRACE(id, yab_prof_twall_##id, YabProfNow()); \
   } while (0)

void YabProfFrame(void);   // call once per emulated frame
void YabProfReport(void);

#else

#define YAB_PROF_BEGIN(id)
#define YAB_PROF_END(id)
#define YAB_PROF_COUNT(id)
#define YAB_PROF_TBEGIN(id)
#define YAB_PROF_TEND(id)
#define YabProfFrame()
#define YabProfReport()

#endif

#endif
