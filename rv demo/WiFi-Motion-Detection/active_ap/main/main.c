/*
 * ESP32 CSI Motion Detection - AP Firmware (Improved Detection)
 * 
 * Built by Cole Studer — X: @photoncmndr
 * Based on ESP32-CSI-Tool by StevenMHernandez
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
// Old single-phase calibration - replaced by CAL_SAMPLES in motion_state_t
#define HISTORY_SIZE           20       // Not used in new algorithm, kept for struct

// Runtime-adjustable parameters (with defaults)
static float cfg_smoothing_alpha = 0.03f;    // EMA smoothing (lower = smoother)
static float cfg_min_threshold = 5.0f;       // Motion triggers when metric exceeds this (adjustable 1-50)
static int cfg_hysteresis_on = 3;            // Consecutive detections to trigger ON
static int cfg_hysteresis_off = 8;           // Consecutive non-detections to trigger OFF

// Fixed parameters - USE ABSOLUTE THRESHOLD instead of percentage
#define BASE_THRESHOLD         0.08f    // Only 8% of baseline (makes min_threshold dominant)
#define RSSI_WEAK_THRESHOLD    -55      // RSSI below this = weak signal
#define WEAK_SIGNAL_MULT       1.5f     // Multiply threshold for weak signals
#define BASELINE_ADAPT_RATE    0.001f   // Very slow adaptation to prevent baseline drift

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

// Two-phase calibration state
typedef enum {
    CAL_PHASE_QUIET,      // Collecting quiet samples (keep still!)
    CAL_PHASE_MOTION,     // Collecting motion samples (wave hand!)
    CAL_PHASE_READY       // Calibration complete
} calibration_phase_t;

#define CAL_SAMPLES 2000  // ~10 seconds at 200 pps per phase (20 sec total calibration)

// Motion detection state
// 
// NOTE: Currently using a SINGLE combined state for all clients.
// CSI from all clients is mixed together for calibration and detection.
// 
// FUTURE ENHANCEMENT: Per-client tracking could be implemented by:
// 1. Storing client MAC addresses when they connect (in wifi_event_handler)
// 2. Creating an array of motion_state_t, one per client (e.g., motion_state_t clients[MAX_STA_CONN])
// 3. In wifi_csi_cb(), extract source MAC from wifi_csi_info_t and route to correct client state
// 4. Run separate calibration and detection per client
// 5. Report per-client motion status to dashboard (e.g., "client1_motion":1, "client2_motion":0)
// 
// Benefits of per-client tracking:
// - Know WHICH signal path detected motion (localization hint)
// - Different sensitivity/threshold per path
// - Identify if one path is noisier than another
// - Could implement triangulation with 3+ clients
//
typedef struct {
    float smoothed_metric;        // Smoothed motion metric
    
    // Calibration
    calibration_phase_t cal_phase;
    float quiet_sum;              // Sum of metrics during quiet phase
    int quiet_samples;
    float quiet_level;            // Average metric when still
    
    float motion_sum;             // Sum of metrics during motion phase
    int motion_samples;
    float motion_level;           // Average metric when moving
    
    int motion_count;             // Consecutive motion detections
    int no_motion_count;          // Consecutive non-detections
    bool motion_detected;
    
    float current_threshold;
    float current_metric;         // For dashboard display
    float current_amplitude;      // For debug - should change with foil!
} motion_state_t;

// Single combined state for all clients (see NOTE above for per-client alternative)
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
        // Don't override calibration colors - just log
        if (motion.cal_phase == CAL_PHASE_READY) {
            neopixel_set(0, 0, 50);   // Blue when ready
        }
        // During calibration, the CSI callback handles LED colors
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
 * CSI Feature Extraction - TEMPORAL AMPLITUDE VARIANCE
 * 
 * ESP32-S3 CSI buffer format (per ESP-IDF v5.x documentation):
 * - Buffer contains [Imaginary, Real] pairs per subcarrier (NOT [Real, Imag]!)
 * - If first_word_invalid is true, skip first 4 bytes (2 subcarriers)
 * - Each I/Q is a signed 8-bit integer
 * 
 * Motion Detection: Track amplitude of each subcarrier over time.
 * When someone moves, amplitudes fluctuate. Variance over time = motion.
 * This is IMMUNE to clock drift (amplitude = |H| doesn't change with phase rotation).
 */

