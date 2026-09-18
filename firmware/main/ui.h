#pragma once

#include "lvgl.h"

/**
 * @brief Pet emotion states inspired by Tabbie, with vibrant color themes.
 */
typedef enum {
    PET_EMOTION_HAPPY = 0,   // Playful & friendly (Vibrant Cyan #00E5FF)
    PET_EMOTION_FOCUS,       // Working hard / High CPU/GPU load (Warm Amber #FF9F0A)
    PET_EMOTION_RELAX,       // Zen / chilling / idle system (Mint Green #30D158)
    PET_EMOTION_LOVE,        // Loved / tapped / affectionate (Rose Pink #FF375F)
    PET_EMOTION_HOT,         // High Temp (>70C) or heavy load (Fiery Crimson #FF453A)
    PET_EMOTION_COUNT
} pet_emotion_t;

/**
 * @brief Initialize all desk pet UI elements, screens, and timers.
 *
 * Must be called with bsp_display_lock(0) held.
 */
void ui_init(void);

/**
 * @brief Toggle between the Idle face/clock screen and the AI/System Status screen.
 *
 * Thread-safe to call from button or touch callbacks.
 */
void ui_toggle_ai_mode(void);

/**
 * @brief Manually set the pet's active emotion.
 */
void ui_set_emotion(pet_emotion_t emotion);

/**
 * @brief Get the pet's current emotion.
 */
pet_emotion_t ui_get_emotion(void);
