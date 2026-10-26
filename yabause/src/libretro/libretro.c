#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  /* dl_iterate_phdr() for the crash log */
#endif
#ifndef _MSC_VER
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include <sys/stat.h>

#include <libretro.h>

#include <file/file_path.h>
#include <retro_miscellaneous.h>

#include "vdp1.h"
#include "../yabprof.h"
#include "vdp2.h"
#include "peripheral.h"
#include "cdbase.h"
#include "yabause.h"
#include "yui.h"
#include "threads.h"
#include "cheat.h"

#include "cs0.h"
#include "cs2.h"
#include "scu.h"
#include "scsp.h"
#include "smpc.h"

#include "m68kcore.h"
#ifdef HAVE_MUSASHI
#include "m68kmusashi.h"
#endif
#include "vidogl.h"
#include "vidsoft.h"
#include "libretro_core_options.h"
#include "devsession.h"
#ifdef HAVE_PLAY_JIT
#include "sh2_jit.h"
#endif
#ifdef SH2_DYNAREC
#include "sh2_dynarec/sh2_dynarec.h"
#endif

yabauseinit_struct yinit;

static char slash = PATH_DEFAULT_SLASH_C();

static char g_roms_dir[PATH_MAX];
static char g_save_dir[PATH_MAX_LENGTH];
static char g_system_dir[PATH_MAX_LENGTH];
static char bios_path[PATH_MAX_LENGTH];
static char bup_path[PATH_MAX_LENGTH];

static char game_basename[PATH_MAX_LENGTH];

int game_width;
int game_height;

static bool one_frame_rendered = false;
static bool can_dupe = false;   /* RETRO_ENVIRONMENT_GET_CAN_DUPE */
static bool hle_bios_force = false;
/* No BIOS found and one is required: instead of the game, show a screen
 * saying where to put one (see no_bios_run()) */
static bool no_bios_screen = false;
static bool no_bios_msg_sent = false;

/* Frameskip, driven by the frontend's audio buffer status */
enum { FRAMESKIP_NONE, FRAMESKIP_AUTO, FRAMESKIP_AUTO_THRESHOLD, FRAMESKIP_FIXED_INTERVAL };
static unsigned frameskip_type             = FRAMESKIP_NONE;
static unsigned frameskip_threshold        = 33;
static unsigned frameskip_interval         = 3;
/* Skip VDP1 too on skipped frames (only the frame before a rendered one runs
 * it). Faster, but a skipped frame's frame change then shows a sprite buffer
 * that was never drawn: shown frames mix older polygons or FMV images with
 * current VDP2 layers. Off (default): rendered frames are identical to a run
 * without frameskip; VDP1 drawing is deferred and only what is shown is
 * drawn (vidsoft.c, VIDSoftVdp1LazyFrame). */
static bool frameskip_sprites              = false;
static unsigned frameskip_counter          = 0;
static bool frameskip_skip_current         = false;
static bool retro_audio_buff_active        = false;
static unsigned retro_audio_buff_occupancy = 0;
static bool retro_audio_buff_underrun      = false;
static unsigned audio_latency              = 0;
static bool update_audio_latency           = false;
static int addon_cart_type = CART_NONE;
/* BIOS option: 0 = the game's region, 1 = Japan, 2 = USA/Europe */
static int bios_choice = 0;
static int numthreads = 1;
static bool sh2_use_dynarec = false;
static int deciline_mode = 1;
/* Video standard (core option): -1 = the disc's region, else VIDEOFORMATTYPE_* */
static int video_standard = -1;
static int reported_pal = -1;  /* the timing the frontend was last given */

static bool libretro_supports_bitmasks = false;
static int16_t libretro_input_bitmask[12] = {-1,};
static int pad_type[12] = {RETRO_DEVICE_NONE,};
static int multitap[2] = {0,0};
static unsigned players = 7;
static bool all_devices_ready = false;

// disk swapping
#define M3U_MAX_FILE 5
static char disk_paths[M3U_MAX_FILE][PATH_MAX];
static char disk_labels[M3U_MAX_FILE][PATH_MAX];
static struct retro_disk_control_callback retro_disk_control_cb;
static struct retro_disk_control_ext_callback retro_disk_control_ext_cb;
static unsigned disk_initial_index = 0;
static char disk_initial_path[PATH_MAX];
static unsigned disk_index = 0;
static unsigned disk_total = 0;
static bool disk_tray_open = false;

static bool libretro_supports_option_categories = false;

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;