#define TEMPORAL_HISTORY 32   // Number of packets to track (at 200pps = 160ms window)
#define NUM_SUBCARRIERS 48    // Max subcarriers we track (after edge trimming)

// 2D buffer: amplitude history for each subcarrier
static float amp_history[NUM_SUBCARRIERS][TEMPORAL_HISTORY] = {0};
static int history_idx = 0;
static int history_count = 0;  // How many packets we've stored (up to TEMPORAL_HISTORY)

// Store debug info for dashboard
static int last_csi_len = 0;
static bool last_first_word_invalid = false;
static int debug_print_counter = 0;

static void extract_csi_features(wifi_csi_info_t *csi, float *mean_amp, float *motion_metric)
{
    if (csi->buf == NULL || csi->len < 20) {
        *mean_amp = 0;
        *motion_metric = 0;
        return;
    }
    
    int8_t *buf = csi->buf;
    
    // Store debug info
    last_csi_len = csi->len;
    last_first_word_invalid = csi->first_word_invalid;
    
    // Print raw CSI bytes every 500 packets for debugging (less frequent)
    debug_print_counter++;
    if (debug_print_counter >= 500) {
        debug_print_counter = 0;
        printf("CSI_DEBUG: len=%d, fwi=%d, history=%d\n", csi->len, csi->first_word_invalid, history_count);
    }
    
    // Check if first 4 bytes are invalid (ESP32 hardware limitation)
    int start_byte = 0;
    if (csi->first_word_invalid) {
        start_byte = 4;  // Skip first 2 subcarriers (4 bytes)
    }
    
    // Calculate how many subcarriers we have
    int num_subcarriers = (csi->len - start_byte) / 2;
    if (num_subcarriers > 64) num_subcarriers = 64;
    if (num_subcarriers < 20) {
        *mean_amp = 0;
        *motion_metric = 0;
        return;
    }
    
    // Skip noisy edge subcarriers (first/last 8)
    #define EDGE_SKIP 8
    int first_sc = EDGE_SKIP;
    int last_sc = num_subcarriers - EDGE_SKIP;
    int used_subcarriers = last_sc - first_sc;
    if (used_subcarriers > NUM_SUBCARRIERS) used_subcarriers = NUM_SUBCARRIERS;
    
    float amp_sum = 0;
    
    // Extract amplitude for each subcarrier and store in history
    for (int i = 0; i < used_subcarriers; i++) {
        int sc_idx = first_sc + i;
        int buf_idx = start_byte + (sc_idx * 2);
        float Q = (float)buf[buf_idx];        // Imaginary FIRST
        float I = (float)buf[buf_idx + 1];    // Real SECOND
        
        float amplitude = sqrtf(I * I + Q * Q);
        amp_sum += amplitude;
        
        // Store in circular buffer
        amp_history[i][history_idx] = amplitude;
    }
    
    // Advance circular buffer index
    history_idx = (history_idx + 1) % TEMPORAL_HISTORY;
    if (history_count < TEMPORAL_HISTORY) history_count++;
    
    *mean_amp = amp_sum / used_subcarriers;
    
    // Need enough history to calculate variance
    if (history_count < 8) {
        *motion_metric = 0;
        return;
    }
    
    // Calculate TEMPORAL variance for each subcarrier, then sum
    float total_variance = 0;
    
    for (int sc = 0; sc < used_subcarriers; sc++) {
        // Calculate mean amplitude for this subcarrier over time
        float mean = 0;
        for (int t = 0; t < history_count; t++) {
            mean += amp_history[sc][t];
        }
        mean /= history_count;
        
        // Calculate variance
        float var = 0;
        for (int t = 0; t < history_count; t++) {
            float diff = amp_history[sc][t] - mean;
            var += diff * diff;
        }
        var /= history_count;
        
        total_variance += var;
    }
    
    // Average variance across subcarriers, scale for usable range
    *motion_metric = (total_variance / used_subcarriers) * 10.0f;
}

