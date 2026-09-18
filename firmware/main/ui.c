#include "ui.h"
#include "telemetry.h"
#include <stdio.h>
#include <time.h>
#include <math.h>
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "ui";

#define SCREEN_W  (320)
#define SCREEN_H  (240)

#define LEFT_EYE_X   (72)
#define RIGHT_EYE_X  (202)
#define BASE_EYE_Y   (110)
#define BASE_EYE_W   (46)
#define BASE_EYE_H   (62)
#define BASE_EYE_R   (20)

#define MOUTH_X      (145)
#define BASE_MOUTH_Y (180)

/* Screen objects */
static lv_obj_t *s_scr_idle = NULL;
static lv_obj_t *s_scr_ai   = NULL;

/* Clock widgets on Idle Screen */
static lv_obj_t *s_clock_time = NULL;
static lv_obj_t *s_clock_date = NULL;

/* Face widgets on Idle Screen */
static lv_obj_t *s_brow_l = NULL;
static lv_obj_t *s_brow_r = NULL;
static lv_obj_t *s_eye_l = NULL;
static lv_obj_t *s_eye_r = NULL;
static lv_obj_t *s_eye_l_hl1 = NULL;
static lv_obj_t *s_eye_l_hl2 = NULL;
static lv_obj_t *s_eye_r_hl1 = NULL;
static lv_obj_t *s_eye_r_hl2 = NULL;
static lv_obj_t *s_blush_l = NULL;
static lv_obj_t *s_blush_r = NULL;
static lv_obj_t *s_mouth   = NULL;

/* AI Mode Screen widgets */
static lv_obj_t *s_cpu_val  = NULL;
static lv_obj_t *s_cpu_bar  = NULL;
static lv_obj_t *s_ram_val  = NULL;
static lv_obj_t *s_ram_bar  = NULL;
static lv_obj_t *s_gpu_val  = NULL;
static lv_obj_t *s_gpu_bar  = NULL;
static lv_obj_t *s_temp_val = NULL;
static lv_obj_t *s_temp_bar = NULL;
static lv_obj_t *s_vram_val = NULL;
static lv_obj_t *s_vram_bar = NULL;

/* Emotion palette and state */
typedef struct {
    uint32_t eye_color;      // Color of eyes, eyebrows, mouth
    uint32_t blush_color;    // Cheek blush color
    uint8_t  blush_opa;      // Cheek opacity (0..255)
    uint32_t text_color;     // Date / mood subtitle color
    const char *status_tag;  // Mood label
} emotion_palette_t;

static const emotion_palette_t s_palettes[PET_EMOTION_COUNT] = {
    [PET_EMOTION_HAPPY] = {
        .eye_color   = 0x00E5FF, // Vibrant Cyan
        .blush_color = 0xFF6B8B, // Sweet Rose Pink
        .blush_opa   = 170,
        .text_color  = 0x70A5FF, // Sky Blue
        .status_tag  = "HAPPY",
    },
    [PET_EMOTION_FOCUS] = {
        .eye_color   = 0xFF9F0A, // Warm Amber / Golden Glow
        .blush_color = 0xFF8500, // Warm Peach Blush
        .blush_opa   = 190,
        .text_color  = 0xFFA520, // Amber
        .status_tag  = "FOCUS",
    },
    [PET_EMOTION_RELAX] = {
        .eye_color   = 0x30D158, // Fresh Mint / Emerald
        .blush_color = 0x25C474, // Soft Mint Pink
        .blush_opa   = 140,
        .text_color  = 0x34C759, // Green
        .status_tag  = "CHILL",
    },
    [PET_EMOTION_LOVE] = {
        .eye_color   = 0xFF375F, // Radiant Blossom Pink
        .blush_color = 0xFF2D55, // Deep Magenta Blush
        .blush_opa   = 245,
        .text_color  = 0xFF6482, // Rosy
        .status_tag  = "LOVE <3",
    },
    [PET_EMOTION_HOT] = {
        .eye_color   = 0xFF453A, // Fiery Crimson Red
        .blush_color = 0xFF3B30, // Burning Coral Flush
        .blush_opa   = 230,
        .text_color  = 0xFF5F56, // Crimson
        .status_tag  = "HOT!",
    },
};

