#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#ifndef HAVE_NO_LANGEXTRA
#include "libretro_core_options_intl.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
*/

/* RETRO_LANGUAGE_ENGLISH */

/* Default language:
 * - All other languages must include the same keys and values
 * - Will be used as a fallback in the event that frontend language
 *   is not available
 * - Will be used as a fallback for any missing entries in
 *   frontend language definition */


struct retro_core_option_v2_category option_cats_us[] = {
   { NULL, NULL, NULL },
};

struct retro_core_option_v2_definition option_defs_us[] = {
   {
      "yabadabadoo_frameskip",
      "Frameskip",
      NULL,
      "Skip rendering frames to improve performance. 'Auto' skips when the frontend reports an audio buffer underrun. 'Auto (Threshold)' skips when the audio buffer drops below 'Frameskip Threshold'. 'Fixed Interval' always skips 'Frameskip Interval' frames between rendered frames.",
      NULL,
      NULL,
      {
         { "disabled",       NULL },
         { "auto",           "Auto" },
         { "auto_threshold", "Auto (Threshold)" },
         { "fixed_interval", "Fixed Interval" },
         { NULL, NULL},
      },
      "auto"
   },
   {
      "yabadabadoo_frameskip_threshold",
      "Frameskip Threshold (%)",
      NULL,
      "When 'Frameskip' is 'Auto (Threshold)', the audio buffer occupancy (percent) below which frames are skipped. Higher values reduce crackling at the cost of more skipped frames.",
      NULL,
      NULL,
      {
         { "15", NULL },
         { "18", NULL },
         { "21", NULL },
         { "24", NULL },
         { "27", NULL },
         { "30", NULL },
         { "33", NULL },
         { "36", NULL },
         { "39", NULL },
         { "42", NULL },
         { "45", NULL },
         { "48", NULL },
         { "51", NULL },
         { "54", NULL },
         { "57", NULL },
         { "60", NULL },
         { NULL, NULL },
      },
      "33"
   },
   {
      "yabadabadoo_frameskip_interval",
      "Frameskip Interval",
      NULL,
      "With 'Fixed Interval', the number of frames skipped after each rendered frame (1 = render every 2nd frame). With the 'Auto' modes, the maximum number of consecutive skipped frames.",
      NULL,
      NULL,
      {
         { "1", NULL },
         { "2", NULL },
         { "3", NULL },
         { "4", NULL },
         { "5", NULL },
         { NULL, NULL },
      },
      "2"
   },
   {
      "yabadabadoo_frameskip_sprites",
      "Frameskip: Skip Polygon Drawing",
      NULL,
      "Also skip drawing sprites and polygons (VDP1) on skipped frames. Faster in some games (Shining Force III), but shown frames can mix polygons or FMV images from an older frame with current backgrounds, which looks like stutter, rubber banding or jumpy video (most 3D games, sprite-based FMVs). 'Disabled' shows every rendered frame exactly as without frameskip, drawing only what is shown where it can.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
#if 0 /* removed: a real BIOS and the recompiler are required (Yabadabadoo) */
   {
      "yabadabadoo_force_hle_bios",
      "Force HLE BIOS (Restart Required)",
      NULL,
      "Use High Level Emulation BIOS. Not recommended, as it's causing more issues than it solves.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL},
      },
      "disabled"
   },
#endif
   {
      "yabadabadoo_addon_cartridge",
      "Add-on Cartridge (Restart Required)",
      NULL,
      "Emulate an add-on RAM expansion cartridge. Some games require this to be played, while a few others will have improved performance.",
      NULL,
      NULL,
      {
         { "none", "disabled" },
         { "1M_ram", "1MB RAM" },
         { "4M_ram", "4MB RAM" },
         { NULL, NULL},
      },
      "4M_ram"
   },
   {
      "yabadabadoo_bios",
      "BIOS (Restart Required)",
      NULL,
      "Which Saturn BIOS to start. 'Auto' uses the one for the game's region, like a real console (Japan: sega_101.bin; USA/Europe: mpr-17933.bin). If that file is missing, saturn_bios.bin or the other region's file is used. Versions before this option preferred the Japanese BIOS: if a save state made with one of them misbehaves, choose 'Japan'.",
      NULL,
      NULL,
      {
         { "auto", "Auto (game's region)" },
         { "japan", "Japan" },
         { "usa_europe", "USA/Europe" },
         { NULL, NULL},
      },
      "auto"
   },
   {
      "yabadabadoo_multitap_port1",
      "6Player Adaptor on Port 1",
      NULL,
      "Enable multitap in port 1.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL},
      },
      "disabled"
   },
   {
      "yabadabadoo_multitap_port2",
      "6Player Adaptor on Port 2",
      NULL,
      "Enable multitap in port 2.",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled", NULL },
         { NULL, NULL},
      },
      "disabled"
   },
