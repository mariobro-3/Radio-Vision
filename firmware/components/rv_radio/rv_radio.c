/*
 * rv_radio - ESP32-S3 raw-IQ capture + replay over the undocumented dump engine.
 * See include/rv_radio.h for the register-map provenance and safety notes.
 */
#include "rv_radio.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "soc/soc.h"
#include "esp_rom_sys.h"
#include "esp_phy_cert_test.h"      /* esp_phy_wifi_tx_tone() */
#include "heap_memory_layout.h"     /* SOC_RESERVE_MEMORY_REGION */

static const char *TAG = "rv_radio";

/* --- confirmed ESP32-S3 dump-engine registers (docs/UNDOCUMENTED_PHY.md) --- */
#define RV_IQ_APERTURE     0x3fcd0000u      /* capture/playback SRAM bank (bank 2) */
#define RV_SRAM_OWNER_REG  0x600c101cu      /* SENSITIVE_INTERNAL_SRAM_USAGE_3   */
#define RV_SRAM_BANK_BIT   (1u << 2)        /* hand bank 2 to the dump engine    */
#define RV_DUMP_CTRL_REG   0x60033d5cu      /* RX: run/trigger/count             */
#define RV_DUMP_WRIDX_REG  0x60033d60u      /* RX words written (RO)             */
#define RV_DUMP_DAC_REG    0x60033d64u      /* TX: run/trigger/count            */
#define RV_DUMP_CONFIG_REG 0x60033d90u      /* lane selectors                    */
#define RV_DUMP_LANES_IQ   0x000c2040u      /* lanes 0,1,2,3 = receive I/Q       */

#define RV_DUMP_RUN        (1u << 31)
#define RV_DUMP_TRIGGER    (1u << 19)
#define RV_DUMP_DONE       (1u << 18)
#define RV_RATE40_BIT      (1u << 15)
#define RV_RATE16_BIT      (1u << 16)
#define RV_DAC_RATE80_BIT  (1u << 15)
#define RV_MARKER          0xa5a0055au

/* PHY-internal helpers: defined in librftest.a / libphy.a, no public header. */
extern void force_rx_gain(unsigned enable, unsigned index, unsigned reserved);

/* Keep the heap and static data out of the capture bank. */
SOC_RESERVE_MEMORY_REGION(RV_IQ_APERTURE, RV_IQ_APERTURE + 0x10000u, rv_rf_dump);

static volatile uint32_t *const IQ = (volatile uint32_t *)RV_IQ_APERTURE;
static portMUX_TYPE rv_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_channel = 1;
static bool s_inited = false;

static inline int sext10(uint32_t v)
{
    v &= 0x3ffu;
    return (v & 0x200u) ? (int)(v | 0xfffffc00u) : (int)v;
}

