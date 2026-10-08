#include "ui.h"
#include "telemetry.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "quotes.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/display.h"

static const char *TAG = "ui";

#define SCREEN_W  (320)
#define SCREEN_H  (240)

#define LEFT_EYE_X   (72)
#define RIGHT_EYE_X  (202)
#define BASE_EYE_Y   (110)
#define BASE_EYE_W   (46)
#define BASE_EYE_H   (62)
#define BASE_EYE_R   (20)

#define MOUTH_X      (146)
#define BASE_MOUTH_Y (182)

/* Screen objects */
static lv_obj_t *s_scr_idle = NULL;
static lv_obj_t *s_scr_ai   = NULL;
static lv_obj_t *s_scr_quotes = NULL;
static lv_timer_t *s_ui_timer = NULL;
static lv_obj_t *s_quote_text = NULL;
static lv_obj_t *s_quote_category = NULL;

static int s_quote_index = -1;

static void ui_load_screen(lv_obj_t *screen)
{
    lv_scr_load(screen);
    if (s_ui_timer) {
        lv_timer_set_period(s_ui_timer, screen == s_scr_idle ? 40 : 200);
    }
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

/* Clock widgets on Idle Screen */
static lv_obj_t *s_clock_time = NULL;
static lv_obj_t *s_clock_date = NULL;

/* Shared Face widgets on Idle Screen */
static lv_obj_t *s_brow_l = NULL;
static lv_obj_t *s_brow_r = NULL;
static lv_obj_t *s_eye_l = NULL;
static lv_obj_t *s_eye_r = NULL;
static lv_obj_t *s_eye_l_hl1 = NULL;
static lv_obj_t *s_eye_l_hl2 = NULL;
static lv_obj_t *s_eye_r_hl1 = NULL;
static lv_obj_t *s_eye_r_hl2 = NULL;

/* Crescent smile eyes for Love reaction */
static lv_obj_t *s_eye_l_smile = NULL;
static lv_obj_t *s_eye_r_smile = NULL;

/* Clean single-layer soft blush cheeks */
static lv_obj_t *s_blush_l = NULL;
static lv_obj_t *s_blush_r = NULL;

/* Original Cyber Pet mouth */
static lv_obj_t *s_mouth = NULL;

/* --------------------------------------------------------------------------
 * Appearance Layer Containers (Clean, Minimal, Non-Overlapping)
 * -------------------------------------------------------------------------- */
static lv_obj_t *s_cat_bg     = NULL;
static lv_obj_t *s_cat_fg     = NULL;

/* Cat specific widgets */
static lv_obj_t *s_cat_ear_l      = NULL;
static lv_obj_t *s_cat_ear_r      = NULL;
static lv_obj_t *s_cat_whisk_l[2] = {NULL};
static lv_obj_t *s_cat_whisk_r[2] = {NULL};
static lv_obj_t *s_cat_nose       = NULL;
static lv_obj_t *s_cat_mouth_l    = NULL;
static lv_obj_t *s_cat_mouth_r    = NULL;

/* --------------------------------------------------------------------------
 * Clean Geometry Points for Polylines
 * -------------------------------------------------------------------------- */
/* Cat */
static const lv_point_t s_cat_ear_l_pts[]    = {{50, 94}, {70, 46}, {94, 88}, {50, 94}};
static const lv_point_t s_cat_ear_r_pts[]    = {{226, 88}, {250, 46}, {270, 94}, {226, 88}};
static const lv_point_t s_cat_whisk_l1_pts[] = {{38, 158}, {10, 152}};
static const lv_point_t s_cat_whisk_l2_pts[] = {{38, 170}, {10, 176}};
static const lv_point_t s_cat_whisk_r1_pts[] = {{282, 158}, {310, 152}};
static const lv_point_t s_cat_whisk_r2_pts[] = {{282, 170}, {310, 176}};

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

/* --------------------------------------------------------------------------
 * Emotion Palette and State
 * -------------------------------------------------------------------------- */
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
        .blush_opa   = 150,
        .text_color  = 0x70A5FF, // Sky Blue
        .status_tag  = "HAPPY",
    },
    [PET_EMOTION_FOCUS] = {
        .eye_color   = 0xFF9F0A, // Warm Amber / Golden Glow
        .blush_color = 0xFF8500, // Warm Peach Blush
        .blush_opa   = 160,
        .text_color  = 0xFFA520, // Amber
        .status_tag  = "FOCUS",
    },
    [PET_EMOTION_RELAX] = {
        .eye_color   = 0x30D158, // Fresh Mint / Emerald
        .blush_color = 0x25C474, // Soft Mint Pink
        .blush_opa   = 120,
        .text_color  = 0x34C759, // Green
        .status_tag  = "CHILL",
    },
    [PET_EMOTION_LOVE] = {
        .eye_color   = 0xFF375F, // Radiant Blossom Pink
        .blush_color = 0xFF2D55, // Deep Magenta Blush
        .blush_opa   = 220,
        .text_color  = 0xFF6482, // Rosy
        .status_tag  = "LOVE <3",
    },
    [PET_EMOTION_HOT] = {
        .eye_color   = 0xFF453A, // Fiery Crimson Red
        .blush_color = 0xFF3B30, // Burning Coral Flush
        .blush_opa   = 200,
        .text_color  = 0xFF5F56, // Crimson
        .status_tag  = "HOT!",
    },
};

static pet_emotion_t s_current_emotion = PET_EMOTION_HAPPY;
static int64_t s_love_until_us = 0;