static pet_emotion_t s_current_emotion = PET_EMOTION_HAPPY;
static int s_love_countdown = 0;

/* Face animation state machine */
typedef enum {
    EYE_STATE_OPEN = 0,
    EYE_STATE_CLOSING,
    EYE_STATE_CLOSED,
    EYE_STATE_OPENING
} eye_state_t;

static eye_state_t s_eye_state = EYE_STATE_OPEN;
static int s_blink_timer   = 80;
static int s_blink_frame   = 0;
static uint32_t s_anim_tick = 0;
static int s_look_offset_x = 0;
static int s_target_look_x = 0;
static int s_look_timer    = 100;

/* Public Emotion Accessors */
void ui_set_emotion(pet_emotion_t emotion)
{
    if (emotion < PET_EMOTION_COUNT) {
        s_current_emotion = emotion;
    }
}

pet_emotion_t ui_get_emotion(void)
{
    return s_current_emotion;
}

/* Touch & Button handler to toggle screens */
static void screen_toggle_cb(lv_event_t *e)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) return;
    lv_obj_t *act = lv_disp_get_scr_act(disp);
    if (act == s_scr_idle) {
        lv_scr_load(s_scr_ai);
        ESP_LOGI(TAG, "Touch: Switched to SYSTEM STATUS screen");
    } else {
        lv_scr_load(s_scr_idle);
        s_love_countdown = 75; // ~3s of happy Love react when returning to pet
        ESP_LOGI(TAG, "Touch: Switched to PET screen");
    }
}

/* Public: toggle AI Mode (called from MAIN / MUTE button callbacks). */
void ui_toggle_ai_mode(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) {
        return;
    }
    lv_obj_t *act = lv_disp_get_scr_act(disp);
    if (act == s_scr_idle) {
        lv_scr_load(s_scr_ai);
        ESP_LOGI(TAG, "Button: switched to SYSTEM STATUS screen");
    } else {
        lv_scr_load(s_scr_idle);
        s_love_countdown = 75; // ~3s of happy Love react
        ESP_LOGI(TAG, "Button: switched to PET screen");
    }
}

/* Helper to attach touch overlay covering whole screen */
static void add_touch_overlay(lv_obj_t *parent)
{
    lv_obj_t *overlay = lv_obj_create(parent);
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, screen_toggle_cb, LV_EVENT_CLICKED, NULL);
}

