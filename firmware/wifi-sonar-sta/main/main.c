#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi-sonar-sta";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SONAR_MAGIC 0x582D4353U /* "X-CS" */
#define WIFI_SONAR_LABEL_LEN 8
#define WIFI_SONAR_MAX_SUBCARRIERS 64

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t amplitude_len;
    uint64_t timestamp_us;
    uint32_t csi_seq;
    uint8_t sender_mac[6];
    int8_t rssi;
    uint8_t channel;
    char label[WIFI_SONAR_LABEL_LEN];
    float amplitude[WIFI_SONAR_MAX_SUBCARRIERS];
} wifi_sonar_udp_frame_t;

static EventGroupHandle_t wifi_event_group;
static QueueHandle_t csi_queue;
static esp_netif_t *sta_netif;
static wifi_sonar_udp_frame_t frame_template;

static bool parse_ipv4(const char *str, esp_ip4_addr_t *out) {
    if (!str || !out) {
        return false;
    }
    ip4_addr_t tmp;
    if (!ip4addr_aton(str, &tmp)) {
        return false;
    }
    out->addr = tmp.addr;
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, retrying in 200 ms");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void init_network(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();
    assert(wifi_event_group);

    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    esp_netif_ip_info_t ip_info;
    bool static_ip_requested = strcmp(CONFIG_WIFI_SONAR_STATIC_IP, "0.0.0.0") != 0;
    if (static_ip_requested) {
        esp_ip4_addr_t ip, gw, netmask;
        if (!parse_ipv4(CONFIG_WIFI_SONAR_STATIC_IP, &ip) ||
            !parse_ipv4(CONFIG_WIFI_SONAR_STATIC_GW, &gw) ||
            !parse_ipv4(CONFIG_WIFI_SONAR_STATIC_NETMASK, &netmask)) {
            ESP_LOGE(TAG, "Invalid static IP configuration");
            abort();
        }
        ip_info.ip = ip;
        ip_info.gw = gw;
        ip_info.netmask = netmask;
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));
        ESP_LOGI(TAG, "Static IPv4 %s/24 (GW %s)", CONFIG_WIFI_SONAR_STATIC_IP, CONFIG_WIFI_SONAR_STATIC_GW);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.csi_enable = true;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // Thesis p. 118: disable powersave to reduce jitter

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SONAR_AP_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_SONAR_AP_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.listen_interval = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));  // Thesis p. 66
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void wait_for_ip(void) {
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(10000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Failed to obtain IP address");
        esp_restart();
    }
}

static void configure_csi_pipeline(void) {
    wifi_csi_config_t csi_config = {
        .lltf_en = false,
        .htltf_en = true,
        .stbc_en = false,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = WIFI_CSI_SHIFT_RIGHT_0,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

static void IRAM_ATTR wifi_csi_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || !csi_queue) {
        return;
    }

    wifi_sonar_udp_frame_t frame = frame_template;
    frame.timestamp_us = esp_timer_get_time();
    frame.csi_seq = info->rx_ctrl.seq_num;
    frame.rssi = info->rx_ctrl.rssi;
    frame.channel = info->rx_ctrl.channel;

    if (info->len == 0 || !info->buf) {
        return;
    }

    int subcarriers = info->len / 2;  // each subcarrier uses I/Q signed 8-bit
    if (subcarriers > WIFI_SONAR_MAX_SUBCARRIERS) {
        subcarriers = WIFI_SONAR_MAX_SUBCARRIERS;
    }

    frame.amplitude_len = subcarriers;

    for (int i = 0; i < subcarriers; ++i) {
        int8_t real = info->buf[2 * i];
        int8_t imag = info->buf[2 * i + 1];
        frame.amplitude[i] = sqrtf((float)(real * real + imag * imag));  // Thesis p. 152: amplitude-only pipeline
    }
    for (int i = subcarriers; i < WIFI_SONAR_MAX_SUBCARRIERS; ++i) {
        frame.amplitude[i] = 0.0f;
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(csi_queue, &frame, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void udp_tx_task(void *arg) {
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_WIFI_SONAR_COLLECTOR_PORT),
        .sin_addr.s_addr = inet_addr(CONFIG_WIFI_SONAR_COLLECTOR_IP),
    };

    if (dest_addr.sin_addr.s_addr == INADDR_NONE) {
        ESP_LOGE(TAG, "Invalid collector IP: %s", CONFIG_WIFI_SONAR_COLLECTOR_IP);
        vTaskDelete(NULL);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Streaming CSI to %s:%d", CONFIG_WIFI_SONAR_COLLECTOR_IP, CONFIG_WIFI_SONAR_COLLECTOR_PORT);

    wifi_sonar_udp_frame_t frame;
    while (xQueueReceive(csi_queue, &frame, portMAX_DELAY) == pdTRUE) {
        size_t payload_bytes = offsetof(wifi_sonar_udp_frame_t, amplitude) + frame.amplitude_len * sizeof(float);
        int err = sendto(sock, &frame, payload_bytes, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGW(TAG, "sendto failed: errno=%d", errno);
        }
    }

    ESP_LOGE(TAG, "CSI queue unexpectedly empty");
    close(sock);
    vTaskDelete(NULL);
}

void app_main(void) {
    init_nvs();
    init_network();
    wait_for_ip();

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    memcpy(frame_template.sender_mac, mac, sizeof(mac));
    frame_template.magic = WIFI_SONAR_MAGIC;
    frame_template.version = 1;
    frame_template.amplitude_len = WIFI_SONAR_MAX_SUBCARRIERS;
    memset(frame_template.label, 0, sizeof(frame_template.label));
    strncpy(frame_template.label, CONFIG_WIFI_SONAR_STA_LABEL, WIFI_SONAR_LABEL_LEN - 1);

    csi_queue = xQueueCreate(CONFIG_WIFI_SONAR_SAMPLE_QUEUE_DEPTH, sizeof(wifi_sonar_udp_frame_t));
    configASSERT(csi_queue);

    configure_csi_pipeline();
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&wifi_csi_cb, NULL));

    xTaskCreatePinnedToCore(udp_tx_task, "udp_tx", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "CSI streaming armed; waiting for AP bursts (Thesis p. 140).");
}

