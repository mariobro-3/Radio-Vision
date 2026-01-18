/*
 * ESP32 CSI Motion Detection - AP Firmware (Improved Detection)
 * 
 * Architecture:
 * - AP board creates WiFi network
 * - Client boards connect and ping (generate traffic)
 * - AP collects CSI from clients' packets
 * - Motion detection runs ON-DEVICE with adaptive thresholds
 * - NeoPixel shows motion status
 * - Simple UDP status sent to PC dashboard
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"

#include "../../_components/csi_component.h"

static const char *TAG = "CSI-AP";

/*
 * Configuration
 */
#define WIFI_SSID              "ESP32-AP"
#define WIFI_PASS              "esp32-ap"
#define WIFI_CHANNEL           1       // Channel 1 often less congested than 6
#define MAX_STA_CONN           4

// NeoPixel on FeatherS3D
#define NEOPIXEL_GPIO          40
#define RMT_RESOLUTION_HZ      10000000

// Motion detection tuning parameters (compile-time defaults)
#define CSI_QUEUE_SIZE         16
#define BASELINE_SAMPLES       160      // ~4 seconds at 40 pps for very stable baseline
#define HISTORY_SIZE           20       // Not used in new algorithm, kept for struct

// Runtime-adjustable parameters (with defaults)
static float cfg_smoothing_alpha = 0.10f;    // EMA smoothing (lower = smoother)
static float cfg_min_threshold = 3.0f;       // Absolute minimum deviation to trigger
static int cfg_hysteresis_on = 5;            // Consecutive detections to trigger ON
static int cfg_hysteresis_off = 12;          // Consecutive non-detections to trigger OFF

// Fixed parameters
#define BASE_THRESHOLD         0.3f     // Deviation must exceed 30% of baseline variance
#define RSSI_WEAK_THRESHOLD    -55      // RSSI below this = weak signal
#define WEAK_SIGNAL_MULT       1.5f     // Multiply threshold for weak signals
#define BASELINE_ADAPT_RATE    0.003f   // Slower adaptation to prevent drift

// UDP status reporting - use broadcast so any device on the network can receive
#define HOST_IP_ADDR           "192.168.4.255"  // Broadcast to all devices on AP network
#define HOST_UDP_PORT          8848
#define CMD_UDP_PORT           8849             // Port to receive commands from dashboard
#define STATUS_INTERVAL_MS     100

/*
 * Global state
 */
static QueueHandle_t csi_queue;
static rmt_channel_handle_t led_channel = NULL;
static rmt_encoder_handle_t led_encoder = NULL;

// Motion detection state
typedef struct {
    float amplitude_history[HISTORY_SIZE];
    float variance_history[HISTORY_SIZE];
    int history_idx;
    int history_count;
    
    float smoothed_amplitude;
    float smoothed_variance;
    
    float baseline_mean;
    float baseline_variance;
    int baseline_samples;
    bool baseline_ready;
    
    int motion_count;      // Consecutive motion detections
    int no_motion_count;   // Consecutive non-detections
    bool motion_detected;
    
    int settling_countdown; // Post-calibration settling period
    
    float current_threshold;
    float last_variance;
} motion_state_t;

static motion_state_t motion = {0};
static uint32_t packets_received = 0;
static int last_rssi = 0;
static int connected_clients = 0;

/*
 * NeoPixel Control
 */
static uint8_t led_pixels[3];

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

// Pulsing effect for calibration
static void neopixel_pulse_yellow(int phase)
{
    int brightness = 20 + (phase % 30);
    neopixel_set(brightness, brightness, 0);
}

/*
 * WiFi AP Setup
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected", MAC2STR(event->mac));
        connected_clients++;
        if (!motion.baseline_ready) {
            neopixel_set(50, 30, 0);  // Orange during calibration
        } else {
            neopixel_set(0, 0, 50);   // Blue when ready
        }
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected", MAC2STR(event->mac));
        if (connected_clients > 0) connected_clients--;
        if (connected_clients == 0) {
            neopixel_set(50, 0, 0);  // Red when no clients
        }
    }
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    
    // Increase WiFi stability
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // Disable power save
    
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Use a moderate rate for stability - MCS2 is a good balance
    // Lower MCS = more robust at distance, higher MCS = faster but needs better signal
    // MCS0 = most robust, MCS7 = fastest but fragile
    ESP_ERROR_CHECK(esp_wifi_config_80211_tx_rate(WIFI_IF_AP, WIFI_PHY_RATE_MCS2_LGI));

    ESP_LOGI(TAG, "AP started: SSID=%s Channel=%d", WIFI_SSID, WIFI_CHANNEL);
}

/*
 * Improved CSI Feature Extraction
 * Instead of just mean amplitude, calculate variance across subcarriers
 * Motion causes subcarrier amplitudes to diverge
 */