#ifdef SH2_DYNAREC
#if 0 /* removed: a real BIOS and the recompiler are required (Yabadabadoo) */
   {
      "yabadabadoo_sh2_core",
      "SH2 Core (Restart Required)",
      NULL,
      "CPU emulation for the two SH-2 processors. 'Dynamic Recompiler' is much faster but experimental and less accurate; it requires a real BIOS.",
      NULL,
      NULL,
      {
         { "interpreter", "Interpreter" },
         { "dynarec",     "Dynamic Recompiler" },
         { NULL, NULL },
      },
      "dynarec"
   },
#endif
#endif
   {
      "yabadabadoo_vdp1_draw_timing",
      "VDP1 Draw Timing",
      NULL,
      "When games running at 30 fps or less (NiGHTS, Panzer Dragoon, Shining Force III) draw their 3D. 'After Frame Change' draws once after each frame change, like the hardware: much faster in those games, and the polygons shown are the ones drawn right after the change. 'Every Frame' is the old behaviour: redraws every frame, slower, polygons can be a frame ahead of the backgrounds. Games that change every frame look the same either way.",
      NULL,
      NULL,
      {
         { "frame_change", "After Frame Change" },
         { "every_frame",  "Every Frame" },
         { NULL, NULL },
      },
      "frame_change"
   },
   {
      "yabadabadoo_video_standard",
      "Video Standard",
      NULL,
      "Timing of the emulated console. 'Auto' follows the disc: European (PAL) games run at 50 frames per second, as on a PAL Saturn, and need less speed for full speed. 'NTSC' runs every game at 60 (PAL games then run 20% fast, as with this core before). Takes effect immediately; saves made with the other timing are converted when loaded.",
      NULL,
      NULL,
      {
         { "auto", "Auto" },
         { "ntsc", "NTSC (60 Hz)" },
         { "pal",  "PAL (50 Hz)" },
         { NULL, NULL },
      },
      "auto"
   },
   {
      "yabadabadoo_m68k_idle_skip",
      "Sound CPU Idle Skipping",
      NULL,
      "Skip the sound CPU's time while its driver only polls for work (most of the time in most games), which saves 2-3 ms of each frame. Notes can start a few samples earlier or later than without it. 'Disabled' runs every cycle.",
      NULL,
      NULL,
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "yabadabadoo_sound_thread",
      "Sound CPU on Second Core",
      NULL,
      "Runs the sound CPU and the sound chip's timers and mixing on the second CPU core, a few scanlines behind the main CPUs; they wait for it whenever they touch sound hardware, so games and their audio behave the same. Frees about 2-3 ms of each frame on the main thread. 'Disabled' runs everything on one thread.",
      NULL,
      NULL,
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "yabadabadoo_rotation_line_color",
      "Per-Pixel Rotation Line Colour",
      NULL,
      "Rotating backgrounds whose coefficient table carries line colour data (Panzer Dragoon's sea haze) get it per pixel, as on the hardware. 'Disabled' uses one colour per scanline: slightly faster, but the haze shows a horizontal edge when the camera rolls.",
      NULL,
      NULL,
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "yabadabadoo_sh2_timeslice",
      "SH2 Time Slice (Restart Required)",
      NULL,
      "How often the SH-2 CPUs are synchronized with the rest of the system. 'Scanline' is faster than 'Tenth of a scanline' but less accurate. Only affects the interpreter; the dynamic recompiler always slices per tenth of a scanline.",
      NULL,
      NULL,
      {
         { "deciline", "Tenth of a scanline" },
         { "line",     "Scanline" },
         { NULL, NULL },
      },
      "line"
   },