#define RETRO_DEVICE_3DPAD RETRO_DEVICE_ANALOG
#define RETRO_DEVICE_WHEEL RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0)

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   libretro_set_core_options(environ_cb,
         &libretro_supports_option_categories);

   static const struct retro_controller_description peripherals[] = {
       { "Saturn Pad", RETRO_DEVICE_JOYPAD },
       { "Saturn 3D Pad", RETRO_DEVICE_3DPAD },
       { "Saturn Wheel", RETRO_DEVICE_WHEEL },
       { "Saturn Mouse", RETRO_DEVICE_MOUSE },
       { "None", RETRO_DEVICE_NONE },
   };

   static const struct retro_controller_info ports[] = {
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { peripherals, 5 },
      { NULL, 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)
{
#ifdef YABADABADOO_DEV
   input_state_cb = devsession_wrap_input(cb);
#else
   input_state_cb = cb;
#endif
}

/* ---- No BIOS: a screen instead of the game -------------------------------
 * The recompiler needs a real BIOS and the interpreter (with the HLE BIOS) is
 * far too slow on the Miyoo, so builds with the recompiler refuse to start a
 * game without one. YAB_REQUIRE_BIOS=1 turns this on in other builds (to test
 * it), YAB_SH2_INTERP=1 turns it off (debugging on the device). */
#include "../font.h"   /* Yabause's OSD font: 9x10 cells, '#' = ink, '.' = outline */

static bool bios_required(void)
{
   const char *e = getenv("YAB_REQUIRE_BIOS");
   if (e)
      return e[0] == '1';
#ifdef SH2_DYNAREC
   e = getenv("YAB_SH2_INTERP");
   return !(e && e[0] == '1');
#else
   return false;
#endif
}

/* Punctuation the OSD font lacks, same 9x10 format */
static const char *no_bios_glyph(char c)
{
   static const char dot[]   = "         " "         " "         " "         " "         "
                               "         " " ...     " " .##.    " " .##.    " "  ..     ";
   static const char dash[]  = "         " "         " "         " "         " " ....... "
                               " .#####. " " ....... " "         " "         " "         ";
   static const char under[] = "         " "         " "         " "         " "         "
                               "         " "         " "........." ".#######." "........."; 
   static const char colon[] = "         " "         " " ...     " " .##.    " " .##.    "
                               "  ..     " " ...     " " .##.    " " .##.    " "  ..     ";
   switch (c)
   {
      case '.': return dot;
      case '-': return dash;
      case '_': return under;
      case ':': return colon;
   }
   return NULL;
}

static void draw_text(u16 *fb, int w, int h, int x, int y, const char *str)
{
#ifdef USE_RGB_565
   const u16 ink = 0xFFFF;
#else
   const u16 ink = 0x7FFF;
#endif
   for (; *str; str++, x += 8)
   {
      const char *g = no_bios_glyph(*str);
      int l, p;
      for (l = 0; l < 10; l++)
         for (p = 0; p < 9; p++)
         {
            char c;
            if (g)
               c = g[l * 9 + p];
            else if (*str > ' ' && (unsigned char)*str < 128)
               c = font[*str * 10 + l][p];
            else
               c = ' ';
            if (x + p < 0 || x + p >= w || y + l < 0 || y + l >= h)
               continue;
            if (c == '#')
               fb[(y + l) * w + x + p] = ink;
            else if (c == '.')
               fb[(y + l) * w + x + p] = 0;
         }
   }
}

static void no_bios_text(u16 *fb, int x, int y, const char *str)
{
   draw_text(fb, 320, 240, x, y, str);
}

/* Called by YabauseInit once the SMPC knows the disc's region: the BIOS of
 * that region (or of the BIOS option), else saturn_bios.bin, else the other
 * region's file. Area codes J (1), T (2) and K (6) use the Japanese BIOS. */
static int does_file_exist(const char *filename);
static char bios_chosen[PATH_MAX_LENGTH];
static const char *bios_for_region(u8 regionid)
{
   static const char *names[2][3] = {
      { "sega_101.bin", "saturn_bios.bin", "mpr-17933.bin" },
      { "mpr-17933.bin", "saturn_bios.bin", "sega_101.bin" },
   };
   bool japan = bios_choice == 1 ||
                (bios_choice == 0 && (regionid == 1 || regionid == 2 || regionid == 6));
   int i;

   for (i = 0; i < 3; i++)
   {
      snprintf(bios_chosen, sizeof(bios_chosen), "%s%c%s", g_system_dir, slash, names[japan ? 0 : 1][i]);
      if (does_file_exist(bios_chosen) == 1)
         return bios_chosen;
   }
   return NULL;
}

static void no_bios_run(void)
{
   static u16 fb[320 * 240];
   static int16_t silence[2 * 735];
   static const char *lines[] = {
      "NO SATURN BIOS FOUND",
      "",
      "A REAL SATURN BIOS IS NEEDED",
      "TO RUN GAMES AT FULL SPEED.",
      "",
      "PUT ONE OF THESE FILES IN",
      "THE BIOS FOLDER:",
      "",
      "SEGA_101.BIN    JAPAN     ",
      "MPR-17933.BIN   USA/EUROPE",
      "SATURN_BIOS.BIN ANY REGION",
      "",
      "THEN START THE GAME AGAIN.",
   };
   int i, n = sizeof(lines) / sizeof(*lines);

   if (input_poll_cb)
      input_poll_cb();
   if (!no_bios_msg_sent)
   {
      /* The frontend's own message, with the real (lowercase) file names */
      struct retro_message msg = {
         "No Saturn BIOS found. Put sega_101.bin (Japan) or mpr-17933.bin (USA/Europe) in the BIOS folder.",
         60 * 30 };
      environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
      no_bios_msg_sent = true;
   }
   for (i = 0; i < 320 * 240; i++)
      fb[i] = 0x000C; /* dark blue in both 16-bit formats */
   for (i = 0; i < n; i++)
      no_bios_text(fb, (320 - (int)strlen(lines[i]) * 8) / 2, (240 - n * 13) / 2 + i * 13, lines[i]);
   video_cb(fb, 320, 240, 320 * 2);
   audio_batch_cb(silence, 735);  /* keeps audio-synced frontends pacing at 60 fps */
}

// PERLIBRETRO
#define PERCORE_LIBRETRO 2

static PerMouse_struct* mousebits = NULL;

int PERLIBRETROInit(void)
{
   void *controller;

   uint32_t i, j;
   PortData_struct* portdata = NULL;

   //1 multitap + 1 peripherial
   if(multitap[0] == 0 && multitap[1] == 0)
      players = 2;
   else if(multitap[0] == 1 && multitap[1] == 1)
      players = 12;

   PerPortReset();

   for(i = 0; i < players; i++)
   {
      //Ports can handle 6 peripherals, fill port 1 first.
      portdata = ((players > 2 && i < 6) || i == 0 ? &PORTDATA1 : &PORTDATA2);

      switch(pad_type[i])
      {
         case RETRO_DEVICE_NONE:
            controller = NULL;
            break;
         case RETRO_DEVICE_3DPAD:
            controller = (void*)Per3DPadAdd(portdata);
            for(j = PERPAD_UP; j <= PERPAD_Z; j++)
               PerSetKey((i << 8) + j, j, controller);
            for(j = PERANALOG_AXIS1; j <= PERANALOG_AXIS7; j++)
               PerSetKey((i << 8) + j, j, controller);
            break;
         case RETRO_DEVICE_WHEEL:
            controller = (void*)PerWheelAdd(portdata);
            for(j = PERPAD_UP; j <= PERPAD_Z; j++)
               PerSetKey((i << 8) + j, j, controller);
            PerSetKey((i << 8) + PERANALOG_AXIS1, PERANALOG_AXIS1, controller);
            break;
         case RETRO_DEVICE_MOUSE:
            mousebits = PerMouseAdd(portdata);
            for(j = PERMOUSE_LEFT; j <= PERMOUSE_START; j++)
               PerSetKey((i << 8) + j, j, mousebits);
            break;
         case RETRO_DEVICE_JOYPAD:
         default:
            controller = (void*)PerPadAdd(portdata);
            for(j = PERPAD_UP; j <= PERPAD_Z; j++)
               PerSetKey((i << 8) + j, j, controller);
            break;
      }
   }

   return 0;
}

static int input_state_cb_wrapper(unsigned port, unsigned device, unsigned index, unsigned id)
{
   if (libretro_supports_bitmasks && device == RETRO_DEVICE_JOYPAD)
   {
      if (libretro_input_bitmask[port] == -1)
         libretro_input_bitmask[port] = input_state_cb(port, RETRO_DEVICE_JOYPAD, index, RETRO_DEVICE_ID_JOYPAD_MASK);
      return (libretro_input_bitmask[port] & (1 << id));
   }
   else
      return input_state_cb(port, device, index, id);
}

static int PERLIBRETROHandleEvents(void)
{
   unsigned i = 0;

   input_poll_cb();

   for(i = 0; i < players; i++)
   {
         int analog_left_x = 0;
         int analog_left_y = 0;
         int analog_right_x = 0;
         int analog_right_y = 0;
         uint16_t l_trigger, r_trigger;
         libretro_input_bitmask[i] = -1;

         switch(pad_type[i])
         {
            case RETRO_DEVICE_3DPAD:

               analog_left_y = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

               PerAxisValue((i << 8) + PERANALOG_AXIS2, (u8)((analog_left_y + 0x8000) >> 8));

               // analog triggers
               l_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2 );
               r_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2 );

               // if no analog trigger support, use digital
               if (l_trigger == 0)
                  l_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2 ) ? 0x7FFF : 0;
               if (r_trigger == 0)
                  r_trigger = input_state_cb_wrapper( i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2 ) ? 0x7FFF : 0;

               PerAxisValue((i << 8) + PERANALOG_AXIS3, (u8)((r_trigger > 0 ? r_trigger + 0x8000 : 0) >> 8));
               PerAxisValue((i << 8) + PERANALOG_AXIS4, (u8)((l_trigger > 0 ? l_trigger + 0x8000 : 0) >> 8));

            case RETRO_DEVICE_WHEEL:

               analog_left_x = input_state_cb_wrapper(i, RETRO_DEVICE_ANALOG,
                     RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);

               PerAxisValue((i << 8) + PERANALOG_AXIS1, (u8)((analog_left_x + 0x8000) >> 8));

            case RETRO_DEVICE_JOYPAD:

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
                  PerKeyDown((i << 8) + PERPAD_UP);
               else
                  PerKeyUp((i << 8) + PERPAD_UP);
               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
                  PerKeyDown((i << 8) + PERPAD_DOWN);
               else
                  PerKeyUp((i << 8) + PERPAD_DOWN);
               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
                  PerKeyDown((i << 8) + PERPAD_LEFT);
               else
                  PerKeyUp((i << 8) + PERPAD_LEFT);
               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
                  PerKeyDown((i << 8) + PERPAD_RIGHT);
               else
                  PerKeyUp((i << 8) + PERPAD_RIGHT);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
                  PerKeyDown((i << 8) + PERPAD_X);
               else
                  PerKeyUp((i << 8) + PERPAD_X);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
                  PerKeyDown((i << 8) + PERPAD_A);
               else
                  PerKeyUp((i << 8) + PERPAD_A);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
                  PerKeyDown((i << 8) + PERPAD_B);
               else
                  PerKeyUp((i << 8) + PERPAD_B);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))
                  PerKeyDown((i << 8) + PERPAD_Y);
               else
                  PerKeyUp((i << 8) + PERPAD_Y);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
                  PerKeyDown((i << 8) + PERPAD_C);
               else
                  PerKeyUp((i << 8) + PERPAD_C);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
                  PerKeyDown((i << 8) + PERPAD_Z);
               else
                  PerKeyUp((i << 8) + PERPAD_Z);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
                  PerKeyDown((i << 8) + PERPAD_START);
               else
                  PerKeyUp((i << 8) + PERPAD_START);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))
                  PerKeyDown((i << 8) + PERPAD_LEFT_TRIGGER);
               else
                  PerKeyUp((i << 8) + PERPAD_LEFT_TRIGGER);

               if (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))
                  PerKeyDown((i << 8) + PERPAD_RIGHT_TRIGGER);
               else
                  PerKeyUp((i << 8) + PERPAD_RIGHT_TRIGGER);
               break;

            case RETRO_DEVICE_MOUSE:

               (input_state_cb_wrapper(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) ? PerKeyDown((i << 8) + PERMOUSE_LEFT) : PerKeyUp((i << 8) + PERMOUSE_LEFT));
               (input_state_cb_wrapper(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) ? PerKeyDown((i << 8) + PERMOUSE_MIDDLE) : PerKeyUp((i << 8) + PERMOUSE_MIDDLE));
               (input_state_cb_wrapper(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) ? PerKeyDown((i << 8) + PERMOUSE_RIGHT) : PerKeyUp((i << 8) + PERMOUSE_RIGHT));
               // there are some issues with libretro's mouse button 4 & 5 ?
               // let's also use joypad's start for safety
               (input_state_cb_wrapper(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) || input_state_cb_wrapper(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_4) || input_state_cb_wrapper(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_5) ? PerKeyDown((i << 8) + PERMOUSE_START) : PerKeyUp((i << 8) + PERMOUSE_START));

               // is saturn supposed to be able to use several mouse ?
               // because i don't think this code is right in that case
               s32 dispx = input_state_cb_wrapper(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
               s32 dispy = input_state_cb_wrapper(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
               PerMouseMove(mousebits, dispx, -dispy);
               break;

            default:
               break;
         }
   }

   if ( YabauseExec() != 0 )
      return -1;
   return 0;
}

