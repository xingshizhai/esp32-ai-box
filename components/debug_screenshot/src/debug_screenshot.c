#include "sdkconfig.h"
#include "debug_screenshot.h"

#if CONFIG_UI_DEBUG_SCREENSHOT_ENABLED

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
#include "src/display/lv_display_private.h"

#define TAG "DBGSHOT"

/* Full-screen mirror buffer (allocated in PSRAM) */
static uint8_t *s_frame_buf = NULL;
static uint16_t s_frame_width = 0;
static uint16_t s_frame_height = 0;
static size_t s_frame_buf_size = 0;
static SemaphoreHandle_t s_frame_mutex = NULL;
static lv_display_flush_cb_t s_orig_flush_cb = NULL;
static TaskHandle_t s_server_task = NULL;
static SemaphoreHandle_t s_startup_sem = NULL;
static esp_err_t s_startup_result = ESP_FAIL;

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
    int32_t x2 = area->x2 >= s_frame_width ? s_frame_width - 1 : area->x2;
    int32_t y2 = area->y2 >= s_frame_height ? s_frame_height - 1 : area->y2;

    int32_t src_row_w = area->x2 - area->x1 + 1;
    int32_t dst_row_w = x2 - x1 + 1;
    int32_t dst_rows  = y2 - y1 + 1;

    if (dst_row_w <= 0 || dst_rows <= 0) return;

    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int32_t r = 0; r < dst_rows; r++) {
            uint8_t *dst = s_frame_buf + ((y1 + r) * s_frame_width + x1) * 2;
            uint8_t *src = px_map + (((y1 - area->y1) + r) * src_row_w +
                                     (x1 - area->x1)) * 2;
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
        .w        = s_frame_width,
        .h        = s_frame_height,
        .stride   = s_frame_width * 2,
        .reserved = 0,
        .data_len = s_frame_buf_size,
    };
    if (send_all(sock, &hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGW(TAG, "header send failed");
        return;
    }

    uint32_t chunk = s_frame_width * 16 * 2;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    if (!buf) { ESP_LOGW(TAG, "no mem for chunk"); return; }

    uint32_t sent = 0;
    while (sent < s_frame_buf_size) {
        uint32_t n = s_frame_buf_size - sent;
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

    ESP_LOGI(TAG, "sent %lu / %u bytes", sent, (unsigned)s_frame_buf_size);
    free(buf);
}

/* ---- TCP server task ---- */

static void screenshot_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "screenshot task started");

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) {
        ESP_LOGE(TAG, "socket: %d", errno);
        s_startup_result = ESP_FAIL;
        xSemaphoreGive(s_startup_sem);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

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
        s_startup_result = ESP_FAIL;
        xSemaphoreGive(s_startup_sem);
        s_server_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on port %d", CONFIG_UI_DEBUG_SCREENSHOT_PORT);
    s_startup_result = ESP_OK;
    xSemaphoreGive(s_startup_sem);

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
    if (s_server_task != NULL) {
        return ESP_OK;
    }

    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) return ESP_ERR_INVALID_STATE;
    s_frame_width = (uint16_t)lv_display_get_horizontal_resolution(disp);
    s_frame_height = (uint16_t)lv_display_get_vertical_resolution(disp);
    s_frame_buf_size = (size_t)s_frame_width * s_frame_height * 2;
    if (s_frame_width == 0 || s_frame_height == 0 || s_frame_buf_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Allocate mirror buffer in PSRAM */
    s_frame_buf = (uint8_t *)heap_caps_malloc(s_frame_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame_buf) {
        s_frame_buf = (uint8_t *)malloc(s_frame_buf_size);
    }
    if (!s_frame_buf) {
        ESP_LOGE(TAG, "frame buf alloc failed (%u bytes)", (unsigned)s_frame_buf_size);
        return ESP_ERR_NO_MEM;
    }
    memset(s_frame_buf, 0, s_frame_buf_size);

    s_frame_mutex = xSemaphoreCreateMutex();
    if (!s_frame_mutex) {
        free(s_frame_buf); s_frame_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_startup_sem = xSemaphoreCreateBinary();
    if (s_startup_sem == NULL) {
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
        free(s_frame_buf);
        s_frame_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Hook LVGL flush callback */
    if (lvgl_port_lock(500)) {
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

    s_startup_result = ESP_FAIL;
    if (xTaskCreatePinnedToCore(screenshot_task, "dbg_shot", 8192, NULL, 3,
                                &s_server_task, 0) != pdPASS) {
        vSemaphoreDelete(s_startup_sem);
        s_startup_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_startup_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "server startup timed out");
        return ESP_ERR_TIMEOUT;
    }
    if (s_startup_result != ESP_OK) {
        return s_startup_result;
    }

    ESP_LOGI(TAG, "started (%ux%u, frame buf %u bytes @ %p)",
             s_frame_width, s_frame_height, (unsigned)s_frame_buf_size, (void *)s_frame_buf);
    return ESP_OK;
}

esp_err_t debug_screenshot_dump_serial(void)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (s_frame_buf == NULL || s_frame_mutex == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t *snapshot = heap_caps_malloc(s_frame_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (snapshot == NULL) snapshot = malloc(s_frame_buf_size);
    if (snapshot == NULL) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        free(snapshot);
        return ESP_ERR_TIMEOUT;
    }
    memcpy(snapshot, s_frame_buf, s_frame_buf_size);
    xSemaphoreGive(s_frame_mutex);

    printf("\n<<<LVGLSHOT %u %u %u>>>\n", s_frame_width, s_frame_height,
           (unsigned)s_frame_buf_size);
    char line[257];
    size_t line_len = 0;
    for (size_t i = 0; i < s_frame_buf_size; i += 3) {
        uint32_t value = (uint32_t)snapshot[i] << 16;
        size_t remain = s_frame_buf_size - i;
        if (remain > 1) value |= (uint32_t)snapshot[i + 1] << 8;
        if (remain > 2) value |= snapshot[i + 2];
        line[line_len++] = b64[(value >> 18) & 0x3f];
        line[line_len++] = b64[(value >> 12) & 0x3f];
        line[line_len++] = remain > 1 ? b64[(value >> 6) & 0x3f] : '=';
        line[line_len++] = remain > 2 ? b64[value & 0x3f] : '=';
        if (line_len == 256) {
            line[line_len] = '\0';
            printf("%s\n", line);
            line_len = 0;
        }
    }
    if (line_len != 0) {
        line[line_len] = '\0';
        printf("%s\n", line);
    }
    printf("<<<LVGLSHOT END>>>\n");
    fflush(stdout);
    free(snapshot);
    return ESP_OK;
}

#else

esp_err_t debug_screenshot_start(void) { return ESP_OK; }
esp_err_t debug_screenshot_dump_serial(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif
