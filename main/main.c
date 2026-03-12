#include <string.h>
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
#include "esp_lcd_touch_tt21100.h"

#include "esp_lvgl_port.h"
#include "config.h"
#include "network.h"
#include "ai_service.h"
#include "conversation.h"
#include "ui.h"
#include "audio.h"

static const char *TAG = "app_main";

static ai_service_t *s_ai_service = NULL;
static conversation_manager_t s_conv;
static bool s_is_processing = false;
static bool s_is_debug_mode = false;
static bool s_is_recording = false;
static uint8_t *s_recorded_audio = NULL;
static int s_recorded_len = 0;
static bool s_ui_ready = false;

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
#define LCD_BL_ON_LEVEL         (0)

#define LCD_GPIO_SCLK           (GPIO_NUM_7)
#define LCD_GPIO_MOSI           (GPIO_NUM_6)
#define LCD_GPIO_RST            (GPIO_NUM_48)
#define LCD_GPIO_DC             (GPIO_NUM_4)
#define LCD_GPIO_CS             (GPIO_NUM_5)
#define LCD_GPIO_BL             (GPIO_NUM_45)

#define TOUCH_I2C_PORT          (0)
#define TOUCH_I2C_CLK_HZ        (400000)
#define TOUCH_GPIO_SCL          (GPIO_NUM_18)
#define TOUCH_GPIO_SDA          (GPIO_NUM_8)
#define TOUCH_GPIO_INT          (GPIO_NUM_3)
#define DISPLAY_ENABLE_TOUCH    (0)

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
    ESP_LOGI(TAG, "Display init: begin");

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_GPIO_BL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "Backlight GPIO config failed");

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
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel), err, TAG, "New panel failed");

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
        const i2c_master_bus_config_t i2c_config = {
            .i2c_port = TOUCH_I2C_PORT,
            .sda_io_num = TOUCH_GPIO_SDA,
            .scl_io_num = TOUCH_GPIO_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
        };
        i2c_master_bus_handle_t i2c_handle = NULL;
        esp_err_t touch_err = i2c_new_master_bus(&i2c_config, &i2c_handle);
        if (touch_err != ESP_OK) {
            ESP_LOGW(TAG, "Display init: touch I2C init failed (%s), continue without touch", esp_err_to_name(touch_err));
            i2c_handle = NULL;
        }

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
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };

        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
        tp_io_config.scl_speed_hz = TOUCH_I2C_CLK_HZ;
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        if (i2c_handle != NULL) {
            touch_err = esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle);
            if (touch_err == ESP_OK) {
                touch_err = esp_lcd_touch_new_i2c_tt21100(tp_io_handle, &tp_cfg, &s_touch_handle);
            }

            if (touch_err != ESP_OK) {
                ESP_LOGW(TAG, "Display init: touch init failed (%s), continue without touch", esp_err_to_name(touch_err));
                s_touch_handle = NULL;
            } else {
                ESP_LOGI(TAG, "Display init: touch ready");
            }
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
    }
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(network_init());

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
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
