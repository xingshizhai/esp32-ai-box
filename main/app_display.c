#include "app_display.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static const char *TAG = "app_display";

typedef struct {
    i2c_master_bus_handle_t shared_i2c_bus;
    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t lcd_panel;
    esp_lcd_touch_handle_t touch_handle;
} app_display_state_t;

static app_display_state_t s_display = {0};

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
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_display.lcd_panel, 0, y, LCD_H_RES, y + 1, line), TAG, "Draw red band failed");
    }

    for (int x = 0; x < LCD_H_RES; x++) {
        line[x] = 0x07E0;
    }
    for (int y = LCD_V_RES / 3; y < (LCD_V_RES * 2) / 3; y++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_display.lcd_panel, 0, y, LCD_H_RES, y + 1, line), TAG, "Draw green band failed");
    }

    for (int x = 0; x < LCD_H_RES; x++) {
        line[x] = 0x001F;
    }
    for (int y = (LCD_V_RES * 2) / 3; y < LCD_V_RES; y++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_display.lcd_panel, 0, y, LCD_H_RES, y + 1, line), TAG, "Draw blue band failed");
    }

    return ESP_OK;
}

esp_err_t app_display_init(void)
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

    if (s_display.shared_i2c_bus != NULL) {
        i2c_handle = s_display.shared_i2c_bus;
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
            s_display.shared_i2c_bus = i2c_handle;
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
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_display.lcd_io), err, TAG, "New panel IO failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_GPIO_RST,
        .flags = {
            .reset_active_high = 1,
        },
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
    };
    if (use_st7789) {
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(s_display.lcd_io, &panel_config, &s_display.lcd_panel), err, TAG, "New ST7789 panel failed");
    } else {
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = ili9341_lcd_init_vendor,
            .init_cmds_size = sizeof(ili9341_lcd_init_vendor) / sizeof(ili9341_lcd_init_vendor[0]),
        };
        esp_lcd_panel_dev_config_t ili9341_panel_config = panel_config;
        ili9341_panel_config.vendor_config = (void *)&vendor_config;
        ESP_GOTO_ON_ERROR(esp_lcd_new_panel_ili9341(s_display.lcd_io, &ili9341_panel_config, &s_display.lcd_panel), err, TAG, "New ILI9341 panel failed");
    }

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(s_display.lcd_panel), err, TAG, "Panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(s_display.lcd_panel), err, TAG, "Panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_mirror(s_display.lcd_panel, true, true), err, TAG, "Panel mirror failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(s_display.lcd_panel, true), err, TAG, "Panel on failed");
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
                    touch_err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_display.touch_handle);
                } else if (use_st7789) {
                    touch_err = esp_lcd_touch_new_i2c_tt21100(tp_io_handle, &tp_cfg, &s_display.touch_handle);
                } else {
                    touch_err = ESP_ERR_NOT_FOUND;
                }
            }

            if (touch_err != ESP_OK) {
                ESP_LOGW(TAG, "Display init: touch init failed (%s), continue without touch", esp_err_to_name(touch_err));
                s_display.touch_handle = NULL;
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
        .io_handle = s_display.lcd_io,
        .panel_handle = s_display.lcd_panel,
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

    if (s_display.touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = s_display.touch_handle,
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

void *app_display_get_shared_i2c_bus(void)
{
    return s_display.shared_i2c_bus;
}