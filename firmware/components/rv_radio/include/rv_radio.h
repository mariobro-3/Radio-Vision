/*
 * rv_radio - ESP32-S3 raw-IQ radio using the undocumented Wi-Fi baseband
 * "dump" engine (Route B: direct PHY access, no second board needed to bridge).
 *
 * This is the on-chip reimplementation of the ESP-SDR capture/replay path. The
 * register map is the one confirmed independently by:
 *   - ESPARGOS esp-sdr (initial upload, main/s3_main.c)
 *   - cyber-murmel's esp-sdr-zephyr register documentation
 *   - h0m3us3r/eSpDR (who first found the SRAM->DAC playback)
 * See docs/UNDOCUMENTED_PHY.md.
 *
 * IMPORTANT: this pokes undocumented registers and calls PHY-internal symbols
 * (esp_phy_wifi_tx_tone from esp_phy_cert_test.h; adctrig/force_rx_gain from
 * librftest.a). It compiles and links against ESP-IDF v5.5.x for esp32s3, but
 * must be brought up and calibrated on real hardware - it cannot be validated
 * in QEMU, which does not emulate the RF path. Transmitting may require a radio
 * licence and/or a dummy load; keep power low.
 *
 * ESP_SDR word format (one 32-bit word per complex sample):
 *   bits 0..9   I (10-bit signed)
 *   bits 10..19 Q (10-bit signed)
 *   bits 20..27 AGC gain index (RX only)
 */
#ifndef RV_RADIO_H
#define RV_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "rv_dsp.h"   /* rv_cf */

#ifdef __cplusplus
extern "C" {
#endif

/* Max samples the single 64 KiB capture bank holds (16380 complex words). */
#define RV_RADIO_MAX_SAMPLES 16380

/* RX sample-rate codes understood by the dump engine. */
typedef enum {
    RV_RATE_80M = 0,   /* 80 MS/s */
    RV_RATE_40M = 1,   /* 40 MS/s (80 MS/s through the decimation filter)     */
    RV_RATE_16M = 6,   /* 16 MS/s separate path                               */
} rv_rate_t;

/*
 * Bring up the radio on a standard Wi-Fi channel (1..13). Uses the normal
 * esp_wifi init so the PHY is tuned, calibrated and RX-enabled, then reserves
 * the capture SRAM bank. Call once. Returns ESP_OK on success.
 */
esp_err_t rv_radio_init(int channel);

/* Fix the RX gain to a calibrated index (0..82); pass <0 to hand back to AGC.
 * Fixing gain removes AGC steps from the capture (the DSP is gain-invariant
 * anyway, but a steady gain gives cleaner coherent integration). */
void rv_radio_set_rx_gain(int index);

/*
 * Capture `nsamp` complex samples at `rate` into `out` (rv_cf, scaled to about
 * [-1,1]). If gain_idx_out != NULL it receives the AGC index of the first
 * sample. Returns nsamp on success, negative on timeout/argument error.
 * Runs inside a critical section; keep nsamp <= RV_RADIO_MAX_SAMPLES.
 */
int rv_radio_capture(rv_cf *out, int nsamp, rv_rate_t rate, uint8_t *gain_idx_out);

/*
 * Start continuously replaying `wave` (nsamp complex samples, |sample|<=1) out
 * of the TX DAC, gapless, until rv_radio_replay_stop(). rate80 selects 80 MS/s
 * (else 40). This is the sounding transmitter for the bistatic link.
 */
esp_err_t rv_radio_replay_start(const rv_cf *wave, int nsamp, bool rate80);

/* Stop replay and return the TX chain to idle. */
void rv_radio_replay_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* RV_RADIO_H */
