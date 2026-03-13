#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_ili9341_init_cmds_2.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_tt21100.h"

#include "esp_lvgl_port.h"
#include "config.h"
#include "network.h"
#include "ai_service.h"
#include "conversation.h"
#include "ui.h"
#include "audio.h"
#if CONFIG_SDCARD_ENABLED
#include "storage.h"
#endif

static const char *TAG = "app_main";

static ai_service_t *s_ai_service = NULL;
static conversation_manager_t s_conv;
static bool s_is_processing = false;
static bool s_is_debug_mode = false;
static bool s_is_recording = false;
static uint8_t *s_recorded_audio = NULL;
static int s_recorded_len = 0;
static bool s_debug_playback_busy = false;

static bool s_ui_ready = false;
static volatile bool s_debug_record_req = false;
static volatile bool s_debug_play_record_req = false;
static volatile bool s_debug_play_req = false;
#if CONFIG_SDCARD_ENABLED
static volatile bool s_debug_sdcard_req = false;
static volatile bool s_debug_test_req = false;

#define SD_MP3_PATH_MAX_LEN     (256)
#endif

/* Shared I2C bus: created once, used by both touch and audio codec
 * when they share the same physical I2C port / GPIO pins. */
static i2c_master_bus_handle_t s_shared_i2c_bus = NULL;

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;

#define LCD_H_RES               (320)
#define LCD_V_RES               (240)
#define LCD_SPI_HOST            (SPI3_HOST)
#define LCD_PIXEL_CLK_HZ        (40 * 1000 * 1000)
#define LCD_CMD_BITS            (8)
#define LCD_PARAM_BITS          (8)
#define LCD_BITS_PER_PIXEL      (16)
#define LCD_DRAW_BUF_HEIGHT     (50)
#define LCD_BL_ON_LEVEL         (1)

#define LCD_GPIO_SCLK           (GPIO_NUM_7)
#define LCD_GPIO_MOSI           (GPIO_NUM_6)
#define LCD_GPIO_RST            (GPIO_NUM_48)
#define LCD_GPIO_DC             (GPIO_NUM_4)
#define LCD_GPIO_CS             (GPIO_NUM_5)
#define LCD_GPIO_BL             (GPIO_NUM_47)

#define TOUCH_I2C_PORT          (0)
#define TOUCH_I2C_CLK_HZ        (400000)
#define TOUCH_GPIO_SCL          (GPIO_NUM_18)
#define TOUCH_GPIO_SDA          (GPIO_NUM_8)
#define TOUCH_GPIO_INT          (GPIO_NUM_3)
#define DISPLAY_ENABLE_TOUCH    (1)

#define TOUCH_PROBE_RETRY_COUNT (20)
#define TOUCH_PROBE_DELAY_MS    (50)
#define TOUCH_PROBE_TIMEOUT_MS  (100)
#define TOUCH_PROBE_SETTLE_MS   (120)

typedef enum {
    TOUCH_CTRL_NONE = 0,
    TOUCH_CTRL_TT21100,
    TOUCH_CTRL_GT911,
    TOUCH_CTRL_GT911_BACKUP,
} touch_controller_t;

static touch_controller_t app_detect_touch_controller(i2c_master_bus_handle_t i2c_handle)
{
    vTaskDelay(pdMS_TO_TICKS(TOUCH_PROBE_SETTLE_MS));

    for (int attempt = 1; attempt <= TOUCH_PROBE_RETRY_COUNT; attempt++) {
        if (i2c_master_probe(i2c_handle, ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS, TOUCH_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "Display init: detected TT21100 (attempt %d/%d)", attempt, TOUCH_PROBE_RETRY_COUNT);
            return TOUCH_CTRL_TT21100;
        }
        if (i2c_master_probe(i2c_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, TOUCH_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "Display init: detected GT911 (attempt %d/%d)", attempt, TOUCH_PROBE_RETRY_COUNT);
            return TOUCH_CTRL_GT911;
        }
        if (i2c_master_probe(i2c_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, TOUCH_PROBE_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGI(TAG, "Display init: detected GT911 backup address (attempt %d/%d)", attempt, TOUCH_PROBE_RETRY_COUNT);
            return TOUCH_CTRL_GT911_BACKUP;
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_PROBE_DELAY_MS));
    }

    return TOUCH_CTRL_NONE;
}