static void extract_csi_features(wifi_csi_info_t *csi, float *mean_amp, float *subcarrier_var)
{
    if (csi->buf == NULL || csi->len < 20) {
        *mean_amp = 0;
        *subcarrier_var = 0;
        return;
    }
    
    int8_t *buf = csi->buf;
    float amplitudes[64];
    int count = 0;
    
    // Calculate amplitude for each subcarrier
    for (int i = 8; i < csi->len - 8 && count < 64; i += 2) {
        float real = buf[i];
        float imag = buf[i + 1];
        amplitudes[count++] = sqrtf(real * real + imag * imag);
    }
    
    if (count < 10) {
        *mean_amp = 0;
        *subcarrier_var = 0;
        return;
    }
    
    // Calculate mean
    float sum = 0;
    for (int i = 0; i < count; i++) {
        sum += amplitudes[i];
    }
    float mean = sum / count;
    *mean_amp = mean;
    
    // Calculate variance across subcarriers
    float sum_sq_diff = 0;
    for (int i = 0; i < count; i++) {
        float diff = amplitudes[i] - mean;
        sum_sq_diff += diff * diff;
    }
    *subcarrier_var = sum_sq_diff / count;
}

/*
 * Improved Motion Detection with:
 * - Exponential moving average smoothing
 * - Adaptive threshold based on RSSI
 * - Hysteresis to prevent flickering
 * - Slow baseline adaptation
 */
static void update_motion_detection(float amplitude, float subcarrier_variance, int rssi)
{
    // Apply EMA smoothing to current reading
    if (motion.smoothed_amplitude == 0) {
        motion.smoothed_amplitude = amplitude;
        motion.smoothed_variance = subcarrier_variance;
    } else {
        motion.smoothed_amplitude = cfg_smoothing_alpha * amplitude + 
                                    (1 - cfg_smoothing_alpha) * motion.smoothed_amplitude;
        motion.smoothed_variance = cfg_smoothing_alpha * subcarrier_variance + 
                                   (1 - cfg_smoothing_alpha) * motion.smoothed_variance;
    }
    
    // Track sample count for calibration
    motion.history_count++;
    
    // Need minimum samples before we can do anything
    if (motion.history_count < 10) {
        return;
    }
    
    // Baseline calibration phase - measure the "quiet" subcarrier variance
    if (!motion.baseline_ready) {
        motion.baseline_mean += motion.smoothed_variance;
        motion.baseline_samples++;
        
        // Visual feedback during calibration
        neopixel_pulse_yellow(motion.baseline_samples);
        
        if (motion.baseline_samples >= BASELINE_SAMPLES) {
            motion.baseline_mean /= BASELINE_SAMPLES;
            motion.baseline_variance = motion.baseline_mean;  // Use mean as baseline
            motion.baseline_ready = true;
            motion.settling_countdown = 20;  // ~0.5 sec settling period after calibration
            
            ESP_LOGI(TAG, "=== BASELINE CALIBRATION COMPLETE ===");
            ESP_LOGI(TAG, "Baseline subcarrier variance: %.2f", motion.baseline_variance);
            ESP_LOGI(TAG, "Settling for 0.5 sec...");
            
            neopixel_set(0, 0, 50);  // Blue = ready
        }
        return;
    }
    
    // Post-calibration settling period - let smoothing stabilize
    if (motion.settling_countdown > 0) {
        motion.settling_countdown--;
        if (motion.settling_countdown == 0) {
            ESP_LOGI(TAG, "Ready for motion detection!");
        }
        return;
    }
    
    // SIMPLE DETECTION: Compare current smoothed variance to baseline
    // Motion causes subcarrier variance to increase significantly
    float deviation = motion.smoothed_variance - motion.baseline_variance;
    motion.last_variance = deviation;  // For dashboard display
    
    // Calculate adaptive threshold  
    float threshold = motion.baseline_variance * BASE_THRESHOLD;
    
    // Increase threshold for weak signals (more noise expected)
    if (rssi < RSSI_WEAK_THRESHOLD) {
        threshold *= WEAK_SIGNAL_MULT;
    }
    
    // Apply minimum threshold
    if (threshold < cfg_min_threshold) {
        threshold = cfg_min_threshold;
    }
    
    motion.current_threshold = threshold;
    
    // Check if current variance deviates enough from baseline
    bool instant_motion = (deviation > threshold);
    
    // Apply hysteresis
    if (instant_motion) {
        motion.motion_count++;
        motion.no_motion_count = 0;
    } else {
        motion.no_motion_count++;
        motion.motion_count = 0;
    }
    
    bool was_detected = motion.motion_detected;
    
    // Turn ON only after sustained detection
    if (!motion.motion_detected && motion.motion_count >= cfg_hysteresis_on) {
        motion.motion_detected = true;
    }
    // Turn OFF only after sustained no-detection
    else if (motion.motion_detected && motion.no_motion_count >= cfg_hysteresis_off) {
        motion.motion_detected = false;
    }
    
    // Slowly adapt baseline during quiet periods (track the "quiet" variance level)
    if (!motion.motion_detected && !instant_motion) {
        motion.baseline_variance = motion.baseline_variance * (1 - BASELINE_ADAPT_RATE) +
                                   motion.smoothed_variance * BASELINE_ADAPT_RATE;
    }
    
    // Update NeoPixel
    if (motion.motion_detected) {
        neopixel_set(0, 255, 0);  // Bright green
    } else {
        neopixel_set(0, 0, 30);   // Dim blue
    }
    
    // Log state changes
    if (motion.motion_detected != was_detected) {
        ESP_LOGI(TAG, "Motion %s (dev=%.2f thresh=%.2f base=%.2f rssi=%d)", 
                 motion.motion_detected ? "DETECTED" : "stopped",
                 deviation, threshold, motion.baseline_variance, rssi);
    }
}

