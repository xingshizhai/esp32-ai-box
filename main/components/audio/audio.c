#include "audio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "audio";

static uint8_t normalize_codec_i2c_addr(uint32_t cfg_addr)
{
    /* esp_codec_dev I2C ctrl expects 8-bit codec address (e.g. 0x30 for ES8311).
     * Keep compatibility with Kconfig 7-bit values by auto-shifting.
     */
    if (cfg_addr <= 0x7F) {
        return (uint8_t)(cfg_addr << 1);
    }
    return (uint8_t)cfg_addr;
}

/* ── Callbacks ──────────────────────────────────────────────────────── */
static audio_stt_callback_t               s_stt_callback       = NULL;
static audio_playback_complete_callback_t s_playback_callback  = NULL;
static audio_mic_level_callback_t         s_mic_level_callback = NULL;

/* ── Audio constants ─────────────────────────────────────────────────── */
#define SAMPLE_RATE          16000
#define BYTES_PER_SAMPLE     2          /* 16-bit sample */
#define MIC_CHANNELS         2          /* Match esp-box capture path */
#define MONITOR_INTERVAL_MS  50
#define MONITOR_BUF_FRAMES   (SAMPLE_RATE * MONITOR_INTERVAL_MS / 1000)
#define MONITOR_BUF_SAMPLES  (MONITOR_BUF_FRAMES * MIC_CHANNELS)
#define MIC_GAIN_DB          20.0f
#define MIC_GAIN_MASK        (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
#define DEBUG_TONE_FREQ_HZ   440.0f
#define DEBUG_TONE_AMP       12000.0f
#define DEBUG_GAIN_TARGET    26000.0f
#define DEBUG_GAIN_MIN_PEAK  80
#define DEBUG_GAIN_MAX       8.0f

/* ── Hardware handles ────────────────────────────────────────────────── */
static i2s_chan_handle_t                s_tx_handle      = NULL;
static i2s_chan_handle_t                s_rx_handle      = NULL;
static esp_codec_dev_handle_t           s_spk_dev        = NULL;
static esp_codec_dev_handle_t           s_mic_dev        = NULL;
static const audio_codec_data_if_t     *s_data_if        = NULL;
static const audio_codec_ctrl_if_t     *s_spk_ctrl_if    = NULL;
static const audio_codec_ctrl_if_t     *s_mic_ctrl_if    = NULL;
static const audio_codec_gpio_if_t     *s_gpio_if        = NULL;
static const audio_codec_if_t          *s_spk_codec_if   = NULL;
static const audio_codec_if_t          *s_mic_codec_if   = NULL;
static i2c_master_bus_handle_t          s_codec_i2c_bus  = NULL;
static bool                             s_i2c_bus_owned  = false;
static bool                             s_hw_ready       = false;
static bool                             s_mic_muted      = false;
static esp_codec_dev_sample_info_t      s_mic_fs_cfg     = {0};

static void fill_debug_tone(int16_t *buf, int frames)
{
    if (buf == NULL || frames <= 0) {
        return;
    }

    for (int i = 0; i < frames; i++) {
        float t = (float)i / SAMPLE_RATE;
        buf[i] = (int16_t)(DEBUG_TONE_AMP * sinf(2.0f * (float)M_PI * DEBUG_TONE_FREQ_HZ * t));
    }
}

/* ── Playback task ───────────────────────────────────────────────────── */
typedef struct {
    uint8_t *data;
    int      len;
    bool     auto_free;
} play_req_t;

static QueueHandle_t  s_play_queue  = NULL;
static TaskHandle_t   s_play_task   = NULL;

static void play_task(void *arg)
{
    play_req_t req;
    while (1) {
        if (xQueueReceive(s_play_queue, &req, portMAX_DELAY) == pdTRUE) {
            if (req.data == NULL) {
                /* poison-pill: stop task */
                break;
            }
            if (s_spk_dev != NULL) {
                ESP_LOGI(TAG, "play_task: writing %d bytes to speaker", req.len);
                int ret = esp_codec_dev_write(s_spk_dev, req.data, req.len);
                if (ret != ESP_CODEC_DEV_OK) {
                    ESP_LOGW(TAG, "Playback write error: %d", ret);
                } else {
                    ESP_LOGI(TAG, "Playback OK: %d bytes written", req.len);
                }
            }
            if (req.auto_free && req.data != NULL) {
                free(req.data);
            }
            if (s_playback_callback != NULL) {
                s_playback_callback();
            }
        }
    }
    vTaskDelete(NULL);
}

