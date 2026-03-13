#include "network.h"
#include "config.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "network";

static net_state_t s_net_state = NET_STATE_DISCONNECTED;
static net_state_callback_t s_state_callback = NULL;
static void *s_user_data = NULL;
static int s_retry_num = 0;
#define MAX_RETRY 5

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        s_net_state = NET_STATE_CONNECTING;
        ESP_LOGI(TAG, "WiFi started, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connecting to AP (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            s_net_state = NET_STATE_ERROR;
            ESP_LOGE(TAG, "Failed to connect to AP");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_net_state = NET_STATE_CONNECTED;
    }

    if (s_state_callback != NULL) {
        s_state_callback(s_net_state, s_user_data);
    }
}

esp_err_t network_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    ESP_LOGI(TAG, "Network initialized");
    return ESP_OK;
}

esp_err_t network_start(void)
{
    app_config_t *config = config_get();
    
    if (strlen(config->wifi_ssid) == 0) {
        ESP_LOGE(TAG, "WiFi SSID not configured");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    strcpy((char *)wifi_config.sta.ssid, config->wifi_ssid);
    strcpy((char *)wifi_config.sta.password, config->wifi_password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Network started, connecting to %s", config->wifi_ssid);
    return ESP_OK;
}

esp_err_t network_stop(void)
{
    ESP_ERROR_CHECK(esp_wifi_stop());
    s_net_state = NET_STATE_DISCONNECTED;
    ESP_LOGI(TAG, "Network stopped");
    return ESP_OK;
}

net_state_t network_get_state(void)
{
    return s_net_state;
}

esp_err_t network_set_callback(net_state_callback_t callback, void *user_data)
{
    s_state_callback = callback;
    s_user_data = user_data;
    return ESP_OK;
}

bool network_is_connected(void)
{
    return s_net_state == NET_STATE_CONNECTED;
}
