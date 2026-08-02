#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load a validated CJKF font and keep its file open for on-demand glyph IO. */
lv_font_t *cjk_font_create(const char *path, const lv_font_t *fallback);

/** Release all file, index and PSRAM cache resources. */
void cjk_font_destroy(lv_font_t *font);

/** Log cache/IO/missing-glyph counters for field diagnostics. */
void cjk_font_log_stats(const lv_font_t *font);

#ifdef __cplusplus
}
#endif
