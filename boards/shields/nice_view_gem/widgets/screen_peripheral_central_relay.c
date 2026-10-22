/*
 * Copyright (c) 2026 Artem Yurov
 * SPDX-License-Identifier: MIT
 *
 * Peripheral screen with central relay data.
 * Shows connection status and battery for both halves,
 * BT profile dots and layer name from central via CSR.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/usb.h>

#include <zmk/split/central-states/cache.h>
#include <zmk/split/central-states/event.h>

#include "battery.h"
#include "layer.h"
#include "output.h"
#include "profile.h"
#include "screen_peripheral_central_relay.h"

#include "../assets/custom_fonts.h"

LV_IMG_DECLARE(bt_no_signal);
LV_IMG_DECLARE(bt_unbonded);
LV_IMG_DECLARE(bt);
LV_IMG_DECLARE(usb);
LV_IMG_DECLARE(bolt);
LV_IMG_DECLARE(middle_art_rotated);

#define MIDDLE_ART_DISPLAY_WIDTH 25

/* Caps Lock bit in the HID keyboard LED report (usage 0x02 -> bit 1) */
#define HID_IND_CAPS_LOCK BIT(1)

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/*
 * ============================================================
 * Draw functions — TOP canvas (dual-column status)
 * ============================================================
 *
 * Layout (68px wide canvas, pre-rotation):
 *
 *   LEFT column (x=0..33)       RIGHT column (x=34..67)
 *   peripheral, left-aligned    central relay, right-aligned
 *
 *   y=0:  [BT icon 24x15]              [BT/USB icon 24x15]
 *   y=19: 72% bolt                                    85%
 */

/* --- LEFT column: peripheral connection icon (x=0..23) --- */

static void draw_peripheral_conn_icon(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);

    /* Icon background rect — 24x15, same width as right column */
    lv_draw_rect_dsc_t rect_dsc;
    init_rect_dsc(&rect_dsc, LVGL_FOREGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, 24, 15, &rect_dsc);

    if (state->connected) {
        /* bt icon 12px wide, centered in 24px rect: (24-12)/2 = 6 */
        lv_canvas_draw_img(canvas, 6, 0, &bt, &img_dsc);
    } else {
        lv_canvas_draw_img(canvas, 6, 0, &bt_no_signal, &img_dsc);
    }
}

/* --- LEFT column: peripheral battery (x=0..33) --- */

static void draw_peripheral_battery(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_LEFT);

    char text[10] = {};
    if (state->battery == 0) {
        /* battery == 0 means no real data yet (driver pre-init or unknown) */
        strcpy(text, "--%");
    } else {
        sprintf(text, "%i%%", state->battery);
    }

    /* Show bolt when charging, except when battery is fully charged (100%) */
    if (state->charging && state->battery != 100) {
        lv_canvas_draw_text(canvas, 0, 19, 28, &label_dsc, text);

        lv_draw_img_dsc_t img_dsc;
        lv_draw_img_dsc_init(&img_dsc);
        lv_canvas_draw_img(canvas, 29, 21, &bolt, &img_dsc);
    } else {
        lv_canvas_draw_text(canvas, 0, 19, 34, &label_dsc, text);
    }
}

/* --- RIGHT column: central relay connection icon (x=43..67) --- */

static void draw_central_relay_conn_icon(lv_obj_t *canvas, const struct status_state *state) {
    /* Don't draw icon when relay data is unavailable */
    if (!state->connected || !state->central_relay_received) {
        return;
    }

    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);

    /* Icon background rect — 24x15, same as left column */
    lv_draw_rect_dsc_t rect_dsc;
    init_rect_dsc(&rect_dsc, LVGL_FOREGROUND);
    lv_canvas_draw_rect(canvas, 43, 0, 24, 15, &rect_dsc);

    if (state->central_relay_usb) {
        lv_canvas_draw_img(canvas, 45, 2, &usb, &img_dsc);
    } else if (state->central_relay_bonded) {
        if (state->central_relay_connected) {
            lv_canvas_draw_img(canvas, 49, 0, &bt, &img_dsc);
        } else {
            lv_canvas_draw_img(canvas, 49, 0, &bt_no_signal, &img_dsc);
        }
    } else {
        lv_canvas_draw_img(canvas, 44, 0, &bt_unbonded, &img_dsc);
    }
}

/* --- RIGHT column: central relay battery (x=34..67) --- */

static void draw_central_relay_battery(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_RIGHT);

    /* Don't draw battery when relay data is unavailable */
    if (!state->connected || !state->central_relay_received) {
        return;
    }

    char text[10] = {};
    if (state->central_relay_battery == 0) {
        /* battery == 0 means no real data yet */
        strcpy(text, "--%");
    } else {
        sprintf(text, "%i%%", state->central_relay_battery);
    }

    /* Show bolt when charging, except when battery is fully charged (100%) */
    if (state->central_relay_charging && state->central_relay_battery != 100) {
        lv_canvas_draw_text(canvas, 26, 19, 35, &label_dsc, text);

        lv_draw_img_dsc_t img_dsc;
        lv_draw_img_dsc_init(&img_dsc);
        lv_canvas_draw_img(canvas, 62, 21, &bolt, &img_dsc);
    } else {
        lv_canvas_draw_text(canvas, 26, 19, 42, &label_dsc, text);
    }
}

/* --- BOTTOM: not-connected fallback label --- */

static void draw_not_connected_label(lv_obj_t *canvas) {
    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_CENTER);
    lv_canvas_draw_text(canvas, 0, 146 + BUFFER_OFFSET_BOTTOM, 68, &label_dsc, "NOT CONN");
}

/* --- BOTTOM: profile dots without active selection --- */

static void draw_profiles_no_active(lv_obj_t *canvas) {
    draw_profiles_row(canvas, -1); /* all slots inactive */
}

/* --- MIDDLE: Caps Lock badge (small rounded box below MIX art) --- */

#define CAPS_GLYPH_W 5
#define CAPS_GLYPH_H 7
#define CAPS_PAD_X 3
#define CAPS_PAD_Y 3
#define CAPS_RADIUS 3
#define CAPS_W (4 * CAPS_GLYPH_W + 3 + 2 * CAPS_PAD_X) /* 29 */
#define CAPS_H (CAPS_GLYPH_H + 2 * CAPS_PAD_Y)         /* 13 */
/* Lowest spot in the middle canvas: centred between MIX art and profile dots */
#define CAPS_Y0 (BUFFER_SIZE - 1 - CAPS_H)

static const char *const caps_glyphs[4][CAPS_GLYPH_H] = {
    {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}, /* C */
    {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}, /* A */
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}, /* P */
    {".###.", "#...#", "#....", ".###.", "....#", "#...#", ".###."}, /* S */
};

static void draw_caps_badge(lv_obj_t *canvas) {
    int x0 = (BUFFER_SIZE - CAPS_W) / 2;

    lv_draw_rect_dsc_t rect_dsc;
    init_rect_dsc(&rect_dsc, LVGL_FOREGROUND);
    rect_dsc.radius = CAPS_RADIUS;
    lv_canvas_draw_rect(canvas, x0, CAPS_Y0, CAPS_W, CAPS_H, &rect_dsc);

    for (int g = 0; g < 4; g++) {
        int gx = x0 + CAPS_PAD_X + g * (CAPS_GLYPH_W + 1);
        for (int r = 0; r < CAPS_GLYPH_H; r++) {
            for (int c = 0; c < CAPS_GLYPH_W; c++) {
                if (caps_glyphs[g][r][c] == '#') {
                    lv_canvas_set_px_color(canvas, gx + c, CAPS_Y0 + CAPS_PAD_Y + r,
                                           LVGL_BACKGROUND);
                }
            }
        }
    }
}

/*
 * ============================================================
 * Draw buffers
 * ============================================================
 */

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);
    fill_background(canvas);

    /* Left column: peripheral status */
    draw_peripheral_conn_icon(canvas, state);
    draw_peripheral_battery(canvas, state);

    /* Right column: central relay status */
    draw_central_relay_conn_icon(canvas, state);
    draw_central_relay_battery(canvas, state);

    rotate_canvas(canvas, cbuf);
}

static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);
    fill_background(canvas);

    if (state->connected && state->central_relay_received && state->caps_lock) {
        draw_caps_badge(canvas);
    }

    rotate_canvas(canvas, cbuf);
}

static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);
    fill_background(canvas);

    if (state->connected && state->central_relay_received) {
        /* Full data available — reuse existing draw functions */
        draw_profile_status(canvas, state);
        draw_layer_status(canvas, state);
    } else {
        /* No relay data — show placeholder */
        draw_profiles_no_active(canvas);
        draw_not_connected_label(canvas);
    }

    rotate_canvas(canvas, cbuf);
}

/*
 * ============================================================
 * Battery status (local peripheral battery)
 * ============================================================
 */

static void set_battery_status(struct zmk_widget_screen *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state);

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

/*
 * ============================================================
 * Peripheral connection status (local BLE to central)
 * ============================================================
 */

static struct peripheral_status_state peripheral_get_state(const zmk_event_t *_eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

static void set_connection_status(struct zmk_widget_screen *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;

    /* Reset relay data validity on disconnect */
    if (!state.connected) {
        widget->state.central_relay_received = false;
    }

    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void peripheral_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            peripheral_status_update_cb, peripheral_get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

/*
 * ============================================================
 * Central relay status (received via zmk-central-states-relay)
 * ============================================================
 */

struct central_relay_state {
    uint8_t battery;
    bool charging;
    bool connected;
    bool bonded;
    bool usb;
    int profile_index;
    uint8_t layer_index;
    const char *layer_label;
    bool caps_lock;
};

static void set_central_relay_status(struct zmk_widget_screen *widget,
                                     struct central_relay_state state) {
    widget->state.central_relay_received = true;
    widget->state.central_relay_battery = state.battery;
    widget->state.central_relay_charging = state.charging;
    widget->state.central_relay_connected = state.connected;
    widget->state.central_relay_bonded = state.bonded;
    widget->state.central_relay_usb = state.usb;

    /* Standard names for draw_profile_status/draw_layer_status reuse */
    widget->state.active_profile_index = state.profile_index;
    widget->state.layer_index = state.layer_index;
    widget->state.layer_label = state.layer_label;
    widget->state.caps_lock = state.caps_lock;

    draw_top(widget->obj, widget->cbuf, &widget->state);
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void central_relay_status_update_cb(struct central_relay_state state) {
    struct zmk_widget_screen *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_central_relay_status(widget, state);
    }
}

static struct central_relay_state central_relay_get_state(const zmk_event_t *_eh) {
    uint8_t flags = csr_cache_get_flags();
    return (struct central_relay_state){
        .battery = csr_cache_get_central_battery(),
        .charging = flags & CSR_FLAG_USB,
        .connected = flags & CSR_FLAG_CONNECTED,
        .bonded = flags & CSR_FLAG_BONDED,
        .usb = flags & CSR_FLAG_USB,
        .profile_index = csr_cache_get_active_profile(),
        .layer_index = csr_cache_get_active_layer(),
        .layer_label = csr_cache_get_layer_name(),
        .caps_lock = csr_cache_get_hid_indicators() & HID_IND_CAPS_LOCK,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_central_relay_status, struct central_relay_state,
                            central_relay_status_update_cb, central_relay_get_state)
ZMK_SUBSCRIPTION(widget_central_relay_status, zmk_central_states_changed);

/*
 * ============================================================
 * Initialization
 * ============================================================
 */

int zmk_widget_screen_init(struct zmk_widget_screen *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, SCREEN_HEIGHT, SCREEN_WIDTH);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, BUFFER_SIZE, BUFFER_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_RIGHT, BUFFER_OFFSET_MIDDLE, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, BUFFER_SIZE, BUFFER_SIZE, LV_IMG_CF_TRUE_COLOR);
    /* Middle canvas: Caps Lock badge, redrawn on relay/connection updates */
    draw_middle(widget->obj, widget->cbuf2, &widget->state);

    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_RIGHT, BUFFER_OFFSET_BOTTOM, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, BUFFER_SIZE, BUFFER_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *middle_art_obj = lv_img_create(widget->obj);
    lv_img_set_src(middle_art_obj, &middle_art_rotated);
    lv_obj_align(middle_art_obj, LV_ALIGN_TOP_LEFT,
                 (SCREEN_HEIGHT - MIDDLE_ART_DISPLAY_WIDTH) / 2, 0);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();
    widget_central_relay_status_init();

    return 0;
}

lv_obj_t *zmk_widget_screen_obj(struct zmk_widget_screen *widget) { return widget->obj; }