static esp_err_t app_display_test_pattern(void)
{
    static uint16_t line[LCD_H_RES];

    for (int x = 0; x < LCD_H_RES; x++) {
        line[x] = 0xF800;
    }
    for (int y = 0; y < LCD_V_RES / 3; y++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y, LCD_H_RES, y + 1, line), TAG, "Draw red band failed");
    }

    for (int x = 0; x < LCD_H_RES; x++) {
        line[x] = 0x07E0;
    }
    for (int y = LCD_V_RES / 3; y < (LCD_V_RES * 2) / 3; y++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y, LCD_H_RES, y + 1, line), TAG, "Draw green band failed");
    }

    for (int x = 0; x < LCD_H_RES; x++) {
        line[x] = 0x001F;
    }
    for (int y = (LCD_V_RES * 2) / 3; y < LCD_V_RES; y++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y, LCD_H_RES, y + 1, line), TAG, "Draw blue band failed");
    }

    return ESP_OK;
}

static esp_err_t app_display_init(void)
{
    esp_err_t ret = ESP_OK;
    i2c_master_bus_handle_t i2c_handle = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_config = {0};
    touch_controller_t touch_ctrl = TOUCH_CTRL_NONE;
    bool use_st7789 = false;
    bool touch_is_gt911 = false;
    ESP_LOGI(TAG, "Display init: begin");

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_GPIO_BL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "Backlight GPIO config failed");

    /* Re-use the shared I2C bus if it was already created, otherwise allocate */
    if (s_shared_i2c_bus != NULL) {
        i2c_handle = s_shared_i2c_bus;
        ESP_LOGI(TAG, "Display init: reusing shared I2C bus for touch");
    } else {
        const i2c_master_bus_config_t i2c_config = {
            .i2c_port = TOUCH_I2C_PORT,
            .sda_io_num = TOUCH_GPIO_SDA,
            .scl_io_num = TOUCH_GPIO_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = true,
            },
        };
        esp_err_t i2c_err = i2c_new_master_bus(&i2c_config, &i2c_handle);
        if (i2c_err != ESP_OK) {
            ESP_LOGW(TAG, "Display init: touch I2C init failed (%s), defaulting to ILI9341", esp_err_to_name(i2c_err));
        } else {
            s_shared_i2c_bus = i2c_handle;
        }
    }

    if (i2c_handle != NULL) {
        touch_ctrl = app_detect_touch_controller(i2c_handle);
        if (touch_ctrl == TOUCH_CTRL_TT21100) {
            touch_io_config = (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
            use_st7789 = true;
            ESP_LOGI(TAG, "Display init: selecting ST7789 panel");
        } else if (touch_ctrl == TOUCH_CTRL_GT911) {
            touch_io_config = (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
            touch_is_gt911 = true;
            ESP_LOGI(TAG, "Display init: selecting ILI9341 panel");
        } else if (touch_ctrl == TOUCH_CTRL_GT911_BACKUP) {
            touch_io_config = (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
            touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
            touch_is_gt911 = true;
            ESP_LOGI(TAG, "Display init: selecting ILI9341 panel (GT911 backup address)");
        } else {
            ESP_LOGW(TAG, "Display init: no supported touch controller detected after retry, defaulting to ILI9341 panel");
        }
    }

    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_GPIO_SCLK,
        .mosi_io_num = LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_GPIO_DC,
        .cs_gpio_num = LCD_GPIO_CS,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_lcd_io), err, TAG, "New panel IO failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_GPIO_RST,
        .flags = {
            .reset_active_high = 1,
        },
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    if (use_st7789) {
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel), err, TAG, "New ST7789 panel failed");
    } else {
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = ili9341_lcd_init_vendor,
            .init_cmds_size = sizeof(ili9341_lcd_init_vendor) / sizeof(ili9341_lcd_init_vendor[0]),
        };
        esp_lcd_panel_dev_config_t ili9341_panel_config = panel_config;
        ili9341_panel_config.vendor_config = (void *)&vendor_config;
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ili9341(s_lcd_io, &ili9341_panel_config, &s_lcd_panel), err, TAG, "New ILI9341 panel failed");
    }

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), err, TAG, "Panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), err, TAG, "Panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, true, true), err, TAG, "Panel mirror failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), err, TAG, "Panel on failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_GPIO_BL, LCD_BL_ON_LEVEL), TAG, "Backlight on failed");
    ESP_LOGI(TAG, "Display init: panel ready, backlight ON level=%d (OFF=%d)", LCD_BL_ON_LEVEL, !LCD_BL_ON_LEVEL);
    ESP_RETURN_ON_ERROR(app_display_test_pattern(), TAG, "Display test pattern failed");
    ESP_LOGI(TAG, "Display init: test pattern rendered");
    vTaskDelay(pdMS_TO_TICKS(200));

    if (DISPLAY_ENABLE_TOUCH) {
        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = LCD_H_RES,
            .y_max = LCD_V_RES,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = TOUCH_GPIO_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = use_st7789 ? 1 : 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        if (i2c_handle != NULL && touch_ctrl != TOUCH_CTRL_NONE) {
            esp_err_t touch_err;
            if (use_st7789 || touch_is_gt911) {
                touch_io_config.scl_speed_hz = TOUCH_I2C_CLK_HZ;
            }

            touch_err = esp_lcd_new_panel_io_i2c(i2c_handle, &touch_io_config, &tp_io_handle);
            if (touch_err == ESP_OK) {
                if (touch_is_gt911) {
                    touch_err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_touch_handle);
                } else if (use_st7789) {
                    touch_err = esp_lcd_touch_new_i2c_tt21100(tp_io_handle, &tp_cfg, &s_touch_handle);
                } else {
                    touch_err = ESP_ERR_NOT_FOUND;
                }
            }

            if (touch_err != ESP_OK) {
                ESP_LOGW(TAG, "Display init: touch init failed (%s), continue without touch", esp_err_to_name(touch_err));
                s_touch_handle = NULL;
            } else {
                ESP_LOGI(TAG, "Display init: touch ready");
            }
        } else if (i2c_handle == NULL) {
            ESP_LOGW(TAG, "Display init: skip touch init because I2C bus is unavailable");
        } else {
            ESP_LOGW(TAG, "Display init: skip touch init because no touch controller was detected");
        }
    } else {
        ESP_LOGW(TAG, "Display init: touch disabled by configuration");
    }

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_GOTO_ON_ERROR(lvgl_port_init(&lvgl_cfg), err, TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
        }
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_GOTO_ON_FALSE(disp != NULL, ESP_FAIL, err, TAG, "Add LVGL display failed");

    if (s_touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = s_touch_handle,
        };
        lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
        if (indev == NULL) {
            ESP_LOGW(TAG, "Display init: add LVGL touch failed, continue without touch input");
        } else {
            ESP_LOGI(TAG, "Display init: LVGL touch input registered");
        }
    }

    ESP_LOGI(TAG, "Display init: success");

    return ESP_OK;