#ifdef HAVE_THREADS
   {
      "yabadabadoo_numthreads",
      "Number of Threads (Restart Required)",
      NULL,
      "Depending on your CPU, changing this value can improve performance.",
      NULL,
      NULL,
      {
         { "1", NULL },
         { "2", NULL },
         { "4", NULL },
         { "8", NULL },
         { "16", NULL },
         { "32", NULL },
         { NULL, NULL},
      },
      "4"
   },
   {
      "yabadabadoo_worker_priority",
      "Render Thread Priority (Restart Required)",
      NULL,
      "Run the rendering worker threads at a lower priority than the emulation thread (the 3D workers as batch threads that don't interrupt it), so on a dual-core CPU the emulation keeps a core when workers wake up. 'Low' is faster on the Miyoo Mini (Linux only).",
      NULL,
      NULL,
      {
         { "low",    "Low" },
         { "normal", "Normal" },
         { NULL, NULL},
      },
      "low"
   },
#endif
#ifdef YABADABADOO_DEV
   {
      "yabadabadoo_dev_session",
      "Session Log (Dev)",
      NULL,
      "Write a log of this session to <saves>/yabadabadoo-sessions for analysis. 'Timing': how long every frame took. 'Full': also the inputs and everything else needed to replay the session exactly (started during play, it begins with a save state of the running game).",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "timing",   "Timing" },
         { "full",     "Full (Replayable)" },
         { NULL, NULL},
      },
      "disabled"
   },
   {
      "yabadabadoo_dev_stats",
      "On-Screen Statistics (Dev)",
      NULL,
      "Drawn into the picture (bottom left), updated once a second: frames drawn and emulated per second, frames skipped; CPU use of the emulation thread and of the whole process (100 = one core), lowest audio buffer level (percent).",
      NULL,
      NULL,
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL},
      },
      "disabled"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

struct retro_core_options_v2 options_us = {
   option_cats_us,
   option_defs_us
};

/*
 ********************************
 * Language Mapping
 ********************************
*/

#ifndef HAVE_NO_LANGEXTRA
struct retro_core_options_v2 *options_intl[RETRO_LANGUAGE_LAST] = {
   &options_us, /* RETRO_LANGUAGE_ENGLISH */
   &options_ja,      /* RETRO_LANGUAGE_JAPANESE */
   &options_fr,      /* RETRO_LANGUAGE_FRENCH */
   &options_es,      /* RETRO_LANGUAGE_SPANISH */
   &options_de,      /* RETRO_LANGUAGE_GERMAN */
   &options_it,      /* RETRO_LANGUAGE_ITALIAN */
   &options_nl,      /* RETRO_LANGUAGE_DUTCH */
   &options_pt_br,   /* RETRO_LANGUAGE_PORTUGUESE_BRAZIL */
   &options_pt_pt,   /* RETRO_LANGUAGE_PORTUGUESE_PORTUGAL */
   &options_ru,      /* RETRO_LANGUAGE_RUSSIAN */
   &options_ko,      /* RETRO_LANGUAGE_KOREAN */
   &options_cht,     /* RETRO_LANGUAGE_CHINESE_TRADITIONAL */
   &options_chs,     /* RETRO_LANGUAGE_CHINESE_SIMPLIFIED */
   &options_eo,      /* RETRO_LANGUAGE_ESPERANTO */
   &options_pl,      /* RETRO_LANGUAGE_POLISH */
   &options_vn,      /* RETRO_LANGUAGE_VIETNAMESE */
   &options_ar,      /* RETRO_LANGUAGE_ARABIC */
   &options_el,      /* RETRO_LANGUAGE_GREEK */
   &options_tr,      /* RETRO_LANGUAGE_TURKISH */
   &options_sk,      /* RETRO_LANGUAGE_SLOVAK */
   &options_fa,      /* RETRO_LANGUAGE_PERSIAN */
   &options_he,      /* RETRO_LANGUAGE_HEBREW */
   &options_ast,     /* RETRO_LANGUAGE_ASTURIAN */
   &options_fi,      /* RETRO_LANGUAGE_FINNISH */
   &options_id,      /* RETRO_LANGUAGE_INDONESIAN */
   &options_sv,      /* RETRO_LANGUAGE_SWEDISH */
   &options_uk,      /* RETRO_LANGUAGE_UKRAINIAN */
};
#endif

