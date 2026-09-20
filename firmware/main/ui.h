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
 * @brief Pet appearance types: Original cyber pet and Cat.
 */
typedef enum {
    PET_TYPE_ORIGINAL = 0,   // Cyber / robot desk pet with glowing visor eyes
    PET_TYPE_CAT,            // Cute cat with pointed ears, whiskers, :3 mouth
    PET_TYPE_COUNT
} pet_type_t;

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
 * @brief Cycle to the next pet appearance (Original <-> Cat).
 *
 * Thread-safe to call from button or touch callbacks (with display lock held).
 */
void ui_cycle_appearance(void);

/**
 * @brief Manually set the pet appearance.
 */
void ui_set_pet_type(pet_type_t type);

/**
 * @brief Get the pet's current appearance.
 */
pet_type_t ui_get_pet_type(void);

/**
 * @brief Get human-readable name of pet appearance.
 */
const char *ui_get_pet_type_name(pet_type_t type);

/**
 * @brief Manually set the pet's active emotion.
 */
void ui_set_emotion(pet_emotion_t emotion);

/**
 * @brief Get the pet's current emotion.
 */
pet_emotion_t ui_get_emotion(void);

/**
 * @brief Set pet visual listening state when speaking is detected.
 */
void ui_set_voice_listening(bool listening);

/**
 * @brief Trigger celebration and subtitle feedback when a voice command is executed.
 */
void ui_trigger_voice_reaction(pet_emotion_t emotion, const char *feedback_text);

/**
 * @brief Explicitly show either the AI Mode screen or the Idle screen.
 */
void ui_show_ai_mode(bool show);
