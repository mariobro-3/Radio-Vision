# The undocumented ESP32-S3 raw-IQ path (RX capture + TX replay)

This is the "Route B" radio layer: the ESP32-S3's own Wi-Fi radio used as a
receive-and-transmit SDR, with no extra hardware, by poking the undocumented
baseband "dump" engine. `firmware/components/rv_radio` implements it.

## Provenance (why we trust the register map)

The same register addresses and bit layout are reported independently by three
efforts, which is why this is safe to reimplement rather than guess:

1. **ESPARGOS `esp-sdr`** - the initial public upload (`main/s3_main.c`) shipped
   a full S3 capture **and** replay/tone path; it was removed two commits later
   in a "remove lots of dead code" cleanup. That deleted code is the primary
   reference for the S3 TX side.
2. **cyber-murmel `esp-sdr-zephyr`** - publishes a register table generated from
   an SVD plus decompilation of `mac_common.o`/`libphy.a`, and a documented TX
   API (`esp_sdr_tx_play`). This is the ["ESP-SDR Can Now
   Transmit"](https://hackaday.com/2026/10/13/esp-sdr-can-now-transmit/) work.
3. **h0m3us3r `eSpDR`** - first found the SRAM->DAC playback engine.

## Mechanism

The receiver's ADC normally feeds the fixed-function Wi-Fi modem. A debug "dump
engine" (used by Espressif's factory RF test, `librftest.a::adctrig`) can instead
copy raw baseband samples straight into an on-chip SRAM bank, one 32-bit word per
complex sample. The CPU then reads that memory. The same engine runs in reverse
to stream words from SRAM out through the TX DAC.

```
antenna -> LNA/mixer -> ADC --(normal)--> Wi-Fi modem
                         |
                         +--(dump engine)--> SRAM bank 0x3fcd0000 -> CPU
SRAM bank 0x3fcd0000 --(DAC engine)--> mixer/PA -> antenna   (replay)
```

## ESP32-S3 registers (confirmed)

| Name | Address | Purpose |
|------|---------|---------|
| capture/playback SRAM bank | `0x3fcd0000` (64 KiB, bank 2) | holds up to 16380 IQ words |
| `SENSITIVE_...SRAM_USAGE_3` | `0x600c101c` | one-hot: bit 2 hands bank 2 to the engine |
| `DUMP_CTRL` (RX) | `0x60033d5c` | bit31 run, bit19 trigger, bit18 done, bit15 40M, bit16 16M, bits0-13 count |
| `DUMP_WRITE_INDEX` | `0x60033d60` | words written / live ring index |
| `DUMP_DAC_TRIG` (TX) | `0x60033d64` | bit31 run, bit19 trigger(held=gapless loop), bit18 done, bit15 80M, bits0-13 count |
| `DUMP_CONFIG` (lanes) | `0x60033d90` | `0x000c2040` selects sources 0,1,2,3 = RX I/Q |
| `AGC_FORCE` | `0x6001c02c` | forced RX gain index + force bit (via `force_rx_gain()`) |
| `FE_TX_CTRL` | `0x60006000` | force TX on + TX gain index |

Captured word: bits 0-9 = I (10-bit signed), 10-19 = Q (10-bit signed),
20-27 = AGC gain index. TX word: same I/Q layout (no gain field).

## Sequences used by `rv_radio`

RX one-shot (`rv_radio_capture`): fill the bank with a marker; in a critical
section take the bank, write lanes, write `RUN|rate|count`, pulse `TRIGGER`
(1 then 0), poll `DONE` (20 ms timeout), release the bank; then unpack
I/Q and reject any word still holding the marker (truncated capture).

TX loop (`rv_radio_replay_start`): pack the waveform into the bank (CPU must own
it), `esp_phy_wifi_tx_tone(1, chan, backoff)` to power and tune the TX chain,
then take the bank and write `RUN|TRIGGER|count` to `DUMP_DAC_TRIG` (trigger held
= gapless loop of the first `count` words until cleared).

## What this build relies on from ESP-IDF

- `esp_phy_wifi_tx_tone()` - public in `esp_phy_cert_test.h`, defined in
  `libphy.a` (needs `CONFIG_ESP_PHY_ENABLE_CERT_TEST=y`).
- `force_rx_gain()` - in `librftest.a`, declared `extern` by us.
- PHY bring-up/tuning uses the normal `esp_wifi` path on a standard channel,
  which is all the sounder needs (no arbitrary-frequency PLL poking, so the
  exotic `rom_pbus_*` / `rom_chip_i2c_*` symbols are not required).

## Status / caveats

`rv_radio` **compiles and links** against ESP-IDF v5.5.6 for esp32s3. It **cannot
be validated in QEMU** (no RF model) and must be brought up on hardware: the
dump-engine timing assumes a fixed 240 MHz clock (set), the TX path needs the
front end actually forced on, and transmit power/image are uncalibrated.
**Transmitting in the 2.4 GHz band may require a licence; use a dummy load or
attenuated link and keep the backoff high.** The receive path is harmless.