/*
 ********************************
 * Functions
 ********************************
*/

/* Handles configuration/setting of core options.
 * Should only be called inside retro_set_environment().
 * > We place the function body in the header to avoid the
 *   necessity of adding more .c files (i.e. want this to
 *   be as painless as possible for core devs)
 */

static INLINE void libretro_set_core_options(retro_environment_t environ_cb,
      bool *categories_supported)
{
   unsigned version  = 0;
#ifndef HAVE_NO_LANGEXTRA
   unsigned language = 0;
#endif

   if (!environ_cb || !categories_supported)
      return;

   *categories_supported = false;

   if (!environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
#ifndef HAVE_NO_LANGEXTRA
      struct retro_core_options_v2_intl core_options_intl;

      core_options_intl.us    = &options_us;
      core_options_intl.local = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
          (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH))
         core_options_intl.local = options_intl[language];

      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL,
            &core_options_intl);
#else
      *categories_supported = environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
            &options_us);
#endif
   }
   else
   {
      size_t i, j;
      size_t option_index              = 0;
      size_t num_options               = 0;
      struct retro_core_option_definition
            *option_v1_defs_us         = NULL;
#ifndef HAVE_NO_LANGEXTRA
      size_t num_options_intl          = 0;
      struct retro_core_option_v2_definition
            *option_defs_intl          = NULL;
      struct retro_core_option_definition
            *option_v1_defs_intl       = NULL;
      struct retro_core_options_intl
            core_options_v1_intl;
#endif
      struct retro_variable *variables = NULL;
      char **values_buf                = NULL;

      /* Determine total number of options */
      while (true)
      {
         if (option_defs_us[num_options].key)
            num_options++;
         else
            break;
      }

      if (version >= 1)
      {
         /* Allocate US array */
         option_v1_defs_us = (struct retro_core_option_definition *)
               calloc(num_options + 1, sizeof(struct retro_core_option_definition));

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            struct retro_core_option_v2_definition *option_def_us = &option_defs_us[i];
            struct retro_core_option_value *option_values         = option_def_us->values;
            struct retro_core_option_definition *option_v1_def_us = &option_v1_defs_us[i];
            struct retro_core_option_value *option_v1_values      = option_v1_def_us->values;

            option_v1_def_us->key           = option_def_us->key;
            option_v1_def_us->desc          = option_def_us->desc;
            option_v1_def_us->info          = option_def_us->info;
            option_v1_def_us->default_value = option_def_us->default_value;

            /* Values must be copied individually... */
            while (option_values->value)
            {
               option_v1_values->value = option_values->value;
               option_v1_values->label = option_values->label;

               option_values++;
               option_v1_values++;
            }
         }

#ifndef HAVE_NO_LANGEXTRA
         if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
             (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH) &&
             options_intl[language])
            option_defs_intl = options_intl[language]->definitions;

         if (option_defs_intl)
         {
            /* Determine number of intl options */
            while (true)
            {
               if (option_defs_intl[num_options_intl].key)
                  num_options_intl++;
               else
                  break;
            }

            /* Allocate intl array */
            option_v1_defs_intl = (struct retro_core_option_definition *)
                  calloc(num_options_intl + 1, sizeof(struct retro_core_option_definition));

            /* Copy parameters from option_defs_intl array */
            for (i = 0; i < num_options_intl; i++)
            {
               struct retro_core_option_v2_definition *option_def_intl = &option_defs_intl[i];
               struct retro_core_option_value *option_values           = option_def_intl->values;
               struct retro_core_option_definition *option_v1_def_intl = &option_v1_defs_intl[i];
               struct retro_core_option_value *option_v1_values        = option_v1_def_intl->values;

               option_v1_def_intl->key           = option_def_intl->key;
               option_v1_def_intl->desc          = option_def_intl->desc;
               option_v1_def_intl->info          = option_def_intl->info;
               option_v1_def_intl->default_value = option_def_intl->default_value;

               /* Values must be copied individually... */
               while (option_values->value)
               {
                  option_v1_values->value = option_values->value;
                  option_v1_values->label = option_values->label;

                  option_values++;
                  option_v1_values++;
               }
            }
         }

         core_options_v1_intl.us    = option_v1_defs_us;
         core_options_v1_intl.local = option_v1_defs_intl;

         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &core_options_v1_intl);