err:
    return ret;
}

static void network_state_callback(net_state_t state, void *user_data)
{
    switch (state) {
        case NET_STATE_DISCONNECTED:
            ESP_LOGI(TAG, "Network disconnected");
            if (s_ui_ready) {
                ui_update_status("Network disconnected");
            }
            break;
        case NET_STATE_CONNECTING:
            ESP_LOGI(TAG, "Network connecting...");
            if (s_ui_ready) {
                ui_update_status("Connecting...");
            }
            break;
        case NET_STATE_CONNECTED:
            ESP_LOGI(TAG, "Network connected");
            if (s_ui_ready) {
                ui_update_status("Connected");
            }
            break;
        case NET_STATE_ERROR:
            ESP_LOGE(TAG, "Network error");
            if (s_ui_ready) {
                ui_update_status("Connection failed");
            }
            break;
    }
}

static void stt_callback(const char *text)
{
    if (text == NULL || strlen(text) == 0) {
        ESP_LOGW(TAG, "Empty STT result");
        return;
    }

    ESP_LOGI(TAG, "User said: %s", text);

    if (s_is_processing) {
        ESP_LOGW(TAG, "Already processing, ignoring");
        return;
    }

    s_is_processing = true;
    if (s_ui_ready) {
        ui_show_panel(UI_PANEL_LOADING);
    }

    conversation_add_message(&s_conv, "user", text);

    ai_message_t *messages = NULL;
    conversation_get_messages(&s_conv, &messages);

    app_config_t *app_config = config_get();

    ai_response_t response;
    memset(&response, 0, sizeof(response));

    esp_err_t err = ai_service_chat_with_history(s_ai_service, messages, &response);

    if (err == ESP_OK && response.is_success) {
        ESP_LOGI(TAG, "AI response: %s", response.content);
        
        conversation_add_message(&s_conv, "assistant", response.content);
        
        if (s_ui_ready) {
            ui_update_chat_message(text, response.content);
            ui_show_panel(UI_PANEL_CHAT);
        }
        
        audio_set_volume(app_config->volume);
    } else {
        ESP_LOGE(TAG, "AI request failed: %s", response.error_msg);
        if (s_ui_ready) {
            ui_update_status("Request failed");
            ui_show_panel(UI_PANEL_MAIN);
        }
    }

    s_is_processing = false;
}

