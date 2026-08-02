#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_stt_callback_t)(const char *text);
typedef void (*audio_playback_complete_callback_t)(void);
typedef void (*audio_mic_level_callback_t)(int level);

/**
 * @brief Optionally provide a pre-created I2C master bus handle to share with
 *        the touch / display driver.  Must be called before audio_init().
 *        If not called, audio_init() will create its own bus on the port
 *        configured via CONFIG_AUDIO_CODEC_I2C_PORT.
 *
 * @param bus  i2c_master_bus_handle_t cast to void* to avoid pulling in
 *             driver/i2c_master.h into every consumer of this header.
 */
void audio_set_codec_i2c_bus(void *bus);

esp_err_t audio_init(void);
esp_err_t audio_start_stt(audio_stt_callback_t callback);
esp_err_t audio_stop_stt(void);
esp_err_t audio_play_tts(const uint8_t *audio_data, int audio_len);
esp_err_t audio_stop_tts(void);
esp_err_t audio_set_volume(int volume);
void audio_register_playback_callback(audio_playback_complete_callback_t callback);

/* Stream interfaces for real-time voice chat (mono PCM S16LE, 16 kHz). */
esp_err_t audio_stream_start_capture(void);
esp_err_t audio_stream_read_capture_chunk(uint8_t *pcm_data, int pcm_capacity, int *pcm_len);
esp_err_t audio_stream_stop_capture(void);
esp_err_t audio_stream_play_chunk(const uint8_t *audio_data, int audio_len);

esp_err_t audio_debug_start_monitor(void);
esp_err_t audio_debug_stop_monitor(void);
void audio_register_mic_level_callback(audio_mic_level_callback_t callback);
esp_err_t audio_debug_record_sample(uint8_t **data, int *len);
esp_err_t audio_debug_play_owned_sample(uint8_t **data, int *len);
esp_err_t audio_debug_play_sample_ref(const uint8_t *data, int len);
esp_err_t audio_debug_play_test_audio(void);
esp_err_t audio_debug_play_mp3_file(const char *file_path);

#ifdef __cplusplus
}
#endif
