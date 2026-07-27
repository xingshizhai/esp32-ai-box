#include "sdkconfig.h"
#include "debug_screenshot.h"

#if CONFIG_UI_DEBUG_SCREENSHOT_ENABLED

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

/* Access LVGL private display struct to hook flush_cb */
#include "../../../../managed_components/lvgl__lvgl/src/display/lv_display_private.h"

#define TAG "DBGSHOT"

#define LCD_H_RES 320
#define LCD_V_RES 240
#define FRAME_BUF_SIZE (LCD_H_RES * LCD_V_RES * 2)

/* Full-screen mirror buffer (allocated in PSRAM) */
static uint8_t *s_frame_buf = NULL;
static SemaphoreHandle_t s_frame_mutex = NULL;
static lv_display_flush_cb_t s_orig_flush_cb = NULL;

/* Intercept LVGL flush to mirror pixel data into s_frame_buf */
static void mirror_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* Forward to original flush first */
    if (s_orig_flush_cb) {
        s_orig_flush_cb(disp, area, px_map);
    }

    if (s_frame_buf == NULL || px_map == NULL) return;

    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t x2 = area->x2 >= LCD_H_RES ? LCD_H_RES - 1 : area->x2;
    int32_t y2 = area->y2 >= LCD_V_RES ? LCD_V_RES - 1 : area->y2;

    int32_t src_row_w = area->x2 - area->x1 + 1;
    int32_t dst_row_w = x2 - x1 + 1;
    int32_t dst_rows  = y2 - y1 + 1;

    if (dst_row_w <= 0 || dst_rows <= 0) return;

    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int32_t r = 0; r < dst_rows; r++) {
            uint8_t *dst = s_frame_buf + ((y1 + r) * LCD_H_RES + x1) * 2;
            uint8_t *src = px_map + r * src_row_w * 2;
            memcpy(dst, src, (size_t)(dst_row_w * 2));
        }
        xSemaphoreGive(s_frame_mutex);
    }
}

/* ---- TCP helpers ---- */

typedef struct __attribute__((packed)) {
    uint16_t w;
    uint16_t h;
    uint16_t stride;
    uint16_t reserved;
    uint32_t data_len;
} screenshot_header_t;

static esp_err_t send_all(int sock, const void *data, size_t len)
{
    const uint8_t *ptr = (const uint8_t *)data;
    while (len > 0) {
        int n = send(sock, ptr, len, 0);
        if (n <= 0) return ESP_FAIL;
        ptr += n;
        len -= (size_t)n;
    }
    return ESP_OK;
}

static void handle_client(int sock)
{
    uint8_t cmd = 0;
    if (recv(sock, &cmd, 1, 0) != 1 || cmd != 'S') {
        ESP_LOGI(TAG, "bad command");
        return;
    }
    ESP_LOGI(TAG, "screenshot requested");

    if (s_frame_buf == NULL) {
        ESP_LOGW(TAG, "frame buffer not ready");
        return;
    }

    screenshot_header_t hdr = {
        .w        = LCD_H_RES,
        .h        = LCD_V_RES,
        .stride   = LCD_H_RES * 2,
        .reserved = 0,
        .data_len = FRAME_BUF_SIZE,
    };
    if (send_all(sock, &hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGW(TAG, "header send failed");
        return;
    }

    uint32_t chunk = LCD_H_RES * 16 * 2;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    if (!buf) { ESP_LOGW(TAG, "no mem for chunk"); return; }

    uint32_t sent = 0;
    while (sent < FRAME_BUF_SIZE) {
        uint32_t n = FRAME_BUF_SIZE - sent;
        if (n > chunk) n = chunk;

        if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(buf, s_frame_buf + sent, n);
            xSemaphoreGive(s_frame_mutex);
        } else {
            ESP_LOGW(TAG, "mutex timeout");
            break;
        }

        if (send_all(sock, buf, n) != ESP_OK) {
            ESP_LOGW(TAG, "send failed at %lu", sent);
            break;
        }
        sent += n;
    }

    ESP_LOGI(TAG, "sent %lu / %d bytes", sent, FRAME_BUF_SIZE);
    free(buf);
}

/* ---- TCP server task ---- */

static void screenshot_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "screenshot task started");

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) { ESP_LOGE(TAG, "socket: %d", errno); vTaskDelete(NULL); return; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons((uint16_t)CONFIG_UI_DEBUG_SCREENSHOT_PORT),
    };

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen: %d", errno);
        close(srv);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on port %d", CONFIG_UI_DEBUG_SCREENSHOT_PORT);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cs = accept(srv, (struct sockaddr *)&cli, &clen);
        if (cs < 0) { ESP_LOGW(TAG, "accept: %d", errno); continue; }
        ESP_LOGI(TAG, "client: %s", inet_ntoa(cli.sin_addr));
        handle_client(cs);
        close(cs);
    }
}

/* ---- Public API ---- */

esp_err_t debug_screenshot_start(void)
{
    /* Allocate mirror buffer in PSRAM */
    s_frame_buf = (uint8_t *)heap_caps_malloc(FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame_buf) {
        s_frame_buf = (uint8_t *)malloc(FRAME_BUF_SIZE);
    }
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "frame buf alloc failed (%d bytes)", FRAME_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    memset(s_frame_buf, 0, FRAME_BUF_SIZE);

    s_frame_mutex = xSemaphoreCreateMutex();
    if (!s_frame_mutex) {
        free(s_frame_buf); s_frame_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Hook LVGL flush callback */
    if (lvgl_port_lock(500)) {
        lv_display_t *disp = lv_display_get_default();
        if (disp) {
            s_orig_flush_cb = disp->flush_cb;
            lv_display_set_flush_cb(disp, mirror_flush_cb);
            /* Invalidate to trigger a full repaint into the mirror buffer */
            lv_obj_invalidate(lv_display_get_screen_active(disp));
            ESP_LOGI(TAG, "flush hook installed (orig=%p)", (void *)s_orig_flush_cb);
        }
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "could not acquire LVGL lock for flush hook");
    }

    if (xTaskCreatePinnedToCore(screenshot_task, "dbg_shot", 8192, NULL, 3, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started (frame buf %d bytes @ %p)", FRAME_BUF_SIZE, (void *)s_frame_buf);
    return ESP_OK;
}

#else

esp_err_t debug_screenshot_start(void) { return ESP_OK; }

#endif
