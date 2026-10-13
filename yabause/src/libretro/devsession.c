/* Development builds only (DEV=1): session log and on-screen statistics.
 *
 * Session log (core option "Session Log", or YAB_SESSION=timing|full; file
 * name YAB_SESSION_FILE, default <save dir>/yabadabadoo-sessions/<game>-<time>.ysl):
 *   timing: per retro_run() call, how long it took, the time since the
 *           previous call (frame pacing), main-thread and process CPU time,
 *           whether a frame was drawn, the frameskip decision and the audio
 *           buffer status the frontend reported.
 *   full:   also everything needed to replay the session exactly in the
 *           harness (-Y): every non-zero input the core read, the clock
 *           values the SMPC read, controller types, option changes, resets,
 *           loaded states, plus a hash of work RAM every 120 calls so a
 *           replay can prove it matches. Started during play, it begins with
 *           a snapshot (save state) of the running game.
 * Records are queued in memory and written by a low-priority thread, so the
 * emulation thread never waits for the SD card.
 *
 * File format: records of a uint32 header (type << 24 | payload length)
 * followed by the payload, host byte order (both ends are little endian):
 *   'H' text "key=value\n" lines: game, mode, start time, version, BIOS, ...
 *   'O' text "key=value\n" lines: every core option (at start, on changes)
 *   'B' (empty) the session starts at power-on (retro_load_game)
 *   'M' the internal backup RAM (64 KB, the game's .srm as loaded) at power-on: a replay
 *       must start with the same save data, or a game that reads its saves goes differently
 *   'S' u32 call; state data: a state loaded before call (or the snapshot)
 * Call numbers count from the session's start (start_call in 'H' = retro_run calls before it).
 *   'R' u32 call: retro_reset before call
 *   'P' u32 call, u32 port, u32 device: controller type set before call
 *   'F' frame record, see struct ds_frame, then n_inputs struct ds_input
 *   'T' u32 call, s64 value: SMPC clock read during call
 *   'C' u32 call, u32 hash: work RAM hash at the end of call
 *   'V' u32 call, u32 hash: hash of a picture (the first drawn at or after every 60th call,
 *       taken before the on-screen statistics are drawn into it)
 *       (sessions started from a snapshot: save states don't restore every timing
 *       detail exactly, so work RAM can drift while the pictures stay the same)
 *   'X' text: the session was stopped (reason)
 */
#ifdef YABADABADOO_DEV
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <rthreads/rthreads.h>
#include "libretro.h"
#include "devsession.h"
#include "../memory.h"

struct ds_frame
{
   uint32_t call;          /* retro_run() call index, from 0 */
   uint32_t run_us;        /* wall time inside retro_run() */
   uint32_t gap_us;        /* wall time since the previous call started */
   uint32_t main_cpu_us;   /* CPU time of the emulation thread in retro_run() */
   uint32_t proc_cpu_us;   /* CPU time of the whole process since the previous call */
   uint8_t flags;          /* DS_* */
   uint8_t occupancy;      /* audio buffer occupancy (%) the frontend reported */
   uint16_t n_inputs;
};
#define DS_RENDERED  1     /* a frame was drawn in this call */
#define DS_SKIPNEXT  2     /* frameskip decided to skip the next frame */
#define DS_AUDIOACT  4     /* audio buffer status active */
#define DS_UNDERRUN  8     /* frontend reported an underrun risk */
#define DS_DECIDED   16    /* the frameskip decision ran in this call */

struct ds_input
{
   uint8_t port, index;
   uint16_t device, id;
   int16_t value;
};

typedef struct ds_chunk
{
   struct ds_chunk *next;
   size_t len, cap;
   uint8_t data[];
} ds_chunk;

static retro_environment_t ds_env;
static retro_input_state_t ds_real_input;
static int ds_mode;                 /* DS_OFF, DS_TIMING, DS_FULL */
static int ds_stats;
static FILE *ds_file;
static char ds_path[1024];
static uint32_t ds_call, ds_start_call;   /* records number calls from the session start */
#define DS_REL (ds_call - ds_start_call)
static uint64_t ds_bytes;                 /* written so far: a session stops at DS_MAX_BYTES */
#define DS_MAX_BYTES (256u << 20)
static int ds_pending;                    /* "Full" switched on during play: starts at the next */
static char ds_pending_game[512], ds_pending_extra[512];   /* frame boundary with no frameskip state */
static int ds_in_run;
static int ds_unserialize_burst, ds_unserialize_call;
static int ds_invalid;

/* current frame */
static struct ds_frame ds_cur;
static struct ds_input ds_inputs[64];
static uint64_t ds_run_start_us, ds_prev_start_us, ds_main_cpu0, ds_proc_cpu_prev;

/* writer */
static ds_chunk *ds_head, *ds_tail, *ds_fill;
static slock_t *ds_lock;
static scond_t *ds_cond;
static sthread_t *ds_thread;
static int ds_quit;

/* statistics overlay */
static uint64_t st_t0, st_main, st_proc;
static unsigned st_runs, st_drawn, st_skips;
static unsigned st_occ_min = 100;
static int st_occ_seen;   /* the frontend reported the audio buffer this second */
static char st_line1[48], st_line2[48];   /* drawn into the picture by libretro.c */
static uint32_t ds_pic_hash; static int ds_pic_due, ds_pic_valid;   /* picture hash, before the statistics text */

static uint64_t now_us(clockid_t id)
{
   struct timespec ts;
   clock_gettime(id, &ts);
   return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

static void ds_writer(void *arg)
{
   (void)arg;
#ifdef __linux__
   if (nice(19) == -1) { /* best effort */ }
#endif
   for (;;)
   {
      ds_chunk *c;
      slock_lock(ds_lock);
      while (!ds_head && !ds_quit)
         scond_wait(ds_cond, ds_lock);
      c = ds_head;
      if (c)
      {
         ds_head = c->next;
         if (!ds_head)
            ds_tail = NULL;
      }
      slock_unlock(ds_lock);
      if (!c)
         break;   /* quit and nothing left */
      fwrite(c->data, 1, c->len, ds_file);
      free(c);
      fflush(ds_file);
   }
}

static void ds_queue(ds_chunk *c)
{
   c->next = NULL;
   slock_lock(ds_lock);
   if (ds_tail)
      ds_tail->next = c;
   else
      ds_head = c;
   ds_tail = c;
   scond_signal(ds_cond);
   slock_unlock(ds_lock);
}

static ds_chunk *ds_new_chunk(size_t cap)
{
   ds_chunk *c = malloc(sizeof(*c) + cap);
   if (c)
   {
      c->len = 0;
      c->cap = cap;
   }
   return c;
}

/* append a record; small ones go into a 64 KB chunk that is queued when
   full (or every second, so a crash loses little), large ones get their own */
static void ds_record(int type, const void *a, size_t alen, const void *b, size_t blen)
{
   uint32_t hdr = ((uint32_t)type << 24) | (uint32_t)(alen + blen);
   size_t need = 4 + alen + blen;
   if (!ds_file)
      return;
   ds_bytes += need;
   if (need > 32768)
   {
      ds_chunk *c = ds_new_chunk(need);
      if (!c)
         return;
      if (ds_fill && ds_fill->len)
      {
         ds_queue(ds_fill);
         ds_fill = NULL;
      }
      memcpy(c->data, &hdr, 4);
      memcpy(c->data + 4, a, alen);
      if (blen)
         memcpy(c->data + 4 + alen, b, blen);
      c->len = need;
      ds_queue(c);
      return;
   }
   if (ds_fill && ds_fill->len + need > ds_fill->cap)
   {
      ds_queue(ds_fill);
      ds_fill = NULL;
   }
   if (!ds_fill && !(ds_fill = ds_new_chunk(65536)))
      return;
   memcpy(ds_fill->data + ds_fill->len, &hdr, 4);
   memcpy(ds_fill->data + ds_fill->len + 4, a, alen);
   if (blen)
      memcpy(ds_fill->data + ds_fill->len + 4 + alen, b, blen);
   ds_fill->len += need;
}

static void ds_text(int type, const char *s)
{
   ds_record(type, s, strlen(s), NULL, 0);
}

static void ds_options(void)
{
   extern struct retro_core_option_v2_definition option_defs_us[];
   char buf[4096];
   size_t n = 0;
   int i;
   for (i = 0; option_defs_us[i].key && n < sizeof(buf) - 256; i++)
   {
      struct retro_variable var = { option_defs_us[i].key, NULL };
      if (ds_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
         n += snprintf(buf + n, sizeof(buf) - n, "%s=%s\n", var.key, var.value);
   }
   ds_record('O', buf, n, NULL, 0);
}

static uint32_t ds_hash(const uint8_t *p, size_t len, uint32_t h)
{
   const uint32_t *w = (const uint32_t *)p;
   size_t i;
   for (i = 0; i < len / 4; i++)
      h = (h ^ w[i]) * 0x9E3779B1u;
   return h;
}

static void ds_stop(const char *why)
{
   if (!ds_file)
      return;
   ds_text('X', why);
   if (ds_fill && ds_fill->len)
      ds_queue(ds_fill);
   ds_fill = NULL;
   slock_lock(ds_lock);
   ds_quit = 1;
   scond_signal(ds_cond);
   slock_unlock(ds_lock);
   sthread_join(ds_thread);
   fclose(ds_file);
   ds_file = NULL;
   slock_free(ds_lock);
   scond_free(ds_cond);
   ds_lock = NULL; ds_cond = NULL; ds_thread = NULL;
   ds_quit = 0;
   ds_mode = 0;
}

static void ds_start(int mode, const char *game, int at_boot, const char *extra)
{
   char hdr[1536];
   const char *dir = NULL, *env = getenv("YAB_SESSION_FILE");
   time_t t = time(NULL);
   struct tm tm;
   if (ds_file)
      return;
   if (env && *env)
      snprintf(ds_path, sizeof(ds_path), "%s", env);
   else
   {
      char name[128], stamp[32];
      const char *base = strrchr(game, '/');
      size_t i;
      base = base ? base + 1 : game;
      snprintf(name, sizeof(name), "%s", base);
      for (i = 0; name[i]; i++)
         if (name[i] == '.' || name[i] == ' ' || name[i] == '(' || name[i] == ')')
            name[i] = '_';
      localtime_r(&t, &tm);
      strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
      if (!ds_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || !dir)
         dir = ".";
      snprintf(ds_path, sizeof(ds_path), "%s/yabadabadoo-sessions", dir);
      mkdir(ds_path, 0755);
      snprintf(ds_path, sizeof(ds_path), "%s/yabadabadoo-sessions/%s-%s.ysl", dir, name, stamp);
   }
   if (!(ds_file = fopen(ds_path, "wb")))
      return;
   ds_lock = slock_new();
   ds_cond = scond_new();
   ds_thread = sthread_create(ds_writer, NULL);
   ds_mode = mode;
   ds_invalid = 0;
   ds_unserialize_burst = 0;
   ds_start_call = ds_call;
   ds_pic_due = ds_pic_valid = 0;
   ds_cur.call = 0;   /* started inside a call (option change): it is the session's first */
   ds_bytes = 0;
   snprintf(hdr, sizeof(hdr), "game=%s\nmode=%s\nstart_time=%lld\nstart_call=%u\n%s",
            game, mode == DS_FULL ? "full" : "timing", (long long)t, ds_call, extra ? extra : "");
   ds_text('H', hdr);
   ds_options();
   if (mode == DS_FULL)
   {
      if (at_boot)
      {
         ds_record('B', NULL, 0, NULL, 0);
         if (BupRam)
            ds_record('M', BupRam, 0x10000, NULL, 0);
      }
      else
      {
         /* snapshot of the running game: replay starts from it */
         size_t size = retro_serialize_size();
         void *buf = size ? malloc(size) : NULL;
         uint32_t rel = DS_REL;
         if (buf && retro_serialize(buf, size))
            ds_record('S', &rel, 4, buf, size);
         else
            ds_text('X', "snapshot failed");
         free(buf);
      }
   }
}

int16_t devsession_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   int16_t v = ds_real_input(port, device, index, id);
   if (ds_mode == DS_FULL && ds_in_run && v && ds_cur.n_inputs < 64)
   {
      struct ds_input *in = &ds_inputs[ds_cur.n_inputs++];
      in->port = port; in->index = index; in->device = device; in->id = id; in->value = v;
   }
   return v;
}

retro_input_state_t devsession_wrap_input(retro_input_state_t cb)
{
   ds_real_input = cb;
   return devsession_input_state;
}

void devsession_init(retro_environment_t env)
{
   ds_env = env;
}

/* called from check_variables() with the option values (and at load) */
void devsession_config(int mode, int stats, const char *game, int at_boot, const char *extra)
{
   const char *e = getenv("YAB_SESSION");
   if (e)
      mode = !strcmp(e, "full") ? DS_FULL : !strcmp(e, "timing") ? DS_TIMING : DS_OFF;
   if (getenv("YAB_DEV_STATS"))
      stats = atoi(getenv("YAB_DEV_STATS"));
   ds_stats = stats;
   ds_pending = 0;
   if (mode != ds_mode)
   {
      ds_stop("option changed");
      if (mode == DS_FULL && !at_boot)
      {
         /* the snapshot must be taken where a replay can continue from it */
         ds_pending = 1;
         snprintf(ds_pending_game, sizeof(ds_pending_game), "%s", game);
         snprintf(ds_pending_extra, sizeof(ds_pending_extra), "%s", extra ? extra : "");
      }
      else if (mode != DS_OFF)
         ds_start(mode, game, at_boot, extra);
   }
   else if (ds_file)
      ds_options();
}

/* top of retro_run(): a pending "Full" session starts once the frameskip state
   that save states don't keep is at its power-on value (no skip in progress) */
void devsession_poll_start(int frameskip_idle)
{
   if (ds_pending && frameskip_idle)
   {
      ds_pending = 0;
      ds_start(DS_FULL, ds_pending_game, 0, ds_pending_extra);
   }
}

void devsession_deinit(void)
{
   ds_stop("unloaded");
}

/* YAB_LOAD_STATE=file: load a state (the core's own data, as harness -L takes) at the first
   frame, so scripts can start RetroArch at a benchmark scene without its state slots */
void devsession_load_state_once(void)
{
   static int done;
   const char *path = getenv("YAB_LOAD_STATE");
   FILE *f;
   if (done || !path)
      return;
   done = 1;
   if ((f = fopen(path, "rb")))
   {
      long size;
      void *buf;
      fseek(f, 0, SEEK_END);
      size = ftell(f);
      fseek(f, 0, SEEK_SET);
      buf = malloc(size);
      if (buf && fread(buf, 1, size, f) == (size_t)size)
         retro_unserialize(buf, size);
      free(buf);
      fclose(f);
   }
}

void devsession_run_begin(void)
{
   uint64_t t = now_us(CLOCK_MONOTONIC);
   ds_in_run = 1;
   memset(&ds_cur, 0, sizeof(ds_cur));
   ds_cur.call = DS_REL;
   if (ds_mode == DS_FULL && DS_REL % 60 == 59)
      ds_pic_due = 1;
   ds_cur.gap_us = ds_prev_start_us ? (uint32_t)(t - ds_prev_start_us) : 0;
   ds_prev_start_us = ds_run_start_us = t;
   ds_main_cpu0 = now_us(CLOCK_THREAD_CPUTIME_ID);
   {
      uint64_t p = now_us(CLOCK_PROCESS_CPUTIME_ID);
      ds_cur.proc_cpu_us = ds_proc_cpu_prev ? (uint32_t)(p - ds_proc_cpu_prev) : 0;
      ds_proc_cpu_prev = p;
   }
}

/* libretro.c, for every picture it sends, before drawing the statistics into it: every 60
   calls the next drawn picture is hashed (so the hash doesn't depend on the statistics) */
void devsession_picture(const uint16_t *fb, unsigned w, unsigned h)
{
   if (ds_mode == DS_FULL && ds_pic_due && fb && w && h)
   {
      ds_pic_hash = ds_hash((const uint8_t *)fb, (size_t)w * h * 2, 0x811C9DC5u);
      ds_pic_valid = 1;
   }
}

/* the statistics lines (once a second), for libretro.c to draw into the picture */
int devsession_stats_text(const char **l1, const char **l2)
{
   if (!ds_stats || !st_line1[0])
      return 0;
   *l1 = st_line1;
   *l2 = st_line2;
   return 1;
}

void devsession_skip_decision(int active, unsigned occupancy, int underrun, int skip_next)
{
   ds_cur.flags |= DS_DECIDED | (active ? DS_AUDIOACT : 0) | (underrun ? DS_UNDERRUN : 0)
                 | (skip_next ? DS_SKIPNEXT : 0);
   ds_cur.occupancy = occupancy > 255 ? 255 : occupancy;
}

static void ds_show_stats(uint64_t t)
{
   double secs = (t - st_t0) / 1e6;
   if (st_t0 && secs >= 1.0)
   {
      /* REC: session log being written ("REC T" timing only, "REC.." Full waiting to start) */
      const char *rec = ds_file ? (ds_mode == DS_FULL ? "  REC" : "  REC T") : ds_pending ? "  REC.." : "";
      char occ[8];
      snprintf(st_line1, sizeof(st_line1), "DRAWN %2.0f  EMU %2.0f  SKIP %u%s", st_drawn / secs,
               st_runs / secs, st_skips, rec);
      /* CPU: percent of one core (emulation thread; whole process); AUDIO: lowest buffer level,
         percent; "--" when the frontend wasn't asked (frameskip off or fixed interval) */
      if (st_occ_seen)
         snprintf(occ, sizeof(occ), "%u", st_occ_min);
      else
         snprintf(occ, sizeof(occ), "--");
      snprintf(st_line2, sizeof(st_line2), "CPU %3.0f  ALL %3.0f  AUDIO %s", st_main / (secs * 1e4),
               st_proc / (secs * 1e4), occ);
      st_t0 = 0;
   }
   if (!st_t0)
   {
      st_t0 = t;
      st_runs = st_drawn = st_skips = 0;
      st_main = st_proc = 0;
      st_occ_min = 100;
      st_occ_seen = 0;
   }
}

void devsession_run_end(int rendered, const uint16_t *fb, unsigned w, unsigned h)
{
   (void)fb; (void)w; (void)h;   /* pictures are hashed in devsession_picture() */
   uint64_t t = now_us(CLOCK_MONOTONIC);
   ds_in_run = 0;
   ds_cur.run_us = (uint32_t)(t - ds_run_start_us);
   ds_cur.main_cpu_us = (uint32_t)(now_us(CLOCK_THREAD_CPUTIME_ID) - ds_main_cpu0);
   if (rendered)
      ds_cur.flags |= DS_RENDERED;
   if (ds_file)
   {
      ds_record('F', &ds_cur, sizeof(ds_cur), ds_inputs, ds_cur.n_inputs * sizeof(*ds_inputs));
      if (ds_pic_valid && !ds_invalid)
      {
         uint32_t rec[2] = { DS_REL, ds_pic_hash };
         ds_record('V', rec, sizeof(rec), NULL, 0);
         ds_pic_due = ds_pic_valid = 0;
      }
      if (ds_mode == DS_FULL && DS_REL % 120 == 119 && !ds_invalid)
      {
         uint32_t rec[2] = { DS_REL, 0x811C9DC5u };
         rec[1] = ds_hash(HighWram, 0x100000, rec[1]);
         rec[1] = ds_hash(LowWram, 0x100000, rec[1]);
         ds_record('C', rec, sizeof(rec), NULL, 0);
      }
      /* hand the partial chunk to the writer about once a second */
      if (ds_call % 60 == 59 && ds_fill && ds_fill->len)
      {
         ds_queue(ds_fill);
         ds_fill = NULL;
      }
      if (ds_bytes > DS_MAX_BYTES)
         ds_stop("size limit (256 MB) reached");
   }
   if (ds_stats)
   {
      st_runs++;
      st_drawn += rendered != 0;
      st_skips += (ds_cur.flags & DS_SKIPNEXT) != 0;
      st_main += ds_cur.main_cpu_us;
      st_proc += ds_cur.proc_cpu_us;
      if (ds_cur.flags & DS_AUDIOACT)
      {
         st_occ_seen = 1;
         if (ds_cur.occupancy < st_occ_min)
            st_occ_min = ds_cur.occupancy;
      }
      ds_show_stats(t);
   }
   ds_call++;
}

void devsession_unserialize(const void *data, size_t size)
{
   if (ds_mode != DS_FULL || ds_invalid)
      return;
   /* rewind and run-ahead load states every frame: a session can't follow
      them (and shouldn't store megabytes per frame) */
   if (ds_call - ds_unserialize_call < 60)
   {
      if (++ds_unserialize_burst >= 3)
      {
         ds_invalid = 1;
         ds_text('X', "states loaded every frame (rewind or run-ahead?): replay stops here");
         return;
      }
   }
   else
      ds_unserialize_burst = 0;
   ds_unserialize_call = ds_call;
   {
      uint32_t rel = DS_REL;
      ds_record('S', &rel, 4, data, size);
   }
}

void devsession_reset(void)
{
   if (ds_mode == DS_FULL)
   {
      uint32_t rel = DS_REL;
      ds_record('R', &rel, 4, NULL, 0);
   }
}

void devsession_port_device(unsigned port, unsigned device)
{
   uint32_t rec[3] = { DS_REL, port, device };
   if (ds_mode == DS_FULL)
      ds_record('P', rec, sizeof(rec), NULL, 0);
}

time_t devsession_rtc(time_t v)
{
   if (ds_mode == DS_FULL && !ds_invalid)
   {
      struct { uint32_t call, pad; int64_t value; } rec = { DS_REL, 0, (int64_t)v };
      ds_record('T', &rec, sizeof(rec), NULL, 0);
   }
   return v;
}
#endif
