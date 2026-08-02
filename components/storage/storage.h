#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t storage_sdcard_mount(void);
esp_err_t storage_sdcard_unmount(void);
bool storage_sdcard_is_mounted(void);
const char *storage_sdcard_get_mount_point(void);
esp_err_t storage_sdcard_find_first_mp3(char *out_path, size_t out_path_len);

/* "storage" SPIFFS partition: holds assets too large for the app partition
 * (e.g. the full-coverage CJK font binary loaded by the UI component). */
esp_err_t storage_spiffs_mount(void);
bool storage_spiffs_is_mounted(void);
const char *storage_spiffs_get_mount_point(void);

#ifdef __cplusplus
}
#endif
