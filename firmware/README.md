# WiFi Sonar Firmware (ESP-IDF 5.x)

Two projects live under `firmware/`:

| Project | Target | Role |
|---------|--------|------|
| `wifi-sonar-ap` | Adafruit QT Py ESP32-S2 | Schedules 120 Hz per-link UDP bursts (Thesis p. 141) |
| `wifi-sonar-sta` | 4× Adafruit FeatherS3 | Extracts CSI amplitudes, streams via UDP (Thesis p. 152) |

## Common build steps
```bash
cd firmware/wifi-sonar-ap
idf.py set-target esp32s2
idf.py build flash monitor

cd ../wifi-sonar-sta
idf.py set-target esp32s3
idf.py build flash monitor
```

## Recommended `sdkconfig.defaults` deltas
- `CONFIG_FREERTOS_HZ=1000` → 1 ms scheduler resolution keeps 120 Hz ping loop stable.
- `CONFIG_ESP32_WIFI_CSI_ENABLED=y` (STA only) to allow `esp_wifi_set_csi_rx_cb`.
- `CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM=64` increases burst tolerance (COMST 2023 Sec. 5.2).
- `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` matches FeatherS3 + QT Py flash layout.

IDs automatically merge during `idf.py build`; run `idf.py menuconfig` if you need to override.

## Menuconfig quick recipes

### Access point
```
idf.py menuconfig
  -> WiFi Sonar AP Settings
      (sonar-ap-20mhz) SoftAP SSID
      (sonarCSI2025) SoftAP password
      (6) Primary channel
      (120) Per-link ping rate (Hz)
      (3333) UDP target port
      (192.168.4.2 / .3 / .4 / .5) STA IPv4 list
```

### Stations (run per FeatherS3, set unique label + static IP)
```
idf.py menuconfig
  -> WiFi Sonar STA Settings
      (S1..S4) Station label
      (sonar-ap-20mhz) Target SSID
      (sonarCSI2025) Target password
      (192.168.4.x) Static IPv4 (unique per STA) or 0.0.0.0 for DHCP
      (192.168.4.10) Collector IPv4 (Windows host)
      (40123) Collector UDP port
      (32) CSI queue depth
```

Ensure every STA compiles with a unique `WIFI_SONAR_STA_LABEL` so the collector can differentiate MACs in CSVs.

