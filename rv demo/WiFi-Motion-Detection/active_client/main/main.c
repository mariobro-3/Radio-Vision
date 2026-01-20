/*
 * ESP32 CSI Motion Detection - Simple Client Firmware
 * 
 * Built by Cole Studer — X: @photoncmndr
 * Based on ESP32-CSI-Tool by StevenMHernandez
 * 
 * This client just connects to the AP and generates traffic via pinging.
 * The AP handles all CSI collection and motion detection.
 * 
 * No UDP to PC, no mDNS - just connect and ping!
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "ping/ping_sock.h"

static const char *TAG = "CSI-Client";

// NeoPixel on FeatherS3D
#define NEOPIXEL_GPIO          40
#define RMT_RESOLUTION_HZ      10000000

static rmt_channel_handle_t led_channel = NULL;
static rmt_encoder_handle_t led_encoder = NULL;
static uint8_t led_pixels[3];
static volatile bool led_fade_active = false;

/*
 * Configuration - must match AP settings
 */
#define WIFI_SSID              "ESP32-AP"
#define WIFI_PASS              "esp32-ap"
#define MAX_RETRY              10

// Ping configuration
#define PING_INTERVAL_MS       10    // Ping every 10ms = 100 pings/sec
#define PING_TIMEOUT_MS        500
#define PING_DATA_SIZE         32    // Larger packets = more CSI data

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1

static int s_retry_num = 0;
static bool s_connected = false;
static bool s_ping_started = false;
static esp_ping_handle_t s_ping_handle = NULL;

// Forward declarations
static void start_ping(void);

/*
 * NeoPixel Control
 */
static void neopixel_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = NEOPIXEL_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_channel));

    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_channel));
    
    ESP_LOGI(TAG, "NeoPixel initialized on GPIO %d", NEOPIXEL_GPIO);
}

static void neopixel_set(uint8_t r, uint8_t g, uint8_t b)
{
    led_pixels[0] = g;
    led_pixels[1] = r;
    led_pixels[2] = b;
    
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    rmt_transmit(led_channel, led_encoder, led_pixels, sizeof(led_pixels), &tx_config);
    rmt_tx_wait_all_done(led_channel, portMAX_DELAY);
}

/*
 * LED fade task - fades red when disconnected
 */
static void led_fade_task(void *pvParameter)
{
    while (1) {
        if (led_fade_active && !s_connected) {
            // Fade red in and out
            for (int i = 10; i <= 50 && led_fade_active && !s_connected; i += 2) {
                neopixel_set(i, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            for (int i = 50; i >= 10 && led_fade_active && !s_connected; i -= 2) {
                neopixel_set(i, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/*
 * WiFi Event Handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        led_fade_active = true;  // Start red fading
        
        // Stop and clean up ping session on disconnect
        if (s_ping_handle != NULL) {
            esp_ping_stop(s_ping_handle);
            esp_ping_delete_session(s_ping_handle);
            s_ping_handle = NULL;
            s_ping_started = false;
            ESP_LOGI(TAG, "Stopped ping session on disconnect");
        }
        
        // Add small delay before reconnect to avoid rapid cycling
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Always try to reconnect
        esp_wifi_connect();
        s_retry_num++;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        led_fade_active = false;  // Stop red fading
        neopixel_set(0, 64, 0);   // 25% brightness green
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Start or restart ping on every connection
        if (!s_ping_started) {
            start_ping();
        }
    }
}

/*
 * Initialize WiFi as Station
 */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // Disable power save for stability
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s...", WIFI_SSID);

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP successfully");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect");
    }
}

/*
 * Ping callbacks
 */
static void ping_success_cb(esp_ping_handle_t hdl, void *args)
{
    // Ping succeeded - don't log to avoid USB spam
    (void)hdl;
    (void)args;
}

static void ping_timeout_cb(esp_ping_handle_t hdl, void *args)
{
    // Don't log timeouts - they spam USB
    (void)hdl;
    (void)args;
}

static void ping_end_cb(esp_ping_handle_t hdl, void *args)
{
    // Ping ended - mark as not started so it can restart
    s_ping_started = false;
    s_ping_handle = NULL;
}

/*
 * Start pinging the AP gateway
 */
static void start_ping(void)
{
    // Get gateway IP (the AP)
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_get_ip_info(netif, &ip_info);

    ESP_LOGI(TAG, "Starting ping to gateway: " IPSTR, IP2STR(&ip_info.gw));

    // Configure ping
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    
    // Set target to gateway
    ip_addr_t target_addr;
    IP_ADDR4(&target_addr,
             esp_ip4_addr1(&ip_info.gw),
             esp_ip4_addr2(&ip_info.gw),
             esp_ip4_addr3(&ip_info.gw),
             esp_ip4_addr4(&ip_info.gw));
    ping_config.target_addr = target_addr;
    
    // Ping forever
    ping_config.count = 0;  // 0 = infinite
    ping_config.interval_ms = PING_INTERVAL_MS;
    ping_config.timeout_ms = PING_TIMEOUT_MS;
    ping_config.data_size = PING_DATA_SIZE;
    ping_config.task_stack_size = 4096;
    ping_config.task_prio = 2;

    // Set callbacks
    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_success_cb,
        .on_ping_timeout = ping_timeout_cb,
        .on_ping_end = ping_end_cb,
        .cb_args = NULL,
    };

    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &s_ping_handle);
    if (err != ESP_OK) {
        return;
    }
    
    err = esp_ping_start(s_ping_handle);
    if (err != ESP_OK) {
        esp_ping_delete_session(s_ping_handle);
        s_ping_handle = NULL;
        return;
    }
    
    s_ping_started = true;
}

/*
 * Main Entry Point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32 CSI Client ===");
    ESP_LOGI(TAG, "Simple ping client for motion detection");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize NeoPixel and show yellow on startup
    neopixel_init();
    neopixel_set(50, 50, 0);  // Yellow = starting up
    
    // Start LED fade task for disconnect indication
    xTaskCreate(led_fade_task, "led_fade", 2048, NULL, 1, NULL);
    
    // Start red fading while trying to connect
    led_fade_active = true;

    // Connect to AP (ping starts automatically when IP is obtained)
    wifi_init_sta();

    // Keep alive - ping runs in the background, restarts on reconnect
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