/* ── Monitor (mic level) task ────────────────────────────────────────── */
static bool         s_is_monitoring = false;
static TaskHandle_t s_monitor_task  = NULL;
static bool         s_stream_capture_active = false;
static bool         s_stream_capture_resume_monitor = false;
static int16_t     *s_stream_raw_buf = NULL;
static int          s_stream_raw_buf_bytes = 0;
static int          s_stream_selected_ch = -1;

static esp_err_t audio_stream_ensure_raw_buffer(int required_bytes)
{
    if (required_bytes <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_stream_raw_buf != NULL && s_stream_raw_buf_bytes >= required_bytes) {
        return ESP_OK;
    }

    int16_t *new_buf = realloc(s_stream_raw_buf, required_bytes);
    if (new_buf == NULL) {
        ESP_LOGE(TAG, "Stream capture buffer alloc failed (%d bytes)", required_bytes);
        return ESP_ERR_NO_MEM;
    }

    s_stream_raw_buf = new_buf;
    s_stream_raw_buf_bytes = required_bytes;
    return ESP_OK;
}

static esp_err_t audio_refresh_mic_input(void)
{
    if (!s_hw_ready || s_mic_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int rc = esp_codec_dev_close(s_mic_dev);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Mic close before refresh returned %d", rc);
    }

    rc = esp_codec_dev_open(s_mic_dev, &s_mic_fs_cfg);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Mic reopen failed: %d", rc);
        return ESP_FAIL;
    }

    rc = esp_codec_dev_set_in_mute(s_mic_dev, s_mic_muted);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Mic mute apply failed: %d", rc);
    }

    rc = esp_codec_dev_set_in_channel_gain(s_mic_dev, MIC_GAIN_MASK, MIC_GAIN_DB);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Mic channel gain set failed: %d, fallback to global gain", rc);
        esp_codec_dev_set_in_gain(s_mic_dev, MIC_GAIN_DB);
    }

    return ESP_OK;
}

static void monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Mic monitor task started");
    int16_t *buf = malloc(MONITOR_BUF_SAMPLES * BYTES_PER_SAMPLE);
    int debug_log_div = 0;
    while (s_is_monitoring) {
        int level = 0;
        if (s_hw_ready && s_mic_dev != NULL && buf != NULL) {
            int bytes = MONITOR_BUF_SAMPLES * BYTES_PER_SAMPLE;
            int ret = esp_codec_dev_read(s_mic_dev, buf, bytes);
            if (ret == ESP_CODEC_DEV_OK) {
                /* Use stronger sample from stereo pair to avoid channel-side bias. */
                long long sum = 0;
                int peak_l = 0;
                int peak_r = 0;
                for (int i = 0; i < MONITOR_BUF_FRAMES; i++) {
                    int l = buf[i * MIC_CHANNELS + 0];
                    int r = buf[i * MIC_CHANNELS + 1];
                    int abs_l = (l < 0) ? -l : l;
                    int abs_r = (r < 0) ? -r : r;
                    if (abs_l > peak_l) {
                        peak_l = abs_l;
                    }
                    if (abs_r > peak_r) {
                        peak_r = abs_r;
                    }
                    int s = (abs_l >= abs_r) ? l : r;
                    sum += (long long)s * s;
                }
                double rms = sqrt((double)sum / MONITOR_BUF_FRAMES);
                level = (int)(rms * 100.0 / 32768.0);
                if (level == 0 && (peak_l > 300 || peak_r > 300)) {
                    /* Avoid UI appearing frozen when signal is small but non-zero. */
                    level = 1;
                }
                if (level > 100) {
                    level = 100;
                }

                if ((debug_log_div++ % 20) == 0) {
                    ESP_LOGI(TAG, "Mic monitor raw peak: L=%d R=%d level=%d", peak_l, peak_r, level);
                }
            } else if ((debug_log_div++ % 20) == 0) {
                ESP_LOGW(TAG, "Mic monitor read failed: %d", ret);
            }
        }
        if (s_mic_level_callback != NULL) {
            s_mic_level_callback(level);
        }
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
    }
    free(buf);
    ESP_LOGI(TAG, "Mic monitor task stopped");
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * Public API: pre-configure shared I2C bus (call before audio_init)
 * ════════════════════════════════════════════════════════════════════ */
void audio_set_codec_i2c_bus(void *bus)
{
    s_codec_i2c_bus = (i2c_master_bus_handle_t)bus;
    s_i2c_bus_owned = false;
}

