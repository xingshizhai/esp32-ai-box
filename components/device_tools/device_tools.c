#include "device_tools.h"
#include "audio.h"
#include "app_display.h"

#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "device_tools";

#define DEVICE_TOOLS_MAX 8

static device_tool_t s_tools[DEVICE_TOOLS_MAX];
static int s_tool_count = 0;

static char *s_tools_json_cache = NULL;

static void invalidate_tools_json_cache(void)
{
    if (s_tools_json_cache != NULL) {
        cJSON_free(s_tools_json_cache);
        s_tools_json_cache = NULL;
    }
}

esp_err_t device_tools_register(const device_tool_t *tool)
{
    if (tool == NULL || tool->name == NULL || tool->handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tool_count >= DEVICE_TOOLS_MAX) {
        ESP_LOGE(TAG, "Tool registry full, cannot register '%s'", tool->name);
        return ESP_ERR_NO_MEM;
    }

    s_tools[s_tool_count++] = *tool;
    invalidate_tools_json_cache();
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
    return ESP_OK;
}

const char *device_tools_get_openai_tools_json(void)
{
    if (s_tool_count == 0) {
        return NULL;
    }
    if (s_tools_json_cache != NULL) {
        return s_tools_json_cache;
    }

    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < s_tool_count; i++) {
        const device_tool_t *tool = &s_tools[i];

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "type", "function");

        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", tool->name);
        cJSON_AddStringToObject(fn, "description", tool->description ? tool->description : "");

        cJSON *params = tool->parameters_schema_json ? cJSON_Parse(tool->parameters_schema_json) : NULL;
        if (params == NULL) {
            /* No/invalid schema -- fall back to "no parameters". */
            params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "type", "object");
            cJSON_AddItemToObject(params, "properties", cJSON_CreateObject());
        }
        cJSON_AddItemToObject(fn, "parameters", params);

        cJSON_AddItemToObject(entry, "function", fn);
        cJSON_AddItemToArray(array, entry);
    }

    s_tools_json_cache = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return s_tools_json_cache;
}

esp_err_t device_tools_call(const char *name, const char *arguments_json, char *result, size_t result_size)
{
    if (name == NULL || result == NULL || result_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    result[0] = '\0';

    const device_tool_t *tool = NULL;
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            tool = &s_tools[i];
            break;
        }
    }
    if (tool == NULL) {
        ESP_LOGW(TAG, "Unknown tool requested: %s", name);
        snprintf(result, result_size, "{\"error\":\"unknown tool: %s\"}", name);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *args = (arguments_json != NULL) ? cJSON_Parse(arguments_json) : NULL;
    cJSON *args_or_empty = args;
    if (args_or_empty == NULL) {
        /* Parse failure or no arguments -- hand the handler an empty object
         * rather than NULL, so simple handlers don't need a NULL check. */
        args_or_empty = cJSON_CreateObject();
    }

    esp_err_t err = tool->handler(args_or_empty, result, result_size);
    if (err != ESP_OK && result[0] == '\0') {
        snprintf(result, result_size, "{\"error\":\"tool '%s' failed\"}", name);
    }

    if (args != NULL) {
        cJSON_Delete(args);
    } else {
        cJSON_Delete(args_or_empty);
    }
    return err;
}

/* ── Built-in tools ──────────────────────────────────────────────────── */

static esp_err_t tool_set_volume(const cJSON *arguments, char *result, size_t result_size)
{
    cJSON *level_json = cJSON_GetObjectItem(arguments, "level");
    if (!cJSON_IsNumber(level_json)) {
        snprintf(result, result_size, "{\"error\":\"missing integer 'level'\"}");
        return ESP_ERR_INVALID_ARG;
    }

    int level = level_json->valueint;
    esp_err_t err = audio_set_volume(level);
    if (err != ESP_OK) {
        snprintf(result, result_size, "{\"error\":\"level must be 0-100\"}");
        return err;
    }

    snprintf(result, result_size, "{\"volume\":%d}", level);
    return ESP_OK;
}

static esp_err_t tool_set_screen_brightness(const cJSON *arguments, char *result, size_t result_size)
{
    cJSON *level_json = cJSON_GetObjectItem(arguments, "level");
    if (!cJSON_IsNumber(level_json)) {
        snprintf(result, result_size, "{\"error\":\"missing integer 'level'\"}");
        return ESP_ERR_INVALID_ARG;
    }

    int level = level_json->valueint;
    esp_err_t err = app_display_set_brightness(level);
    if (err != ESP_OK) {
        snprintf(result, result_size, "{\"error\":\"failed to set brightness\"}");
        return err;
    }

    snprintf(result, result_size, "{\"brightness\":%d}", level);
    return ESP_OK;
}

esp_err_t device_tools_init(void)
{
    static const char *level_0_100_schema =
        "{\"type\":\"object\","
        "\"properties\":{\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
        "\"required\":[\"level\"]}";

    device_tool_t set_volume = {
        .name = "set_volume",
        .description = "Set the speaker playback volume, from 0 (silent) to 100 (max).",
        .parameters_schema_json = level_0_100_schema,
        .handler = tool_set_volume,
    };
    device_tool_t set_brightness = {
        .name = "set_screen_brightness",
        .description = "Set the screen backlight brightness, from 0 (dim) to 100 (max). "
                        "Has no visible effect on boards without a dimmable backlight.",
        .parameters_schema_json = level_0_100_schema,
        .handler = tool_set_screen_brightness,
    };

    esp_err_t err = device_tools_register(&set_volume);
    if (err != ESP_OK) {
        return err;
    }
    return device_tools_register(&set_brightness);
}