/* Create Idle Screen with Clock + Procedural Animated Face */
static void create_idle_screen(void)
{
    s_scr_idle = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_idle, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr_idle, LV_OPA_COVER, 0);

    /* Clock Time: Montserrat 40 */
    s_clock_time = lv_label_create(s_scr_idle);
    lv_obj_set_style_text_font(s_clock_time, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_clock_time, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_clock_time, "--:--");
    lv_obj_align(s_clock_time, LV_ALIGN_TOP_MID, 0, 6);

    /* Clock Date: Montserrat 14 */
    s_clock_date = lv_label_create(s_scr_idle);
    lv_obj_set_style_text_font(s_clock_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_clock_date, lv_color_hex(0x70A5FF), 0);
    lv_label_set_text(s_clock_date, "DESK PET READY");
    lv_obj_align(s_clock_date, LV_ALIGN_TOP_MID, 0, 50);

    /* Left Eyebrow */
    s_brow_l = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_brow_l, 34, 5);
    lv_obj_set_pos(s_brow_l, LEFT_EYE_X + 6, BASE_EYE_Y - 14);
    lv_obj_set_style_radius(s_brow_l, 2, 0);
    lv_obj_set_style_bg_color(s_brow_l, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(s_brow_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_brow_l, 0, 0);
    lv_obj_clear_flag(s_brow_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Right Eyebrow */
    s_brow_r = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_brow_r, 34, 5);
    lv_obj_set_pos(s_brow_r, RIGHT_EYE_X + 6, BASE_EYE_Y - 14);
    lv_obj_set_style_radius(s_brow_r, 2, 0);
    lv_obj_set_style_bg_color(s_brow_r, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(s_brow_r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_brow_r, 0, 0);
    lv_obj_clear_flag(s_brow_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Left Eye */
    s_eye_l = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_eye_l, BASE_EYE_W, BASE_EYE_H);
    lv_obj_set_pos(s_eye_l, LEFT_EYE_X, BASE_EYE_Y);
    lv_obj_set_style_radius(s_eye_l, BASE_EYE_R, 0);
    lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(s_eye_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eye_l, 0, 0);
    lv_obj_set_style_pad_all(s_eye_l, 0, 0);
    lv_obj_clear_flag(s_eye_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Left Eye Sparkles */
    s_eye_l_hl1 = lv_obj_create(s_eye_l);
    lv_obj_set_size(s_eye_l_hl1, 14, 14);
    lv_obj_set_pos(s_eye_l_hl1, 6, 6);
    lv_obj_set_style_radius(s_eye_l_hl1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_l_hl1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_eye_l_hl1, 0, 0);
    lv_obj_clear_flag(s_eye_l_hl1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_eye_l_hl2 = lv_obj_create(s_eye_l);
    lv_obj_set_size(s_eye_l_hl2, 6, 6);
    lv_obj_set_pos(s_eye_l_hl2, 28, 44);
    lv_obj_set_style_radius(s_eye_l_hl2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_l_hl2, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_eye_l_hl2, 190, 0);
    lv_obj_set_style_border_width(s_eye_l_hl2, 0, 0);
    lv_obj_clear_flag(s_eye_l_hl2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Right Eye */
    s_eye_r = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_eye_r, BASE_EYE_W, BASE_EYE_H);
    lv_obj_set_pos(s_eye_r, RIGHT_EYE_X, BASE_EYE_Y);
    lv_obj_set_style_radius(s_eye_r, BASE_EYE_R, 0);
    lv_obj_set_style_bg_color(s_eye_r, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(s_eye_r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eye_r, 0, 0);
    lv_obj_set_style_pad_all(s_eye_r, 0, 0);
    lv_obj_clear_flag(s_eye_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Right Eye Sparkles */
    s_eye_r_hl1 = lv_obj_create(s_eye_r);
    lv_obj_set_size(s_eye_r_hl1, 14, 14);
    lv_obj_set_pos(s_eye_r_hl1, 6, 6);
    lv_obj_set_style_radius(s_eye_r_hl1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_r_hl1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_eye_r_hl1, 0, 0);
    lv_obj_clear_flag(s_eye_r_hl1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_eye_r_hl2 = lv_obj_create(s_eye_r);
    lv_obj_set_size(s_eye_r_hl2, 6, 6);
    lv_obj_set_pos(s_eye_r_hl2, 28, 44);
    lv_obj_set_style_radius(s_eye_r_hl2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_r_hl2, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_eye_r_hl2, 190, 0);
    lv_obj_set_style_border_width(s_eye_r_hl2, 0, 0);
    lv_obj_clear_flag(s_eye_r_hl2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Cheeks (Blush) */
    s_blush_l = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_blush_l, 24, 12);
    lv_obj_set_pos(s_blush_l, 36, 160);
    lv_obj_set_style_radius(s_blush_l, 6, 0);
    lv_obj_set_style_bg_color(s_blush_l, lv_color_hex(0xFF6B8B), 0);
    lv_obj_set_style_bg_opa(s_blush_l, 170, 0);
    lv_obj_set_style_border_width(s_blush_l, 0, 0);
    lv_obj_clear_flag(s_blush_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_blush_r = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_blush_r, 24, 12);
    lv_obj_set_pos(s_blush_r, 260, 160);
    lv_obj_set_style_radius(s_blush_r, 6, 0);
    lv_obj_set_style_bg_color(s_blush_r, lv_color_hex(0xFF6B8B), 0);
    lv_obj_set_style_bg_opa(s_blush_r, 170, 0);
    lv_obj_set_style_border_width(s_blush_r, 0, 0);
    lv_obj_clear_flag(s_blush_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Soft mouth (using an arc) */
    s_mouth = lv_arc_create(s_scr_idle);
    lv_obj_set_size(s_mouth, 30, 22);
    lv_obj_set_pos(s_mouth, MOUTH_X, BASE_MOUTH_Y);
    lv_arc_set_angles(s_mouth, 25, 155);
    lv_arc_set_bg_angles(s_mouth, 25, 155);
    lv_obj_remove_style(s_mouth, NULL, LV_PART_KNOB);
    lv_obj_set_style_opa(s_mouth, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_mouth, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_mouth, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_mouth, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_clear_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Transparent full-screen touch overlay */
    add_touch_overlay(s_scr_idle);
}

/* Helper to build a metric row on the AI Mode Screen */
static void create_metric_row(lv_obj_t *parent, int y, const char *title, lv_color_t color,
                              lv_obj_t **val_label_out, lv_obj_t **bar_out)
{
    /* Title */
    lv_obj_t *lbl_title = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_title, color, 0);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_pos(lbl_title, 16, y);

    /* Value label */
    lv_obj_t *lbl_val = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_val, "--");
    lv_obj_align(lbl_val, LV_ALIGN_TOP_RIGHT, -16, y);
    *val_label_out = lbl_val;

    /* Bar */
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 288, 7);
    lv_obj_set_pos(bar, 16, y + 20);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1F1F24), LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
    *bar_out = bar;
}

/* Create AI Mode Screen with State Chip + 4 Metric Rows */
static void create_ai_screen(void)
{
    s_scr_ai = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_ai, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr_ai, LV_OPA_COVER, 0);

    /* Header Title */
    lv_obj_t *title = lv_label_create(s_scr_ai);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEBEBF5), 0);
    lv_label_set_text(title, "SYSTEM STATUS");
    lv_obj_set_pos(title, 16, 8);

    /* 5 Metric Rows: CPU, RAM, GPU, VRAM, GPU Temp */
    create_metric_row(s_scr_ai, 38,  "CPU",  lv_color_hex(0x00E5FF), &s_cpu_val,  &s_cpu_bar);
    create_metric_row(s_scr_ai, 76,  "RAM",  lv_color_hex(0x30D158), &s_ram_val,  &s_ram_bar);
    create_metric_row(s_scr_ai, 114, "GPU",  lv_color_hex(0xFF9F0A), &s_gpu_val,  &s_gpu_bar);
    create_metric_row(s_scr_ai, 152, "VRAM", lv_color_hex(0xBF5AF2), &s_vram_val, &s_vram_bar);
    create_metric_row(s_scr_ai, 190, "TEMP", lv_color_hex(0xFF453A), &s_temp_val, &s_temp_bar);

    /* Footer Hint */
    lv_obj_t *hint = lv_label_create(s_scr_ai);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x636366), 0);
    lv_label_set_text(hint, "TAP ANYWHERE TO EXIT");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* Transparent full-screen touch overlay */
    add_touch_overlay(s_scr_ai);
}

