# Radio regulatory compliance (FCC Part 15 / amateur)

Not legal advice. This summarizes the engineering situation so you can make an
informed decision and verify against the current rules (47 CFR Part 15, especially
§15.247, §15.205, §15.209, and Part 97 for amateur). When in doubt, consult someone
qualified and measure your emissions.

## TL;DR

- **Receiving / sensing is fine.** Capturing raw IQ or CSI is passive; no
  transmitter emission rules apply to the RX/sensor role.
- **The default raw-IQ transmit (illuminator) is almost certainly *not*
  FCC-compliant as-is for open-air use** — not because spread spectrum at 2.4 GHz
  is illegal (it is expressly allowed), but because of *how* it transmits (below).
- Two supported ways to stay legitimate: build the **compliant Wi-Fi illumination
  mode** (uses the module's certified radio path), or operate the raw-IQ mode only
  **under an amateur license** or **on a dummy load / in a shielded enclosure**.

## Why the raw-IQ illuminator is a problem

1. **It operates outside the module's FCC equipment authorization.** Your
   ESP32-S3 board's FCC ID certifies it as a Wi-Fi/BLE transmitter running the
   certified stack. The raw-IQ (`rv_radio`) path emits an arbitrary waveform
   through the undocumented baseband DAC, bypassing the certified modem — a mode
   the grant never covered. Whoever makes that change (you) becomes responsible
   for meeting the Part 15 technical limits.

2. **The raw path is uncalibrated.** Transmitting without the Wi-Fi modem's
   filtering and shaping produces image and LO-leakage spurs (e.g. the ~−6 MHz
   spur noted in the ESP-SDR work) and uncalibrated power. Those spurious and
   out-of-band products are the most likely thing to exceed the restricted-band
   limits (§15.205 / §15.209) without external filtering.

## What the waveform gets *right* (in-band)

2.4 GHz ISM (2400–2483.5 MHz) under **§15.247** is written for digital / spread
spectrum modulation:

| limit (§15.247, digital modulation) | value | RadioVision |
|---|---|---|
| minimum 6 dB bandwidth | ≥ 500 kHz | 40 MHz ✓ (easily) |
| max conducted output power | 1 W (30 dBm) | low by design |
| power spectral density | ≤ 8 dBm / 3 kHz | very low (spread) ✓ |

The keyed DSSS waveform's whole point is a **low power spectral density** (we
measure ~30 dB below a Wi-Fi ping flood for equal detection SNR), which is exactly
what the PSD limit and good spectrum-sharing want. So the *in-band* behavior is
well-mannered; the issues are the **authorization status** and **spurs**, not the
modulation.

## Compliant ways to run it

| option | how | trade-off |
|---|---|---|
| **Compliant Wi-Fi illumination** (`CONFIG_RV_ILLUM_WIFI`) | TX emits probe frames through the **certified** Wi-Fi path (`esp_wifi_80211_tx`); RX senses via Wi-Fi **CSI** with the same gain-invariant detector | stays inside the module's grant; loses raw-IQ processing gain / DSSS low-PSD, so it is standard-CSI-class sensing |
| **Amateur license** (Part 97) | run the raw-IQ mode in the 13 cm amateur segment (~2390–2450 MHz) | must ID, stay in band; **§97.113 forbids obscuring message meaning**, so disable the encrypted DSSS *data payload* (the spreading code for sensing is OK) |
| **Bench / dummy load / shielded** | raw-IQ mode into an attenuated/50 Ω load or Faraday enclosure | development only, not deployment |
| **Formal equipment authorization** | test + certify the intentional radiator | proper but impractical for a hobby build |

## Build switch

`firmware/rv_sounder` → `idf.py menuconfig` → **RadioVision sounder → Illumination
source**:

- `RAW_DAC` (default): maximum-performance raw-IQ sounding via `rv_radio`.
  Open-air use needs an amateur license or a dummy load / shielded setup.
- `WIFI_CERTIFIED`: the compliant path. TX sends HT20 probe frames on the certified
  radio; RX runs the gain-invariant detector on Wi-Fi CSI. No undocumented
  registers are exercised at runtime.

The RX in `WIFI_CERTIFIED` mode reuses the same `rv_detector`; it is the honest,
in-grant fallback and corresponds to the "standard CSI" curves in
[VALIDATION](VALIDATION.md) (no processing gain, no DSSS interference advantage),
but with the gain-invariant feature that keeps it robust to AGC.