static void audio_playback_callback(void)
{
    ESP_LOGI(TAG, "Audio playback completed");
    
    if (s_is_debug_mode) {
        s_debug_playback_busy = false;
        if (s_ui_ready) {
            ui_debug_set_playing_state(false);
            ui_debug_update_status("Playback completed");
        }
    }
}

static void mic_level_callback(int level)
{
    if (s_is_debug_mode) {
        if (s_ui_ready) {
            ui_debug_update_mic_level(level);
        }
    }
}

static void debug_record_action_request(void)
{
    s_debug_record_req = true;
}

static void debug_play_record_action_request(void)
{
    ESP_LOGI(TAG, "Debug play-record action requested");
    s_debug_play_record_req = true;
}

static void debug_play_action_request(void)
{
    ESP_LOGI(TAG, "Debug playback action requested");
    s_debug_play_req = true;
}

static void debug_play_volume_change(int volume)
{
    ESP_LOGI(TAG, "Debug playback volume set to %d", volume);
    (void)audio_set_volume(volume);
}

#if CONFIG_SDCARD_ENABLED
static void debug_sdcard_action_request(void)
{
    s_debug_sdcard_req = true;
}

static void debug_test_audio_action_request(void)
{
    s_debug_test_req = true;
}
#endif

static void app_handle_debug_record_request(void)
{
    s_is_debug_mode = true;

    if (!s_is_recording) {
        if (audio_debug_start_monitor() == ESP_OK) {
            s_is_recording = true;
            if (s_ui_ready) {
                ui_debug_set_recording_state(true);
                ui_debug_update_status("Mic monitor ON");
            }
        }
    } else {
        audio_debug_stop_monitor();
        s_is_recording = false;
        if (s_ui_ready) {
            ui_debug_set_recording_state(false);
            ui_debug_update_status("Mic monitor OFF");
        }
    }
}

static void app_handle_debug_play_record_request(void)
{
    s_is_debug_mode = true;
    ESP_LOGI(TAG, "Handling debug play-record request");

    /* Stop mic monitor first to avoid concurrent read on the same mic device. */
    if (s_is_recording) {
        audio_debug_stop_monitor();
        s_is_recording = false;
        if (s_ui_ready) {
            ui_debug_set_recording_state(false);
        }
    }

    if (s_ui_ready) {
        ui_debug_set_playing_state(false);
        ui_debug_update_status("Recording 5s... speak now!");
    }

    if (s_debug_playback_busy) {
        ESP_LOGW(TAG, "Debug play-record ignored: playback still running");
        if (s_ui_ready) {
            ui_debug_update_status("Playback running, wait...");
        }
        return;
    }

    /* Always discard previous capture so each press records fresh audio */
    free(s_recorded_audio);
    s_recorded_audio = NULL;
    s_recorded_len = 0;

    if (audio_debug_record_sample(&s_recorded_audio, &s_recorded_len) != ESP_OK) {
        ESP_LOGW(TAG, "Debug play: record sample failed");
        if (s_ui_ready) {
            ui_debug_set_playing_state(false);
            ui_debug_update_status("Sample capture failed");
        }
        return;
    }

    ESP_LOGI(TAG, "Debug play-record: captured %d bytes", s_recorded_len);
    if (s_ui_ready) {
        ui_debug_set_playing_state(false);
        ui_debug_update_status("Sample recorded. Press Play.");
    }
}

