#include "app_display.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
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
    ESP_LOGI(TAG, "Display init: begin (esp-box-3 BSP path)");

    /* I2C first — shared by touch + audio codecs on BOX-3. */
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");

    /* Brightness PWM must be ready before backlight on. */
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness_init failed");

    /* Init LVGL port task/timer before adding display/touch devices. */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    bsp_display_config_t bsp_disp_cfg = {
        .max_transfer_sz = (BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT) * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(bsp_display_new(&bsp_disp_cfg, &s_display.lcd_panel, &s_display.lcd_io),
                        TAG, "bsp_display_new failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_display.lcd_panel, true), TAG, "panel on failed");

    const uint32_t buf_px = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT;
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_display.lcd_io,
        .panel_handle = s_display.lcd_panel,
        .buffer_size = buf_px,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
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
            .buff_spiram = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
#endif
        },
    };

    s_display.lv_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_display.lv_disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp failed");

    /* Touch via official BSP probe (TT21100 / GT911 0x5D / GT911 0x14). */
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

    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight on failed");
    ESP_LOGI(TAG, "Display init: success %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

void *app_display_get_shared_i2c_bus(void)
{
    return (void *)bsp_i2c_get_handle();
}
