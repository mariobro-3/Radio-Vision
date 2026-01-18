# WiFi Sonar Edge Suite (ESP32 CSI)

Built from Hernandez ESP32-CSI playbook (COMST 2023 + Thesis 2022). Targets 1× QT Py ESP32-S2 AP + 4× FeatherS3 STAs + Windows 11 collector.

## Contents
- `docs/hardware_diagram.txt` – ASCII placement guide.
- `firmware/wifi-sonar-ap` – QT Py scheduler (20 MHz, channel 6).
- `firmware/wifi-sonar-sta` – FeatherS3 CSI streamer (amplitude only).
- `scripts/setup_firewall.ps1` – Windows firewall rules for UDP streaming.
- `scripts/collector_preprocessor.py` – Multi-threaded UDP collector + Hampel + PCA + presence detector.
- `docs/data_protocol.md` – Activity capture SOP (empty, walk, fall, welding, grinding, breathing).
- `notebooks/training.ipynb` – PyTorch workflow (presence → activity → pose).
- `docs/roadmap.md` – Next-step backlog (fall detection, welding vs. grinding, breathing).

## Quickstart
1. Install ESP-IDF 5.x and set `IDF_PATH`.
2. Flash QT Py AP:
   ```bash
   cd firmware/wifi-sonar-ap
   idf.py set-target esp32s2 build flash
   ```
3. Flash each FeatherS3 STA (unique `WIFI_SONAR_STA_LABEL` + static IP):
   ```bash
   cd firmware/wifi-sonar-sta
   idf.py set-target esp32s3 build flash
   ```
4. On Windows host:
   ```powershell
   # Allow collector UDP port
   scripts\\setup_firewall.ps1 -Port 40123

   # Start collector/preprocessor
   python scripts\\collector_preprocessor.py --session lab_smoke_test
   ```
5. Follow prompts to capture empty + presence calibrations, then run activities per `docs/data_protocol.md`.
6. Use `notebooks/training.ipynb` to train PyTorch presence/activity/pose heads on generated CSVs.

## Design highlights
- Fixed 20 MHz, channel 6, HT-LTF enabled (64 subcarriers) as required for ESP32 CSI stability.
- AP sends 120 Hz per-link UDP bursts (400–480 pkt/s aggregate) → STAs derive CSI for each ping.
- STA firmware streams amplitude-only CSI via UDP with MAC-tagged headers; Hampel + PCA executed on Windows for maximal throughput (UDP > UART per Thesis p. 189).
- Collector writes synchronized CSVs and hosts live PCA-based presence detection to validate deployments in real time.

