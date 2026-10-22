#pragma once

#include <lvgl.h>
#include "util.h"

void draw_profiles_row(lv_obj_t *canvas, int active);
void draw_profile_status(lv_obj_t *canvas, const struct status_state *state);