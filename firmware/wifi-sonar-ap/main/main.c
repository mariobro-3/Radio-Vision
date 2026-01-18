#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi-sonar-ap";

#define WIFI_SONAR_MAGIC 0x43534950U /* "CSIP" */
#define WIFI_SONAR_MAX_STA CONFIG_WIFI_SONAR_STA_COUNT

typedef struct {
    const char *label;
    const char *ip_string;
    struct sockaddr_in sock_addr;
} scheduled_sta_t;

static scheduled_sta_t sta_table[] = {
#if CONFIG_WIFI_SONAR_STA_COUNT >= 1
    {.label = "S1", .ip_string = CONFIG_WIFI_SONAR_STA0_IP},
#endif
#if CONFIG_WIFI_SONAR_STA_COUNT >= 2
    {.label = "S2", .ip_string = CONFIG_WIFI_SONAR_STA1_IP},
#endif
#if CONFIG_WIFI_SONAR_STA_COUNT >= 3
    {.label = "S3", .ip_string = CONFIG_WIFI_SONAR_STA2_IP},
#endif
#if CONFIG_WIFI_SONAR_STA_COUNT >= 4
    {.label = "S4", .ip_string = CONFIG_WIFI_SONAR_STA3_IP},
#endif
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq_no;
    uint64_t timestamp_us;
    uint8_t slot;
    uint8_t sta_count;
    uint8_t payload_len;
    uint8_t reserved;
    uint8_t padding[CONFIG_WIFI_SONAR_PING_PAYLOAD_BYTES];
} ping_payload_t;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "STA " MACSTR " joined, aid=%d", MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGW(TAG, "STA " MACSTR " left, aid=%d", MAC2STR(event->mac), event->aid);
    }
}

static void init_sta_sockaddr(void) {
    for (size_t i = 0; i < sizeof(sta_table) / sizeof(sta_table[0]); ++i) {
        sta_table[i].sock_addr.sin_family = AF_INET;
        sta_table[i].sock_addr.sin_port = htons(CONFIG_WIFI_SONAR_TARGET_PORT);
        sta_table[i].sock_addr.sin_addr.s_addr = inet_addr(sta_table[i].ip_string);
        if (sta_table[i].sock_addr.sin_addr.s_addr == INADDR_NONE) {
            ESP_LOGE(TAG, "Invalid IPv4 string for %s: %s", sta_table[i].label, sta_table[i].ip_string);
        } else {
            ESP_LOGI(TAG, "Scheduled %s @ %s:%d", sta_table[i].label, sta_table[i].ip_string, CONFIG_WIFI_SONAR_TARGET_PORT);
        }
    }
}

static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = 0,
            .channel = CONFIG_WIFI_SONAR_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .ssid_hidden = 0,
            .max_connection = CONFIG_WIFI_SONAR_STA_COUNT,
            .beacon_interval = 100,
            .pmf_cfg = {.required = false},
        },
    };

    static_assert(CONFIG_WIFI_SONAR_CHANNEL == 1 || CONFIG_WIFI_SONAR_CHANNEL == 6 || CONFIG_WIFI_SONAR_CHANNEL == 11,
                  "Only 1/6/11 should be used for 20 MHz per Thesis p. 64");

    strncpy((char *)wifi_config.ap.ssid, CONFIG_WIFI_SONAR_AP_SSID, sizeof(wifi_config.ap.ssid));
    strncpy((char *)wifi_config.ap.password, CONFIG_WIFI_SONAR_AP_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';
    wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';

    if (strlen(CONFIG_WIFI_SONAR_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));  // Thesis p. 66: lock 20 MHz BW
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_config_80211_tx_rate(WIFI_IF_AP, WIFI_PHY_RATE_MCS5_LGI));  // Enables HT-LTF (Thesis p. 70)

    ESP_LOGI(TAG, "SoftAP ready. SSID:%s channel:%d", CONFIG_WIFI_SONAR_AP_SSID, CONFIG_WIFI_SONAR_CHANNEL);
}

static void ping_scheduler_task(void *pvParameters) {
    const TickType_t per_link_delay_ms = pdMS_TO_TICKS(MAX(1, 1000 / CONFIG_WIFI_SONAR_LINK_RATE_HZ));
    const size_t sta_len = sizeof(sta_table) / sizeof(sta_table[0]);
    const size_t payload_len = sizeof(ping_payload_t);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    ping_payload_t payload = {
        .magic = WIFI_SONAR_MAGIC,
        .payload_len = CONFIG_WIFI_SONAR_PING_PAYLOAD_BYTES,
        .sta_count = sta_len,
    };
    memset(payload.padding, 0xAA, sizeof(payload.padding));

    ESP_LOGI(TAG, "Ping scheduler live @ %d Hz/link (%zu links)", CONFIG_WIFI_SONAR_LINK_RATE_HZ, sta_len);

    while (true) {
        payload.seq_no++;
        payload.timestamp_us = esp_timer_get_time();

        for (size_t i = 0; i < sta_len; ++i) {
            payload.slot = (uint8_t)i;
            esp_err_t err = sendto(sock,
                                   &payload,
                                   payload_len,
                                   0,
                                   (struct sockaddr *)&sta_table[i].sock_addr,
                                   sizeof(struct sockaddr_in));
            if (err < 0) {
                ESP_LOGW(TAG, "sendto %s failed: errno=%d", sta_table[i].label, errno);
            } else {
                ESP_LOGD(TAG, "seq=%" PRIu32 " slot=%s", payload.seq_no, sta_table[i].label);
            }
            ets_delay_us(250);  // 0.25 ms spacing, Thesis p. 145
        }

        vTaskDelay(per_link_delay_ms);
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    init_sta_sockaddr();
    wifi_init_softap();
    xTaskCreatePinnedToCore(ping_scheduler_task,
                            "ping_scheduler",
                            4096,
                            NULL,
                            4,
                            NULL,
                            0);
}