/*
 * CSI Callback
 */
static void wifi_csi_cb(void *ctx, wifi_csi_info_t *data)
{
    // Accept packets with valid CSI data (min ~20 bytes for any useful data)
    if (data->buf == NULL || data->len < 20) {
        return;
    }
    
    wifi_csi_info_t csi_copy;
    memcpy(&csi_copy, data, sizeof(wifi_csi_info_t));
    csi_copy.buf = malloc(data->len);
    if (csi_copy.buf == NULL) {
        return;
    }
    memcpy(csi_copy.buf, data->buf, data->len);
    
    if (xQueueSend(csi_queue, &csi_copy, 0) != pdTRUE) {
        free(csi_copy.buf);
    }
}

/*
 * CSI Processing Task
 */
static void csi_processing_task(void *pvParameter)
{
    wifi_csi_info_t csi;
    
    ESP_LOGI(TAG, "CSI processing task started");
    ESP_LOGI(TAG, "Calibrating baseline - keep area still for %d packets...", BASELINE_SAMPLES);
    
    while (xQueueReceive(csi_queue, &csi, portMAX_DELAY) == pdTRUE) {
        packets_received++;
        last_rssi = csi.rx_ctrl.rssi;
        
        float amplitude, subcarrier_var;
        extract_csi_features(&csi, &amplitude, &subcarrier_var);
        update_motion_detection(amplitude, subcarrier_var, last_rssi);
        
        free(csi.buf);
    }
    
    vTaskDelete(NULL);
}

/*
 * UDP Status Reporter
 */