/* --------------------------------------------------------------------------
 * Pet Appearance State
 * -------------------------------------------------------------------------- */
static pet_type_t s_current_pet_type = PET_TYPE_ORIGINAL;

static const char *s_pet_type_names[PET_TYPE_COUNT] = {
    [PET_TYPE_ORIGINAL] = "DESK PET",
    [PET_TYPE_CAT]      = "CAT",
};

static void nvs_load_pet_type(void)
{
    nvs_handle_t nvs_h;
    if (nvs_open("deskpet", NVS_READONLY, &nvs_h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(nvs_h, "pet_type", &val) == ESP_OK && val < PET_TYPE_COUNT) {
            s_current_pet_type = (pet_type_t)val;
            ESP_LOGI(TAG, "Loaded pet appearance from NVS: %s", s_pet_type_names[s_current_pet_type]);
        }
        nvs_close(nvs_h);
    }
}

static void nvs_save_pet_type(void)
{
    nvs_handle_t nvs_h;
    if (nvs_open("deskpet", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_u8(nvs_h, "pet_type", (uint8_t)s_current_pet_type);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
        ESP_LOGI(TAG, "Saved pet appearance to NVS: %s", s_pet_type_names[s_current_pet_type]);
    }
}

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

pet_type_t ui_get_pet_type(void)
{
    return s_current_pet_type;
}

const char *ui_get_pet_type_name(pet_type_t type)
{
    if (type < PET_TYPE_COUNT) {
        return s_pet_type_names[type];
    }
    return "UNKNOWN";
}

static void apply_pet_appearance_visibility(void)
{
    /* Hide character overlay containers first */
    if (s_cat_bg) lv_obj_add_flag(s_cat_bg, LV_OBJ_FLAG_HIDDEN);
    if (s_cat_fg) lv_obj_add_flag(s_cat_fg, LV_OBJ_FLAG_HIDDEN);

    /* Show only the active pet's components */
    switch (s_current_pet_type) {
    case PET_TYPE_ORIGINAL:
        if (s_brow_l) lv_obj_clear_flag(s_brow_l, LV_OBJ_FLAG_HIDDEN);
        if (s_brow_r) lv_obj_clear_flag(s_brow_r, LV_OBJ_FLAG_HIDDEN);
        if (s_mouth)  lv_obj_clear_flag(s_mouth,  LV_OBJ_FLAG_HIDDEN);
        break;

    case PET_TYPE_CAT:
        if (s_brow_l) lv_obj_clear_flag(s_brow_l, LV_OBJ_FLAG_HIDDEN);
        if (s_brow_r) lv_obj_clear_flag(s_brow_r, LV_OBJ_FLAG_HIDDEN);
        if (s_mouth)  lv_obj_add_flag(s_mouth,  LV_OBJ_FLAG_HIDDEN);
        if (s_cat_bg) lv_obj_clear_flag(s_cat_bg, LV_OBJ_FLAG_HIDDEN);
        if (s_cat_fg) lv_obj_clear_flag(s_cat_fg, LV_OBJ_FLAG_HIDDEN);
        break;

    default:
        break;
    }
}

void ui_set_pet_type(pet_type_t type)
{
    if (type < PET_TYPE_COUNT) {
        s_current_pet_type = type;
        apply_pet_appearance_visibility();
        nvs_save_pet_type();
    }
}

void ui_cycle_appearance(void)
{
    s_current_pet_type = (pet_type_t)((s_current_pet_type + 1) % PET_TYPE_COUNT);
    apply_pet_appearance_visibility();

    /* Make sure pet screen is active when switching */
    lv_disp_t *disp = lv_disp_get_default();
    if (disp && lv_disp_get_scr_act(disp) != s_scr_idle) {
        ui_load_screen(s_scr_idle);
    }

    /* Joyful bounce upon transformation */
    s_love_until_us = esp_timer_get_time() + 3000000LL; // ~3s of happy Love reaction

    nvs_save_pet_type();

    ESP_LOGI(TAG, "Pet appearance switched to: %s", s_pet_type_names[s_current_pet_type]);
    printf("EVENT,PET_APPEARANCE,%s\n", s_pet_type_names[s_current_pet_type]);
    fflush(stdout);
}

static bool s_voice_listening = false;
static char s_voice_feedback_buf[64] = {0};
static int64_t s_voice_feedback_until_us = 0;

void ui_set_voice_listening(bool listening)
{
    s_voice_listening = listening;
    if (s_clock_date) {
        if (listening) {
            set_label_text_if_changed(s_clock_date, "• LISTENING...");
            lv_obj_set_style_text_color(s_clock_date, lv_color_hex(0x00E5FF), 0);
        }
    }
}

void ui_trigger_voice_reaction(pet_emotion_t emotion, const char *feedback_text)
{
    s_current_emotion = emotion;
    s_love_until_us = esp_timer_get_time() + 3000000LL; // ~3 seconds reaction
    s_voice_listening = false;

    /* If on AI status screen, switch back to face */
    lv_disp_t *disp = lv_disp_get_default();
    if (disp && s_scr_idle && lv_disp_get_scr_act(disp) != s_scr_idle) {
        ui_load_screen(s_scr_idle);
    }

    if (feedback_text && feedback_text[0] != '\0') {
        strncpy(s_voice_feedback_buf, feedback_text, sizeof(s_voice_feedback_buf) - 1);
        s_voice_feedback_buf[sizeof(s_voice_feedback_buf) - 1] = '\0';
        s_voice_feedback_until_us = esp_timer_get_time() + 3000000LL; // ~3 seconds
        if (s_clock_date) {
            set_label_text_if_changed(s_clock_date, s_voice_feedback_buf);
            lv_obj_set_style_text_color(s_clock_date, lv_color_hex(0xFF375F), 0);
        }
    }
}

void ui_show_ai_mode(bool show)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return;

    if (show && s_scr_ai) {
        if (lv_disp_get_scr_act(disp) != s_scr_ai) {
            ui_load_screen(s_scr_ai);
        }
    } else if (!show && s_scr_idle) {
        if (lv_disp_get_scr_act(disp) != s_scr_idle) {
            ui_load_screen(s_scr_idle);
            s_love_until_us = esp_timer_get_time() + 2400000LL;
        }
    }
}

