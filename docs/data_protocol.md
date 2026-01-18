# WiFi Sonar Data Collection Protocol

Grounded in Hernandez 2022 Thesis (Ch. 5–7) and 2023 COMST survey.

## Pre-run checklist
1. Mount QT Py AP on tripod at 1.5 m height centered in sensing cell (see `hardware_diagram.txt`).
2. Place FeatherS3 stations per diagram; ensure USB-C cables do not shadow antennas.
3. Flash `wifi-sonar-ap` to QT Py and `wifi-sonar-sta` to each FeatherS3 (set unique `WIFI_SONAR_STA_LABEL` in menuconfig).
4. Power all nodes via USB 5 V, wait 10 s for Wi-Fi association logs.
5. On Windows host:
   - Run `scripts\setup_firewall.ps1 -Port 40123` (admin shell).
   - Start collector: `python scripts\collector_preprocessor.py --port 40123 --session <session_name>`.

## Collection phases

### 1. Empty room baseline
- Clear all motion (doors closed, HVAC steady).
- When prompted, capture 20 s (`--cal-empty-sec`) of empty data.
- Note ambient conditions (temperature, humidity) in your lab log.

### 2. Walking (nominal motion)
- After empty calibration, collector requests presence capture.
- Walk a rectangular loop covering all quadrants for 20 s.
- Label session metadata as `walk`.

### 3. Falling (rapid transient)
- Run a new session (`--session fall_mmdd_hhmm`).
- During presence capture, perform controlled fall onto crash mat centered 1.5 m from AP (Thesis p. 173).
- Repeat 3×, allowing 10 s stillness between falls.

### 4. Welding vs. grinding (high-frequency tools)
- Use metallic proxy tool (angle grinder or MIG welder dummy load) 2 m east of AP.
- Sessions:
  1. `welding_idle`: Torch energized, no arc, 15 s.
  2. `welding_arc`: Strike arc / simulate welder noise for 15 s segments.
  3. `grinding`: Run grinder across steel plate sweeps for 15 s.
- Maintain same cable routing to avoid multipath drift (PerCom 2023 Sec. 4.2).

### 5. Breathing micro-motion
- Subject lies supine midway between S2 and S3 (diagram).
- Collector already calibrated; record 60 s quiet breathing, then 60 s held breath.
- Note respiration rate manually for ground truth.

## Recording metadata
Capture the following in `data/session_notes.csv` (create if missing):
- Session name (matches CSV filename).
- Activity label (`empty`, `walk`, `fall`, `welding`, `grinding`, `breathing`).
- Participants involved.
- Notable anomalies (RF interference, packet loss).

## Post-run
1. Copy generated CSV(s) plus session notes into `data/archive/<date>/`.
2. Backup raw files to NAS per lab policy.
3. Update training notebook with new data paths before retraining.