static void app_handle_debug_play_request(void)
{
    s_is_debug_mode = true;
    ESP_LOGI(TAG, "Handling debug playback request");

    if (s_recorded_audio == NULL || s_recorded_len <= 0) {
        ESP_LOGW(TAG, "Debug playback: no recorded sample available");
        if (s_ui_ready) {
            ui_debug_set_playing_state(false);
            ui_debug_update_status("No sample. Press Record first.");
        }
        return;
    }

    if (s_debug_playback_busy) {
        ESP_LOGW(TAG, "Debug playback ignored: playback still running");
        if (s_ui_ready) {
            ui_debug_update_status("Playback running, wait...");
        }
        return;
    }

    if (s_ui_ready) {
        ui_debug_set_playing_state(true);
        ui_debug_update_status("Playing recorded sample...");
    }

    int queued_len = s_recorded_len;
    esp_err_t play_ret = audio_debug_play_sample_ref(s_recorded_audio, s_recorded_len);
    if (play_ret != ESP_OK) {
        ESP_LOGW(TAG, "Debug play: playback queue failed: %s", esp_err_to_name(play_ret));
        s_debug_playback_busy = false;
        if (s_ui_ready) {
            ui_debug_set_playing_state(false);
            ui_debug_update_status("Sample playback failed");
        }
    } else {
        s_debug_playback_busy = true;
        ESP_LOGI(TAG, "Debug play: queued %d bytes", queued_len);
    }
}

#if CONFIG_SDCARD_ENABLED
static void app_handle_debug_sdcard_request(void)
{
    s_is_debug_mode = true;

    esp_err_t err = storage_sdcard_mount();
    if (err != ESP_OK) {
        if (s_ui_ready) {
            ui_debug_update_status("SD mount failed");
        }
        return;
    }

    if (s_ui_ready) {
        char status[96] = {0};
        snprintf(status, sizeof(status), "SD ready: %s", storage_sdcard_get_mount_point());
        ui_debug_update_status(status);
    }
}

static void app_handle_debug_test_request(void)
{
    s_is_debug_mode = true;

    esp_err_t err = storage_sdcard_mount();
    if (err != ESP_OK) {
        if (s_ui_ready) {
            ui_debug_update_status("SD mount failed");
        }
        return;
    }

    char mp3_path[SD_MP3_PATH_MAX_LEN] = {0};
    err = storage_sdcard_find_first_mp3(mp3_path, sizeof(mp3_path));
    if (err != ESP_OK) {
        if (s_ui_ready) {
            ui_debug_update_status("No MP3 on SD");
        }
        return;
    }

    if (s_ui_ready) {
        ui_debug_set_playing_state(true);
    }

    err = audio_debug_play_mp3_file(mp3_path);
    if (err != ESP_OK) {
        if (s_ui_ready) {
            ui_debug_set_playing_state(false);
            ui_debug_update_status("MP3 verify failed");
        }
        return;
    }

    if (s_ui_ready) {
        const char *file_name = strrchr(mp3_path, '/');
        file_name = (file_name == NULL) ? mp3_path : (file_name + 1);

        char status[96] = {0};
        snprintf(status, sizeof(status), "Stage1 MP3: %.64s", file_name);
        ui_debug_update_status(status);
    }
}
#endif /* CONFIG_SDCARD_ENABLED */

