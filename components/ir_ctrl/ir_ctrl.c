#include "ir_ctrl.h"

#include "app_board_ir.h"
#include "device_tools.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "ir_ctrl";

#define IR_RESOLUTION_HZ    1000000 /* 1 tick = 1us */
#define IR_CARRIER_HZ       38000   /* standard IR carrier for the vast majority of remotes */
#define IR_LEARN_TIMEOUT_MS 10000

#define IR_CODE_NAME_MAX    24
#define IR_CODE_MAX_SYMBOLS 200 /* covers NEC/RC5/SIRC and most single-frame AC protocols */
#define IR_CODE_TABLE_MAX   16

#define IR_NVS_NAMESPACE "ir_codes"
#define IR_NVS_KEY       "table"

typedef struct {
    char name[IR_CODE_NAME_MAX];
    uint32_t carrier_hz;
    uint16_t symbol_count;
    rmt_symbol_word_t symbols[IR_CODE_MAX_SYMBOLS];
} ir_code_entry_t;

typedef struct {
    uint8_t count;
    ir_code_entry_t entries[IR_CODE_TABLE_MAX];
} ir_code_table_t;

static ir_code_table_t s_table;

static rmt_channel_handle_t s_tx_channel = NULL;
static rmt_channel_handle_t s_rx_channel = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;
static QueueHandle_t s_rx_queue = NULL;
static rmt_symbol_word_t s_rx_scratch[IR_CODE_MAX_SYMBOLS];

static const rmt_receive_config_t s_receive_config = {
    .signal_range_min_ns = 1000,     /* filter out sub-1us noise */
    .signal_range_max_ns = 20000000, /* 20ms -- tolerate inter-frame gaps without cutting off early */
};

/* ── NVS persistence ─────────────────────────────────────────────────── */