esp_err_t rv_radio_init(int channel)
{
    if (channel < 1 || channel > 13) return ESP_ERR_INVALID_ARG;
    s_channel = channel;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    /* NULL mode: bring the PHY up (tuned, calibrated) without associating. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

    s_inited = true;
    ESP_LOGI(TAG, "radio up on channel %d, capture bank @ 0x%08x", channel,
             (unsigned)RV_IQ_APERTURE);
    return ESP_OK;
}

void rv_radio_set_rx_gain(int index)
{
    if (index < 0) force_rx_gain(0, 0, 0);
    else           force_rx_gain(1, (unsigned)index, 0);
}

int IRAM_ATTR rv_radio_capture(rv_cf *out, int nsamp, rv_rate_t rate,
                               uint8_t *gain_idx_out)
{
    if (!s_inited || out == NULL) return -1;
    if (nsamp < 256 || nsamp > RV_RADIO_MAX_SAMPLES) return -2;

    uint32_t rate_bits = (rate == RV_RATE_40M) ? RV_RATE40_BIT :
                         (rate == RV_RATE_16M) ? RV_RATE16_BIT : 0u;

    for (int i = 0; i < nsamp; i++) IQ[i] = RV_MARKER;

    taskENTER_CRITICAL(&rv_mux);
    uint32_t owner = REG_READ(RV_SRAM_OWNER_REG);
    REG_WRITE(RV_DUMP_CTRL_REG, 0);
    REG_WRITE(RV_DUMP_CONFIG_REG, RV_DUMP_LANES_IQ);
    REG_WRITE(RV_SRAM_OWNER_REG, (owner & ~0xfu) | RV_SRAM_BANK_BIT);
    uint32_t ctrl = RV_DUMP_RUN | rate_bits | (uint32_t)nsamp;
    REG_WRITE(RV_DUMP_CTRL_REG, ctrl);
    REG_WRITE(RV_DUMP_CTRL_REG, ctrl | RV_DUMP_TRIGGER);
    REG_WRITE(RV_DUMP_CTRL_REG, ctrl);
    int64_t t0 = esp_timer_get_time();
    bool done;
    do {
        done = (REG_READ(RV_DUMP_CTRL_REG) & RV_DUMP_DONE) != 0;
    } while (!done && (esp_timer_get_time() - t0) < 20000);
    REG_WRITE(RV_DUMP_CTRL_REG, 0);
    REG_WRITE(RV_SRAM_OWNER_REG, owner);
    taskEXIT_CRITICAL(&rv_mux);

    if (!done) {
        ESP_LOGW(TAG, "capture timeout");
        return -3;
    }
    if (gain_idx_out) *gain_idx_out = (uint8_t)((IQ[0] >> 20) & 0xffu);

    const float inv = 1.0f / 512.0f;
    for (int i = 0; i < nsamp; i++) {
        uint32_t w = IQ[i];
        if (w == RV_MARKER) return -4;            /* truncated capture */
        out[i].re = (float)sext10(w) * inv;
        out[i].im = (float)sext10(w >> 10) * inv;
    }
    return nsamp;
}

esp_err_t rv_radio_replay_start(const rv_cf *wave, int nsamp, bool rate80)
{
    if (!s_inited || wave == NULL) return ESP_ERR_INVALID_STATE;
    if (nsamp < 256 || nsamp > RV_RADIO_MAX_SAMPLES) return ESP_ERR_INVALID_ARG;

    /* pack the waveform into IQ10 words (CPU owns the bank here) */
    for (int i = 0; i < nsamp; i++) {
        int I = (int)lrintf(wave[i].re * 511.0f);
        int Q = (int)lrintf(wave[i].im * 511.0f);
        if (I > 511)  I = 511;
        if (I < -512) I = -512;
        if (Q > 511)  Q = 511;
        if (Q < -512) Q = -512;
        IQ[i] = ((uint32_t)(I & 0x3ff)) | (((uint32_t)(Q & 0x3ff)) << 10);
    }

    /* power up the TX chain and tune (the stock tone is then overridden by the
     * DAC replay stream). backoff 80 = conservative, low power. */
    esp_phy_wifi_tx_tone(1, (uint32_t)s_channel, 80);

    taskENTER_CRITICAL(&rv_mux);
    uint32_t owner = REG_READ(RV_SRAM_OWNER_REG);
    REG_WRITE(RV_DUMP_CTRL_REG, 0);
    REG_WRITE(RV_DUMP_DAC_REG, 0);
    REG_WRITE(RV_SRAM_OWNER_REG, (owner & ~0xfu) | RV_SRAM_BANK_BIT);
    /* run + trigger held => gapless loop of the first nsamp words */
    uint32_t ctrl = RV_DUMP_RUN | RV_DUMP_TRIGGER | (uint32_t)nsamp;
    if (rate80) ctrl |= RV_DAC_RATE80_BIT;
    REG_WRITE(RV_DUMP_DAC_REG, ctrl);
    taskEXIT_CRITICAL(&rv_mux);

    ESP_LOGI(TAG, "replay started: %d samples @ %d MS/s (loop)", nsamp,
             rate80 ? 80 : 40);
    return ESP_OK;
}

void rv_radio_replay_stop(void)
{
    taskENTER_CRITICAL(&rv_mux);
    REG_WRITE(RV_DUMP_DAC_REG, 0);
    taskEXIT_CRITICAL(&rv_mux);
    esp_phy_wifi_tx_tone(0, (uint32_t)s_channel, 80);
    ESP_LOGI(TAG, "replay stopped");
}
