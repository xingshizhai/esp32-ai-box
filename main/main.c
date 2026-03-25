#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_display.h"
#include "app_runtime.h"
#include "config.h"
#include "network.h"
#include "ui.h"
#include "audio.h"
#if CONFIG_SDCARD_ENABLED
#include "storage.h"
#endif

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting AI Chat Demo");

    ESP_ERROR_CHECK(config_init());
    ESP_LOGI(TAG, "Configuration loaded");

    bool ui_ready = false;
    
    esp_err_t err = app_display_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Display initialization failed: %s, continuing without UI", esp_err_to_name(err));
    }

    // Try to initialize UI, but don't crash if it fails
    err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UI initialization failed: %s, continuing without UI", esp_err_to_name(err));
    } else {
        ui_ready = true;
        (void)ui_set_main_action_callback(app_runtime_request_voice_round);
        (void)ui_debug_set_record_action_callback(app_runtime_request_debug_record);
        (void)ui_debug_set_play_record_action_callback(app_runtime_request_debug_play_record);
        (void)ui_debug_set_play_action_callback(app_runtime_request_debug_play);
        (void)ui_debug_set_play_volume_callback(app_runtime_set_debug_play_volume);
#if CONFIG_SDCARD_ENABLED
        (void)ui_debug_set_sdcard_action_callback(app_runtime_request_debug_sdcard);
        (void)ui_debug_set_test_audio_action_callback(app_runtime_request_debug_test_audio);
#endif
    }
    /* If the codec I2C port matches the touch I2C port (port 0), share the
     * bus handle so audio_init() does not try to create a duplicate. */
#if CONFIG_AUDIO_CODEC_I2C_PORT == 0
    if (app_display_get_shared_i2c_bus() != NULL) {
        audio_set_codec_i2c_bus(app_display_get_shared_i2c_bus());
    }
#endif
    err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio initialization failed: %s, continuing in degraded mode", esp_err_to_name(err));
    } else if (ui_ready) {
        app_config_t *app_config = config_get();
        (void)ui_debug_set_play_volume(app_config->volume);
    }
    ESP_ERROR_CHECK(network_init());

#if CONFIG_SDCARD_ENABLED
    err = storage_sdcard_mount();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD card ready for phase-1 MP3 tests");
    } else {
        ESP_LOGW(TAG, "SD card not ready at boot (%s), will retry on test action", esp_err_to_name(err));
    }
#endif

    ESP_ERROR_CHECK(app_runtime_init(ui_ready));

    audio_register_playback_callback(app_runtime_handle_audio_playback_complete);
    audio_register_mic_level_callback(app_runtime_handle_mic_level);
    ESP_ERROR_CHECK(network_set_callback(app_runtime_handle_network_state, NULL));
    ESP_ERROR_CHECK(audio_start_stt(app_runtime_handle_stt_result));

    err = network_start();
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "WiFi is not configured, running in offline mode");
        if (ui_ready) {
            (void)ui_update_status("WiFi not configured");
        }
    } else {
        ESP_ERROR_CHECK(err);
    }

    while (true) {
        app_runtime_process_requests();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
