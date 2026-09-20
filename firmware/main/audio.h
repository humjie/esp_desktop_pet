#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize I2S, ES7210 dual microphone, and ES8311 speaker.
 * Launches background microphone recording and VAD task + speaker playback task.
 */
esp_err_t audio_init(void);

/**
 * @brief Play a soft gentle chime when wake word is detected.
 */
void audio_play_soft_wake_ring(void);

/**
 * @brief Play a distinctive affirmative ring when a command is received and executed.
 */
void audio_play_confirm_ring(void);

/**
 * @brief Play a cheerful ascending chime through the ES8311 speaker.
 */
void audio_play_chime(void);

/**
 * @brief Feed incoming base64-encoded mono PCM audio chunk from host to the speaker buffer.
 */
void audio_feed_spk_base64(const char *b64_str);

/**
 * @brief Stop current speaker playback and flush the audio buffer.
 */
void audio_spk_stop(void);

/**
 * @brief Set music playback mode on/off (enables echo-aware VAD so voice can be heard over music).
 */
void audio_set_music_mode(bool enable);

/**
 * @brief Check if music mode is currently active.
 */
bool audio_is_music_mode(void);

/**
 * @brief Set the speaker volume (0 - 100).
 */
void audio_set_spk_volume(int volume);

/**
 * @brief Get the current speaker volume.
 */
int audio_get_spk_volume(void);

/**
 * @brief Check if the speaker is currently actively playing sound.
 */
bool audio_is_speaker_active(void);

#ifdef __cplusplus
}
#endif
