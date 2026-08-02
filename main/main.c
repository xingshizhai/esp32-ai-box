#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"

#include <ctype.h>
#include <string.h>

#include "app_display.h"
#include "app_runtime.h"
#include "config.h"
#include "provider_catalog.h"
#include "network.h"
#include "ui.h"
#include "audio.h"
#include "debug_screenshot.h"
#include "debug_input.h"
#if CONFIG_SDCARD_ENABLED
#include "storage.h"
#endif

static const char *TAG = "app_main";

#define APP_CONSOLE_LINE_MAX 768
#define APP_CONSOLE_UART_RX_BUF_SIZE 1024

static char s_console_line[APP_CONSOLE_LINE_MAX] = {0};
static size_t s_console_line_len = 0;
static bool s_console_uart_ready = false;
static bool s_console_poll_disabled = false;

static void app_console_init(void)
{
    esp_err_t err = uart_driver_install(UART_NUM_0,
                                        APP_CONSOLE_UART_RX_BUF_SIZE,
                                        0,
                                        0,
                                        NULL,
                                        0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_console_uart_ready = true;
        ESP_LOGI(TAG, "Console UART ready");
        return;
    }

    s_console_uart_ready = false;
    s_console_poll_disabled = true;
    ESP_LOGW(TAG,
             "Console UART init failed (%s), command polling disabled",
             esp_err_to_name(err));
}

static bool app_str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }

    return (*a == '\0' && *b == '\0');
}

static void app_console_print_help(void)
{
    ESP_LOGI(TAG, "Console commands:");
    ESP_LOGI(TAG, "  help");
    ESP_LOGI(TAG, "  provider show");
    ESP_LOGI(TAG, "  provider list");
    ESP_LOGI(TAG, "  provider set <openai|zhipu|deepseek|kimi|minimax|openrouter>");
    ESP_LOGI(TAG, "  provider preset <openai|zhipu|deepseek|kimi|minimax|openrouter>");
    ESP_LOGI(TAG, "  provider key set <api_key>");
    ESP_LOGI(TAG, "  provider base set <url>");
    ESP_LOGI(TAG, "  provider model set <name>");
    ESP_LOGI(TAG, "  provider referer set <value>");
    ESP_LOGI(TAG, "  provider title set <value>");
    ESP_LOGI(TAG, "  provider test [prompt]");
    ESP_LOGI(TAG, "  font test");
    ESP_LOGI(TAG, "  font stats");
    ESP_LOGI(TAG, "  screenshot serial");
}

