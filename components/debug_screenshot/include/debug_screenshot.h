#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t debug_screenshot_start(void);
esp_err_t debug_screenshot_dump_serial(void);

#ifdef __cplusplus
}
#endif
