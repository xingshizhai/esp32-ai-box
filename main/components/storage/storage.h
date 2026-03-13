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

#ifdef __cplusplus
}
#endif