void PERLIBRETRODeInit(void) {}

void PERLIBRETRONothing(void) {}

u32 PERLIBRETROScan(u32 flags) { return 0;}

void PERLIBRETROKeyName(u32 key, char *name, int size) {}

PerInterface_struct PERLIBRETROJoy = {
    PERCORE_LIBRETRO,
    "Libretro Input Interface",
    PERLIBRETROInit,
    PERLIBRETRODeInit,
    PERLIBRETROHandleEvents,
    PERLIBRETROScan,
    0,
    PERLIBRETRONothing,
    PERLIBRETROKeyName
};

// SNDLIBRETRO
#define SNDCORE_LIBRETRO   11
#define SAMPLERATE         44100

static u32 audio_size;
static u32 soundlen;
static u32 soundbufsize;
static s16 *sound_buf;

static int SNDLIBRETROInit(void) {
    int vertfreq = (yabsys.IsPal == 1 ? 50 : 60);
    soundlen = (SAMPLERATE * 100 + (vertfreq >> 1)) / vertfreq;
    soundbufsize = (soundlen<<2 * sizeof(s16));
    if ((sound_buf = (s16 *)malloc(soundbufsize)) == NULL)
        return -1;
    memset(sound_buf, 0, soundbufsize);
    return 0;
}

static void SNDLIBRETRODeInit(void) {
   if (sound_buf)
      free(sound_buf);
}

static int SNDLIBRETROReset(void) { return 0; }

static int SNDLIBRETROChangeVideoFormat(int vertfreq)
{
    soundlen = (SAMPLERATE * 100 + (vertfreq >> 1)) / vertfreq;
    soundbufsize = (soundlen<<2 * sizeof(s16));
    if (sound_buf)
        free(sound_buf);
    if ((sound_buf = (s16 *)malloc(soundbufsize)) == NULL)
        return -1;
    memset(sound_buf, 0, soundbufsize);
    return 0;
}

static void sdlConvert32uto16s(int32_t *srcL, int32_t *srcR, int16_t *dst, size_t len)
{
   u32 i;

   for (i = 0; i < len; i++)
   {
      // Left Channel
      if (*srcL > 0x7FFF)
         *dst = 0x7FFF;
      else if (*srcL < -0x8000)
         *dst = -0x8000;
      else
         *dst = *srcL;
      srcL++;
      dst++;

      // Right Channel
      if (*srcR > 0x7FFF)
         *dst = 0x7FFF;
      else if (*srcR < -0x8000)
         *dst = -0x8000;
      else
         *dst = *srcR;
      srcR++;
      dst++;
   }
}

static void SNDLIBRETROUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   sdlConvert32uto16s((int32_t*)leftchanbuffer, (int32_t*)rightchanbuffer, sound_buf, num_samples);
   audio_batch_cb(sound_buf, num_samples);

   audio_size -= num_samples;
}

static u32 SNDLIBRETROGetAudioSpace(void) { return audio_size; }

void SNDLIBRETROMuteAudio(void) {}

void SNDLIBRETROUnMuteAudio(void) {}

void SNDLIBRETROSetVolume(int volume) {}

SoundInterface_struct SNDLIBRETRO = {
    SNDCORE_LIBRETRO,
    "Libretro Sound Interface",
    SNDLIBRETROInit,
    SNDLIBRETRODeInit,
    SNDLIBRETROReset,
    SNDLIBRETROChangeVideoFormat,
    SNDLIBRETROUpdateAudio,
    SNDLIBRETROGetAudioSpace,
    SNDLIBRETROMuteAudio,
    SNDLIBRETROUnMuteAudio,
    SNDLIBRETROSetVolume
};

M68K_struct *M68KCoreList[] = {
    &M68KDummy,
#ifdef HAVE_MUSASHI
    &M68KMusashi,
#else
    &M68KC68K,
#endif
    NULL
};

SH2Interface_struct *SH2CoreList[] = {
    &SH2Interpreter,
    &SH2DebugInterpreter,
#ifdef HAVE_PLAY_JIT
    &SH2Jit,
#endif
#ifdef SH2_DYNAREC
    &SH2Dynarec,
#endif
    NULL
};

PerInterface_struct *PERCoreList[] = {
    &PERDummy,
    &PERLIBRETROJoy,
    NULL
};

CDInterface *CDCoreList[] = {
    &DummyCD,
    &ISOCD,
    NULL
};

SoundInterface_struct *SNDCoreList[] = {
    &SNDDummy,
    &SNDLIBRETRO,
    NULL
};

VideoInterface_struct *VIDCoreList[] = {
    //&VIDDummy,
    &VIDSoft,
    NULL
};

#pragma mark Yabause Callbacks

static void crash_log_note(const char *string);

void YuiErrorMsg(const char *string)
{
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "Yabause: %s\n", string);
   crash_log_note(string);
}

void YuiSetVideoAttribute(int type, int val)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "Yabause called back to YuSetVideoAttribute.\n");
}

int YuiSetVideoMode(int width, int height, int bpp, int fullscreen)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "Yabause called, it wants to set width of %d and height of %d.\n", width, height);
    return 0;
}

#ifdef YAB_PROFILE
void YuiSwapBuffersBody(void);
void YuiSwapBuffers(void) { YAB_PROF_BEGIN(PROF_OUTPUT); YuiSwapBuffersBody(); YAB_PROF_END(PROF_OUTPUT); }
#define YuiSwapBuffers YuiSwapBuffersBody
#endif
void YuiSwapBuffers(void)
#ifdef YAB_PROFILE
#undef YuiSwapBuffers
#endif
{
   int current_width  = 320;
   int current_height = 240;

   /* Test if VIDCore valid AND NOT the
    * Dummy Interface (or at least VIDCore->id != 0).
    * Avoid calling GetGlSize if Dummy/ID = 0 is selected
    */
   if (VIDCore && VIDCore->id)
      VIDCore->GetGlSize(&current_width, &current_height);

   game_width  = current_width;
   game_height = current_height;
   Vdp2ExactSkipVerifyPicture((const u16 *)dispbuffer, current_width, current_height);

#ifdef YABADABADOO_DEV
   {
      /* on-screen statistics (dev option), drawn into the picture: RetroArch's own
         notifications may be switched off */
      const char *l1, *l2;
      devsession_picture((const uint16_t *)dispbuffer, current_width, current_height);
      if (devsession_stats_text(&l1, &l2))
      {
         /* above the bottom line, where RetroArch shows its own FPS counter */
         draw_text((u16 *)dispbuffer, game_width, game_height, 4, game_height - 37, l1);
         draw_text((u16 *)dispbuffer, game_width, game_height, 4, game_height - 26, l2);
      }
   }
#endif
   video_cb(dispbuffer, game_width, game_height, game_width * 2);
   one_frame_rendered = true;
}

/************************************
 * libretro implementation
 ************************************/

static struct retro_system_av_info g_av_info;

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
#ifdef YABADABADOO_DEV
   info->library_name     = "Yabadabadoo Dev";
   info->library_version  = "0.3-dev (Yabause 0.9.15)" GIT_VERSION;
#elif defined(YABADABADOO_BETA)
   info->library_name     = "Yabadabadoo Beta";
   info->library_version  = BETA_VERSION " (Yabause 0.9.15)" GIT_VERSION;
