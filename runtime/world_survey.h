#pragma once
#include "runtime.h"
#include <stdatomic.h>

/* Test-profile-only control, executed on the game's frame-end thread after its GPU fence. */
extern _Atomic int runtime_survey_pending;
extern const int32_t runtime_survey_sync[2];
void runtime_survey_init(Module *main_module);
int runtime_survey_command(const char *line);
void runtime_survey_tick(uint64_t frame);
void runtime_survey_cancel_user(void);
/* Link's position (x east, y up, z south) and heading (radians, 0 = facing south/+z, increasing toward east/+x),
   read-only from any thread (the second screen's map; 2026-10-23). 0 when there is no player (loading, menus). */
int runtime_player_pose(float xyz[3], float *yaw);
