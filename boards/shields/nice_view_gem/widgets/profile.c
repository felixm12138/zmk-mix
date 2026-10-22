#include <zephyr/kernel.h>
#include "profile.h"

/* 5 BT profile slots, 5x5 px each with a 4 px gap, centred in the canvas */
#define PROFILE_COUNT 5
#define PROFILE_DOT 5
#define PROFILE_GAP 4
#define PROFILE_PITCH (PROFILE_DOT + PROFILE_GAP)
#define PROFILE_ROW_W (PROFILE_COUNT * PROFILE_DOT + (PROFILE_COUNT - 1) * PROFILE_GAP)
#define PROFILE_X0 ((BUFFER_SIZE - PROFILE_ROW_W + 1) / 2)
#define PROFILE_Y0 (129 + BUFFER_OFFSET_BOTTOM)

/* Inactive slot: dotted outline (corners + edge midpoints) */
static void draw_inactive_slot(lv_obj_t *canvas, int x, int y) {
    static const uint8_t pts[][2] = {{0, 0}, {2, 0}, {4, 0}, {0, 2},
                                     {4, 2}, {0, 4}, {2, 4}, {4, 4}};
    for (unsigned i = 0; i < ARRAY_SIZE(pts); i++) {
        lv_canvas_set_px_color(canvas, x + pts[i][0], y + pts[i][1], LVGL_FOREGROUND);
    }
}

/* Draw the profile row; active < 0 draws every slot as inactive */
void draw_profiles_row(lv_obj_t *canvas, int active) {
    lv_draw_rect_dsc_t rect_dsc;
    init_rect_dsc(&rect_dsc, LVGL_FOREGROUND);

    for (int i = 0; i < PROFILE_COUNT; i++) {
        int x = PROFILE_X0 + i * PROFILE_PITCH;
        if (i == active) {
            lv_canvas_draw_rect(canvas, x, PROFILE_Y0, PROFILE_DOT, PROFILE_DOT, &rect_dsc);
        } else {
            draw_inactive_slot(canvas, x, PROFILE_Y0);
        }
    }
}

void draw_profile_status(lv_obj_t *canvas, const struct status_state *state) {
    draw_profiles_row(canvas, state->active_profile_index);
}