#else
   info->library_name     = "Yabadabadoo";
   info->library_version  = "0.3 (Yabause 0.9.15)" GIT_VERSION;
#endif
   info->need_fullpath    = true;
   info->block_extract    = true;
   info->valid_extensions = "cue|iso|mds|ccd|zip|chd|m3u";
}

static void set_descriptors(void)
{
   struct retro_input_descriptor *input_descriptors = (struct retro_input_descriptor*)calloc(((17*players)+1), sizeof(struct retro_input_descriptor));

   unsigned j = 0;
   for (unsigned i = 0; i < players; i++)
   {
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "C" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Z" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,  "Analog X" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,  "Analog Y" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Analog X (Right)" };
      input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y (Right)" };
   }
   input_descriptors[j].description = NULL;

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors);
   free(input_descriptors);
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));

   info->timing.fps            = yabsys.IsPal ? 50 : 60;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = game_width;
   info->geometry.base_height  = game_height;
   info->geometry.max_width    = 704;
   info->geometry.max_height   = 512;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
#ifdef YABADABADOO_DEV
   devsession_port_device(port, device);
#endif
   if(pad_type[port] != device)
   {
      pad_type[port] = device;
      if(PERCore)
         PERCore->Init();
      // When all devices are set, we can send input descriptors
      if (all_devices_ready)
         set_descriptors();
   }
}

size_t retro_serialize_size(void)
{
   size_t size;

   if (no_bios_screen)
      return 0;

   ScspMuteAudio(SCSP_MUTE_SYSTEM);
   YabSaveStateBuffer(NULL, &size);
   ScspUnMuteAudio(SCSP_MUTE_SYSTEM);

   return size;
}

bool retro_serialize(void *data, size_t size)
{
   void *buffer;
   size_t out_size;

   if (no_bios_screen)
      return false;

   ScspMuteAudio(SCSP_MUTE_SYSTEM);
   int error = YabSaveStateBuffer (&buffer, &out_size);
   ScspUnMuteAudio(SCSP_MUTE_SYSTEM);

   memcpy(data, buffer, size);

   free(buffer);

   return !error;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (no_bios_screen)
      return false;
   ScspMuteAudio(SCSP_MUTE_SYSTEM);
   int error = YabLoadStateBuffer(data, size);
   ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
#ifdef YABADABADOO_DEV
   if (!error)
      devsession_unserialize(data, size);
#endif

   return !error;
}

void retro_cheat_reset(void)
{
   if (no_bios_screen)
      return;
   CheatClearCodes();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;

   if (no_bios_screen)
      return;
   if (CheatAddARCode(code) == 0)
      return;
}

static void retro_audio_buff_status_cb(bool active, unsigned occupancy, bool underrun_likely)
{
   retro_audio_buff_active    = active;
   retro_audio_buff_occupancy = occupancy;
   retro_audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   /* The built-in frameskip measures wall-clock time and busy-waits when it
    * thinks it is ahead; under a libretro frontend, pacing is the frontend's
    * job, so it stays disabled and skipping is decided in retro_run(). */
   DisableAutoFrameSkip();
   frameskip_counter = 0;

   if (frameskip_type != FRAMESKIP_NONE)
   {
      struct retro_audio_buffer_status_callback buf_status_cb;
      buf_status_cb.callback = retro_audio_buff_status_cb;

      if (frameskip_type != FRAMESKIP_FIXED_INTERVAL &&
          !environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, &buf_status_cb))
      {
         /* Without buffer status the auto modes would never skip; render
          * every other frame instead */
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Frontend has no audio buffer status interface, using fixed interval frameskip (1)\n");
         frameskip_type             = FRAMESKIP_FIXED_INTERVAL;
         frameskip_interval         = 1;
         retro_audio_buff_active    = false;
         retro_audio_buff_occupancy = 0;
         retro_audio_buff_underrun  = false;
      }

      /* Skipping frames needs some audio buffer headroom: ask for at least
       * 6 frames' worth, rounded up to a multiple of 32 ms */
      audio_latency = (unsigned)(6.0f * 1000.0f / (yabsys.IsPal ? 50.0f : 60.0f) + 0.5f);
      audio_latency = (audio_latency + 0x1F) & ~0x1F;
   }
   else
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }

   update_audio_latency = true;
   frameskip_skip_current = false;
   Vdp2SetExternalFrameSkip(frameskip_type != FRAMESKIP_NONE ? VDP2_FRAMESKIP_NONE : -1);
}

static bool frameskip_should_skip(void)
{
   bool skip = false;
   unsigned max_skip = frameskip_interval;

   switch (frameskip_type)
   {
      case FRAMESKIP_AUTO:
         skip = retro_audio_buff_active && retro_audio_buff_underrun;
         break;
      case FRAMESKIP_AUTO_THRESHOLD:
         skip = retro_audio_buff_active && (retro_audio_buff_occupancy < frameskip_threshold);
         break;
      case FRAMESKIP_FIXED_INTERVAL:
         skip = true;
         break;
      default:
         break;
   }

   if (!skip || frameskip_counter >= max_skip)
   {
      frameskip_counter = 0;
      return false;
   }

   frameskip_counter++;
   return true;
}

static int wanted_video_format(void)
{
   if (video_standard >= 0)
      return video_standard;
   /* SMPC area codes above 6 (A, E, L) are the PAL areas; the SMPC takes the
    * first one listed on the disc */
   return (SmpcInternalVars && SmpcInternalVars->regionid > 6) ? VIDEOFORMATTYPE_PAL
                                                              : VIDEOFORMATTYPE_NTSC;
}

/* The emulation always ran with NTSC timing (263 lines and 735 samples per
 * frame, 60 frames a second) while the frontend was told 50 fps for PAL
 * discs: they ran 20% fast, and starved the audio when the frontend paced
 * them at 50. Switches the timing (lines per frame, clocks, audio per frame)
 * whenever it differs from the wanted one: at load, after a state saved with
 * the other one, and when the option changes. Called between frames only. */
static void apply_video_format(void)
{
   int pal = wanted_video_format();
   yinit.videoformattype = pal;  /* retro_reset() runs YabauseInit() again */
   if (yabsys.IsPal != pal)
      YabauseSetVideoFormat(pal);
}

#ifdef YABADABADOO_DEV
static int dev_game_loaded;
/* development options: session log and on-screen statistics (devsession.c) */
static void dev_apply_options(int at_boot)
{
   struct retro_variable var = { "yabadabadoo_dev_session", NULL };
   int mode = DS_OFF, stats = 0;
   char extra[512];
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      mode = !strcmp(var.value, "full") ? DS_FULL : !strcmp(var.value, "timing") ? DS_TIMING : DS_OFF;
   var.key = "yabadabadoo_dev_stats";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      stats = !strcmp(var.value, "enabled");
   snprintf(extra, sizeof(extra), "version=%s\nbios=%s\nbitmasks=%d\nport0=%u\nport1=%u\n",
            GIT_VERSION, bios_path, libretro_supports_bitmasks ? 1 : 0, pad_type[0], pad_type[1]);
   devsession_config(mode, stats, game_basename, at_boot, extra);
}
#endif

static void check_variables(void)
{
   struct retro_variable var;
   var.key = "yabadabadoo_frameskip";
   var.value = NULL;

   unsigned prev_frameskip_type = frameskip_type;
   frameskip_type = FRAMESKIP_NONE;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      /* "enabled" is the value used by older versions of this core */
      if (strcmp(var.value, "auto") == 0 || strcmp(var.value, "enabled") == 0)
         frameskip_type = FRAMESKIP_AUTO;
      else if (strcmp(var.value, "auto_threshold") == 0)
         frameskip_type = FRAMESKIP_AUTO_THRESHOLD;
      else if (strcmp(var.value, "fixed_interval") == 0)
         frameskip_type = FRAMESKIP_FIXED_INTERVAL;
   }

   var.key = "yabadabadoo_frameskip_threshold";
   var.value = NULL;
   frameskip_threshold = 33;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   var.key = "yabadabadoo_frameskip_interval";
   var.value = NULL;
   frameskip_interval = 3;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_interval = strtol(var.value, NULL, 10);

   var.key = "yabadabadoo_frameskip_sprites";
   var.value = NULL;
   frameskip_sprites = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
                       !strcmp(var.value, "enabled");

   if (frameskip_type != prev_frameskip_type)
      init_frameskip();

   /* The "Force HLE BIOS" option is gone: a real BIOS is required (see
    * bios_required()), so hle_bios_force stays false */

   var.key = "yabadabadoo_addon_cartridge";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "none") == 0)
         addon_cart_type = CART_NONE;
      else if (strcmp(var.value, "1M_ram") == 0)
         addon_cart_type = CART_DRAM8MBIT;
      else if (strcmp(var.value, "4M_ram") == 0)
         addon_cart_type = CART_DRAM32MBIT;
   }

   var.key = "yabadabadoo_bios";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "auto") == 0)
         bios_choice = 0;
      else if (strcmp(var.value, "japan") == 0)
         bios_choice = 1;
      else if (strcmp(var.value, "usa_europe") == 0)
         bios_choice = 2;
   }

   var.key = "yabadabadoo_multitap_port1";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         multitap[0] = 0;
      else if (strcmp(var.value, "enabled") == 0)
         multitap[0] = 1;
   }

   var.key = "yabadabadoo_multitap_port2";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         multitap[1] = 0;
      else if (strcmp(var.value, "enabled") == 0)
         multitap[1] = 1;
   }