/*
 * Two-Phase Calibration Motion Detection:
 * 
 * Phase 1 (QUIET): Collect 300 packets while still → quiet_level
 * Phase 2 (MOTION): Collect 300 packets while moving → motion_level
 * Auto-threshold: (quiet_level + motion_level) / 2
 */
static void update_motion_detection(float amplitude, float motion_metric, int rssi)
{
    // Apply EMA smoothing to the motion metric
    if (motion.smoothed_metric == 0) {
        motion.smoothed_metric = motion_metric;
    } else {
        motion.smoothed_metric = cfg_smoothing_alpha * motion_metric + 
                                 (1 - cfg_smoothing_alpha) * motion.smoothed_metric;
    }
    
    // Store for dashboard display
    motion.current_metric = motion.smoothed_metric;
    motion.current_amplitude = amplitude;  // Track amplitude - should change with foil!
    
    // ===== PHASE 1: QUIET CALIBRATION =====
    if (motion.cal_phase == CAL_PHASE_QUIET) {
        motion.quiet_sum += motion.smoothed_metric;
        motion.quiet_samples++;
        
        // Pulsing BLUE = keep still
        int brightness = 20 + (motion.quiet_samples % 30);
        neopixel_set(0, 0, brightness);
        
        if (motion.quiet_samples >= CAL_SAMPLES) {
            motion.quiet_level = motion.quiet_sum / CAL_SAMPLES;
            motion.cal_phase = CAL_PHASE_MOTION;
            
            ESP_LOGI(TAG, "=== PHASE 1 COMPLETE: QUIET ===");
            ESP_LOGI(TAG, "Quiet level: %.2f", motion.quiet_level);
            ESP_LOGI(TAG, ">>> NOW WAVE YOUR HAND! <<<");
            
            // Flash green to signal "now move!"
            for (int i = 0; i < 3; i++) {
                neopixel_set(0, 100, 0);
                vTaskDelay(pdMS_TO_TICKS(150));
                neopixel_set(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
        }
        return;
    }
    
    // ===== PHASE 2: MOTION CALIBRATION =====
    if (motion.cal_phase == CAL_PHASE_MOTION) {
        motion.motion_sum += motion.smoothed_metric;
        motion.motion_samples++;
        
        // Pulsing GREEN = wave your hand!
        int brightness = 30 + (motion.motion_samples % 40);
        neopixel_set(0, brightness, 0);
        
        if (motion.motion_samples >= CAL_SAMPLES) {
            motion.motion_level = motion.motion_sum / CAL_SAMPLES;
            
            // Calculate suggested threshold as midpoint between quiet and motion
            float auto_threshold = (motion.quiet_level + motion.motion_level) / 2.0f;
            // Only set auto-threshold if user hasn't manually set one
            // (check if still at default 5.0)
            if (cfg_min_threshold == 5.0f) {
                cfg_min_threshold = auto_threshold;
            }
            ESP_LOGI(TAG, "Suggested threshold: %.2f (current: %.2f)", auto_threshold, cfg_min_threshold);
            
            motion.cal_phase = CAL_PHASE_READY;
            
            ESP_LOGI(TAG, "=== PHASE 2 COMPLETE: MOTION ===");
            ESP_LOGI(TAG, "Motion level: %.2f", motion.motion_level);
            ESP_LOGI(TAG, "=== CALIBRATION COMPLETE ===");
            ESP_LOGI(TAG, "Quiet: %.2f | Motion: %.2f | Threshold: %.2f",
                     motion.quiet_level, motion.motion_level, cfg_min_threshold);
            
            // Flash white to signal "ready!"
            for (int i = 0; i < 2; i++) {
                neopixel_set(80, 80, 80);
                vTaskDelay(pdMS_TO_TICKS(200));
                neopixel_set(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            neopixel_set(0, 0, 50);  // Blue = ready
        }
        return;
    }
    
    // ===== DETECTION MODE =====
    // Use the user-set threshold directly (no RSSI adjustment)
    motion.current_threshold = cfg_min_threshold;
    
    // Detect motion when metric exceeds threshold
    bool instant_motion = (motion.smoothed_metric > motion.current_threshold);
    
    // Apply hysteresis
    if (instant_motion) {
        motion.motion_count++;
        motion.no_motion_count = 0;
    } else {
        motion.no_motion_count++;
        motion.motion_count = 0;
    }
    
    // Turn ON only after sustained detection
    if (!motion.motion_detected && motion.motion_count >= cfg_hysteresis_on) {
        motion.motion_detected = true;
    }
    // Turn OFF only after sustained no-detection
    else if (motion.motion_detected && motion.no_motion_count >= cfg_hysteresis_off) {
        motion.motion_detected = false;
    }
    
    // Update NeoPixel
    if (motion.motion_detected) {
        neopixel_set(0, 255, 0);  // Bright green = motion!
    } else {
        neopixel_set(0, 0, 30);   // Dim blue = idle
    }
}

/*
 * CSI Callback
 * 
 * NOTE: For per-client tracking, the source MAC is available in:
 *   data->mac[6] - MAC address of the device that sent this packet
 * 
 * To implement per-client detection:
 *   1. Compare data->mac against known client MACs
 *   2. Route to appropriate client-specific motion_state_t
 *   3. Process calibration/detection independently per client
 */
static void wifi_csi_cb(void *ctx, wifi_csi_info_t *data)
{
    // Accept packets with valid CSI data (min ~20 bytes for any useful data)
    if (data->buf == NULL || data->len < 20) {
        return;
    }
    
    // Per-client tracking would extract MAC here:
    // uint8_t *src_mac = data->mac;
    // int client_idx = find_client_by_mac(src_mac);
    
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
    ESP_LOGI(TAG, "=== TWO-PHASE CALIBRATION ===");
    ESP_LOGI(TAG, "Phase 1: Keep area STILL for %d packets (pulsing blue)...", CAL_SAMPLES);
    ESP_LOGI(TAG, "Phase 2: WAVE YOUR HAND for %d packets (pulsing green)...", CAL_SAMPLES);
    
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
        
        // Send extended status with calibration info, amplitude, and CSI debug info
        int len = snprintf(status_msg, sizeof(status_msg),
            "{\"motion\":%d,\"rssi\":%d,\"pps\":%lu,\"pkts\":%lu,\"cal\":%d,\"thresh\":%.4f,\"var\":%.4f,\"amp\":%.2f,\"csi_len\":%d,\"fwi\":%d,\"clients\":%d}",
            motion.motion_detected ? 1 : 0,
            last_rssi,
            (unsigned long)pps,
            (unsigned long)packets_received,
            (motion.cal_phase == CAL_PHASE_READY) ? 1 : 0,
            motion.current_threshold,
            motion.current_metric,
            motion.current_amplitude,
            last_csi_len,
            last_first_word_invalid ? 1 : 0,
            connected_clients);
        
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
                if (fval >= 0.01f && fval <= 0.5f) {
                    cfg_smoothing_alpha = fval;
                    ESP_LOGI(TAG, "Set smoothing_alpha = %.2f", cfg_smoothing_alpha);
                    cmd_received = true;
                }
            } else if (sscanf(rx_buffer, "threshold=%f", &fval) == 1) {
                if (fval >= 0.0f && fval <= 100.0f) {
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
                // Reset both calibration phases
                motion.cal_phase = CAL_PHASE_QUIET;
                motion.quiet_samples = 0;
                motion.quiet_sum = 0;
                motion.motion_samples = 0;
                motion.motion_sum = 0;
                motion.smoothed_metric = 0;
                ESP_LOGI(TAG, "=== RECALIBRATING ===");
                ESP_LOGI(TAG, "Phase 1: Keep area STILL for %d packets...", CAL_SAMPLES);
                cmd_received = true;
            }
            
            // Visual feedback: quick purple flash to acknowledge command
            if (cmd_received) {
                neopixel_set(80, 0, 120);  // Purple
                vTaskDelay(pdMS_TO_TICKS(100));
                // Return to current state color (calibration colors handled by CSI callback)
                if (motion.cal_phase == CAL_PHASE_READY) {
                    if (motion.motion_detected) {
                        neopixel_set(0, 255, 0);  // Green if motion
                    } else {
                        neopixel_set(0, 0, 30);   // Blue if idle
                    }
                }
                // During calibration, let the CSI callback handle colors
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