#else
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, option_v1_defs_us);
#endif
      }
      else
      {
         /* Allocate arrays */
         variables  = (struct retro_variable *)calloc(num_options + 1,
               sizeof(struct retro_variable));
         values_buf = (char **)calloc(num_options, sizeof(char *));

         if (!variables || !values_buf)
            goto error;

         /* Copy parameters from option_defs_us array */
         for (i = 0; i < num_options; i++)
         {
            const char *key                        = option_defs_us[i].key;
            const char *desc                       = option_defs_us[i].desc;
            const char *default_value              = option_defs_us[i].default_value;
            struct retro_core_option_value *values = option_defs_us[i].values;
            size_t buf_len                         = 3;
            size_t default_index                   = 0;

            values_buf[i] = NULL;

            if (desc)
            {
               size_t num_values = 0;

               /* Determine number of values */
               while (true)
               {
                  if (values[num_values].value)
                  {
                     /* Check if this is the default value */
                     if (default_value)
                        if (strcmp(values[num_values].value, default_value) == 0)
                           default_index = num_values;

                     buf_len += strlen(values[num_values].value);
                     num_values++;
                  }
                  else
                     break;
               }

               /* Build values string */
               if (num_values > 0)
               {
                  buf_len += num_values - 1;
                  buf_len += strlen(desc);

                  values_buf[i] = (char *)calloc(buf_len, sizeof(char));
                  if (!values_buf[i])
                     goto error;

                  strcpy(values_buf[i], desc);
                  strcat(values_buf[i], "; ");

                  /* Default value goes first */
                  strcat(values_buf[i], values[default_index].value);

                  /* Add remaining values */
                  for (j = 0; j < num_values; j++)
                  {
                     if (j != default_index)
                     {
                        strcat(values_buf[i], "|");
                        strcat(values_buf[i], values[j].value);
                     }
                  }
               }
            }

            variables[option_index].key   = key;
            variables[option_index].value = values_buf[i];
            option_index++;
         }

         /* Set variables */
         environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
      }

error:
      /* Clean up */

      if (option_v1_defs_us)
      {
         free(option_v1_defs_us);
         option_v1_defs_us = NULL;
      }

#ifndef HAVE_NO_LANGEXTRA
      if (option_v1_defs_intl)
      {
         free(option_v1_defs_intl);
         option_v1_defs_intl = NULL;
      }
#endif

      if (values_buf)
      {
         for (i = 0; i < num_options; i++)
         {
            if (values_buf[i])
            {
               free(values_buf[i]);
               values_buf[i] = NULL;
            }
         }

         free(values_buf);
         values_buf = NULL;
      }

      if (variables)
      {
         free(variables);
         variables = NULL;
      }
   }
}

#ifdef __cplusplus
}
#endif

#endif