#ifdef SH2_DYNAREC
   /* No "SH2 Core" option any more: always the recompiler. YAB_SH2_INTERP=1
    * (debugging only) selects the interpreter */
   {
      const char *e = getenv("YAB_SH2_INTERP");
      sh2_use_dynarec = !(e && e[0] == '1');
   }
#endif

   var.key = "yabadabadoo_vdp1_draw_timing";
   var.value = NULL;
   Vdp2SetVdp1DrawEveryFrame(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
                             strcmp(var.value, "every_frame") == 0);

#ifdef HAVE_MUSASHI
   var.key = "yabadabadoo_m68k_idle_skip";
   var.value = NULL;
   m68k_idle_skip = !(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
                      strcmp(var.value, "disabled") == 0);
#endif

   var.key = "yabadabadoo_sound_thread";
   var.value = NULL;
   M68KSetThreaded(!(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
                     strcmp(var.value, "disabled") == 0));

   var.key = "yabadabadoo_video_standard";
   var.value = NULL;
   video_standard = -1;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "ntsc") == 0)
         video_standard = VIDEOFORMATTYPE_NTSC;
      else if (strcmp(var.value, "pal") == 0)
         video_standard = VIDEOFORMATTYPE_PAL;
   }

   var.key = "yabadabadoo_rotation_line_color";
   var.value = NULL;
   vidsoft_rotation_dot_linecolor = !(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
                                      strcmp(var.value, "disabled") == 0);

   var.key = "yabadabadoo_sh2_timeslice";
   var.value = NULL;
   deciline_mode = 1;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      deciline_mode = strcmp(var.value, "line") != 0;

   var.key = "yabadabadoo_numthreads";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "1") == 0)
         numthreads = 1;
      else if (strcmp(var.value, "2") == 0)
         numthreads = 2;
      else if (strcmp(var.value, "4") == 0)
         numthreads = 4;
      else if (strcmp(var.value, "8") == 0)
         numthreads = 8;
      else if (strcmp(var.value, "16") == 0)
         numthreads = 16;
      else if (strcmp(var.value, "32") == 0)
         numthreads = 32;
   }

#ifdef HAVE_THREADS
   var.key = "yabadabadoo_worker_priority";
   var.value = NULL;
   YabThreadSetWorkerNice((environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
                           strcmp(var.value, "normal") == 0) ? 0 : 10);
#endif

#ifdef YABADABADOO_DEV
   if (dev_game_loaded)
      dev_apply_options(0);
#endif
}

static int does_file_exist(const char *filename)
{
   struct stat st;
   int result = stat(filename, &st);
   return result == 0;
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   strncpy(buf, path_basename(path), size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, slash);

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

static bool disk_set_eject_state(bool ejected)
{
   if (no_bios_screen)
      return false;
   disk_tray_open = ejected;
   if (ejected)
   {
      Cs2ForceOpenTray();
      return true;
   }
   else
   {
      return (Cs2ForceCloseTray(CDCORE_ISO, disk_paths[disk_index]) == 0);
   }
}

static bool disk_get_eject_state()
{
   return disk_tray_open;
}

static bool disk_set_image_index(unsigned index)
{
   if ( disk_tray_open == true )
   {
      if ( index < disk_total ) {
         disk_index = index;
         return true;
      }
   }

   return false;
}

static unsigned disk_get_image_index()
{
   return disk_index;
}

static unsigned disk_get_num_images(void)
{
   return disk_total;
}

static bool disk_add_image_index(void)
{
   if (disk_total >= M3U_MAX_FILE)
      return false;
   disk_total++;
   disk_paths[disk_total-1][0] = '\0';
   disk_labels[disk_total-1][0] = '\0';
   return true;
}

static bool disk_replace_image_index(unsigned index, const struct retro_game_info *info)
{
   if ((index >= disk_total))
      return false;

   if (!info)
   {
      disk_paths[index][0] = '\0';
      disk_labels[index][0] = '\0';
      disk_total--;

      if ((disk_index >= index) && (disk_index > 0))
         disk_index--;
   }
   else
   {
      snprintf(disk_paths[index], sizeof(disk_paths[index]), "%s", info->path);
      fill_pathname(disk_labels[index], path_basename(disk_paths[index]), "", sizeof(disk_labels[index]));
   }

   return true;
}

static bool disk_set_initial_image(unsigned index, const char *path)
{
   if (!path || (*path == '\0'))
      return false;

   disk_initial_index = index;
   snprintf(disk_initial_path, sizeof(disk_initial_path), "%s", path);

   return true;
}

static bool disk_get_image_path(unsigned index, char *path, size_t len)
{
   if (len < 1)
      return false;

   if (index >= disk_total)
      return false;

   if (disk_paths[index] == NULL || disk_paths[index][0] == '\0')
      return false;

   strncpy(path, disk_paths[index], len - 1);
   path[len - 1] = '\0';

   return true;
}

static bool disk_get_image_label(unsigned index, char *label, size_t len)
{
   if (len < 1)
      return false;

   if (index >= disk_total)
      return false;

   if (disk_labels[index] == NULL || disk_labels[index][0] == '\0')
      return false;

   strncpy(label, disk_labels[index], len - 1);
   label[len - 1] = '\0';

   return true;
}

static void init_disk_control_interface(void)
{
   unsigned dci_version = 0;

   retro_disk_control_cb.set_eject_state     = disk_set_eject_state;
   retro_disk_control_cb.get_eject_state     = disk_get_eject_state;
   retro_disk_control_cb.set_image_index     = disk_set_image_index;
   retro_disk_control_cb.get_image_index     = disk_get_image_index;
   retro_disk_control_cb.get_num_images      = disk_get_num_images;
   retro_disk_control_cb.add_image_index     = disk_add_image_index;
   retro_disk_control_cb.replace_image_index = disk_replace_image_index;

   retro_disk_control_ext_cb.set_eject_state     = disk_set_eject_state;
   retro_disk_control_ext_cb.get_eject_state     = disk_get_eject_state;
   retro_disk_control_ext_cb.set_image_index     = disk_set_image_index;
   retro_disk_control_ext_cb.get_image_index     = disk_get_image_index;
   retro_disk_control_ext_cb.get_num_images      = disk_get_num_images;
   retro_disk_control_ext_cb.add_image_index     = disk_add_image_index;
   retro_disk_control_ext_cb.replace_image_index = disk_replace_image_index;
   retro_disk_control_ext_cb.set_initial_image   = disk_set_initial_image;
   retro_disk_control_ext_cb.get_image_path      = disk_get_image_path;
   retro_disk_control_ext_cb.get_image_label     = disk_get_image_label;

   disk_initial_index = 0;
   disk_initial_path[0] = '\0';
   if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &dci_version) && (dci_version >= 1))
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &retro_disk_control_ext_cb);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &retro_disk_control_cb);
}

