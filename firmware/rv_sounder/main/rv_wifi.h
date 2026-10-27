/*
 * rv_wifi - certified-path illumination + CSI sensing for the compliant build
 * (CONFIG_RV_ILLUM_WIFI). Uses only the standard, FCC-certified Wi-Fi radio:
 * esp_wifi_80211_tx for the probe, and the Wi-Fi CSI callback for sensing. No
 * undocumented registers are touched. See docs/COMPLIANCE.md.
 */
#ifndef RV_WIFI_H
#define RV_WIFI_H

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up Wi-Fi (STA, promiscuous, fixed channel) with an HT20 TX rate so the
 * probe frames carry an HT-LTF the receiver can turn into 64-subcarrier CSI. */
esp_err_t rv_wifi_init(int channel);

/* Transmit one HT20 broadcast probe frame through the certified radio. */
void rv_wifi_tx_probe(void);

/* Start Wi-Fi CSI with the given receive callback. */
esp_err_t rv_wifi_csi_start(wifi_csi_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* RV_WIFI_H */