static int s_current_brightness = 50;
static int64_t s_last_night_touch_us = 0;

/* Top-Left: open a quote, or choose a different random quote. */
static void quad_top_left_cb(lv_event_t *e)
{
    (void)e;
    if (deskpet_quote_count > 0) {
        /* Pick among all entries except the currently displayed one. */
        int choices = deskpet_quote_count - (s_quote_index >= 0 && deskpet_quote_count > 1 ? 1 : 0);
        int next = (int)(esp_random() % (uint32_t)choices);
        if (deskpet_quote_count > 1 && s_quote_index >= 0 && next >= s_quote_index) {
            next++;
        }
        s_quote_index = next;
        lv_label_set_text_static(s_quote_text, deskpet_quotes[next].text);
        lv_label_set_text_static(s_quote_category, deskpet_quotes[next].category);
    }
    ui_load_screen(s_scr_quotes);
    ESP_LOGI(TAG, "Touch [Top-Left]: Quote %d", s_quote_index + 1);
}

/* Top-Right: Monitor Brightness 0% / Night Mode <-> 30% / Day Mode (ESP 0% <-> 50%) */
static void quad_top_right_cb(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_night_touch_us < 600000LL) {
        return; // 600ms debounce
    }
    s_last_night_touch_us = now;

    if (s_current_brightness > 0) {
        s_current_brightness = 0;
        bsp_display_brightness_set(0);
        ESP_LOGI(TAG, "Touch [Top-Right]: Night Mode ON (ESP -> 0%%)");
    } else {
        s_current_brightness = 50;
        bsp_display_brightness_set(50);
        s_love_until_us = esp_timer_get_time() + 3000000LL; // Sweet reaction on wake
        ESP_LOGI(TAG, "Touch [Top-Right]: Night Mode OFF (ESP -> 50%%)");
    }
    printf("CMD,NIGHT_TOGGLE\n");
    fflush(stdout);
}

/* Bottom Row: Switch to System Status screen */
static void bottom_row_status_cb(lv_event_t *e)
{
    ui_load_screen(s_scr_ai);
    ESP_LOGI(TAG, "Touch [Bottom Row]: Switched to SYSTEM STATUS screen");
}

/* Return from AI Mode Screen to Pet Face */
static void ai_screen_back_cb(lv_event_t *e)
{
    ui_load_screen(s_scr_idle);
    s_love_until_us = esp_timer_get_time() + 3000000LL; // ~3s of happy Love react when returning to pet
    ESP_LOGI(TAG, "Touch: Switched to PET screen");
}

/* Public: toggle AI Mode (called from capacitive button callbacks). */
void ui_toggle_ai_mode(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (disp == NULL) {
        return;
    }
    lv_obj_t *act = lv_disp_get_scr_act(disp);
    if (act == s_scr_idle) {
        ui_load_screen(s_scr_ai);
        ESP_LOGI(TAG, "Button: switched to SYSTEM STATUS screen");
    } else {
        ui_load_screen(s_scr_idle);
        s_love_until_us = esp_timer_get_time() + 3000000LL; // ~3s of happy Love react
        ESP_LOGI(TAG, "Button: switched to PET screen");
    }
}

/* Helper to attach transparent touch zone */
static lv_obj_t* add_touch_zone(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_event_cb_t cb)
{
    lv_obj_t *zone = lv_obj_create(parent);
    lv_obj_set_pos(zone, x, y);
    lv_obj_set_size(zone, w, h);
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(zone, 0, 0);
    lv_obj_clear_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(zone, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(zone, LV_DIR_NONE);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, cb, LV_EVENT_CLICKED, NULL);
    return zone;
}

/* Helper to create a transparent overlay container with bounded height to prevent scrollbar */
static lv_obj_t* create_pet_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, SCREEN_W, 214);
    lv_obj_set_pos(layer, 0, 2);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(layer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(layer, LV_DIR_NONE);
    return layer;
}

/* Helper to create a polyline with rounded joints */
static lv_obj_t* create_line(lv_obj_t *parent, const lv_point_t *pts, uint16_t num, lv_color_t color, lv_coord_t width, lv_opa_t opa)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, pts, num);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    if (opa != LV_OPA_COVER) {
        lv_obj_set_style_line_opa(line, opa, 0);
    }
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return line;
}