static bool read_m3u(const char *file)
{
   char line[PATH_MAX];
   char name[PATH_MAX];
   FILE *f = fopen(file, "r");

   disk_total = 0;

   if (!f)
   {
      log_cb(RETRO_LOG_ERROR, "Could not read file\n");
      return false;
   }

   while (fgets(line, sizeof(line), f) && disk_total <= M3U_MAX_FILE)
   {
      if (line[0] == '#')
         continue;

      char *carriage_return = strchr(line, '\r');
      if (carriage_return)
         *carriage_return = '\0';

      char *newline = strchr(line, '\n');
      if (newline)
         *newline = '\0';

      if (line[0] == '"')
         memmove(line, line + 1, strlen(line));

      if (line[strlen(line) - 1] == '"')
         line[strlen(line) - 1]  = '\0';

      if (line[0] != '\0')
      {
         snprintf(disk_paths[disk_total], sizeof(disk_paths[disk_total]), "%s%c%s", g_roms_dir, slash, line);
         fill_pathname(disk_labels[disk_total], path_basename(disk_paths[disk_total]), "", sizeof(disk_labels[disk_total]));
         disk_total++;
      }
   }

   fclose(f);
   return (disk_total != 0);
}

void retro_init(void)
{
   struct retro_log_callback log;
   const char *dir = NULL;

   log_cb                   = NULL;
   perf_get_cpu_features_cb = NULL;
   game_width               = 320;
   game_height              = 240;
   uint64_t serialization_quirks = RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION;
   /* Performance level for interpreter CPU core is 16 */
   unsigned level           = 16;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe))
      can_dupe = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      perf_get_cpu_features_cb = perf_cb.get_cpu_features;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      strncpy(g_system_dir, dir, sizeof(g_system_dir));
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      strncpy(g_save_dir, dir, sizeof(g_save_dir));
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

   environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &serialization_quirks);

   init_disk_control_interface();
}

#if defined(__linux__) && defined(__arm__)
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <unistd.h>
#include <link.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <pthread.h>

/* Crash log. RetroArch on the Miyoo leaves no trace when a core crashes, so
 * on a fatal signal append the fault, the registers and the stack words that
 * point into the core's code to <save dir>/yabadabadoo_crash.log, as offsets from
 * the core's load address (for objdump), then let the signal take its
 * course. Only async-signal-safe calls in the handler. */
#define CRASH_NSIG 5
static const int crash_signals[CRASH_NSIG] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
static struct sigaction crash_old[CRASH_NSIG];
static int crash_installed = 0;
static char crash_log_path[PATH_MAX_LENGTH];
static char crash_header[PATH_MAX_LENGTH + 64];
static uintptr_t crash_base, crash_text_size;
static volatile unsigned crash_frame;
static volatile int crash_in_run;  /* the frontend is inside retro_run() */
static int crash_probe[2] = { -1, -1 };  /* pipe: tests whether a stack word is readable */
static unsigned crash_test_frame;  /* YAB_CRASH_TEST=N: crash at frame N, to test the log */
static unsigned crash_stall_test_frame;  /* YAB_STALL_TEST=N: stall at frame N */
#ifdef SH2_DYNAREC
extern char sh2_dynarec_target[];
#endif

static void crash_puts(int fd, const char *str)
{
   size_t n = 0;
   while (str[n])
      n++;
   if (write(fd, str, n) < 0) { }
}

static void crash_hex(int fd, const char *label, uintptr_t v)
{
   char buf[16];
   int i;
   crash_puts(fd, label);
   for (i = 7; i >= 0; i--, v >>= 4)
      buf[i] = "0123456789abcdef"[v & 15];
   buf[8] = 0;
   crash_puts(fd, buf);
}

static void crash_dump(int sig, siginfo_t *si, void *ctx)
{
   const ucontext_t *uc = (const ucontext_t *)ctx;
   const struct sigcontext *mc = &uc->uc_mcontext;
   int i, fd = open(crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);

   if (fd >= 0)
   {
      const uintptr_t *sp = (const uintptr_t *)mc->arm_sp;
      const uintptr_t regs[13] = { mc->arm_r0, mc->arm_r1, mc->arm_r2, mc->arm_r3, mc->arm_r4,
         mc->arm_r5, mc->arm_r6, mc->arm_r7, mc->arm_r8, mc->arm_r9, mc->arm_r10, mc->arm_fp, mc->arm_ip };
      crash_puts(fd, crash_header);
      crash_hex(fd, "signal ", sig);
      crash_hex(fd, " frame ", crash_frame);
      crash_hex(fd, " tid ", (uintptr_t)syscall(SYS_gettid));
      crash_hex(fd, " fault_addr ", (uintptr_t)si->si_addr);
      crash_hex(fd, "\ncore_base ", crash_base);
      crash_hex(fd, " pc ", mc->arm_pc);
      crash_hex(fd, " (core+", mc->arm_pc - crash_base);
      crash_hex(fd, ") lr ", mc->arm_lr);
      crash_hex(fd, " (core+", mc->arm_lr - crash_base);
      crash_hex(fd, ") sp ", mc->arm_sp);
#ifdef SH2_DYNAREC
      if (mc->arm_pc - (uintptr_t)sh2_dynarec_target < (8u << 20))
         crash_puts(fd, " [pc in dynarec code]");
#endif
      crash_puts(fd, "\nregs");
      for (i = 0; i < 13; i++)
         crash_hex(fd, " ", regs[i]);
      crash_puts(fd, "\nstack code refs (core+)");
      for (i = 0; i < 512; i++)
      {
         /* stop at the end of the stack: write() of an unmapped address
          * fails with EFAULT instead of faulting (checked once a page) */
         if (i == 0 || !((uintptr_t)&sp[i] & 0xFFF))
         {
            uintptr_t tmp;
            if (crash_probe[1] < 0 || write(crash_probe[1], &sp[i], sizeof(tmp)) != (ssize_t)sizeof(tmp))
               break;
            if (read(crash_probe[0], &tmp, sizeof(tmp)) < 0) { }
         }
         if (sp[i] - crash_base < crash_text_size)
            crash_hex(fd, " ", sp[i] - crash_base);
      }
      crash_puts(fd, "\n\n");
      close(fd);
   }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx)
{
   int i;
   crash_dump(sig, si, ctx);

   /* back to the previous action: a fault re-executes the instruction and
    * gets it, abort() raises again */
   for (i = 0; i < CRASH_NSIG; i++)
      if (crash_signals[i] == sig)
         sigaction(sig, &crash_old[i], NULL);
   if (sig == SIGABRT || sig == SIGFPE)
      raise(sig);
}

/* finds the core among the loaded objects: its load address and the end of
 * its code */
static int crash_find_core(struct dl_phdr_info *info, size_t size, void *data)
{
   const uintptr_t self = (uintptr_t)data;
   int i, found = 0;
   (void)size;
   for (i = 0; i < info->dlpi_phnum; i++)
   {
      const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
      if (ph->p_type == PT_LOAD && self - (info->dlpi_addr + ph->p_vaddr) < ph->p_memsz)
         found = 1;
   }
   if (!found)
      return 0;
   crash_base = info->dlpi_addr;
   for (i = 0; i < info->dlpi_phnum; i++)
   {
      const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
      if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && ph->p_vaddr + ph->p_memsz > crash_text_size)
         crash_text_size = ph->p_vaddr + ph->p_memsz;
   }
   return 1;
}

/* Freeze watchdog. A frozen game leaves no crash: when one retro_run() has
 * taken STALL_SECONDS, log every thread's name and state, then have each
 * thread log its registers and stack (STALL_SIG handler, same format as a
 * crash) and carry on. Once per stall. Time outside retro_run() (the
 * frontend paused or in its menu) doesn't count. */
#define STALL_SECONDS 10
#define STALL_SIG (SIGRTMIN + 6)
static pthread_t stall_thread;
static volatile int stall_quit;
static struct sigaction stall_old;

static void stall_handler(int sig, siginfo_t *si, void *ctx)
{
   crash_dump(sig, si, ctx);
}