static esp_err_t app_console_update_active_provider(const char *api_key,
                                                    const char *base_url,
                                                    const char *model_name)
{
    app_config_t *cfg = config_get();
    if (cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = config_set_ai_credentials_for_provider(cfg->provider,
                                                            api_key,
                                                            base_url,
                                                            model_name);
    if (err != ESP_OK) {
        return err;
    }

    return app_runtime_reload_ai_service();
}

static esp_err_t app_console_activate_provider(ai_provider_config_t provider)
{
    app_config_t *cfg = config_get();
    if (cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (cfg->provider == provider) {
        return app_runtime_reload_ai_service();
    }

    return app_runtime_switch_chat_provider(provider);
}

static void app_console_handle_line(char *line)
{
    if (line == NULL) {
        return;
    }

    char *cmd = strtok(line, " \t");
    if (cmd == NULL) {
        return;
    }

    if (app_str_ieq(cmd, "help")) {
        app_console_print_help();
        return;
    }

    if (app_str_ieq(cmd, "font")) {
        char *sub = strtok(NULL, " \t");
        if (sub != NULL && app_str_ieq(sub, "test")) {
            esp_err_t err = ui_font_run_self_test();
            if (err != ESP_OK) ESP_LOGE(TAG, "Font self-test failed: %s", esp_err_to_name(err));
        } else if (sub != NULL && app_str_ieq(sub, "stats")) {
            ui_font_log_stats();
        } else {
            ESP_LOGW(TAG, "Usage: font <test|stats>");
        }
        return;
    }

    if (app_str_ieq(cmd, "screenshot")) {
        char *sub = strtok(NULL, " \t");
        if (sub != NULL && app_str_ieq(sub, "serial")) {
            esp_err_t err = debug_screenshot_dump_serial();
            if (err != ESP_OK) ESP_LOGE(TAG, "Serial screenshot failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "Usage: screenshot serial");
        }
        return;
    }

    if (!app_str_ieq(cmd, "provider")) {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
        app_console_print_help();
        return;
    }

    char *sub = strtok(NULL, " \t");
    if (sub == NULL) {
        ESP_LOGW(TAG, "Missing provider subcommand");
        app_console_print_help();
        return;
    }

    if (app_str_ieq(sub, "show")) {
        app_config_t *cfg = config_get();
        ESP_LOGI(TAG,
                 "Current provider=%s model=%s base_url=%s key=%s",
                 provider_catalog_name(cfg->provider),
                 cfg->model_name,
                 cfg->base_url,
                 (cfg->api_key[0] != '\0') ? "set" : "empty");
        if (cfg->provider == AI_PROVIDER_CONFIG_OPENROUTER) {
            ESP_LOGI(TAG,
                     "OpenRouter headers: referer=%s x_title=%s",
                     (cfg->openrouter_http_referer[0] != '\0') ? cfg->openrouter_http_referer : "<empty>",
                     (cfg->openrouter_x_title[0] != '\0') ? cfg->openrouter_x_title : "<empty>");
        }
        return;
    }

    if (app_str_ieq(sub, "list")) {
        ESP_LOGI(TAG, "Available providers: openai, zhipu, deepseek, kimi, minimax, openrouter");
        return;
    }

    if (app_str_ieq(sub, "set")) {
        char *name = strtok(NULL, " \t");
        if (name == NULL) {
            ESP_LOGW(TAG, "Usage: provider set <name>");
            return;
        }

        ai_provider_config_t provider;
        if (!provider_catalog_parse(name, &provider)) {
            ESP_LOGW(TAG, "Unknown provider: %s", name);
            return;
        }

        esp_err_t err = app_runtime_switch_chat_provider(provider);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Provider switched to %s", provider_catalog_name(provider));
        } else {
            ESP_LOGE(TAG,
                     "Provider switch failed: %s",
                     esp_err_to_name(err));
        }
        return;
    }

    if (app_str_ieq(sub, "preset")) {
        char *name = strtok(NULL, " \t");
        if (name == NULL) {
            ESP_LOGW(TAG, "Usage: provider preset <name>");
            return;
        }

        ai_provider_config_t provider;
        if (!provider_catalog_parse(name, &provider)) {
            ESP_LOGW(TAG, "Unknown provider: %s", name);
            return;
        }

        const char *default_base = NULL;
        const char *default_model = NULL;
        if (!provider_catalog_default_profile(provider, &default_base, &default_model)) {
            ESP_LOGW(TAG, "No default profile for provider: %s", name);
            return;
        }

        esp_err_t err = config_set_ai_credentials_for_provider(provider,
                                                                NULL,
                                                                default_base,
                                                                default_model);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Preset apply failed: %s", esp_err_to_name(err));
            return;
        }

        err = app_console_activate_provider(provider);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "Preset applied: provider=%s base=%s model=%s",
                     provider_catalog_name(provider),
                     default_base,
                     default_model);
        } else {
            ESP_LOGE(TAG, "Preset activation failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (app_str_ieq(sub, "key") || app_str_ieq(sub, "base") || app_str_ieq(sub, "model") ||
        app_str_ieq(sub, "referer") || app_str_ieq(sub, "title")) {
        char *verb = strtok(NULL, " \t");
        if (verb == NULL || !app_str_ieq(verb, "set")) {
            ESP_LOGW(TAG, "Usage: provider %s set <value>", sub);
            return;
        }

        char *value = strtok(NULL, "");
        if (value == NULL || value[0] == '\0') {
            ESP_LOGW(TAG, "Usage: provider %s set <value>", sub);
            return;
        }

        while (*value == ' ' || *value == '\t') {
            ++value;
        }
        if (value[0] == '\0') {
            ESP_LOGW(TAG, "Usage: provider %s set <value>", sub);
            return;
        }

        esp_err_t err = ESP_OK;
        if (app_str_ieq(sub, "referer") || app_str_ieq(sub, "title")) {
            const char *referer = app_str_ieq(sub, "referer") ? value : NULL;
            const char *title = app_str_ieq(sub, "title") ? value : NULL;
            err = config_set_openrouter_headers(referer, title);
            if (err == ESP_OK) {
                app_config_t *cfg = config_get();
                if (cfg != NULL && cfg->provider == AI_PROVIDER_CONFIG_OPENROUTER) {
                    err = app_runtime_reload_ai_service();
                }
            }
        } else {
            const char *api_key = NULL;
            const char *base_url = NULL;
            const char *model_name = NULL;
            if (app_str_ieq(sub, "key")) {
                api_key = value;
            } else if (app_str_ieq(sub, "base")) {
                base_url = value;
            } else {
                model_name = value;
            }
            err = app_console_update_active_provider(api_key, base_url, model_name);
        }

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Updated provider %s", sub);
        } else {
            ESP_LOGE(TAG, "Update failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (app_str_ieq(sub, "test")) {
        char *prompt = strtok(NULL, "");
        if (prompt != NULL) {
            while (*prompt == ' ' || *prompt == '\t') {
                ++prompt;
            }
        }

        esp_err_t err = app_runtime_test_chat(prompt);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Provider test failed: %s", esp_err_to_name(err));
        }
        return;
    }

    ESP_LOGW(TAG, "Unknown provider subcommand: %s", sub);
    app_console_print_help();
}

static void app_console_poll(void)
{
    if (!s_console_uart_ready || s_console_poll_disabled) {
        return;
    }

    uint8_t ch = 0;
    for (int i = 0; i < 64; ++i) {
        int read = uart_read_bytes(UART_NUM_0, &ch, 1, 0);
        if (read < 0) {
            s_console_poll_disabled = true;
            ESP_LOGW(TAG,
                     "Console polling disabled due to UART read error: %d",
                     read);
            break;
        }
        if (read == 0) {
            break;
        }

        if (ch == '\r' || ch == '\n') {
            if (s_console_line_len > 0) {
                s_console_line[s_console_line_len] = '\0';
                app_console_handle_line(s_console_line);
                s_console_line_len = 0;
            }
            continue;
        }

        if (ch == 0x08 || ch == 0x7F) {
            if (s_console_line_len > 0) {
                --s_console_line_len;
            }
            continue;
        }

        if (isprint(ch) && s_console_line_len < (APP_CONSOLE_LINE_MAX - 1)) {
            s_console_line[s_console_line_len++] = (char)ch;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting AI Chat Demo");

    app_console_init();

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
    /* Some boards route touch and audio codecs through a BSP-owned I2C bus.
     * Always reuse that handle when present: creating another master on the
     * same port is rejected by the ESP-IDF I2C driver. */
    if (app_display_get_shared_i2c_bus() != NULL) {
        audio_set_codec_i2c_bus(app_display_get_shared_i2c_bus());
    }
    err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio initialization failed: %s, continuing in degraded mode", esp_err_to_name(err));
    } else {
        /* No-op on boards whose PA enable is a raw GPIO already driven by
         * audio_init() itself (e.g. BOX-3); needed on boards where the PA is
         * behind an I2C IO-expander (e.g. LCD-EV-BOARD-2). */
        (void)app_display_enable_speaker_amp(true);
        if (ui_ready) {
            app_config_t *app_config = config_get();
            (void)ui_debug_set_play_volume(app_config->volume);
        }
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

    err = app_runtime_init(ui_ready);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "Runtime started in degraded mode (chat provider config incomplete). "
                 "Use monitor command: provider set <name>");
        if (ui_ready) {
            (void)ui_update_status("Chat provider not configured");
        }
    } else {
        ESP_ERROR_CHECK(err);
    }

    audio_register_playback_callback(app_runtime_handle_audio_playback_complete);
    audio_register_mic_level_callback(app_runtime_handle_mic_level);
    ESP_ERROR_CHECK(network_set_callback(app_runtime_handle_network_state, NULL));
    ESP_ERROR_CHECK(audio_start_stt(app_runtime_handle_stt_result));

    ESP_LOGI(TAG, "About to call network_start");
    err = network_start();
    ESP_LOGI(TAG, "network_start returned: %s", esp_err_to_name(err));
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "WiFi is not configured, running in offline mode");
        if (ui_ready) {
            (void)ui_update_status("WiFi not configured");
        }
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_LOGI(TAG, "Starting debug services...");

    err = debug_screenshot_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "debug_screenshot_start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "debug_screenshot_start succeeded");
    }

    err = debug_input_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "debug_input_start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "debug_input_start succeeded");
    }

    ESP_LOGI(TAG, "Entering main loop");

    while (true) {
        app_runtime_process_requests();
        app_console_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