/* Face animation state machine tick (runs every ~40ms = 25 FPS) */
static void face_anim_tick(void)
{
    s_anim_tick++;

    /* 1. Dynamic breathing bob adjusted by emotion */
    float breath_speed = 0.08f;
    float breath_amp = 2.0f;
    int base_eye_h = BASE_EYE_H;
    int base_eye_r = BASE_EYE_R;

    if (s_current_emotion == PET_EMOTION_RELAX) {
        breath_speed = 0.04f; // Calm, deep, slow breath
        breath_amp = 1.5f;
        base_eye_h = 32;     // Half-closed sleepy zen eyes
        base_eye_r = 12;
    } else if (s_current_emotion == PET_EMOTION_FOCUS) {
        breath_speed = 0.10f; // Alert, steady
        breath_amp = 1.8f;
        base_eye_h = 50;     // Focused squint
        base_eye_r = 16;
    } else if (s_current_emotion == PET_EMOTION_HOT) {
        breath_speed = 0.12f; // Fast, agitated breathing
        breath_amp = 2.5f;
        base_eye_h = 56;
        base_eye_r = 14;
    } else if (s_current_emotion == PET_EMOTION_LOVE) {
        breath_speed = 0.09f; // Bouncy happy bob
        breath_amp = 3.0f;
        base_eye_h = 62;
        base_eye_r = 22;
    }

    float breath = sinf(s_anim_tick * breath_speed);
    int bob_y = (int)roundf(breath_amp * breath);

    /* 2. Looking around shift */
    s_look_timer--;
    if (s_look_timer <= 0) {
        s_look_timer = 75 + (rand() % 75); // Every 3 - 6 seconds
        int r = rand() % 10;
        if (s_current_emotion == PET_EMOTION_FOCUS) {
            s_target_look_x = 0; // Focus looks straight ahead
        } else if (r < 6) {
            s_target_look_x = 0; // 60% look center
        } else if (r < 8) {
            s_target_look_x = -6; // 20% glance left
        } else {
            s_target_look_x = 6;  // 20% glance right
        }
    }
    s_look_offset_x = (int)roundf(s_look_offset_x * 0.85f + s_target_look_x * 0.15f);

    /* 3. Blinking animation state machine */
    int current_h = base_eye_h;
    int current_r = base_eye_r;

    switch (s_eye_state) {
    case EYE_STATE_OPEN:
        s_blink_timer--;
        if (s_blink_timer <= 0) {
            s_eye_state = EYE_STATE_CLOSING;
            s_blink_frame = 0;
        }
        break;

    case EYE_STATE_CLOSING:
        s_blink_frame++;
        if (s_blink_frame == 1) {
            current_h = (base_eye_h * 5) / 8;
            current_r = base_eye_r / 2;
        } else if (s_blink_frame == 2) {
            current_h = 16;
            current_r = 6;
            lv_obj_add_flag(s_eye_l_hl1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eye_l_hl2, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eye_r_hl1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eye_r_hl2, LV_OBJ_FLAG_HIDDEN);
        } else {
            current_h = 5;
            current_r = 2;
            s_eye_state = EYE_STATE_CLOSED;
            s_blink_frame = 0;
        }
        break;

    case EYE_STATE_CLOSED:
        current_h = 5;
        current_r = 2;
        s_blink_frame++;
        if (s_blink_frame >= 1) {
            s_eye_state = EYE_STATE_OPENING;
            s_blink_frame = 0;
        }
        break;

    case EYE_STATE_OPENING:
        s_blink_frame++;
        if (s_blink_frame == 1) {
            current_h = 18;
            current_r = 8;
        } else if (s_blink_frame == 2) {
            current_h = (base_eye_h * 5) / 8;
            current_r = (base_eye_r * 2) / 3;
            lv_obj_clear_flag(s_eye_l_hl1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_eye_l_hl2, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_eye_r_hl1, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_eye_r_hl2, LV_OBJ_FLAG_HIDDEN);
        } else {
            current_h = base_eye_h;
            current_r = base_eye_r;
            s_eye_state = EYE_STATE_OPEN;
            // 20% chance of double blink
            if ((rand() % 5) == 0) {
                s_blink_timer = 5;
            } else {
                s_blink_timer = 70 + (rand() % 70); // 3 - 5.5s
            }
        }
        break;
    }

    /* Apply eye positions keeping vertical center constant */
    int eye_y = BASE_EYE_Y + bob_y + (BASE_EYE_H - current_h) / 2;

    lv_obj_set_size(s_eye_l, BASE_EYE_W, current_h);
    lv_obj_set_style_radius(s_eye_l, current_r, 0);
    lv_obj_set_pos(s_eye_l, LEFT_EYE_X + s_look_offset_x, eye_y);

    lv_obj_set_size(s_eye_r, BASE_EYE_W, current_h);
    lv_obj_set_style_radius(s_eye_r, current_r, 0);
    lv_obj_set_pos(s_eye_r, RIGHT_EYE_X + s_look_offset_x, eye_y);

    /* Eyebrow positions based on emotion */
    int brow_y = BASE_EYE_Y - 14 + bob_y;
    if (s_current_emotion == PET_EMOTION_FOCUS) {
        brow_y = BASE_EYE_Y - 10 + bob_y;
    } else if (s_current_emotion == PET_EMOTION_HOT) {
        brow_y = BASE_EYE_Y - 8 + bob_y;
    } else if (s_current_emotion == PET_EMOTION_RELAX || s_current_emotion == PET_EMOTION_LOVE) {
        brow_y = BASE_EYE_Y - 18 + bob_y;
    }
    if (s_brow_l) lv_obj_set_pos(s_brow_l, LEFT_EYE_X + 6 + s_look_offset_x, brow_y);
    if (s_brow_r) lv_obj_set_pos(s_brow_r, RIGHT_EYE_X + 6 + s_look_offset_x, brow_y);

    /* Mouth vertical position with breathing bob */
    lv_obj_set_pos(s_mouth, MOUTH_X, BASE_MOUTH_Y + bob_y);

    /* Dynamic mouth shape per emotion */
    if (s_current_emotion == PET_EMOTION_HOT) {
        lv_arc_set_angles(s_mouth, 205, 335); // angry / hot frown (>_<)
    } else if (s_current_emotion == PET_EMOTION_LOVE) {
        lv_arc_set_angles(s_mouth, 15, 165);  // happy wide smile (^o^)
    } else {
        lv_arc_set_angles(s_mouth, 25, 155);  // gentle smile
    }
}

/* Update Telemetry on AI Screen + Clock on Idle Screen */
static void telemetry_update_tick(void)
{
    pet_telemetry_t t = telemetry_get();

    /* 1. Evaluate Emotion State Machine (Tabbie-inspired) */
    if (s_love_countdown > 0) {
        s_current_emotion = PET_EMOTION_LOVE;
        s_love_countdown--;
    } else if (t.has_data) {
        if (t.gpu_temp_c >= 70 || t.cpu_pct >= 85 || t.gpu_pct >= 85) {
            s_current_emotion = PET_EMOTION_HOT;
        } else if (t.cpu_pct >= 35 || t.gpu_pct >= 30) {
            s_current_emotion = PET_EMOTION_FOCUS;
        } else if (t.cpu_pct <= 10 && t.gpu_pct <= 15) {
            s_current_emotion = PET_EMOTION_RELAX;
        } else {
            s_current_emotion = PET_EMOTION_HAPPY;
        }
    } else {
        s_current_emotion = PET_EMOTION_HAPPY;
    }

    const emotion_palette_t *p = &s_palettes[s_current_emotion];

    /* 2. Update Clock & Date with Mood on Idle Screen */
    if (t.has_data) {
        time_t local_sec = (time_t)(t.epoch_utc + TIMEZONE_OFFSET_SEC);
        struct tm tm_info;
        gmtime_r(&local_sec, &tm_info);

        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
        lv_label_set_text(s_clock_time, time_str);

        static const char *days[] = {
            "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
        };
        static const char *months[] = {
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
        };
        char date_str[64];
        snprintf(date_str, sizeof(date_str), "%s %d %s %04d • %s",
                 days[tm_info.tm_wday % 7], tm_info.tm_mday,
                 months[tm_info.tm_mon % 12], tm_info.tm_year + 1900,
                 p->status_tag);
        lv_label_set_text(s_clock_date, date_str);
        lv_obj_set_style_text_color(s_clock_date, lv_color_hex(p->text_color), 0);
    }

    /* 3. Apply Active Emotion Colors */
    lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(p->eye_color), 0);
    lv_obj_set_style_bg_color(s_eye_r, lv_color_hex(p->eye_color), 0);
    if (s_brow_l) lv_obj_set_style_bg_color(s_brow_l, lv_color_hex(p->eye_color), 0);
    if (s_brow_r) lv_obj_set_style_bg_color(s_brow_r, lv_color_hex(p->eye_color), 0);
    lv_obj_set_style_arc_color(s_mouth, lv_color_hex(p->eye_color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_blush_l, lv_color_hex(p->blush_color), 0);
    lv_obj_set_style_bg_color(s_blush_r, lv_color_hex(p->blush_color), 0);
    lv_obj_set_style_bg_opa(s_blush_l, p->blush_opa, 0);
    lv_obj_set_style_bg_opa(s_blush_r, p->blush_opa, 0);

    /* 4. Update Load Metrics */
    char buf[32];

    // CPU
    snprintf(buf, sizeof(buf), "%d %%", t.cpu_pct);
    lv_label_set_text(s_cpu_val, buf);
    lv_bar_set_value(s_cpu_bar, t.cpu_pct, LV_ANIM_OFF);

    // RAM (label in GB; bar keeps MB range)
    snprintf(buf, sizeof(buf), "%.1f / %.1f GB",
             (t.ram_used_mb / 1024.0f), (t.ram_total_mb / 1024.0f));
    lv_label_set_text(s_ram_val, buf);
    int ram_max = (t.ram_total_mb > 0) ? t.ram_total_mb : 100;
    lv_bar_set_range(s_ram_bar, 0, ram_max);
    lv_bar_set_value(s_ram_bar, t.ram_used_mb, LV_ANIM_OFF);

    // GPU
    snprintf(buf, sizeof(buf), "%d %%", t.gpu_pct);
    lv_label_set_text(s_gpu_val, buf);
    lv_bar_set_value(s_gpu_bar, t.gpu_pct, LV_ANIM_OFF);

    // GPU Temp
    snprintf(buf, sizeof(buf), "%d C", t.gpu_temp_c);
    lv_label_set_text(s_temp_val, buf);
    lv_bar_set_range(s_temp_bar, 0, 120);
    lv_bar_set_value(s_temp_bar, t.gpu_temp_c, LV_ANIM_OFF);

    // VRAM (label in GB; bar keeps MB range)
    snprintf(buf, sizeof(buf), "%.1f / %.1f GB",
             (t.vram_used_mb / 1024.0f), (t.vram_total_mb / 1024.0f));
    lv_label_set_text(s_vram_val, buf);
    int vram_max = (t.vram_total_mb > 0) ? t.vram_total_mb : 100;
    lv_bar_set_range(s_vram_bar, 0, vram_max);
    lv_bar_set_value(s_vram_bar, t.vram_used_mb, LV_ANIM_OFF);
}

/* Master UI Timer Callback (40ms interval = 25 FPS) */
static void ui_timer_cb(lv_timer_t *timer)
{
    /* Animate face every tick */
    face_anim_tick();

    /* Update telemetry and clock every 5 ticks (~200ms) */
    static int s_telemetry_tick = 0;
    if (++s_telemetry_tick >= 5) {
        s_telemetry_tick = 0;
        telemetry_update_tick();
    }
}

void ui_init(void)
{
    /* Build idle and AI screens */
    create_idle_screen();
    create_ai_screen();

    /* Start on Idle Screen */
    lv_scr_load(s_scr_idle);

    /* Create animation & refresh timer (runs in LVGL context) */
    lv_timer_create(ui_timer_cb, 40, NULL);

    ESP_LOGI(TAG, "Desk Pet UI initialized successfully");
}