static void *stall_watch(void *arg)
{
   unsigned last = crash_frame, idle = 0;
   int dumped = 0;
   (void)arg;
   while (!stall_quit)
   {
      sleep(1);
      if (crash_frame != last || !crash_in_run)
      {
         last = crash_frame;
         idle = 0;
         dumped = 0;
         continue;
      }
      if (++idle < STALL_SECONDS || dumped)
         continue;
      dumped = 1;
      {
         FILE *f = fopen(crash_log_path, "a");
         DIR *d = opendir("/proc/self/task");
         struct dirent *e;
         const pid_t self = (pid_t)syscall(SYS_gettid);
         if (f)
         {
            fprintf(f, "%s=== stall: no frame for %d s at frame %08x; threads:\n", crash_header, STALL_SECONDS, last);
            if (d)
            {
               while ((e = readdir(d)))
               {
                  char path[64], comm[32] = "", stat[256] = "";
                  FILE *t;
                  if (e->d_name[0] == '.')
                     continue;
                  snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
                  if ((t = fopen(path, "r"))) { if (!fgets(comm, sizeof(comm), t)) comm[0] = 0; fclose(t); }
                  snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
                  if ((t = fopen(path, "r"))) { if (!fgets(stat, sizeof(stat), t)) stat[0] = 0; fclose(t); }
                  comm[strcspn(comm, "\n")] = 0;
                  stat[strcspn(stat, "\n")] = 0;
                  fprintf(f, "  tid %08x %s | %s\n", atoi(e->d_name), comm, stat);
               }
               rewinddir(d);
            }
            {
               /* T6: what the emulated hardware was doing (a snapshot) */
               char info[512];
               ScuStallInfo(info, sizeof(info));
               fprintf(f, "  %s\n", info);
               ScspStallInfo(info, sizeof(info));
               fprintf(f, "  %s\n", info);
               Cs2StallInfo(info, sizeof(info));
               fprintf(f, "  %s\n", info);
            }
            fclose(f);
         }
         if (d)
         {
            while ((e = readdir(d)))
            {
               pid_t tid = atoi(e->d_name);
               if (tid <= 0 || tid == self)
                  continue;
               syscall(SYS_tgkill, getpid(), tid, STALL_SIG);
               usleep(100000);  /* one thread's dump at a time */
            }
            closedir(d);
         }
      }
   }
   return NULL;
}

static void crash_log_install(void)
{
   struct sigaction sa;
   int i;

   if (crash_installed)
      return;
   crash_text_size = 0;
   if (!dl_iterate_phdr(crash_find_core, (void *)(uintptr_t)crash_log_install))
      return;
   snprintf(crash_log_path, sizeof(crash_log_path), "%s%cyabadabadoo_crash.log",
            g_save_dir[0] ? g_save_dir : g_system_dir, slash);
   snprintf(crash_header, sizeof(crash_header), "=== %s, core %s\n", game_basename, GIT_VERSION);

   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = crash_handler;
   sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
   sigemptyset(&sa.sa_mask);
   for (i = 0; i < CRASH_NSIG; i++)
      sigaction(crash_signals[i], &sa, &crash_old[i]);
   if (crash_probe[0] < 0 && pipe(crash_probe) == 0)
   {
      fcntl(crash_probe[0], F_SETFL, O_NONBLOCK);
      fcntl(crash_probe[1], F_SETFL, O_NONBLOCK);
   }
   crash_installed = 1;
   crash_test_frame = getenv("YAB_CRASH_TEST") ? atoi(getenv("YAB_CRASH_TEST")) : 0;
   crash_stall_test_frame = getenv("YAB_STALL_TEST") ? atoi(getenv("YAB_STALL_TEST")) : 0;

   sa.sa_sigaction = stall_handler;
   sa.sa_flags = SA_SIGINFO | SA_RESTART;
   sigaction(STALL_SIG, &sa, &stall_old);
   stall_quit = 0;
   if (pthread_create(&stall_thread, NULL, stall_watch, NULL) != 0)
      stall_quit = 1;
}

/* Core errors (e.g. a runaway SCU DMA that was cut, U9) also go to the crash
 * log: testers send that file, not RetroArch's log. Capped per session: a
 * crashed game can raise an error per emulated instruction (invalid opcodes),
 * which must not fill the SD card. */
#define CRASH_LOG_MAX_NOTES 16

static void crash_log_note(const char *string)
{
   static int notes = 0;
   FILE *f;

   if (!crash_installed || notes > CRASH_LOG_MAX_NOTES || !(f = fopen(crash_log_path, "a")))
      return;
   if (notes++ == CRASH_LOG_MAX_NOTES)
      fprintf(f, "%snote at frame %08x: more core errors, no longer logged\n\n", crash_header, crash_frame);
   else
      fprintf(f, "%snote at frame %08x: %s\n\n", crash_header, crash_frame, string);
   fclose(f);
}

static void crash_log_uninstall(void)
{
   int i;
   if (!crash_installed)
      return;
   if (!stall_quit)
   {
      stall_quit = 1;
      pthread_join(stall_thread, NULL);
   }
   sigaction(STALL_SIG, &stall_old, NULL);
   for (i = 0; i < CRASH_NSIG; i++)
      sigaction(crash_signals[i], &crash_old[i], NULL);
   crash_installed = 0;
}
#else
#define crash_log_install()
#define crash_log_uninstall()
static void crash_log_note(const char *string) { (void)string; }
static unsigned crash_frame, crash_test_frame;
#endif

bool retro_load_game(const struct retro_game_info *info)
{
   int ret;

   if (!info)
      return false;

#ifdef USE_RGB_565
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
#else
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_0RGB1555;
#endif
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   check_variables();

   snprintf(bios_path, sizeof(bios_path), "%s%csaturn_bios.bin", g_system_dir, slash);
   if (does_file_exist(bios_path) != 1)
   {
      log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
      snprintf(bios_path, sizeof(bios_path), "%s%csega_101.bin", g_system_dir, slash);
      if (does_file_exist(bios_path) != 1)
      {
         log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
         snprintf(bios_path, sizeof(bios_path), "%s%cmpr-17933.bin", g_system_dir, slash);
         if (does_file_exist(bios_path) != 1)
         {
            log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
         }
      }
   }

   bool have_bios = does_file_exist(bios_path) == 1;
   if (!have_bios && bios_required())
   {
      /* Lazy-proofing: without a BIOS the recompiler can't run and the
       * interpreter is far too slow here, so don't start the game at all */
      log_cb(RETRO_LOG_ERROR, "No Saturn BIOS in %s (saturn_bios.bin, sega_101.bin or mpr-17933.bin): showing the no-BIOS screen\n", g_system_dir);
      no_bios_screen   = true;
      no_bios_msg_sent = false;
      game_width       = 320;
      game_height      = 240;
      return true;
   }
   no_bios_screen = false;

   if (!have_bios || hle_bios_force)
   {
      log_cb(RETRO_LOG_WARN, "HLE bios is enabled, you should use a real bios, expect lots of issues otherwise\n");
   }

   extract_basename(game_basename, info->path, sizeof(game_basename));
   extract_directory(g_roms_dir, info->path, sizeof(g_roms_dir));

   snprintf(bup_path, sizeof(bup_path), "%s%c%s.srm", g_save_dir, slash, game_basename);

   if (strcmp(path_get_extension(info->path), "m3u") == 0)
   {
      if (!read_m3u(info->path))
      {
         log_cb(RETRO_LOG_ERROR, "Aborting, this m3u file is invalid\n");
         return false;
      }
      else if (!have_bios)
      {
         log_cb(RETRO_LOG_ERROR, "Aborting, you don't have a real bios, but it is required for m3u support\n");
         return false;
      }
      else
      {
         disk_index = 0;
         // saturn requires real bios to swap discs
         if (hle_bios_force)
         {
            log_cb(RETRO_LOG_WARN, "Forcing real bios, it is required for m3u support\n");
            hle_bios_force = false;
         }

         if ((disk_total > 1) && (disk_initial_index > 0) && (disk_initial_index < disk_total))
            if (strcmp(disk_paths[disk_initial_index], disk_initial_path) == 0)
               disk_index = disk_initial_index;
      }
   }
   else
   {
      snprintf(disk_paths[disk_total], sizeof(disk_paths[disk_total]), "%s", info->path);
      fill_pathname(disk_labels[disk_total], path_basename(disk_paths[disk_total]), "", sizeof(disk_labels[disk_total]));
      disk_total++;
   }

   yinit.cdcoretype           = CDCORE_ISO;
   yinit.cdpath               = disk_paths[disk_index];
   /* Emulate BIOS */
   yinit.biospath             = (have_bios && !hle_bios_force) ? bios_path : NULL;
   yinit.biospath_for_region  = bios_for_region;
   yinit.percoretype          = PERCORE_LIBRETRO;
#ifdef HAVE_PLAY_JIT
   yinit.sh2coretype          = SH2CORE_JIT;
   yinit.use_scu_dsp_jit      = 1;
   yinit.use_scsp_dsp_dynarec = 1;
#else
   yinit.sh2coretype          = SH2CORE_INTERPRETER;
#endif
#ifdef SH2_DYNAREC
   /* The recompiler's HLE BIOS hooks are unmaintained; only use it with a
    * real BIOS */
   if (sh2_use_dynarec)
   {
      if (yinit.biospath)
         yinit.sh2coretype = SH2CORE_DYNAREC;
      else
         log_cb(RETRO_LOG_WARN, "SH2 dynarec needs a real BIOS, using the interpreter\n");
   }
#endif
   yinit.vidcoretype          = VIDCORE_SOFT;
   yinit.sndcoretype          = SNDCORE_LIBRETRO;
#ifdef HAVE_MUSASHI
   yinit.m68kcoretype         = M68KCORE_MUSASHI;
#else
   yinit.m68kcoretype         = M68KCORE_C68K;
#endif
   yinit.carttype             = addon_cart_type;
   yinit.regionid             = REGION_AUTODETECT;
   yinit.buppath              = bup_path;
   yinit.use_new_scsp         = 0;
   yinit.mpegpath             = NULL;
   yinit.videoformattype      = VIDEOFORMATTYPE_NTSC;
   yinit.frameskip            = 0; /* see init_frameskip() */
   yinit.clocksync            = 0;
   yinit.basetime             = 0;
#ifdef HAVE_THREADS
   yinit.usethreads           = 1;
   yinit.numthreads           = numthreads;
#else
   yinit.usethreads           = 0;
#endif

   ret = YabauseInit(&yinit);
   /* after YabauseInit: it picks the BIOS by region (bios_for_region) */
   log_cb(RETRO_LOG_INFO, "Starting emulation: SH2 %s, BIOS %s\n",
          yinit.sh2coretype == SH2CORE_INTERPRETER ? "interpreter" : "recompiler",
          yinit.biospath ? yinit.biospath : "HLE");
   if (ret == 0)
   {
      apply_video_format();
      crash_log_install();
   }
   reported_pal = yabsys.IsPal;  /* the frontend asks for the AV info after loading */
   init_frameskip();
   OSDChangeCore(OSDCORE_DUMMY);
   YabauseSetDecilineMode(deciline_mode);

   log_cb(RETRO_LOG_DEBUG, "Enabling cheevos\n");
   bool cheevos_supported                  = true;
   struct retro_memory_map mmaps           = {0};
   struct retro_memory_descriptor descs[2] =
   {
      { RETRO_MEMDESC_SYSTEM_RAM, LowWram,  0, 0x0200000, 0, 0, 0x100000, "LowWram"  },
      { RETRO_MEMDESC_SYSTEM_RAM, HighWram, 0, 0x6000000, 0, 0, 0x100000, "HighWram" },
   };

   mmaps.descriptors     = descs;
   mmaps.num_descriptors = sizeof(descs) / sizeof(*descs);

   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmaps);
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &cheevos_supported);

