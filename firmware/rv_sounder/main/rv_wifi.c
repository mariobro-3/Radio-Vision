/* rv_wifi - certified Wi-Fi illumination + CSI sensing. See rv_wifi.h. */
#include "rv_wifi.h"

#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if CONFIG_RV_PAYLOAD_ENCRYPT
#include "rv_dsp.h"              /* rv_payload_crypt (Part 15 only) */
#endif

static const char *TAG = "rv_wifi";

#define RV_HDR_LEN 24           /* 802.11 data header before the payload */

#if CONFIG_RV_PAYLOAD_ENCRYPT
static uint64_t s_key_lo, s_key_hi;
static uint64_t fnv1a(const char *s, uint64_t h)
{
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}
#endif

/* A minimal HT20 broadcast data frame used as the sounding probe. The receiver
 * only needs to *receive* it to estimate CSI; the payload is a fixed marker. */
static uint8_t s_probe[] = {
    0x08, 0x00,                         /* frame control: data */
    0x00, 0x00,                         /* duration */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* dest: broadcast */
    0x52, 0x56, 0x32, 0x00, 0x00, 0x01, /* src: "RV2.." */
    0x52, 0x56, 0x32, 0x00, 0x00, 0x01, /* bssid */
    0x00, 0x00,                         /* seq */
    'R', 'A', 'D', 'I', 'O', 'V', 'I', 'S', 'I', 'O', 'N', '-', 'P', 'R', 'O', 'B', 'E',
};

esp_err_t rv_wifi_init(int channel)
{
    if (channel < 1 || channel > 13) return ESP_ERR_INVALID_ARG;

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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* HT20 MCS0 so frames include the HT-LTF -> 64-subcarrier CSI at the RX.
     * Must be set after init and before start. */
    ESP_ERROR_CHECK(esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_MCS0_LGI));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));

#if CONFIG_RV_PAYLOAD_ENCRYPT
    s_key_lo = fnv1a(CONFIG_RV_PAYLOAD_KEY, 1469598103934665603ULL);
    s_key_hi = fnv1a(CONFIG_RV_PAYLOAD_KEY, 1099511628211ULL);
    ESP_LOGI(TAG, "probe payload encryption on (Part 15)");
#endif
    ESP_LOGI(TAG, "certified Wi-Fi up on channel %d (HT20 MCS0)", channel);
    return ESP_OK;
}

void rv_wifi_tx_probe(void)
{
    const void *buf = s_probe;
    int len = sizeof(s_probe);
#if CONFIG_RV_PAYLOAD_ENCRYPT
    /* encrypt a copy of the payload (Part 15 only; never compiled in RAW mode) */
    static uint8_t txf[sizeof(s_probe)];
    memcpy(txf, s_probe, sizeof(s_probe));
    rv_payload_crypt(txf + RV_HDR_LEN, (int)sizeof(s_probe) - RV_HDR_LEN,
                     s_key_lo, s_key_hi);
    buf = txf;
#endif
    esp_err_t e = esp_wifi_80211_tx(WIFI_IF_STA, buf, len, false);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "probe tx: %s", esp_err_to_name(e));
    }
}

esp_err_t rv_wifi_csi_start(wifi_csi_cb_t cb)
{
    wifi_csi_config_t csi = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        .ltf_merge_en      = true,
        .channel_filter_en = false,  /* keep subcarriers independent for sensing */
        .manu_scale        = false,
        .shift             = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI sensing enabled");
    return ESP_OK;
}