static void ir_table_load(void)
{
    memset(&s_table, 0, sizeof(s_table));

    nvs_handle_t h;
    if (nvs_open(IR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved IR codes found, starting empty");
        return;
    }

    size_t size = sizeof(s_table);
    esp_err_t err = nvs_get_blob(h, IR_NVS_KEY, &s_table, &size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load IR code table: %s", esp_err_to_name(err));
        memset(&s_table, 0, sizeof(s_table));
    } else {
        ESP_LOGI(TAG, "Loaded %d learned IR code(s)", s_table.count);
    }
    nvs_close(h);
}

static esp_err_t ir_table_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(IR_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, IR_NVS_KEY, &s_table, sizeof(s_table));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static int ir_table_find(const char *name)
{
    for (int i = 0; i < s_table.count; i++) {
        if (strncmp(s_table.entries[i].name, name, IR_CODE_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

/* ── RMT capture / replay ────────────────────────────────────────────── */

static bool IRAM_ATTR ir_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;
    xQueueSendFromISR(queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

esp_err_t ir_ctrl_learn(const char *name, uint32_t timeout_ms, size_t *out_symbol_count)
{
    if (s_rx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xQueueReset(s_rx_queue);
    esp_err_t err = rmt_receive(s_rx_channel, s_rx_scratch, sizeof(s_rx_scratch), &s_receive_config);
    if (err != ESP_OK) {
        return err;
    }

    rmt_rx_done_event_data_t rx_data;
    if (xQueueReceive(s_rx_queue, &rx_data, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (rx_data.num_symbols == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    int idx = ir_table_find(name);
    if (idx < 0) {
        if (s_table.count >= IR_CODE_TABLE_MAX) {
            return ESP_ERR_NO_MEM;
        }
        idx = s_table.count++;
    }

    ir_code_entry_t *entry = &s_table.entries[idx];
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->carrier_hz = IR_CARRIER_HZ;
    entry->symbol_count = (rx_data.num_symbols > IR_CODE_MAX_SYMBOLS) ? IR_CODE_MAX_SYMBOLS : rx_data.num_symbols;
    memcpy(entry->symbols, rx_data.received_symbols, entry->symbol_count * sizeof(rmt_symbol_word_t));

    if (out_symbol_count != NULL) {
        *out_symbol_count = entry->symbol_count;
    }
    return ir_table_save();
}

esp_err_t ir_ctrl_send(const char *name)
{
    if (s_tx_channel == NULL || s_copy_encoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int idx = ir_table_find(name);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ir_code_entry_t *entry = &s_table.entries[idx];
    rmt_transmit_config_t transmit_config = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_tx_channel, s_copy_encoder, entry->symbols,
                                  entry->symbol_count * sizeof(rmt_symbol_word_t), &transmit_config);
    if (err != ESP_OK) {
        return err;
    }
    return rmt_tx_wait_all_done(s_tx_channel, pdMS_TO_TICKS(1000));
}

esp_err_t ir_ctrl_forget(const char *name)
{
    int idx = ir_table_find(name);
    if (idx < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Swap-remove -- learned-code order is not user-visible. */
    s_table.entries[idx] = s_table.entries[s_table.count - 1];
    s_table.count--;
    return ir_table_save();
}

esp_err_t ir_ctrl_list(char *result, size_t result_size)
{
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < s_table.count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(s_table.entries[i].name));
    }
    bool ok = cJSON_PrintPreallocated(array, result, (int)result_size, false);
    cJSON_Delete(array);
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ── AI tool handlers ────────────────────────────────────────────────── */

static const char *ir_name_arg_schema =
    "{\"type\":\"object\","
    "\"properties\":{\"name\":{\"type\":\"string\","
    "\"description\":\"Short slug identifying the remote button, e.g. 'ac_power' or 'tv_vol_up'. "
    "Use the same slug again to replay or forget it.\"}},"
    "\"required\":[\"name\"]}";

static bool get_name_arg(const cJSON *arguments, const char **out_name)
{
    cJSON *name_json = cJSON_GetObjectItem(arguments, "name");
    if (!cJSON_IsString(name_json) || name_json->valuestring[0] == '\0') {
        return false;
    }
    *out_name = name_json->valuestring;
    return true;
}

static esp_err_t tool_ir_learn_code(const cJSON *arguments, char *result, size_t result_size)
{
    const char *name;
    if (!get_name_arg(arguments, &name)) {
        snprintf(result, result_size, "{\"error\":\"missing string 'name'\"}");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ir_ctrl_learn(name, IR_LEARN_TIMEOUT_MS, NULL);
    switch (err) {
    case ESP_OK:
        snprintf(result, result_size, "{\"learned\":\"%s\"}", name);
        return ESP_OK;
    case ESP_ERR_TIMEOUT:
        snprintf(result, result_size, "{\"error\":\"no infrared signal detected within %ds\"}", IR_LEARN_TIMEOUT_MS / 1000);
        return err;
    case ESP_ERR_NO_MEM:
        snprintf(result, result_size, "{\"error\":\"learned-code table full (max %d), forget one first\"}", IR_CODE_TABLE_MAX);
        return err;
    default:
        snprintf(result, result_size, "{\"error\":\"ir receiver not available\"}");
        return err;
    }
}

static esp_err_t tool_ir_send_code(const cJSON *arguments, char *result, size_t result_size)
{
    const char *name;
    if (!get_name_arg(arguments, &name)) {
        snprintf(result, result_size, "{\"error\":\"missing string 'name'\"}");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ir_ctrl_send(name);
    switch (err) {
    case ESP_OK:
        snprintf(result, result_size, "{\"sent\":\"%s\"}", name);
        return ESP_OK;
    case ESP_ERR_NOT_FOUND:
        snprintf(result, result_size, "{\"error\":\"no code named '%s' has been learned\"}", name);
        return err;
    default:
        snprintf(result, result_size, "{\"error\":\"ir transmitter not available\"}");
        return err;
    }
}

static esp_err_t tool_ir_forget_code(const cJSON *arguments, char *result, size_t result_size)
{
    const char *name;
    if (!get_name_arg(arguments, &name)) {
        snprintf(result, result_size, "{\"error\":\"missing string 'name'\"}");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ir_ctrl_forget(name);
    if (err == ESP_OK) {
        snprintf(result, result_size, "{\"forgot\":\"%s\"}", name);
        return ESP_OK;
    }
    snprintf(result, result_size, "{\"error\":\"no code named '%s' has been learned\"}", name);
    return err;
}

static esp_err_t tool_ir_list_codes(const cJSON *arguments, char *result, size_t result_size)
{
    (void)arguments;
    return ir_ctrl_list(result, result_size);
}

static esp_err_t ir_register_tools(void)
{
    device_tool_t learn_tool = {
        .name = "ir_learn_code",
        .description =
            "Learn (capture) one infrared remote button code so it can be replayed later to control an "
            "appliance. IMPORTANT: before calling this, first tell the user out loud to point their "
            "original remote at the device and press the button now -- this call blocks and waits up to "
            "10 seconds for an infrared signal. Only call it after the user has confirmed they are ready.",
        .parameters_schema_json = ir_name_arg_schema,
        .handler = tool_ir_learn_code,
    };
    device_tool_t send_tool = {
        .name = "ir_send_code",
        .description =
            "Replay a previously learned infrared remote code by name to control an appliance (e.g. turn "
            "an air conditioner, TV, or fan on/off). Fails if that name hasn't been learned yet -- use "
            "ir_list_codes to check what's available, or ir_learn_code to learn it first.",
        .parameters_schema_json = ir_name_arg_schema,
        .handler = tool_ir_send_code,
    };
    device_tool_t list_tool = {
        .name = "ir_list_codes",
        .description = "List the names of all infrared remote codes learned so far.",
        .parameters_schema_json = NULL,
        .handler = tool_ir_list_codes,
    };
    device_tool_t forget_tool = {
        .name = "ir_forget_code",
        .description = "Delete a previously learned infrared remote code by name, e.g. to re-learn it.",
        .parameters_schema_json = ir_name_arg_schema,
        .handler = tool_ir_forget_code,
    };

    esp_err_t err;
    if ((err = device_tools_register(&learn_tool)) != ESP_OK) return err;
    if ((err = device_tools_register(&send_tool)) != ESP_OK) return err;
    if ((err = device_tools_register(&list_tool)) != ESP_OK) return err;
    if ((err = device_tools_register(&forget_tool)) != ESP_OK) return err;
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

esp_err_t ir_ctrl_init(void)
{
    app_board_ir_config_t board_cfg;
    esp_err_t err = app_board_get_ir_config(&board_cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (board_cfg.ir_tx_gpio < 0 && board_cfg.ir_rx_gpio < 0) {
        ESP_LOGW(TAG, "No IR GPIOs configured for this board -- IR control disabled");
        return ESP_ERR_INVALID_STATE;
    }

    ir_table_load();

    if (board_cfg.ir_rx_gpio >= 0) {
        rmt_rx_channel_config_t rx_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = IR_RESOLUTION_HZ,
            .mem_block_symbols = 64,
            .gpio_num = board_cfg.ir_rx_gpio,
        };
        err = rmt_new_rx_channel(&rx_cfg, &s_rx_channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
            s_rx_channel = NULL;
        } else {
            s_rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
            rmt_rx_event_callbacks_t cbs = { .on_recv_done = ir_rx_done_callback };
            rmt_rx_register_event_callbacks(s_rx_channel, &cbs, s_rx_queue);
            rmt_enable(s_rx_channel);
            ESP_LOGI(TAG, "IR receiver ready on GPIO%d", board_cfg.ir_rx_gpio);
        }
    }

    if (board_cfg.ir_tx_gpio >= 0) {
        rmt_tx_channel_config_t tx_cfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = IR_RESOLUTION_HZ,
            .mem_block_symbols = 64,
            .trans_queue_depth = 4,
            .gpio_num = board_cfg.ir_tx_gpio,
        };
        err = rmt_new_tx_channel(&tx_cfg, &s_tx_channel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
            s_tx_channel = NULL;
        } else {
            rmt_carrier_config_t carrier_cfg = { .duty_cycle = 0.33, .frequency_hz = IR_CARRIER_HZ };
            rmt_apply_carrier(s_tx_channel, &carrier_cfg);

            rmt_copy_encoder_config_t copy_cfg = {};
            rmt_new_copy_encoder(&copy_cfg, &s_copy_encoder);

            rmt_enable(s_tx_channel);
            ESP_LOGI(TAG, "IR transmitter ready on GPIO%d", board_cfg.ir_tx_gpio);
        }
    }

    return ir_register_tools();
}