#ifdef YABADABADOO_DEV
   if (ret == 0)
   {
      devsession_init(environ_cb);
      dev_game_loaded = 1;
      dev_apply_options(1);
   }
#endif
   return !ret;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   (void)game_type;
   (void)info;
   (void)num_info;
   return false;
}

void retro_unload_game(void)
{
   if (no_bios_screen)
   {
      no_bios_screen = false;
      return;
   }
#ifdef YABADABADOO_DEV
   devsession_deinit();
   dev_game_loaded = 0;
#endif
	YabauseDeInit();
	crash_log_uninstall();
}

unsigned retro_get_region(void)
{
   /* Not Cs2GetRegionID(): it reads the disc's first sector through the CD
    * block, which must not happen whenever the frontend asks */
   return yabsys.IsPal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void *retro_get_memory_data(unsigned id)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   return 0;
}

void retro_deinit(void)
{
   libretro_supports_bitmasks = false;
}

void retro_reset(void)
{
   if (no_bios_screen)
      return;
#ifdef YABADABADOO_DEV
   devsession_reset();
#endif
   YabauseResetButton();
   YabauseInit(&yinit);
   YabauseSetDecilineMode(deciline_mode);
}

void retro_run(void)
{
   unsigned i;
   bool updated     = false;
#ifdef YABADABADOO_DEV
   u32 exact_dupes_before;
#endif
   if (no_bios_screen)
   {
      no_bios_run();
      return;
   }
   one_frame_rendered = false;
#ifdef YABADABADOO_DEV
   devsession_load_state_once();
   devsession_poll_start(!frameskip_skip_current && frameskip_counter == 0);
   devsession_run_begin();
#endif
#if defined(__linux__) && defined(__arm__)
   crash_in_run = 1;
#endif
   if (++crash_frame == crash_test_frame)
      *(volatile int *)0 = 0;
#ifdef YAB_VDP1LOG
   vdp1log_frame = crash_frame - 1;   /* the harness's frame number */
#endif
#if defined(__linux__) && defined(__arm__)
   if (crash_frame == crash_stall_test_frame)
      sleep(STALL_SECONDS + 3);
#endif
   if (!all_devices_ready)
   {
      // Running first frame, so we can assume all devices id were set
      // Let's send input descriptors
      all_devices_ready = true;
      set_descriptors();
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      int prev_multitap[2] = {multitap[0],multitap[1]};
      check_variables();
      Vdp2ExactSkipInvalidate();
      if(PERCore && (prev_multitap[0] != multitap[0] || prev_multitap[1] != multitap[1]))
         PERCore->Init();
   }

   apply_video_format();
   /* only when the rate changes (the option): RetroArch reinitialises its
    * drivers for this, and a state saved with the other timing is switched
    * back to the same one */
   if (reported_pal != yabsys.IsPal)
   {
      struct retro_system_av_info av_info;
      retro_get_system_av_info(&av_info);
      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
      reported_pal = yabsys.IsPal;
   }

   if (update_audio_latency)
   {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY, &audio_latency);
      update_audio_latency = false;
   }

   /* The mode set here is applied at this frame's VBlank-OUT and covers the
    * rendering shown during the next retro_run(). Skip decisions are made one
    * frame ahead. With "Skip Polygon Drawing" (default) only the skipped
    * frame that precedes a rendered one runs VDP1; games that change frames
    * every 2nd frame or less can then show polygons 1-3 frames older than
    * the VDP2 layers. Without it every skipped frame runs VDP1 (exact). */
   if (frameskip_type != FRAMESKIP_NONE)
   {
      bool skip_next = frameskip_should_skip();
#ifdef YABADABADOO_DEV
      devsession_skip_decision(retro_audio_buff_active, retro_audio_buff_occupancy,
                               retro_audio_buff_underrun, skip_next);
#endif
      Vdp2SetExternalFrameSkipNext(!skip_next, !frameskip_sprites);
      if (!frameskip_skip_current)
         Vdp2SetExternalFrameSkip(VDP2_FRAMESKIP_NONE);
      else
         Vdp2SetExternalFrameSkip(skip_next && frameskip_sprites ? VDP2_FRAMESKIP_ALL : VDP2_FRAMESKIP_VDP2_ONLY);
      frameskip_skip_current = skip_next;
   }

   /* Audio budget is per emulated frame; it used to be refilled only when a
    * frame was rendered, which dropped all audio on skipped frames */
   audio_size = soundlen;

   /* The picture of the next call is decided (drawn or skipped as unchanged)
    * at this call's VBlank-OUT. A session log hashes the first picture drawn
    * at or after every 60th call: that one is drawn, as it would be without
    * the exact skip, so recordings and replays hash the same calls. */
#ifdef YABADABADOO_DEV
   Vdp2SetExactSkipAllowed(!devsession_picture_wanted_next());
   exact_dupes_before = vdp2_exact_dupes;
#endif

   //YabauseExec(); runs from handle events
   if(PERCore)
      PERCore->HandleEvents();

   /* If no frame rendered, dupe. RetroArch keeps the last frame's data
    * pointer but stores the size and pitch passed here, and its screenshots
    * and save-state thumbnails read them: pitch 0 gave row 0 repeated down
    * the image. game_width/height are still those of the last rendered
    * frame, which is what dispbuffer holds. A frontend that can't dupe gets
    * that picture again. */
   if(!one_frame_rendered)
      video_cb(can_dupe ? NULL : dispbuffer, game_width, game_height, game_width * 2);
#ifdef YABADABADOO_DEV
   /* 2: the picture didn't change, it wasn't drawn again (exact skip) */
   devsession_run_end(one_frame_rendered ? 1 : vdp2_exact_dupes != exact_dupes_before ? 2 : 0,
                      (const uint16_t *)dispbuffer, game_width, game_height);
#endif
#if defined(__linux__) && defined(__arm__)
   crash_in_run = 0;
#endif
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (!pwcs)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif
