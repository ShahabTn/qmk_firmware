#include QMK_KEYBOARD_H
#include "keychron_common.h"
#include "wireless.h"  // For wireless_get_current_host()

enum layers {
    MAC_BASE,
    MAC_FN,
    WIN_BASE,
    WIN_FN,
};

enum custom_keycodes {
    TV_DRAW = SAFE_RANGE,
    ALT_OPT_R,
    MOD_F_TAP,
    MOD_H_TAP,
    MOD_V_TAP,
    MOD_J_TAP,
    MOD_R_TAP,
    MOD_C_TAP,
    MOD_S_TAP,
    MOD_L_TAP,
    TM_SNAP,
    TM_SIRI,
    TM_RGBMOD,
    TASK_VIEW,   // Host-aware: Mac -> Ctrl+Up, Windows -> Win+Tab

    // Double-tap PageUp/PageDown
    DT_PGUP,
    DT_PGDN,
};

typedef struct {
    uint8_t count;
    uint16_t timer;
    bool active;
    uint32_t led_timer;
    uint8_t led_color;
} tap_macro_t;

static tap_macro_t snap_tap = {0, 0, false, 0, 0};
static tap_macro_t siri_tap = {0, 0, false, 0, 0};
static tap_macro_t rgbmod_tap = {0, 0, false, 0, 0};

static uint16_t mod_timers[8];
static bool mod_interrupted[8];
enum { T_F, T_H, T_V, T_J, T_R, T_C, T_S, T_L };

bool drawing_mode = false;
static uint16_t alt_opt_timer;
static bool alt_opt_active = false;
static uint32_t global_led_clear_timer = 0;

// Track MOD_V_TAP state for LED feedback
static uint16_t mod_v_press_time = 0;
static bool mod_v_led_active = false;

// Track MOD_C_TAP state for LED feedback
static uint16_t mod_c_press_time = 0;
static bool mod_c_led_active = false;

// Track MOD_S_TAP state for LED feedback
static uint16_t mod_s_press_time = 0;
static bool mod_s_led_active = false;

// Track MOD_L_TAP state for LED feedback
static uint16_t mod_l_press_time = 0;
static bool mod_l_led_active = false;

#define MY_TAPPING_TERM 300
#define MY_LONG_HOLD_TERM 1000

// --- Hold TAB + R macro (Alt/Opt + R) with blue LED flash ---
static bool tab_is_held = false;
static bool tab_r_consumed = false;     // prevents MOD_R_TAP release logic after TAB+R macro fires
static uint32_t r_led_blue_timer = 0;   // timer_read32() timestamp, 0 = inactive
// ----------------------------------------------------------------

// ===== Deterministic OS selection based on which BT host key was pressed =====
// BT_HST1 -> macOS
// BT_HST2 -> Windows
typedef enum {
    OS_UNKNOWN = 0,
    OS_MAC,
    OS_WIN
} os_mode_t;

// IMPORTANT: Default should be Windows so behavior is correct even when USB is connected
// and before you press BT_HST1/BT_HST2.
static os_mode_t os_mode = OS_WIN;

static inline bool is_mac_mode(void) {
    return (os_mode == OS_MAC);
}

// ===== Double-tap logic (PageUp/PageDown -> Home/End) =====
#define DT_TERM 200
static uint16_t dt_pgup_timer = 0;
static uint16_t dt_pgdn_timer = 0;
static bool dt_pgup_waiting = false;
static bool dt_pgdn_waiting = false;
// =========================================================

void set_macro_led(uint8_t index, uint8_t color_type) {
    if (color_type == 1)      rgb_matrix_set_color(index, 0, 255, 0);   // Green
    else if (color_type == 2) rgb_matrix_set_color(index, 255, 255, 0); // Yellow
    else if (color_type == 3) rgb_matrix_set_color(index, 255, 0, 0);   // Red
    else if (color_type == 4) rgb_matrix_set_color(index, 0, 100, 255); // Blue
}

// === Timeframe indicator (LED 13-15) logic per your spec ===
// - TM_SNAP   -> Yellow
// - TM_SIRI   -> Green
// - TM_RGBMOD -> Red
// - 1 tap  -> LED13
// - 2 taps -> LED13+LED14
// - 3 taps -> LED13+LED14+LED15
// - Duration: 500ms
static void set_timeframe_indicator(uint16_t keycode, uint8_t count) {
    uint8_t r = 0, g = 0, b = 0;

    switch (keycode) {
        case TM_SNAP:   // Yellow
            r = 255; g = 255; b = 0;
            break;
        case TM_SIRI:   // Green
            r = 0; g = 255; b = 0;
            break;
        case TM_RGBMOD: // Red
            r = 255; g = 0; b = 0;
            break;
        default:
            return;
    }

    if (count >= 1) rgb_matrix_set_color(13, r, g, b);
    if (count >= 2) rgb_matrix_set_color(14, r, g, b);
    if (count >= 3) rgb_matrix_set_color(15, r, g, b);
}
// =================================================================

// Updated to send Timeframe + Enter (No Reset)
void run_timeframe_macro(uint16_t keycode, tap_macro_t *tap_data) {
    if (tap_data->count == 0) return;

    // Store count and start indicator timer (500ms)
    tap_data->led_color = tap_data->count;          // reuse as "count"
    tap_data->led_timer = timer_read32();           // indicator start time

    switch (keycode) {
        case TM_SNAP:
            if (tap_data->count == 1) tap_code(KC_1);
            else if (tap_data->count == 2) tap_code(KC_3);
            else tap_code(KC_5);
            break;
        case TM_SIRI:
            if (tap_data->count == 1) SEND_STRING("15");
            else if (tap_data->count == 2) SEND_STRING("1H");
            else SEND_STRING("4H");
            break;
        case TM_RGBMOD:
            if (tap_data->count == 1) SEND_STRING("1D");
            else if (tap_data->count == 2) SEND_STRING("1W");
            else SEND_STRING("1M");
            break;
    }
    // Send Enter to confirm the timeframe change
    tap_code(KC_ENT);
}

void check_mod_interruption(uint16_t keycode) {
    if (mod_timers[T_F] && keycode != MOD_F_TAP) { tap_code(KC_F); mod_timers[T_F] = 0; mod_interrupted[T_F] = true; }
    if (mod_timers[T_R] && keycode != MOD_R_TAP) { tap_code(KC_R); mod_timers[T_R] = 0; mod_interrupted[T_R] = true; }
    if (mod_timers[T_H] && keycode != MOD_H_TAP) { tap_code(KC_H); mod_timers[T_H] = 0; mod_interrupted[T_H] = true; }
    if (mod_timers[T_V] && keycode != MOD_V_TAP) { tap_code(KC_V); mod_timers[T_V] = 0; mod_interrupted[T_V] = true; }
    if (mod_timers[T_J] && keycode != MOD_J_TAP) { tap_code(KC_J); mod_timers[T_J] = 0; mod_interrupted[T_J] = true; }
    if (mod_timers[T_C] && keycode != MOD_C_TAP) { tap_code(KC_C); mod_timers[T_C] = 0; mod_interrupted[T_C] = true; }
    if (mod_timers[T_S] && keycode != MOD_S_TAP) { tap_code(KC_S); mod_timers[T_S] = 0; mod_interrupted[T_S] = true; }
    if (mod_timers[T_L] && keycode != MOD_L_TAP) { tap_code(KC_L); mod_timers[T_L] = 0; mod_interrupted[T_L] = true; }
}

static void trigger_tab_r_macro(bool is_mac) {
    tap_code16(is_mac ? LOPT(KC_R) : LALT(KC_R));
    r_led_blue_timer = timer_read32();  // 500ms blue on LEDs 13/14/15
}

const uint16_t PROGMEM keymaps[4][MATRIX_ROWS][MATRIX_COLS] = {
    [MAC_BASE] = LAYOUT_tkl_iso(
        QK_GESC,  KC_BRID,  KC_BRIU,  TASK_VIEW, _______, RGB_VAD,  RGB_VAI,  _______,  _______,  _______,  BT_HST1,  BT_HST2,  KC_DEL,    KC_MUTE,    TM_SNAP,  TM_SIRI,  TM_RGBMOD,
        TV_DRAW,  KC_1,     KC_2,     KC_3,     KC_4,     KC_5,     KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_MINS,  KC_EQL,     KC_BSPC,    S(G(KC_4)),   LALT(KC_R),  DT_PGUP,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     MOD_R_TAP, KC_T,     KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_LBRC,  KC_RBRC,                _______,   _______,   DT_PGDN,
        KC_CAPS,  KC_A,     KC_S,     KC_D,     MOD_F_TAP, KC_G,    MOD_H_TAP, MOD_J_TAP, KC_K,     KC_L,     KC_SCLN,  KC_QUOT,  KC_NUHS,    KC_ENT,
        KC_LSFT,  KC_NUBS,  KC_Z,     KC_X,     MOD_C_TAP, MOD_V_TAP, KC_B,     KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,              KC_RSFT,              KC_UP,
        KC_LCTL,  KC_LCMD,  ALT_OPT_R,                             KC_SPC,                                 KC_RCMMD, KC_ROPTN, MO(MAC_FN), KC_RCTL,    KC_LEFT,  KC_DOWN,  KC_RGHT),

    [MAC_FN] = LAYOUT_tkl_iso(
        QK_BOOT,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,    _______,    _______,  _______,  _______,
        KC_GRV,  BT_HST1,  BT_HST2,  BT_HST3,  P2P4G,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,    _______,    _______,  _______,  _______,
        _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,                _______,  _______,  _______,
        _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,    _______,
        _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,              _______,              _______,
        _______,  _______,  _______,                                _______,                                _______,  _______,  _______,    _______,    _______,  _______,  _______),

    [WIN_BASE] = LAYOUT_tkl_iso(
        QK_GESC,  KC_BRID,  KC_BRIU,  TASK_VIEW, _______,  RGB_VAD,  RGB_VAI,  _______,  _______,  _______,  BT_HST1,  BT_HST2,  KC_DEL,    KC_MUTE,    TM_SNAP,  TM_SIRI,  TM_RGBMOD,
        TV_DRAW,  KC_1,     KC_2,     KC_3,     KC_4,     KC_5,     KC_6,     KC_7,     KC_8,     KC_9,     KC_0,     KC_MINS,  KC_EQL,     KC_BSPC,    S(G(KC_S)),   LALT(KC_R),  DT_PGUP,
        KC_TAB,   KC_Q,     KC_W,     KC_E,     MOD_R_TAP, KC_T,     KC_Y,     KC_U,     KC_I,     KC_O,     KC_P,     KC_LBRC,  KC_RBRC,                _______,   _______,   DT_PGDN,
        KC_CAPS,  KC_A,     MOD_S_TAP, KC_D,    MOD_F_TAP, KC_G,    MOD_H_TAP, MOD_J_TAP, KC_K,     MOD_L_TAP, KC_SCLN,  KC_QUOT,  KC_NUHS,    KC_ENT,
        KC_LSFT,  KC_NUBS,  KC_Z,     KC_X,     MOD_C_TAP, MOD_V_TAP, KC_B,     KC_N,     KC_M,     KC_COMM,  KC_DOT,   KC_SLSH,              KC_RSFT,              KC_UP,
        KC_LCTL,  KC_LWIN,  ALT_OPT_R,                             KC_SPC,                                 KC_RALT,  KC_RWIN,  MO(WIN_FN), KC_RCTL,    KC_LEFT,  KC_DOWN,  KC_RGHT),

    [WIN_FN] = LAYOUT_tkl_iso(
        QK_BOOT,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,    _______,    _______,  _______,  _______,
        KC_GRV,  BT_HST1,  BT_HST2,  BT_HST3,  P2P4G,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,    _______,    _______,  _______,  _______,
        _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,                _______,  _______,  _______,
        _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,    _______,
        _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,  _______,              _______,              _______,
        _______,  _______,  _______,                                _______,                                _______,  _______,  _______,    _______,    _______,  _______,  _______),
};

#if defined(ENCODER_MAP_ENABLE)
const uint16_t PROGMEM encoder_map[4][NUM_ENCODERS][2] = {
    [MAC_BASE] = {ENCODER_CCW_CW(KC_VOLD, KC_VOLU)},
    [MAC_FN]   = {ENCODER_CCW_CW(RGB_VAD, RGB_VAI)},
    [WIN_BASE] = {ENCODER_CCW_CW(KC_VOLD, KC_VOLU)},
    [WIN_FN]   = {ENCODER_CCW_CW(RGB_VAD, RGB_VAI)},
};
#endif

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    // Update OS mode deterministically based on BT host selection keys
    if (record->event.pressed) {
        if (keycode == BT_HST1) {
            os_mode = OS_MAC;
        } else if (keycode == BT_HST2) {
            os_mode = OS_WIN;
        }
    }

    bool is_mac = is_mac_mode();

    // Double-tap PGUP/PGDN
    if (record->event.pressed) {
        if (keycode == DT_PGUP) {
            if (dt_pgup_waiting && timer_elapsed(dt_pgup_timer) <= DT_TERM) {
                dt_pgup_waiting = false;
                tap_code(KC_HOME);
            } else {
                dt_pgup_waiting = true;
                dt_pgup_timer = timer_read();
                tap_code(KC_PGUP);
            }
            return false;
        } else if (keycode == DT_PGDN) {
            if (dt_pgdn_waiting && timer_elapsed(dt_pgdn_timer) <= DT_TERM) {
                dt_pgdn_waiting = false;
                tap_code(KC_END);
            } else {
                dt_pgdn_waiting = true;
                dt_pgdn_timer = timer_read();
                tap_code(KC_PGDN);
            }
            return false;
        }
    }

    // Host-aware Task View / Mission Control key
    if (keycode == TASK_VIEW) {
        if (record->event.pressed) {
            if (is_mac) {
                tap_code16(LCTL(KC_UP));     // macOS Mission Control (if configured as Ctrl+Up)
            } else {
                tap_code16(LGUI(KC_TAB));    // Windows Task View
            }
        }
        return false;
    }

    // Track TAB held state (physical Tab key)
    if (keycode == KC_TAB) {
        if (record->event.pressed) {
            tab_is_held = true;
        } else {
            tab_is_held = false;
        }
        // Let QMK continue processing Tab normally
    }

    if (record->event.pressed) {
        check_mod_interruption(keycode);
    }

    // If TAB is held and user presses the R key (MOD_R_TAP), trigger Alt/Opt+R
    if (record->event.pressed && tab_is_held && keycode == MOD_R_TAP) {
        tab_r_consumed = true;          // mark consumed so release won't trigger MOD_R_TAP hold logic after macro fires
        trigger_tab_r_macro(is_mac);
        return false;
    }

    switch (keycode) {
        case TM_SNAP:
            if (record->event.pressed) { snap_tap.active = true; snap_tap.timer = timer_read(); snap_tap.count++; if (snap_tap.count > 3) snap_tap.count = 3; }
            return false;
        case TM_SIRI:
            if (record->event.pressed) { siri_tap.active = true; siri_tap.timer = timer_read(); siri_tap.count++; if (siri_tap.count > 3) siri_tap.count = 3; }
            return false;
        case TM_RGBMOD:
            if (record->event.pressed) { rgbmod_tap.active = true; rgbmod_tap.timer = timer_read(); rgbmod_tap.count++; if (rgbmod_tap.count > 3) rgbmod_tap.count = 3; }
            return false;

        case ALT_OPT_R:
            if (record->event.pressed) {
                alt_opt_timer = timer_read();
                alt_opt_active = true;
                register_code(is_mac ? KC_LOPT : KC_LALT);
            } else {
                unregister_code(is_mac ? KC_LOPT : KC_LALT);
                if (timer_elapsed(alt_opt_timer) < MY_TAPPING_TERM) {
                    tap_code16(is_mac ? LOPT(KC_R) : LALT(KC_R));
                }
                alt_opt_active = false;
            }
            return false;

        case MOD_F_TAP:
            if (record->event.pressed) { mod_timers[T_F] = timer_read(); mod_interrupted[T_F] = false; }
            else {
                if (!mod_interrupted[T_F]) {
                    if (timer_elapsed(mod_timers[T_F]) >= MY_TAPPING_TERM) { tap_code16(is_mac ? LOPT(KC_F) : LALT(KC_F)); }
                    else { tap_code(KC_F); }
                }
                mod_timers[T_F] = 0;
            }
            return false;

        case MOD_R_TAP:
            if (tab_r_consumed) {
                if (!record->event.pressed) tab_r_consumed = false;
                return false;
            }

            if (record->event.pressed) {
                mod_timers[T_R] = timer_read();
                mod_interrupted[T_R] = false;
            } else {
                if (!mod_interrupted[T_R]) {
                    uint16_t elapsed = timer_elapsed(mod_timers[T_R]);

                    if (elapsed < MY_TAPPING_TERM) tap_code(KC_R);
                    else tap_code16(is_mac ? LOPT(LSFT(KC_R)) : LALT(LSFT(KC_R)));
                }
                mod_timers[T_R] = 0;
            }
            return false;

        case MOD_H_TAP:
            if (record->event.pressed) { mod_timers[T_H] = timer_read(); mod_interrupted[T_H] = false; }
            else {
                if (!mod_interrupted[T_H]) {
                    if (timer_elapsed(mod_timers[T_H]) >= MY_TAPPING_TERM) { tap_code16(is_mac ? LOPT(KC_H) : LALT(KC_H)); }
                    else { tap_code(KC_H); }
                }
                mod_timers[T_H] = 0;
            }
            return false;

        case MOD_V_TAP:
            if (record->event.pressed) {
                mod_timers[T_V] = timer_read();
                mod_interrupted[T_V] = false;
                mod_v_press_time = timer_read();
                mod_v_led_active = true;
            } else {
                if (!mod_interrupted[T_V]) {
                    uint16_t elapsed = timer_elapsed(mod_timers[T_V]);

                    if (elapsed < MY_TAPPING_TERM) tap_code(KC_V);
                    else if (elapsed < MY_LONG_HOLD_TERM) tap_code16(is_mac ? LOPT(KC_V) : LALT(KC_V));
                    else tap_code16(is_mac ? LGUI(KC_V) : LCTL(KC_V));
                }
                mod_timers[T_V] = 0;
                global_led_clear_timer = 0;
                mod_v_led_active = false;
            }
            return false;

        case MOD_J_TAP:
            if (record->event.pressed) { mod_timers[T_J] = timer_read(); mod_interrupted[T_J] = false; }
            else {
                if (!mod_interrupted[T_J]) {
                    if (timer_elapsed(mod_timers[T_J]) >= MY_TAPPING_TERM) { tap_code16(is_mac ? LOPT(KC_J) : LALT(KC_J)); }
                    else { tap_code(KC_J); }
                }
                mod_timers[T_J] = 0;
            }
            return false;

        case MOD_C_TAP:
            if (record->event.pressed) {
                mod_timers[T_C] = timer_read();
                mod_interrupted[T_C] = false;
                mod_c_press_time = timer_read();
                mod_c_led_active = true;
            } else {
                if (!mod_interrupted[T_C]) {
                    if (timer_elapsed(mod_timers[T_C]) >= MY_LONG_HOLD_TERM) tap_code16(is_mac ? LGUI(KC_C) : LCTL(KC_C));
                    else tap_code(KC_C);
                }
                mod_timers[T_C] = 0;
                global_led_clear_timer = 0;
                mod_c_led_active = false;
            }
            return false;

        case MOD_S_TAP:
            if (record->event.pressed) {
                mod_timers[T_S] = timer_read();
                mod_interrupted[T_S] = false;
                mod_s_press_time = timer_read();
                mod_s_led_active = true;
            } else {
                if (!mod_interrupted[T_S]) {
                    uint16_t elapsed = timer_elapsed(mod_timers[T_S]);

                    if (elapsed < MY_TAPPING_TERM) tap_code(KC_S);
                    else if (elapsed < MY_LONG_HOLD_TERM) tap_code16(LCTL(KC_S));
                    else tap_code16(LCTL(LALT(KC_S)));
                }
                mod_timers[T_S] = 0;
                global_led_clear_timer = 0;
                mod_s_led_active = false;
            }
            return false;

        case MOD_L_TAP:
            if (record->event.pressed) {
                mod_timers[T_L] = timer_read();
                mod_interrupted[T_L] = false;
                mod_l_press_time = timer_read();
                mod_l_led_active = true;
            } else {
                if (!mod_interrupted[T_L]) {
                    if (timer_elapsed(mod_timers[T_L]) >= MY_LONG_HOLD_TERM) tap_code16(LALT(KC_S));
                    else tap_code(KC_L);
                }
                mod_timers[T_L] = 0;
                global_led_clear_timer = 0;
                mod_l_led_active = false;
            }
            return false;

        case TV_DRAW:
            if (record->event.pressed) {
                if (!drawing_mode) { tap_code16(is_mac ? LOPT(KC_T) : LALT(KC_T)); wait_ms(100); register_code(KC_LSFT); drawing_mode = true; }
                else { unregister_code(KC_LSFT); drawing_mode = false; global_led_clear_timer = timer_read32(); }
            }
            return false;
    }

    return process_record_keychron_common(keycode, record);
}

void matrix_scan_user(void) {
    if (snap_tap.active && timer_elapsed(snap_tap.timer) > 300) { run_timeframe_macro(TM_SNAP, &snap_tap); snap_tap.count = 0; snap_tap.active = false; }
    if (siri_tap.active && timer_elapsed(siri_tap.timer) > 300) { run_timeframe_macro(TM_SIRI, &siri_tap); siri_tap.count = 0; siri_tap.active = false; }
    if (rgbmod_tap.active && timer_elapsed(rgbmod_tap.timer) > 300) { run_timeframe_macro(TM_RGBMOD, &rgbmod_tap); rgbmod_tap.count = 0; rgbmod_tap.active = false; }

    for (int i = 0; i < 8; i++) {
        if (mod_timers[i] > 0 && timer_elapsed(mod_timers[i]) >= MY_TAPPING_TERM && !mod_interrupted[i]) {
            if (global_led_clear_timer == 0) global_led_clear_timer = timer_read32();
        }
    }

    // Indicator duration: 500ms
    if (snap_tap.led_color > 0 && timer_elapsed32(snap_tap.led_timer) > 500) snap_tap.led_color = 0;
    if (siri_tap.led_color > 0 && timer_elapsed32(siri_tap.led_timer) > 500) siri_tap.led_color = 0;
    if (rgbmod_tap.led_color > 0 && timer_elapsed32(rgbmod_tap.led_timer) > 500) rgbmod_tap.led_color = 0;

    if (global_led_clear_timer > 0 && timer_elapsed32(global_led_clear_timer) >= 1000) global_led_clear_timer = 0;
}

bool rgb_matrix_indicators_user(void) {
    // ===== BLUETOOTH DEVICE INDICATOR LEDs (HIGHEST PRIORITY) =====
    // LED1 (Blue) when connected to Mac (BT1 = host_index 1)
    // LED2 (Blue) when connected to Windows (BT2 = host_index 2)
    // Always-on indicator - independent from layers
    if (wireless_get_current_host() == 1) {
        rgb_matrix_set_color(10, 0, 100, 255);     // F10 - Blue for Mac
        rgb_matrix_set_color(11, 0, 0, 0);         // F11 - Off
    } else if (wireless_get_current_host() == 2) {
        rgb_matrix_set_color(10, 0, 0, 0);         // F10 - Off
        rgb_matrix_set_color(11, 0, 100, 255);     // F11 - Blue for Windows
    } else {
        rgb_matrix_set_color(10, 0, 0, 0);         // Off
        rgb_matrix_set_color(11, 0, 0, 0);         // Off
    }
    // ==============================================================

    // ===== Hold TAB + R => Blue flash (500ms) on LEDs 13/14/15 =====
    if (r_led_blue_timer > 0) {
        if (timer_elapsed32(r_led_blue_timer) <= 500) {
            rgb_matrix_set_color(13, 0, 100, 255);
            rgb_matrix_set_color(14, 0, 100, 255);
            rgb_matrix_set_color(15, 0, 100, 255);
            return true;
        } else {
            r_led_blue_timer = 0;
        }
    }
    // =============================================================

    // ===== Timeframe indicators (500ms) per your requested pattern =====
    if (snap_tap.led_color > 0 && timer_elapsed32(snap_tap.led_timer) <= 500) {
        set_timeframe_indicator(TM_SNAP, snap_tap.led_color);
        return true;
    }
    if (siri_tap.led_color > 0 && timer_elapsed32(siri_tap.led_timer) <= 500) {
        set_timeframe_indicator(TM_SIRI, siri_tap.led_color);
        return true;
    }
    if (rgbmod_tap.led_color > 0 && timer_elapsed32(rgbmod_tap.led_timer) <= 500) {
        set_timeframe_indicator(TM_RGBMOD, rgbmod_tap.led_color);
        return true;
    }
    // =================================================================

    bool caps_on = host_keyboard_led_state().caps_lock;

    // MOD_V_TAP LED FEEDBACK - HIGHEST PRIORITY (check first)
    if (mod_v_led_active) {
        uint16_t v_elapsed = timer_elapsed(mod_v_press_time);

        if (v_elapsed < 300) {
            // 0ms - 300ms: NO INDICATOR
            return true;

        } else if (v_elapsed < 1000) {
            // 300ms - 1000ms: YELLOW INDICATOR
            rgb_matrix_set_color(13, 255, 255, 0);
            rgb_matrix_set_color(14, 255, 255, 0);
            rgb_matrix_set_color(15, 255, 255, 0);
            return true;
        } else {
            // > 1000ms: BLUE INDICATOR
            rgb_matrix_set_color(13, 0, 100, 255);
            rgb_matrix_set_color(14, 0, 100, 255);
            rgb_matrix_set_color(15, 0, 100, 255);
            return true;
        }
    }

    // MOD_S_TAP LED FEEDBACK
    if (mod_s_led_active) {
        uint16_t s_elapsed = timer_elapsed(mod_s_press_time);

        if (s_elapsed < 300) {
            // 0ms - 300ms: NO INDICATOR
            return true;

        } else if (s_elapsed < 1000) {
            // 300ms - 1000ms: YELLOW INDICATOR
            rgb_matrix_set_color(13, 255, 255, 0);
            rgb_matrix_set_color(14, 255, 255, 0);
            rgb_matrix_set_color(15, 255, 255, 0);
            return true;
        } else {
            // > 1000ms: BLUE INDICATOR
            rgb_matrix_set_color(13, 0, 100, 255);
            rgb_matrix_set_color(14, 0, 100, 255);
            rgb_matrix_set_color(15, 0, 100, 255);
            return true;
        }
    }

    // MOD_C_TAP LED FEEDBACK
    if (mod_c_led_active) {
        uint16_t c_elapsed = timer_elapsed(mod_c_press_time);

        if (c_elapsed >= MY_LONG_HOLD_TERM) {
            // >= 1000ms: BLUE INDICATOR
            rgb_matrix_set_color(13, 0, 100, 255);
            rgb_matrix_set_color(14, 0, 100, 255);
            rgb_matrix_set_color(15, 0, 100, 255);
            return true;
        }
        return true;
    }

    // MOD_L_TAP LED FEEDBACK
    if (mod_l_led_active) {
        uint16_t l_elapsed = timer_elapsed(mod_l_press_time);

        if (l_elapsed >= MY_LONG_HOLD_TERM) {
            // >= 1000ms: BLUE INDICATOR
            rgb_matrix_set_color(13, 0, 100, 255);
            rgb_matrix_set_color(14, 0, 100, 255);
            rgb_matrix_set_color(15, 0, 100, 255);
            return true;
        }
        return true;
    }

    // Other LED indicators (only if MOD taps are not active)
    if (caps_on) rgb_matrix_set_color(50, 255, 0, 0);

    if (alt_opt_active) {
        if (timer_elapsed(alt_opt_timer) < MY_TAPPING_TERM) {
            rgb_matrix_set_color(81, 255, 255, 0);
            rgb_matrix_set_color(79, 255, 255, 0);
        } else {
            rgb_matrix_set_color(81, 0, 255, 0);
            rgb_matrix_set_color(79, 0, 255, 0);
        }
    }

    // Keep existing fallback behavior for LEDs 13-15 (when no timeframe indicator is active)
    if (caps_on) {
        rgb_matrix_set_color(13, 255, 0, 0);
        rgb_matrix_set_color(14, 255, 0, 0);
        rgb_matrix_set_color(15, 255, 0, 0);
    }

    if (global_led_clear_timer > 0) {
        rgb_matrix_set_color(13, 255, 255, 0);
        rgb_matrix_set_color(14, 255, 255, 0);
        rgb_matrix_set_color(15, 255, 255, 0);
    }

    if (drawing_mode) {
        if ((timer_read32() % 500) < 300) {
            rgb_matrix_set_color(13, 255, 0, 0);
        } else {
            rgb_matrix_set_color(13, 0, 0, 0);
        }
    }

    return true;
}
