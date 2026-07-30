#include "app_display.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_io_expander.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"
#include "esp_lvgl_port_touch.h"

static const char *TAG = "app_display";

typedef struct {
    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t lcd_panel;
    esp_lcd_touch_handle_t touch_handle;
    lv_display_t *lv_disp;
} app_display_state_t;

static app_display_state_t s_display = {0};

esp_err_t app_display_init(void)
{
    ESP_LOGI(TAG, "Display init: begin (esp32-s3-lcd-ev-board BSP path)");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    /* This BSP allocates its own RGB frame buffer(s) in PSRAM; max_transfer_sz
     * is unused for the RGB interface. */
    bsp_display_config_t bsp_disp_cfg = { .max_transfer_sz = 0 };
    ESP_RETURN_ON_ERROR(bsp_display_new(&bsp_disp_cfg, &s_display.lcd_panel, &s_display.lcd_io),
                        TAG, "bsp_display_new failed");

    /* Resolution depends on the sub-board selected via menuconfig
     * (BSP_LCD_SUB_BOARD_480_480 vs BSP_LCD_SUB_BOARD_800_480). */
    uint16_t h_res = bsp_display_get_h_res();
    uint16_t v_res = bsp_display_get_v_res();

    /* Bounce buffer mode: the panel DMA reads from a small SRAM bounce buffer
     * instead of directly from the PSRAM frame buffer, which avoids PSRAM
     * bandwidth contention between the CPU and LCD DMA (root cause of a
     * horizontal scroll tearing artifact on this BSP). LVGL draws into its
     * own SPIRAM buffer and the BSP refills the bounce buffer in the
     * background, so there is no conflict. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_display.lcd_io,
        .panel_handle = s_display.lcd_panel,
        .buffer_size = h_res * 40, /* 40-line partial draw buffer in SPIRAM */
        .double_buffer = true,
        .hres = h_res,
        .vres = v_res,
        .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_spiram = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true, /* sync to on_bounce_frame_finish callback */
            .avoid_tearing = false,
        }
    };
    s_display.lv_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(s_display.lv_disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp_rgb failed");

    esp_err_t touch_err = bsp_touch_new(NULL, &s_display.touch_handle);
    if (touch_err != ESP_OK || s_display.touch_handle == NULL) {
        ESP_LOGW(TAG, "touch controller not found (%s) — continue with synthetic input only",
                 esp_err_to_name(touch_err));
        s_display.touch_handle = NULL;
    } else {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_display.lv_disp,
            .handle = s_display.touch_handle,
        };
        lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
        if (indev == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed");
        } else {
            ESP_LOGI(TAG, "Display init: LVGL touch input registered");
        }
        ESP_LOGI(TAG, "Display init: touch ready");
    }

    ESP_LOGI(TAG, "Display init: success %dx%d (RGB, bounce_buffer mode)", h_res, v_res);
    return ESP_OK;
}

void *app_display_get_shared_i2c_bus(void)
{
    return (void *)bsp_i2c_get_handle();
}

esp_err_t app_display_set_brightness(int percent)
{
    /* This BSP documents bsp_display_brightness_set() as a no-op ("useless,
     * just for compatibility") — the RGB panel has no backlight PWM control
     * on this board, so the idle-dim power-saving feature has no visible
     * effect here. Call it anyway to keep the call chain uniform. */
    return bsp_display_brightness_set(percent);
}

esp_err_t app_display_enable_speaker_amp(bool enable)
{
    /* The speaker PA enable line is behind the TCA9554 IO-expander on this
     * board, not a raw ESP32 GPIO. bsp_audio_poweramp_enable() itself does
     * NOT lazily init the expander or configure its pin direction -- only
     * the BSP's own bsp_audio_init() does both of those, which this project
     * never calls (audio.c hand-rolls I2S + codecs instead of using the
     * BSP's audio helpers). Without this, esp_io_expander_set_level() inside
     * bsp_audio_poweramp_enable() aborts: NULL handle if the expander was
     * never created, or ESP_ERR_INVALID_STATE if the pin is still in its
     * default INPUT direction. Both confirmed on real hardware. */
    esp_io_expander_handle_t expander = bsp_io_expander_init();
    if (expander == NULL) {
        ESP_LOGW(TAG, "IO-expander init failed, cannot control speaker PA");
        return ESP_FAIL;
    }
    esp_err_t err = esp_io_expander_set_dir(expander, BSP_POWER_AMP_IO, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PA pin set_dir failed: %s", esp_err_to_name(err));
        return err;
    }
    return bsp_audio_poweramp_enable(enable);
}
