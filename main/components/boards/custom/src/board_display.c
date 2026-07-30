/* board_display.c — custom board skeleton
 *
 * Selected when "Target board" = "Custom board (user-defined)" in
 * menuconfig. Fill in the TODOs below to wire up your own LCD panel and
 * touch controller. The only contract with the rest of the firmware is the
 * four functions declared in app_display.h (board_iface component):
 *
 *   esp_err_t app_display_init(void);
 *   void     *app_display_get_shared_i2c_bus(void);
 *   esp_err_t app_display_set_brightness(int percent);
 *   esp_err_t app_display_enable_speaker_amp(bool enable);
 *
 * If your board has no display at all, app_display_init() can just return
 * ESP_OK without creating any LVGL display — the caller (main.c) already
 * tolerates display init failure and continues without UI.
 */

#include "app_display.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"

/* TODO: include your LCD panel / touch driver headers, e.g.:
 *   #include "esp_lcd_panel_io.h"
 *   #include "esp_lcd_ili9341.h"
 *   #include "esp_lcd_touch_gt911.h"
 *   #include "driver/i2c_master.h"
 *   #include "driver/spi_master.h"
 */

static const char *TAG = "app_display";

/* TODO: set to your panel's resolution. */
#define CUSTOM_LCD_H_RES 320
#define CUSTOM_LCD_V_RES 240

esp_err_t app_display_init(void)
{
    ESP_LOGW(TAG, "Custom board: app_display_init() is a skeleton — "
                  "fill in main/components/boards/custom/src/board_display.c");

    /* TODO: bring up your I2C bus (if any), LCD panel and touch controller
     * here, following the pattern in
     * main/components/boards/esp32s3_box3/src/board_display.c or
     * main/components/boards/esp32s3_lcd_ev_board/src/board_display.c, e.g.:
     *
     *   lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
     *   lvgl_port_init(&lvgl_cfg);
     *
     *   esp_lcd_panel_io_handle_t io_handle = NULL;
     *   esp_lcd_panel_handle_t panel_handle = NULL;
     *   // ... create io_handle / panel_handle for your panel ...
     *
     *   const lvgl_port_display_cfg_t disp_cfg = {
     *       .io_handle = io_handle,
     *       .panel_handle = panel_handle,
     *       .buffer_size = CUSTOM_LCD_H_RES * 40,
     *       .double_buffer = true,
     *       .hres = CUSTOM_LCD_H_RES,
     *       .vres = CUSTOM_LCD_V_RES,
     *       .flags = { .buff_dma = true, .buff_spiram = true },
     *   };
     *   lvgl_port_add_disp(&disp_cfg);
     *
     *   // ... then bsp/driver-specific touch init + lvgl_port_add_touch() ...
     */

    return ESP_OK;
}

void *app_display_get_shared_i2c_bus(void)
{
    /* TODO: if your display/touch owns an I2C bus that the audio codecs
     * should also share (see main/components/audio/audio.c), return its
     * i2c_master_bus_handle_t here. Returning NULL (default) makes
     * audio_init() create its own I2C bus from the "Audio Hardware
     * Configuration" Kconfig pins instead. */
    return NULL;
}

esp_err_t app_display_set_brightness(int percent)
{
    /* TODO: drive backlight PWM/GPIO if your panel supports it. */
    (void)percent;
    return ESP_OK;
}

esp_err_t app_display_enable_speaker_amp(bool enable)
{
    /* TODO: only needed if your PA enable line isn't a raw GPIO already
     * handled by CONFIG_AUDIO_PA_GPIO in the audio component. */
    (void)enable;
    return ESP_OK;
}
