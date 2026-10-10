/* Development builds only (DEV=1): session log and on-screen statistics, see devsession.c */
#ifndef DEVSESSION_H
#define DEVSESSION_H
#ifdef YABADABADOO_DEV
#include <time.h>
#include <stdint.h>
#include "libretro.h"

enum { DS_OFF, DS_TIMING, DS_FULL };

void devsession_init(retro_environment_t env);
void devsession_config(int mode, int stats, const char *game, int at_boot, const char *extra);
void devsession_deinit(void);
retro_input_state_t devsession_wrap_input(retro_input_state_t cb);
void devsession_picture(const uint16_t *fb, unsigned w, unsigned h);
int devsession_stats_text(const char **l1, const char **l2);
void devsession_load_state_once(void);
void devsession_poll_start(int frameskip_idle);
void devsession_run_begin(void);
void devsession_skip_decision(int active, unsigned occupancy, int underrun, int skip_next);
void devsession_run_end(int rendered, const uint16_t *fb, unsigned w, unsigned h);
void devsession_unserialize(const void *data, size_t size);
void devsession_reset(void);
void devsession_port_device(unsigned port, unsigned device);
time_t devsession_rtc(time_t v);
#endif
#endif