/* Helper to create an arc mouth */
static lv_obj_t* create_arc_mouth(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t color)
{
    lv_obj_t *mouth = lv_arc_create(parent);
    lv_obj_set_size(mouth, w, h);
    lv_obj_set_pos(mouth, x, y);
    lv_arc_set_angles(mouth, 20, 160);
    lv_arc_set_bg_angles(mouth, 20, 160);
    lv_obj_remove_style(mouth, NULL, LV_PART_KNOB);
    lv_obj_set_style_opa(mouth, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_width(mouth, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(mouth, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mouth, color, LV_PART_INDICATOR);
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return mouth;
}

/* Helper to create upward-curving crescent smile eyes (⌒) */
static lv_obj_t* create_smile_eye(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_color_t color)
{
    lv_obj_t *eye = lv_arc_create(parent);
    lv_obj_set_size(eye, 46, 36);
    lv_obj_set_pos(eye, x, y);
    lv_arc_set_angles(eye, 205, 335);    // Upward-curving smile arch
    lv_arc_set_bg_angles(eye, 205, 335);
    lv_obj_remove_style(eye, NULL, LV_PART_KNOB);
    lv_obj_set_style_opa(eye, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_width(eye, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(eye, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(eye, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(eye, color, LV_PART_INDICATOR);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(eye, LV_OBJ_FLAG_HIDDEN); // Hidden by default until Love reaction
    return eye;
}

/* Create Idle Screen with Clock + Clean, Spacious Character Layers */
static void create_idle_screen(void)
{
    s_scr_idle = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_idle, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr_idle, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr_idle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_scr_idle, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_scr_idle, LV_DIR_NONE);

    /* ----------------------------------------------------------------------
     * 1. Background Character Layers (Clean, non-cluttered silhouettes)
     * ---------------------------------------------------------------------- */

    /* Cat Background: Clean Outer Ears */
    s_cat_bg = create_pet_layer(s_scr_idle);
    s_cat_ear_l = create_line(s_cat_bg, s_cat_ear_l_pts, 4, lv_color_hex(0x00E5FF), 4, LV_OPA_COVER);
    s_cat_ear_r = create_line(s_cat_bg, s_cat_ear_r_pts, 4, lv_color_hex(0x00E5FF), 4, LV_OPA_COVER);

    /* ----------------------------------------------------------------------
     * 2. Shared Face Elements (Clean Eyes, Brows, Soft Cheeks)
     * ---------------------------------------------------------------------- */

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

    /* Left Eye (Clean Pill with 2 Luminous Highlights) */
    s_eye_l = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_eye_l, BASE_EYE_W, BASE_EYE_H);
    lv_obj_set_pos(s_eye_l, LEFT_EYE_X, BASE_EYE_Y);
    lv_obj_set_style_radius(s_eye_l, BASE_EYE_R, 0);
    lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_bg_opa(s_eye_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eye_l, 0, 0);
    lv_obj_set_style_pad_all(s_eye_l, 0, 0);
    lv_obj_clear_flag(s_eye_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_eye_l_hl1 = lv_obj_create(s_eye_l);
    lv_obj_set_size(s_eye_l_hl1, 14, 14);
    lv_obj_set_pos(s_eye_l_hl1, 6, 6);
    lv_obj_set_style_radius(s_eye_l_hl1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_l_hl1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_eye_l_hl1, 0, 0);
    lv_obj_clear_flag(s_eye_l_hl1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_eye_l_hl2 = lv_obj_create(s_eye_l);
    lv_obj_set_size(s_eye_l_hl2, 7, 7);
    lv_obj_set_pos(s_eye_l_hl2, 26, 42);
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

    s_eye_r_hl1 = lv_obj_create(s_eye_r);
    lv_obj_set_size(s_eye_r_hl1, 14, 14);
    lv_obj_set_pos(s_eye_r_hl1, 6, 6);
    lv_obj_set_style_radius(s_eye_r_hl1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_r_hl1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_eye_r_hl1, 0, 0);
    lv_obj_clear_flag(s_eye_r_hl1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_eye_r_hl2 = lv_obj_create(s_eye_r);
    lv_obj_set_size(s_eye_r_hl2, 7, 7);
    lv_obj_set_pos(s_eye_r_hl2, 26, 42);
    lv_obj_set_style_radius(s_eye_r_hl2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_r_hl2, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_eye_r_hl2, 190, 0);
    lv_obj_set_style_border_width(s_eye_r_hl2, 0, 0);
    lv_obj_clear_flag(s_eye_r_hl2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Crescent Smile Eyes: Joyful upward curving smile arches (⌒) */
    s_eye_l_smile = create_smile_eye(s_scr_idle, LEFT_EYE_X, BASE_EYE_Y + 12, lv_color_hex(0x00E5FF));
    s_eye_r_smile = create_smile_eye(s_scr_idle, RIGHT_EYE_X, BASE_EYE_Y + 12, lv_color_hex(0x00E5FF));

    /* Clean, uncluttered soft blush cheeks (spaced with clear breathing room) */
    s_blush_l = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_blush_l, 30, 14);
    lv_obj_set_pos(s_blush_l, 56, 164);
    lv_obj_set_style_radius(s_blush_l, 7, 0);
    lv_obj_set_style_bg_color(s_blush_l, lv_color_hex(0xFF6B8B), 0);
    lv_obj_set_style_bg_opa(s_blush_l, 150, 0);
    lv_obj_set_style_border_width(s_blush_l, 0, 0);
    lv_obj_clear_flag(s_blush_l, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_blush_r = lv_obj_create(s_scr_idle);
    lv_obj_set_size(s_blush_r, 30, 14);
    lv_obj_set_pos(s_blush_r, 234, 164);
    lv_obj_set_style_radius(s_blush_r, 7, 0);
    lv_obj_set_style_bg_color(s_blush_r, lv_color_hex(0xFF6B8B), 0);
    lv_obj_set_style_bg_opa(s_blush_r, 150, 0);
    lv_obj_set_style_border_width(s_blush_r, 0, 0);
    lv_obj_clear_flag(s_blush_r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Single clean arc mouth for Original Cyber Pet */
    s_mouth = create_arc_mouth(s_scr_idle, MOUTH_X, BASE_MOUTH_Y, 28, 18, lv_color_hex(0x00E5FF));

    /* ----------------------------------------------------------------------
     * 3. Foreground Character Layers (Minimal, Crisp, No Stacking)
     * ---------------------------------------------------------------------- */

    /* Cat Foreground: 2 Clean Whiskers per side, Cute Nose, Split :3 Mouth */
    s_cat_fg = create_pet_layer(s_scr_idle);

    s_cat_whisk_l[0] = create_line(s_cat_fg, s_cat_whisk_l1_pts, 2, lv_color_hex(0xD8EEFD), 2, 200);
    s_cat_whisk_l[1] = create_line(s_cat_fg, s_cat_whisk_l2_pts, 2, lv_color_hex(0xD8EEFD), 2, 200);
    s_cat_whisk_r[0] = create_line(s_cat_fg, s_cat_whisk_r1_pts, 2, lv_color_hex(0xD8EEFD), 2, 200);
    s_cat_whisk_r[1] = create_line(s_cat_fg, s_cat_whisk_r2_pts, 2, lv_color_hex(0xD8EEFD), 2, 200);

    // Cute clean cat nose
    s_cat_nose = lv_obj_create(s_cat_fg);
    lv_obj_set_size(s_cat_nose, 8, 5);
    lv_obj_set_pos(s_cat_nose, 156, 170);
    lv_obj_set_style_radius(s_cat_nose, 2, 0);
    lv_obj_set_style_bg_color(s_cat_nose, lv_color_hex(0xFF6B8B), 0);
    lv_obj_set_style_bg_opa(s_cat_nose, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cat_nose, 0, 0);
    lv_obj_clear_flag(s_cat_nose, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Split :3 mouth arcs
    s_cat_mouth_l = create_arc_mouth(s_cat_fg, 145, 173, 15, 11, lv_color_hex(0x00E5FF));
    s_cat_mouth_r = create_arc_mouth(s_cat_fg, 160, 173, 15, 11, lv_color_hex(0x00E5FF));

    /* ----------------------------------------------------------------------
     * 4. Clock and Date Labels
     * ---------------------------------------------------------------------- */
    s_clock_time = lv_label_create(s_scr_idle);
    lv_obj_set_style_text_font(s_clock_time, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_clock_time, lv_color_hex(0xFFFFFF), 0);
    set_label_text_if_changed(s_clock_time, "--:--");
    lv_obj_align(s_clock_time, LV_ALIGN_TOP_MID, 0, 6);

    s_clock_date = lv_label_create(s_scr_idle);
    lv_obj_set_style_text_font(s_clock_date, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_clock_date, lv_color_hex(0x70A5FF), 0);
    set_label_text_if_changed(s_clock_date, "DESK PET READY");
    lv_obj_align(s_clock_date, LV_ALIGN_TOP_MID, 0, 50);

    /* ----------------------------------------------------------------------
     * 5. Interactive Touch Hotzones
     * ---------------------------------------------------------------------- */
    add_touch_zone(s_scr_idle, 0,   0,   SCREEN_W / 2, SCREEN_H / 2, quad_top_left_cb);
    add_touch_zone(s_scr_idle, 160, 0,   SCREEN_W / 2, SCREEN_H / 2, quad_top_right_cb);
    add_touch_zone(s_scr_idle, 0,   120, SCREEN_W,     SCREEN_H / 2, bottom_row_status_cb);

    /* Apply initial visibility for selected pet */
    apply_pet_appearance_visibility();
}

/* Helper to build a metric row on the AI Mode Screen */
static void create_metric_row(lv_obj_t *parent, int y, const char *title, lv_color_t color,
                              lv_obj_t **val_label_out, lv_obj_t **bar_out)
{
    lv_obj_t *lbl_title = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_title, color, 0);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_pos(lbl_title, 16, y);

    lv_obj_t *lbl_val = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_val, "--");
    lv_obj_align(lbl_val, LV_ALIGN_TOP_RIGHT, -16, y);
    *val_label_out = lbl_val;

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

/* Create AI Mode Screen with State Chip + 5 Metric Rows */
static void create_ai_screen(void)
{
    s_scr_ai = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_ai, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scr_ai, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr_ai, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_scr_ai, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_scr_ai, LV_DIR_NONE);

    lv_obj_t *title = lv_label_create(s_scr_ai);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xEBEBF5), 0);
    lv_label_set_text(title, "SYSTEM STATUS");
    lv_obj_set_pos(title, 16, 8);

    create_metric_row(s_scr_ai, 38,  "CPU",  lv_color_hex(0x00E5FF), &s_cpu_val,  &s_cpu_bar);
    create_metric_row(s_scr_ai, 76,  "RAM",  lv_color_hex(0x30D158), &s_ram_val,  &s_ram_bar);
    create_metric_row(s_scr_ai, 114, "GPU",  lv_color_hex(0xFF9F0A), &s_gpu_val,  &s_gpu_bar);
    create_metric_row(s_scr_ai, 152, "VRAM", lv_color_hex(0xBF5AF2), &s_vram_val, &s_vram_bar);
    create_metric_row(s_scr_ai, 190, "TEMP", lv_color_hex(0xFF453A), &s_temp_val, &s_temp_bar);

    add_touch_zone(s_scr_ai, 0, 0, SCREEN_W, SCREEN_H, ai_screen_back_cb);
}

static void create_quotes_screen(void)
{
    s_scr_quotes = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_quotes, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_scr_quotes, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr_quotes, LV_OBJ_FLAG_SCROLLABLE);

    s_quote_category = lv_label_create(s_scr_quotes);
    lv_obj_set_pos(s_quote_category, 16, 12);
    lv_obj_set_width(s_quote_category, 288);
    lv_label_set_long_mode(s_quote_category, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_quote_category, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_quote_category, lv_color_hex(0x30D158), 0);
    lv_label_set_text(s_quote_category, "QUOTES");

    s_quote_text = lv_label_create(s_scr_quotes);
    lv_obj_set_pos(s_quote_text, 16, 44);
    lv_obj_set_size(s_quote_text, 288, 150);
    lv_label_set_long_mode(s_quote_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_quote_text, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_quote_text, lv_color_hex(0xEBEBF5), 0);
    lv_obj_set_style_text_line_space(s_quote_text, 5, 0);
    lv_label_set_text(s_quote_text, "No quotes available.");

    /* The other zones only exit; they never toggle night mode or status. */
    add_touch_zone(s_scr_quotes, 0, 0, SCREEN_W / 2, SCREEN_H / 2, quad_top_left_cb);
    add_touch_zone(s_scr_quotes, SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H / 2, ai_screen_back_cb);
    add_touch_zone(s_scr_quotes, 0, SCREEN_H / 2, SCREEN_W, SCREEN_H / 2, ai_screen_back_cb);
}

/* Face animation state machine tick (runs every ~40ms = 25 FPS) */
static void face_anim_tick(void)
{
    s_anim_tick++;

    /* 1. Dynamic breathing bob adjusted by emotion (bounded to prevent overflow / scrollbars) */
    float breath_speed = 0.08f;
    float breath_amp = 1.8f;
    int base_eye_h = BASE_EYE_H;
    int base_eye_r = BASE_EYE_R;

    if (s_current_emotion == PET_EMOTION_RELAX) {
        breath_speed = 0.04f; // Calm, deep, slow breath
        breath_amp = 1.2f;
        base_eye_h = 32;     // Half-closed sleepy zen eyes
        base_eye_r = 12;
    } else if (s_current_emotion == PET_EMOTION_FOCUS) {
        breath_speed = 0.10f; // Alert, steady
        breath_amp = 1.5f;
        base_eye_h = 50;     // Focused squint
        base_eye_r = 16;
    } else if (s_current_emotion == PET_EMOTION_HOT) {
        breath_speed = 0.12f; // Fast, agitated breathing
        breath_amp = 2.0f;
        base_eye_h = 56;
        base_eye_r = 14;
    } else if (s_current_emotion == PET_EMOTION_LOVE) {
        breath_speed = 0.09f; // Bouncy happy bob
        breath_amp = 2.0f;
        base_eye_h = 62;
        base_eye_r = 22;
    }

    float breath = sinf(s_anim_tick * breath_speed);
    int bob_y = (int)roundf(breath_amp * breath);
    if (bob_y < -2) bob_y = -2;
    if (bob_y > 2)  bob_y = 2;

    /* 2. Looking around shift */
    s_look_timer--;
    if (s_look_timer <= 0) {
        s_look_timer = 75 + (rand() % 75); // Every 3 - 6 seconds
        int r = rand() % 10;
        if (s_current_emotion == PET_EMOTION_FOCUS) {
            s_target_look_x = 0;
        } else if (r < 6) {
            s_target_look_x = 0;
        } else if (r < 8) {
            s_target_look_x = -4;
        } else {
            s_target_look_x = 4;
        }
    }
    s_look_offset_x = (int)roundf(s_look_offset_x * 0.85f + s_target_look_x * 0.15f);
    if (s_look_offset_x < -4) s_look_offset_x = -4;
    if (s_look_offset_x > 4)  s_look_offset_x = 4;

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
            if ((rand() % 5) == 0) {
                s_blink_timer = 5; // double blink
            } else {
                s_blink_timer = 70 + (rand() % 70); // 3 - 5.5s
            }
        }
        break;
    }

    /* 4. Eye Rendering: Smile Eyes in Love mode vs Clean Pill Eyes */
    if (s_current_emotion == PET_EMOTION_LOVE) {
        lv_obj_add_flag(s_eye_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_eye_l_smile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_eye_l_smile, LEFT_EYE_X + s_look_offset_x, BASE_EYE_Y + 12 + bob_y);

        lv_obj_add_flag(s_eye_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_eye_r_smile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_eye_r_smile, RIGHT_EYE_X + s_look_offset_x, BASE_EYE_Y + 12 + bob_y);
    } else {
        lv_obj_clear_flag(s_eye_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye_l_smile, LV_OBJ_FLAG_HIDDEN);
        int eye_l_y = BASE_EYE_Y + bob_y + (BASE_EYE_H - current_h) / 2;
        lv_obj_set_size(s_eye_l, BASE_EYE_W, current_h);
        lv_obj_set_style_radius(s_eye_l, current_r, 0);
        lv_obj_set_pos(s_eye_l, LEFT_EYE_X + s_look_offset_x, eye_l_y);

        lv_obj_clear_flag(s_eye_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye_r_smile, LV_OBJ_FLAG_HIDDEN);
        int eye_r_y = BASE_EYE_Y + bob_y + (BASE_EYE_H - current_h) / 2;
        lv_obj_set_size(s_eye_r, BASE_EYE_W, current_h);
        lv_obj_set_style_radius(s_eye_r, current_r, 0);
        lv_obj_set_pos(s_eye_r, RIGHT_EYE_X + s_look_offset_x, eye_r_y);
    }

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

    /* Cheeks: Soft single-layer oval with ample breathing room */
    if (s_blush_l) lv_obj_set_pos(s_blush_l, 56 + s_look_offset_x / 2, 164 + bob_y);
    if (s_blush_r) lv_obj_set_pos(s_blush_r, 234 + s_look_offset_x / 2, 164 + bob_y);

    /* Synchronous breathing bob for layers: keep base Y at 2 so 2 + bob_y is always >= 0 */
    int layer_y = 2 + bob_y;
    if (s_cat_bg) lv_obj_set_pos(s_cat_bg, 0, layer_y);
    if (s_cat_fg) lv_obj_set_pos(s_cat_fg, 0, layer_y);

    /* Mouth vertical position and shape */
    if (s_mouth) {
        lv_obj_set_pos(s_mouth, MOUTH_X, BASE_MOUTH_Y + bob_y);
        if (s_current_emotion == PET_EMOTION_HOT) {
            lv_arc_set_angles(s_mouth, 205, 335); // angry frown (>_<)
        } else if (s_current_emotion == PET_EMOTION_LOVE) {
            lv_arc_set_angles(s_mouth, 15, 165);  // happy wide smile (^o^)
        } else {
            lv_arc_set_angles(s_mouth, 25, 155);  // gentle smile
        }
    }

    /* Cat :3 mouth shape */
    if (s_current_pet_type == PET_TYPE_CAT) {
        if (s_current_emotion == PET_EMOTION_HOT) {
            if (s_cat_mouth_l) lv_arc_set_angles(s_cat_mouth_l, 205, 335);
            if (s_cat_mouth_r) lv_arc_set_angles(s_cat_mouth_r, 205, 335);
        } else if (s_current_emotion == PET_EMOTION_LOVE) {
            if (s_cat_mouth_l) lv_arc_set_angles(s_cat_mouth_l, 10, 170);
            if (s_cat_mouth_r) lv_arc_set_angles(s_cat_mouth_r, 10, 170);
        } else {
            if (s_cat_mouth_l) lv_arc_set_angles(s_cat_mouth_l, 20, 160);
            if (s_cat_mouth_r) lv_arc_set_angles(s_cat_mouth_r, 20, 160);
        }
    }
}

/* Update Telemetry on AI Screen + Clock on Idle Screen */
static void telemetry_update_tick(void)
{
    static pet_telemetry_t t;
    static int64_t next_sample_us = 0;
    static lv_obj_t *previous_screen = NULL;
    static pet_emotion_t painted_emotion = PET_EMOTION_COUNT;
    static int previous_subtitle = -1;
    int64_t now = esp_timer_get_time();
    bool fresh_sample = now >= next_sample_us;
    if (fresh_sample) {
        t = telemetry_get();
        next_sample_us = now + 1000000LL;
    }
    lv_obj_t *screen = lv_scr_act();
    bool screen_changed = screen != previous_screen;
    previous_screen = screen;

    /* 1. Evaluate Emotion State Machine (Tabbie-inspired) */
    if (now < s_love_until_us) {
        s_current_emotion = PET_EMOTION_LOVE;
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
    bool palette_changed = painted_emotion != s_current_emotion;
    int subtitle = s_voice_listening ? 1 : (now < s_voice_feedback_until_us ? 2 : 0);
    bool subtitle_changed = subtitle != previous_subtitle;
    previous_subtitle = subtitle;

    /* 2. Update Clock & Date on Idle Screen */
    if (t.has_data && screen == s_scr_idle &&
        (fresh_sample || screen_changed || palette_changed || subtitle_changed)) {
        time_t local_sec = (time_t)(t.epoch_utc + TIMEZONE_OFFSET_SEC);
        struct tm tm_info;
        gmtime_r(&local_sec, &tm_info);

        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
        set_label_text_if_changed(s_clock_time, time_str);

        static const char *days[] = {
            "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"
        };
        static const char *months[] = {
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
        };
        if (s_voice_listening) {
            set_label_text_if_changed(s_clock_date, "• LISTENING...");
            lv_obj_set_style_text_color(s_clock_date, lv_color_hex(0x00E5FF), 0);
        } else if (now < s_voice_feedback_until_us) {
            set_label_text_if_changed(s_clock_date, s_voice_feedback_buf);
            lv_obj_set_style_text_color(s_clock_date, lv_color_hex(0xFF375F), 0);
        } else {
            char date_str[64];
            snprintf(date_str, sizeof(date_str), "%s %d %s %04d • %s",
                     days[tm_info.tm_wday % 7], tm_info.tm_mday,
                     months[tm_info.tm_mon % 12], tm_info.tm_year + 1900,
                     p->status_tag);
            set_label_text_if_changed(s_clock_date, date_str);
            lv_obj_set_style_text_color(s_clock_date, lv_color_hex(p->text_color), 0);
        }
    }

    /* Repaint styles only when the mood actually changes. */
    if (palette_changed && screen == s_scr_idle) {
        painted_emotion = s_current_emotion;
        /* 3. Apply Active Emotion Colors */
        lv_obj_set_style_bg_color(s_eye_l, lv_color_hex(p->eye_color), 0);
        lv_obj_set_style_bg_color(s_eye_r, lv_color_hex(p->eye_color), 0);
        lv_obj_set_style_arc_color(s_eye_l_smile, lv_color_hex(p->eye_color), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_eye_r_smile, lv_color_hex(p->eye_color), LV_PART_INDICATOR);

        if (s_brow_l) lv_obj_set_style_bg_color(s_brow_l, lv_color_hex(p->eye_color), 0);
        if (s_brow_r) lv_obj_set_style_bg_color(s_brow_r, lv_color_hex(p->eye_color), 0);
        if (s_mouth)  lv_obj_set_style_arc_color(s_mouth,  lv_color_hex(p->eye_color), LV_PART_INDICATOR);

        if (s_blush_l && s_blush_r) {
            lv_obj_set_style_bg_color(s_blush_l, lv_color_hex(p->blush_color), 0);
            lv_obj_set_style_bg_color(s_blush_r, lv_color_hex(p->blush_color), 0);
            lv_obj_set_style_bg_opa(s_blush_l, p->blush_opa, 0);
            lv_obj_set_style_bg_opa(s_blush_r, p->blush_opa, 0);
        }

        /* Cat reactive theme colors */
        if (s_cat_ear_l)   lv_obj_set_style_line_color(s_cat_ear_l, lv_color_hex(p->eye_color), 0);
        if (s_cat_ear_r)   lv_obj_set_style_line_color(s_cat_ear_r, lv_color_hex(p->eye_color), 0);
        if (s_cat_mouth_l) lv_obj_set_style_arc_color(s_cat_mouth_l, lv_color_hex(p->eye_color), LV_PART_INDICATOR);
        if (s_cat_mouth_r) lv_obj_set_style_arc_color(s_cat_mouth_r, lv_color_hex(p->eye_color), LV_PART_INDICATOR);

    }

    /* Keep hidden dashboard widgets untouched. */
    if (screen != s_scr_ai || (!fresh_sample && !screen_changed)) return;

    /* 4. Update Load Metrics on AI Dashboard */
    char buf[32];

    // CPU
    snprintf(buf, sizeof(buf), "%d %%", t.cpu_pct);
    set_label_text_if_changed(s_cpu_val, buf);
    lv_bar_set_value(s_cpu_bar, t.cpu_pct, LV_ANIM_OFF);

    // RAM
    snprintf(buf, sizeof(buf), "%.2f / %.2f GB",
             (t.ram_used_mb / 1024.0f), (t.ram_total_mb / 1024.0f));
    set_label_text_if_changed(s_ram_val, buf);
    int ram_max = (t.ram_total_mb > 0) ? (int)t.ram_total_mb : 100;
    lv_bar_set_range(s_ram_bar, 0, ram_max);
    lv_bar_set_value(s_ram_bar, (int)t.ram_used_mb, LV_ANIM_OFF);

    // GPU
    snprintf(buf, sizeof(buf), "%d %%", t.gpu_pct);
    set_label_text_if_changed(s_gpu_val, buf);
    lv_bar_set_value(s_gpu_bar, t.gpu_pct, LV_ANIM_OFF);

    // GPU Temp
    snprintf(buf, sizeof(buf), "%d C", t.gpu_temp_c);
    set_label_text_if_changed(s_temp_val, buf);
    lv_bar_set_range(s_temp_bar, 0, 120);
    lv_bar_set_value(s_temp_bar, t.gpu_temp_c, LV_ANIM_OFF);

    // VRAM
    snprintf(buf, sizeof(buf), "%.2f / %.2f GB",
             (t.vram_used_mb / 1024.0f), (t.vram_total_mb / 1024.0f));
    set_label_text_if_changed(s_vram_val, buf);
    int vram_max = (t.vram_total_mb > 0) ? (int)t.vram_total_mb : 100;
    lv_bar_set_range(s_vram_bar, 0, vram_max);
    lv_bar_set_value(s_vram_bar, (int)t.vram_used_mb, LV_ANIM_OFF);
}

/* Master UI Timer Callback (40ms interval = 25 FPS) */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (lv_scr_act() == s_scr_idle) {
        face_anim_tick();
    }

    static int64_t next_refresh_us = 0;
    int64_t now = esp_timer_get_time();
    if (now >= next_refresh_us) {
        next_refresh_us = now + 200000LL;
        telemetry_update_tick();
    }
}

void ui_init(void)
{
    /* Disable scrolling on active display / default screen */
    lv_disp_t *disp = lv_disp_get_default();
    if (disp && disp->act_scr) {
        lv_obj_clear_flag(disp->act_scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(disp->act_scr, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(disp->act_scr, LV_DIR_NONE);
    }

    /* 1. Load saved pet appearance from NVS flash */
    nvs_load_pet_type();

    /* 2. Build idle and AI screens */
    create_idle_screen();
    create_ai_screen();
    create_quotes_screen();

    /* 3. Start on Idle Screen */
    ui_load_screen(s_scr_idle);

    /* 4. Create animation & refresh timer (runs in LVGL context) */
    s_ui_timer = lv_timer_create(ui_timer_cb, 40, NULL);

    ESP_LOGI(TAG, "Desk Pet UI initialized successfully (Active appearance: %s)",
             s_pet_type_names[s_current_pet_type]);
}