static void app_process_debug_requests(void)
{
    if (s_debug_record_req) {
        s_debug_record_req = false;
        app_handle_debug_record_request();
    }
    if (s_debug_play_record_req) {
        s_debug_play_record_req = false;
        app_handle_debug_play_record_request();
    }
    if (s_debug_play_req) {
        s_debug_play_req = false;
        app_handle_debug_play_request();
    }
#if CONFIG_SDCARD_ENABLED
    if (s_debug_sdcard_req) {
        s_debug_sdcard_req = false;
        app_handle_debug_sdcard_request();
    }
    if (s_debug_test_req) {
        s_debug_test_req = false;
        app_handle_debug_test_request();
    }
#endif
}

static esp_err_t initialize_ai_service(void)
{
    app_config_t *config = config_get();

    if (s_ai_service != NULL) {
        ai_service_destroy(s_ai_service);
    }

    ai_provider_type_t provider = (ai_provider_type_t)config->provider;
    s_ai_service = ai_service_create(provider);
    if (s_ai_service == NULL) {
        ESP_LOGE(TAG, "Failed to create AI service");
        return ESP_FAIL;
    }

    esp_err_t err = ai_service_init(s_ai_service, config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize AI service");
        ai_service_destroy(s_ai_service);
        s_ai_service = NULL;
        return err;
    }

    const char *provider_name = "Unknown";
    switch (config->provider) {
        case AI_PROVIDER_CONFIG_OPENAI:
            provider_name = "OpenAI";
            break;
        case AI_PROVIDER_CONFIG_ZHIPU:
            provider_name = "Zhipu AI";
            break;
        case AI_PROVIDER_CONFIG_DEEPSEEK:
            provider_name = "DeepSeek";
            break;
        default:
            break;
    }

    if (s_ui_ready) {
        ui_update_provider(provider_name);
    }
    ESP_LOGI(TAG, "AI service initialized: %s", provider_name);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting AI Chat Demo");

    ESP_ERROR_CHECK(config_init());
    ESP_LOGI(TAG, "Configuration loaded");
    
    esp_err_t err = app_display_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Display initialization failed: %s, continuing without UI", esp_err_to_name(err));
    }

    // Try to initialize UI, but don't crash if it fails
    err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UI initialization failed: %s, continuing without UI", esp_err_to_name(err));
        s_ui_ready = false;
    } else {
        s_ui_ready = true;
        ui_debug_set_record_action_callback(debug_record_action_request);
        ui_debug_set_play_record_action_callback(debug_play_record_action_request);
        ui_debug_set_play_action_callback(debug_play_action_request);
        ui_debug_set_play_volume_callback(debug_play_volume_change);
#if CONFIG_SDCARD_ENABLED
        ui_debug_set_sdcard_action_callback(debug_sdcard_action_request);
        ui_debug_set_test_audio_action_callback(debug_test_audio_action_request);
#endif
    }
    /* If the codec I2C port matches the touch I2C port (port 0), share the
     * bus handle so audio_init() does not try to create a duplicate. */
#if CONFIG_AUDIO_CODEC_I2C_PORT == 0
    if (s_shared_i2c_bus != NULL) {
        audio_set_codec_i2c_bus(s_shared_i2c_bus);
    }
#endif
    err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio initialization failed: %s, continuing in degraded mode", esp_err_to_name(err));
    } else if (s_ui_ready) {
        app_config_t *app_config = config_get();
        ui_debug_set_play_volume(app_config->volume);
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

    audio_register_playback_callback(audio_playback_callback);
    audio_register_mic_level_callback(mic_level_callback);
    network_set_callback(network_state_callback, NULL);
    ESP_ERROR_CHECK(audio_start_stt(stt_callback));

    ESP_ERROR_CHECK(conversation_init(&s_conv, CONVERSATION_MAX_HISTORY));

    ESP_ERROR_CHECK(initialize_ai_service());

    err = network_start();
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "WiFi is not configured, running in offline mode");
        if (s_ui_ready) {
            ui_update_status("WiFi not configured");
        }
    } else {
        ESP_ERROR_CHECK(err);
    }

    while (true) {
        app_process_debug_requests();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