static void status_reporter_task(void *pvParameter)
{
    struct sockaddr_in dest_addr = {
        .sin_addr.s_addr = inet_addr(HOST_IP_ADDR),
        .sin_family = AF_INET,
        .sin_port = htons(HOST_UDP_PORT),
    };
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }
    
    // Enable broadcast so we can send to 192.168.4.255
    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
    
    ESP_LOGI(TAG, "Status reporter broadcasting to %s:%d", HOST_IP_ADDR, HOST_UDP_PORT);
    
    char status_msg[256];
    uint32_t last_packets = 0;
    
    while (1) {
        uint32_t pps = (packets_received - last_packets) * (1000 / STATUS_INTERVAL_MS);
        last_packets = packets_received;
        
        // Send extended status with calibration info
        int len = snprintf(status_msg, sizeof(status_msg),
            "{\"motion\":%d,\"rssi\":%d,\"pps\":%lu,\"pkts\":%lu,\"cal\":%d,\"thresh\":%.4f,\"var\":%.4f}",
            motion.motion_detected ? 1 : 0,
            last_rssi,
            (unsigned long)pps,
            (unsigned long)packets_received,
            motion.baseline_ready ? 1 : 0,
            motion.current_threshold,
            motion.last_variance);
        
        sendto(sock, status_msg, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}

/*
 * UDP Command Receiver - receives tuning commands from dashboard
 */
static void command_receiver_task(void *pvParameter)
{
    struct sockaddr_in listen_addr = {
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_family = AF_INET,
        .sin_port = htons(CMD_UDP_PORT),
    };
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create command socket");
        vTaskDelete(NULL);
        return;
    }
    
    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind command socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Command receiver listening on port %d", CMD_UDP_PORT);
    
    char rx_buffer[256];
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    
    while (1) {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&source_addr, &socklen);
        
        if (len > 0) {
            rx_buffer[len] = 0;  // Null-terminate
            
            // Parse simple key=value commands
            // Format: "alpha=0.15" or "hyst_on=3" or "threshold=5.0" or "hyst_off=8"
            float fval;
            int ival;
            
            bool cmd_received = false;
            
            if (sscanf(rx_buffer, "alpha=%f", &fval) == 1) {
                if (fval >= 0.05f && fval <= 0.5f) {
                    cfg_smoothing_alpha = fval;
                    ESP_LOGI(TAG, "Set smoothing_alpha = %.2f", cfg_smoothing_alpha);
                    cmd_received = true;
                }
            } else if (sscanf(rx_buffer, "threshold=%f", &fval) == 1) {
                if (fval >= 0.5f && fval <= 50.0f) {
                    cfg_min_threshold = fval;
                    ESP_LOGI(TAG, "Set min_threshold = %.2f", cfg_min_threshold);
                    cmd_received = true;
                }
            } else if (sscanf(rx_buffer, "hyst_on=%d", &ival) == 1) {
                if (ival >= 1 && ival <= 20) {
                    cfg_hysteresis_on = ival;
                    ESP_LOGI(TAG, "Set hysteresis_on = %d", cfg_hysteresis_on);
                    cmd_received = true;
                }
            } else if (sscanf(rx_buffer, "hyst_off=%d", &ival) == 1) {
                if (ival >= 1 && ival <= 50) {
                    cfg_hysteresis_off = ival;
                    ESP_LOGI(TAG, "Set hysteresis_off = %d", cfg_hysteresis_off);
                    cmd_received = true;
                }
            } else if (strncmp(rx_buffer, "recalibrate", 11) == 0) {
                // Reset calibration
                motion.baseline_ready = false;
                motion.baseline_samples = 0;
                motion.baseline_mean = 0;
                motion.history_count = 0;
                motion.smoothed_amplitude = 0;
                motion.smoothed_variance = 0;
                ESP_LOGI(TAG, "Recalibrating...");
                cmd_received = true;
            }
            
            // Visual feedback: quick purple flash to acknowledge command
            if (cmd_received) {
                neopixel_set(80, 0, 120);  // Purple
                vTaskDelay(pdMS_TO_TICKS(100));
                // Return to current state color
                if (!motion.baseline_ready) {
                    neopixel_set(50, 30, 0);  // Yellow/orange during calibration
                } else if (motion.motion_detected) {
                    neopixel_set(0, 255, 0);  // Green if motion
                } else {
                    neopixel_set(0, 0, 30);   // Blue if idle
                }
            }
        }
    }
}

/*
 * Main Entry Point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "  ESP32 CSI Motion Detection (Improved)");
    ESP_LOGI(TAG, "===========================================");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize NeoPixel
    neopixel_init();
    neopixel_set(50, 50, 0);  // Yellow during startup
    
    // Initialize WiFi AP
    wifi_init_softap();
    
    // Create CSI queue
    csi_queue = xQueueCreate(CSI_QUEUE_SIZE, sizeof(wifi_csi_info_t));
    if (csi_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create CSI queue");
        return;
    }
    
    // Register CSI callback
    csi_init("AP", &wifi_csi_cb);
    
    // Start tasks
    xTaskCreate(csi_processing_task, "csi_proc", 4096, NULL, 5, NULL);
    xTaskCreate(status_reporter_task, "status", 4096, NULL, 3, NULL);
    xTaskCreate(command_receiver_task, "cmd_rx", 4096, NULL, 2, NULL);
    
    // Ready
    neopixel_set(50, 0, 0);  // Red = waiting for clients
    ESP_LOGI(TAG, "Ready - waiting for client connections");
    ESP_LOGI(TAG, "Once clients connect, keep area STILL for calibration!");
}