/* ══════════════════════════════════════════════════════════════════════
 * audio_init – initialise I2S + ES8311 (speaker) + ES7210 (mic)
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t spk_addr = normalize_codec_i2c_addr(CONFIG_AUDIO_ES8311_I2C_ADDR);
    uint8_t mic_addr = normalize_codec_i2c_addr(CONFIG_AUDIO_ES7210_I2C_ADDR);

    ESP_LOGI(TAG, "Codec I2C addr config: ES8311=0x%02X -> 0x%02X, ES7210=0x%02X -> 0x%02X",
             CONFIG_AUDIO_ES8311_I2C_ADDR, spk_addr,
             CONFIG_AUDIO_ES7210_I2C_ADDR, mic_addr);

    /* 1. I2C bus for codec control ──────────────────────────────────── */
    if (s_codec_i2c_bus == NULL) {
        i2c_master_bus_config_t i2c_cfg = {
            .i2c_port             = CONFIG_AUDIO_CODEC_I2C_PORT,
            .sda_io_num           = CONFIG_AUDIO_CODEC_I2C_SDA_GPIO,
            .scl_io_num           = CONFIG_AUDIO_CODEC_I2C_SCL_GPIO,
            .clk_source           = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt    = 7,
            .flags                = { .enable_internal_pullup = true },
        };
        ret = i2c_new_master_bus(&i2c_cfg, &s_codec_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Codec I2C bus (port %d) init failed: %s – "
                     "if shared with touch, call audio_set_codec_i2c_bus() first",
                     CONFIG_AUDIO_CODEC_I2C_PORT, esp_err_to_name(ret));
            return ret;
        }
        s_i2c_bus_owned = true;
        ESP_LOGI(TAG, "Codec I2C bus created on port %d (SCL=%d SDA=%d)",
                 CONFIG_AUDIO_CODEC_I2C_PORT,
                 CONFIG_AUDIO_CODEC_I2C_SCL_GPIO, CONFIG_AUDIO_CODEC_I2C_SDA_GPIO);
    }

    /* 2. I2S channels ───────────────────────────────────────────────── */
    i2s_chan_config_t chan_cfg = {
        .id                   = I2S_NUM_0,
        .role                 = I2S_ROLE_MASTER,
        .dma_desc_num         = 6,
        .dma_frame_num        = 240,
        .auto_clear_after_cb  = true,
        .auto_clear_before_cb = false,
        .intr_priority        = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle),
                        TAG, "I2S new channel failed");

    /* 3. I2S TX/RX in STD stereo mode (align with esp-box BSP examples). */
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
            .ext_clk_freq_hz = 0,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk  = CONFIG_AUDIO_I2S_MCLK_GPIO,
            .bclk  = CONFIG_AUDIO_I2S_BCLK_GPIO,
            .ws    = CONFIG_AUDIO_I2S_WS_GPIO,
            .dout  = CONFIG_AUDIO_I2S_DOUT_GPIO,
            .din   = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg  = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
            .ext_clk_freq_hz = 0,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk  = CONFIG_AUDIO_I2S_MCLK_GPIO,
            .bclk  = CONFIG_AUDIO_I2S_BCLK_GPIO,
            .ws    = CONFIG_AUDIO_I2S_WS_GPIO,
            .dout  = I2S_GPIO_UNUSED,
            .din   = CONFIG_AUDIO_I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &tx_std_cfg),
                        TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &rx_std_cfg),
                        TAG, "I2S RX init failed");

    /* 4. esp_codec_dev data interface (wraps I2S handles) ───────────── */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (s_data_if == NULL) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }

    /* 5. GPIO interface (used by ES8311 PA pin) ─────────────────────── */
    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) {
        ESP_LOGE(TAG, "Failed to create GPIO interface");
        return ESP_FAIL;
    }

    /* 6. ES8311 speaker codec ───────────────────────────────────────── */
    audio_codec_i2c_cfg_t spk_i2c = {
        .port       = CONFIG_AUDIO_CODEC_I2C_PORT,
        .addr       = spk_addr,
        .bus_handle = s_codec_i2c_bus,
    };
    s_spk_ctrl_if = audio_codec_new_i2c_ctrl(&spk_i2c);
    if (s_spk_ctrl_if == NULL) {
        ESP_LOGE(TAG, "Failed to create ES8311 I2C ctrl interface");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if        = s_spk_ctrl_if,
        .gpio_if        = s_gpio_if,
        .codec_mode     = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin         = (int16_t)CONFIG_AUDIO_PA_GPIO,
    #ifdef CONFIG_AUDIO_PA_INVERTED
        .pa_reverted    = true,
    #else
        .pa_reverted    = false,
    #endif
        .master_mode    = false,
        .use_mclk       = (CONFIG_AUDIO_I2S_MCLK_GPIO >= 0),
        .hw_gain        = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    s_spk_codec_if = es8311_codec_new(&es8311_cfg);
    if (s_spk_codec_if == NULL) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t spk_dev_cfg = {
        .dev_type  = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if  = s_spk_codec_if,
        .data_if   = s_data_if,
    };
    s_spk_dev = esp_codec_dev_new(&spk_dev_cfg);
    if (s_spk_dev == NULL) {
        ESP_LOGE(TAG, "Failed to create speaker codec device");
        return ESP_FAIL;
    }

    /* 7. ES7210 microphone codec ─────────────────────────────────────── */
    audio_codec_i2c_cfg_t mic_i2c = {
        .port       = CONFIG_AUDIO_CODEC_I2C_PORT,
        .addr       = mic_addr,
        .bus_handle = s_codec_i2c_bus,
    };
    s_mic_ctrl_if = audio_codec_new_i2c_ctrl(&mic_i2c);
    if (s_mic_ctrl_if == NULL) {
        ESP_LOGE(TAG, "Failed to create ES7210 I2C ctrl interface");
        return ESP_FAIL;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if     = s_mic_ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    s_mic_codec_if = es7210_codec_new(&es7210_cfg);
    if (s_mic_codec_if == NULL) {
        ESP_LOGE(TAG, "Failed to create ES7210 codec interface");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t mic_dev_cfg = {
        .dev_type  = ESP_CODEC_DEV_TYPE_IN,
        .codec_if  = s_mic_codec_if,
        .data_if   = s_data_if,
    };
    s_mic_dev = esp_codec_dev_new(&mic_dev_cfg);
    if (s_mic_dev == NULL) {
        ESP_LOGE(TAG, "Failed to create microphone codec device");
        return ESP_FAIL;
    }

    /* 8. Open codec devices (configures I2S and codec registers) ─────── */
    esp_codec_dev_sample_info_t spk_fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .channel_mask    = 0,
        .sample_rate     = SAMPLE_RATE,
        .mclk_multiple   = 0,
    };

    esp_codec_dev_sample_info_t mic_fs = {
        .bits_per_sample = 16,
        .channel         = MIC_CHANNELS,
        .channel_mask    = 0,
        .sample_rate     = SAMPLE_RATE,
        .mclk_multiple   = 0,
    };
    s_mic_fs_cfg = mic_fs;

    int rc = esp_codec_dev_open(s_spk_dev, &spk_fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Speaker codec open failed (%d) – check I2C address cfg=0x%02X bus=0x%02X and GPIOs",
                 rc, CONFIG_AUDIO_ES8311_I2C_ADDR, spk_addr);
        return ESP_FAIL;
    }

    rc = esp_codec_dev_open(s_mic_dev, &mic_fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Mic codec open failed (%d) – check I2C address cfg=0x%02X bus=0x%02X",
                 rc, CONFIG_AUDIO_ES7210_I2C_ADDR, mic_addr);
        return ESP_FAIL;
    }

    /* 9. Set default volume and mic gain ─────────────────────────────── */
    esp_codec_dev_set_out_vol(s_spk_dev, CONFIG_VOLUM);
    s_mic_muted = false;
    rc = esp_codec_dev_set_in_mute(s_mic_dev, s_mic_muted);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Mic unmute failed: %d", rc);
    }
    rc = esp_codec_dev_set_in_channel_gain(s_mic_dev, MIC_GAIN_MASK, MIC_GAIN_DB);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Mic channel gain set failed: %d, fallback to global gain", rc);
        esp_codec_dev_set_in_gain(s_mic_dev, MIC_GAIN_DB);
    }

    /* 10. Playback task ──────────────────────────────────────────────── */
    s_play_queue = xQueueCreate(4, sizeof(play_req_t));
    if (s_play_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create playback queue");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(play_task, "audio_play", 4096, NULL, 5, &s_play_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        vQueueDelete(s_play_queue);
        s_play_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_hw_ready = true;
    ESP_LOGI(TAG, "Audio ready: ES8311 (spk) + ES7210 (mic), %d Hz, vol=%d",
             SAMPLE_RATE, CONFIG_VOLUM);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * STT
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_start_stt(audio_stt_callback_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_stt_callback = callback;
    ESP_LOGI(TAG, "STT started");
    return ESP_OK;
}

esp_err_t audio_stop_stt(void)
{
    s_stt_callback = NULL;
    ESP_LOGI(TAG, "STT stopped");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * TTS playback
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_play_tts(const uint8_t *audio_data, int audio_len)
{
    if (audio_data == NULL || audio_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_hw_ready || s_play_queue == NULL) {
        ESP_LOGE(TAG, "audio_play_tts: hardware not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *copy = malloc(audio_len);
    if (copy == NULL) {
        ESP_LOGE(TAG, "audio_play_tts: no memory to copy %d bytes", audio_len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, audio_data, audio_len);

    play_req_t req = { .data = copy, .len = audio_len, .auto_free = true };
    if (xQueueSend(s_play_queue, &req, pdMS_TO_TICKS(500)) != pdTRUE) {
        UBaseType_t waiting = uxQueueMessagesWaiting(s_play_queue);
        UBaseType_t spaces = uxQueueSpacesAvailable(s_play_queue);
        ESP_LOGW(TAG, "audio_play_tts queue full/fail: len=%d waiting=%u spaces=%u",
                 audio_len, (unsigned)waiting, (unsigned)spaces);
        free(copy);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Queued %d bytes for playback", audio_len);
    return ESP_OK;
}

esp_err_t audio_stream_play_chunk(const uint8_t *audio_data, int audio_len)
{
    return audio_play_tts(audio_data, audio_len);
}

esp_err_t audio_debug_play_owned_sample(uint8_t **data, int *len)
{
    if (data == NULL || len == NULL || *data == NULL || *len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_hw_ready || s_spk_dev == NULL || s_play_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int owned_len = *len;
    play_req_t req = { .data = *data, .len = *len, .auto_free = true };
    if (xQueueSend(s_play_queue, &req, pdMS_TO_TICKS(500)) != pdTRUE) {
        UBaseType_t waiting = uxQueueMessagesWaiting(s_play_queue);
        UBaseType_t spaces = uxQueueSpacesAvailable(s_play_queue);
        ESP_LOGW(TAG, "audio_debug_play_owned_sample queue full/fail: len=%d waiting=%u spaces=%u",
                 owned_len, (unsigned)waiting, (unsigned)spaces);
        return ESP_FAIL;
    }

    *data = NULL;
    *len = 0;
    ESP_LOGI(TAG, "Queued owned sample %d bytes for playback", owned_len);
    return ESP_OK;
}

esp_err_t audio_debug_play_sample_ref(const uint8_t *data, int len)
{
    if (data == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_hw_ready || s_spk_dev == NULL || s_play_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    play_req_t req = { .data = (uint8_t *)data, .len = len, .auto_free = false };
    if (xQueueSend(s_play_queue, &req, pdMS_TO_TICKS(500)) != pdTRUE) {
        UBaseType_t waiting = uxQueueMessagesWaiting(s_play_queue);
        UBaseType_t spaces = uxQueueSpacesAvailable(s_play_queue);
        ESP_LOGW(TAG, "audio_debug_play_sample_ref queue full/fail: len=%d waiting=%u spaces=%u",
                 len, (unsigned)waiting, (unsigned)spaces);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Queued sample ref %d bytes for playback", len);
    return ESP_OK;
}

esp_err_t audio_stop_tts(void)
{
    ESP_LOGI(TAG, "TTS stop (not yet implemented for in-progress playback)");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Volume
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_spk_dev != NULL) {
        esp_codec_dev_set_out_vol(s_spk_dev, volume);
    }
    ESP_LOGI(TAG, "Volume set to %d", volume);
    return ESP_OK;
}

void audio_register_playback_callback(audio_playback_complete_callback_t callback)
{
    s_playback_callback = callback;
}

/* ══════════════════════════════════════════════════════════════════════
 * Debug: mic level monitor
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_debug_start_monitor(void)
{
    if (!s_hw_ready || s_mic_dev == NULL) {
        ESP_LOGW(TAG, "Mic monitor unavailable: audio hardware not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stream_capture_active) {
        ESP_LOGW(TAG, "Mic monitor unavailable: stream capture is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_is_monitoring) {
        ESP_LOGW(TAG, "Mic monitor already running");
        return ESP_OK;
    }

    esp_err_t ret = audio_refresh_mic_input();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mic refresh failed before monitor start: %s", esp_err_to_name(ret));
    }

    s_is_monitoring = true;
    xTaskCreate(monitor_task, "audio_monitor", 4096, NULL, 5, &s_monitor_task);
    ESP_LOGI(TAG, "Mic monitor started");
    return ESP_OK;
}

esp_err_t audio_debug_stop_monitor(void)
{
    if (!s_is_monitoring) {
        return ESP_OK;
    }
    s_is_monitoring = false;
    if (s_mic_level_callback != NULL) {
        s_mic_level_callback(0);
    }
    ESP_LOGI(TAG, "Mic monitor stopped");
    return ESP_OK;
}

void audio_register_mic_level_callback(audio_mic_level_callback_t callback)
{
    s_mic_level_callback = callback;
}

esp_err_t audio_stream_start_capture(void)
{
    if (!s_hw_ready || s_mic_dev == NULL) {
        ESP_LOGW(TAG, "Stream capture unavailable: audio hardware not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stream_capture_active) {
        return ESP_OK;
    }

    s_stream_capture_resume_monitor = false;
    if (s_is_monitoring) {
        audio_debug_stop_monitor();
        s_stream_capture_resume_monitor = true;
    }

    esp_err_t ret = audio_refresh_mic_input();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Stream capture start: mic refresh failed: %s", esp_err_to_name(ret));
    }

    s_stream_selected_ch = -1;
    s_stream_capture_active = true;
    ESP_LOGI(TAG, "Stream capture started");
    return ESP_OK;
}

esp_err_t audio_stream_read_capture_chunk(uint8_t *pcm_data, int pcm_capacity, int *pcm_len)
{
    if (pcm_data == NULL || pcm_len == NULL || pcm_capacity < BYTES_PER_SAMPLE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_stream_capture_active || !s_hw_ready || s_mic_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int frames_to_read = pcm_capacity / BYTES_PER_SAMPLE;
    if (frames_to_read <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_bytes_to_read = frames_to_read * MIC_CHANNELS * BYTES_PER_SAMPLE;
    esp_err_t mem_ret = audio_stream_ensure_raw_buffer(raw_bytes_to_read);
    if (mem_ret != ESP_OK) {
        return mem_ret;
    }

    int read_rc = esp_codec_dev_read(s_mic_dev, s_stream_raw_buf, raw_bytes_to_read);
    if (read_rc != ESP_CODEC_DEV_OK) {
        esp_err_t refresh_ret = audio_refresh_mic_input();
        if (refresh_ret == ESP_OK) {
            read_rc = esp_codec_dev_read(s_mic_dev, s_stream_raw_buf, raw_bytes_to_read);
        }
    }
    if (read_rc != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Stream capture read failed: %d", read_rc);
        return ESP_FAIL;
    }

    if (s_stream_selected_ch < 0) {
        long long energy_l = 0;
        long long energy_r = 0;
        for (int i = 0; i < frames_to_read; i++) {
            int l = s_stream_raw_buf[i * MIC_CHANNELS + 0];
            int r = s_stream_raw_buf[i * MIC_CHANNELS + 1];
            energy_l += (l < 0) ? -l : l;
            energy_r += (r < 0) ? -r : r;
        }
        s_stream_selected_ch = (energy_r > energy_l) ? 1 : 0;
        ESP_LOGI(TAG, "Stream capture channel selected: %c (energy L=%lld R=%lld)",
                 s_stream_selected_ch ? 'R' : 'L', energy_l, energy_r);
    }

    int16_t *mono = (int16_t *)pcm_data;
    for (int i = 0; i < frames_to_read; i++) {
        mono[i] = s_stream_raw_buf[i * MIC_CHANNELS + s_stream_selected_ch];
    }

    *pcm_len = frames_to_read * BYTES_PER_SAMPLE;
    return ESP_OK;
}

esp_err_t audio_stream_stop_capture(void)
{
    if (!s_stream_capture_active) {
        return ESP_OK;
    }

    s_stream_capture_active = false;
    s_stream_selected_ch = -1;

    if (s_stream_raw_buf != NULL) {
        free(s_stream_raw_buf);
        s_stream_raw_buf = NULL;
        s_stream_raw_buf_bytes = 0;
    }

    if (s_stream_capture_resume_monitor) {
        s_stream_capture_resume_monitor = false;
        (void)audio_debug_start_monitor();
    }

    ESP_LOGI(TAG, "Stream capture stopped");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Debug: record audio sample from real microphone
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_debug_record_sample(uint8_t **data, int *len)
{
    if (data == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_stream_capture_active) {
        ESP_LOGW(TAG, "Record sample unavailable: stream capture is active");
        return ESP_ERR_INVALID_STATE;
    }

    static const int capture_ms_candidates[] = {5000, 3000, 1500, 1000, 500};
    int total_frames = 0;
    int total_bytes = 0;

    *data = NULL;
    *len = 0;

    for (size_t i = 0; i < sizeof(capture_ms_candidates) / sizeof(capture_ms_candidates[0]); i++) {
        total_frames = (SAMPLE_RATE * capture_ms_candidates[i]) / 1000;
        total_bytes = total_frames * BYTES_PER_SAMPLE; /* mono output */
        *data = malloc(total_bytes);
        if (*data != NULL) {
            if (capture_ms_candidates[i] != 5000) {
                ESP_LOGW(TAG, "Low memory: record duration fallback to %d ms", capture_ms_candidates[i]);
            }
            break;
        }
    }

    if (*data == NULL) {
        ESP_LOGE(TAG, "Record buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    *len = total_bytes;
    int16_t *mono = (int16_t *)*data;

    if (s_hw_ready && s_mic_dev != NULL) {
        const int chunk_frames = MONITOR_BUF_FRAMES;
        const int chunk_bytes = chunk_frames * MIC_CHANNELS * BYTES_PER_SAMPLE;
        int16_t *raw = malloc(chunk_bytes);
        if (raw == NULL) {
            ESP_LOGW(TAG, "Chunk buffer alloc failed - using fallback tone");
            fill_debug_tone(mono, total_frames);
            return ESP_OK;
        }

        /* Drop a short pre-roll to flush stale DMA frames from previous monitor/read cycles.
         * Reuse heap buffer to avoid large stack allocations in the main task. */
        {
            int discard_bytes = chunk_bytes;
            (void)esp_codec_dev_read(s_mic_dev, raw, discard_bytes);
        }

        int peak = 0;
        int frames_done = 0;
        int read_rc = ESP_CODEC_DEV_OK;
        bool retried_after_refresh = false;
        int selected_ch = -1;
        int64_t sample_sum = 0;

        while (frames_done < total_frames) {
            int frames_to_read = total_frames - frames_done;
            if (frames_to_read > chunk_frames) {
                frames_to_read = chunk_frames;
            }

            int bytes_to_read = frames_to_read * MIC_CHANNELS * BYTES_PER_SAMPLE;
            read_rc = esp_codec_dev_read(s_mic_dev, raw, bytes_to_read);
            if (read_rc != ESP_CODEC_DEV_OK) {
                if (!retried_after_refresh) {
                    retried_after_refresh = true;
                    esp_err_t refresh_ret = audio_refresh_mic_input();
                    if (refresh_ret == ESP_OK) {
                        ESP_LOGW(TAG, "Mic read error %d at frame %d/%d, refreshed mic path and retry",
                                 read_rc, frames_done, total_frames);
                        continue;
                    }
                    ESP_LOGW(TAG, "Mic read error %d and refresh failed: %s",
                             read_rc, esp_err_to_name(refresh_ret));
                }
                break;
            }

            if (selected_ch < 0) {
                long long energy_l = 0;
                long long energy_r = 0;
                for (int i = 0; i < frames_to_read; i++) {
                    int l = raw[i * MIC_CHANNELS + 0];
                    int r = raw[i * MIC_CHANNELS + 1];
                    energy_l += (l < 0) ? -l : l;
                    energy_r += (r < 0) ? -r : r;
                }
                selected_ch = (energy_r > energy_l) ? 1 : 0;
                ESP_LOGI(TAG, "Debug capture channel selected: %c (energy L=%lld R=%lld)",
                         selected_ch ? 'R' : 'L', energy_l, energy_r);
            }

            for (int i = 0; i < frames_to_read; i++) {
                int16_t v = raw[i * MIC_CHANNELS + selected_ch];
                mono[frames_done + i] = v;
                sample_sum += v;
            }

            frames_done += frames_to_read;
        }

        free(raw);

        if (read_rc != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Mic read error %d at frame %d/%d - using fallback tone",
                     read_rc, frames_done, total_frames);
            fill_debug_tone(mono, total_frames);
            return ESP_OK;
        }

        /* Remove constant DC offset with a whole-buffer mean, which is gentler than sample-wise gating. */
        int32_t dc_offset = 0;
        if (total_frames > 0) {
            dc_offset = (int32_t)(sample_sum / total_frames);
        }

        for (int i = 0; i < total_frames; i++) {
            int32_t centered = (int32_t)mono[i] - dc_offset;
            if (centered > 32767) {
                centered = 32767;
            } else if (centered < -32768) {
                centered = -32768;
            }
            mono[i] = (int16_t)centered;

            int abs_v = (centered < 0) ? -(int)centered : (int)centered;
            if (abs_v > peak) {
                peak = abs_v;
            }
        }

        /* Mild normalization only when the capture is clearly below full-scale. */
        if (peak >= DEBUG_GAIN_MIN_PEAK && peak < (int)DEBUG_GAIN_TARGET) {
            float gain = DEBUG_GAIN_TARGET / (float)peak;
            if (gain > DEBUG_GAIN_MAX) {
                gain = DEBUG_GAIN_MAX;
            }
            if (gain > 1.02f) {
                for (int i = 0; i < total_frames; i++) {
                    int32_t scaled = (int32_t)((float)mono[i] * gain);
                    if (scaled > 32767) {
                        scaled = 32767;
                    } else if (scaled < -32768) {
                        scaled = -32768;
                    }
                    mono[i] = (int16_t)scaled;
                }
                ESP_LOGI(TAG, "Recorded %d frames (peak=%d), applied debug gain x%.2f",
                         total_frames, peak, gain);
            } else {
                ESP_LOGI(TAG, "Recorded %d frames (peak=%d), no extra gain needed",
                         total_frames, peak);
            }
        } else {
            ESP_LOGI(TAG, "Recorded %d frames (peak=%d)", total_frames, peak);
        }

        if (peak < DEBUG_GAIN_MIN_PEAK) {
            ESP_LOGW(TAG, "Recorded sample peak is very low (%d) - playback may be quiet", peak);
        }
    } else {
        /* Fallback tone so the debug playback path is still testable without mic HW. */
        ESP_LOGW(TAG, "HW not ready - using fallback tone");
        fill_debug_tone(mono, total_frames);
    }

    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Debug: play a short test tone through the speaker
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_debug_play_test_audio(void)
{
    ESP_LOGI(TAG, "Playing 440 Hz test tone via speaker");

    int total_frames = SAMPLE_RATE;         /* 1 second */
    int total_bytes  = total_frames * BYTES_PER_SAMPLE;

    uint8_t *data = malloc(total_bytes);
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int16_t *s = (int16_t *)data;
    fill_debug_tone(s, total_frames);

    if (!s_hw_ready || s_spk_dev == NULL) {
        ESP_LOGW(TAG, "Speaker HW not ready");
        free(data);
        if (s_playback_callback != NULL) s_playback_callback();
        return ESP_OK;
    }

    play_req_t req = { .data = data, .len = total_bytes, .auto_free = true };
    if (xQueueSend(s_play_queue, &req, pdMS_TO_TICKS(500)) != pdTRUE) {
        UBaseType_t waiting = uxQueueMessagesWaiting(s_play_queue);
        UBaseType_t spaces = uxQueueSpacesAvailable(s_play_queue);
        ESP_LOGW(TAG, "audio_debug_play_test_audio queue full/fail: waiting=%u spaces=%u",
                 (unsigned)waiting, (unsigned)spaces);
        free(data);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * Debug: play MP3 file (header validation only; decoder TBD)
 * ════════════════════════════════════════════════════════════════════ */
esp_err_t audio_debug_play_mp3_file(const char *file_path)
{
    if (file_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(file_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open MP3 file: %s", file_path);
        return ESP_FAIL;
    }

    uint8_t header[10] = {0};
    size_t header_len = fread(header, 1, sizeof(header), fp);
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fclose(fp);

    bool has_id3   = (header_len >= 3) && (memcmp(header, "ID3", 3) == 0);
    bool has_sync  = (header_len >= 2) && (header[0] == 0xFF) && ((header[1] & 0xE0) == 0xE0);

    if (!has_id3 && !has_sync) {
        ESP_LOGW(TAG, "Not a valid MP3 file: %s", file_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "MP3 header OK: %s (%ld bytes) – decoder not yet implemented", file_path, file_size);

    if (s_playback_callback != NULL) {
        s_playback_callback();
    }
    return ESP_OK;
}
