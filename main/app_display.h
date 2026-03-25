#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_display_init(void);
void *app_display_get_shared_i2c_bus(void);

#ifdef __cplusplus
}
#endif