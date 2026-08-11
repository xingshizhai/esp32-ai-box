#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_TOOL_MAX_RESULT 256

/* Handler writes a short human/model-readable result string (plain text or
 * JSON, either is fine as tool-result content) into result/result_size. */
typedef esp_err_t (*device_tool_fn_t)(const cJSON *arguments, char *result, size_t result_size);

typedef struct {
    const char *name;
    const char *description;
    /* Literal JSON Schema "parameters" object, e.g.
     * "{\"type\":\"object\",\"properties\":{...},\"required\":[...]}" */
    const char *parameters_schema_json;
    device_tool_fn_t handler;
} device_tool_t;

/* Registers the built-in device tools (set_volume, set_screen_brightness).
 * Safe to call once during startup. */
esp_err_t device_tools_init(void);

esp_err_t device_tools_register(const device_tool_t *tool);

/* Returns a cached OpenAI-style "tools" JSON array string, e.g.
 * [{"type":"function","function":{"name":...,"description":...,"parameters":{...}}}]
 * NULL if no tools are registered. Owned by this component -- do not free. */
const char *device_tools_get_openai_tools_json(void);

/* Looks up `name`, parses arguments_json, and invokes its handler. On
 * "unknown tool" or handler failure, writes a short JSON error string into
 * result instead of failing silently, so the model gets a graceful signal. */
esp_err_t device_tools_call(const char *name, const char *arguments_json, char *result, size_t result_size);

#ifdef __cplusplus
}
#endif
