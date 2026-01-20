# WiFi Motion Detection System - Complete Setup Documentation

**Built by Cole Studer** — X: [@photoncmndr](https://x.com/photoncmndr)

## Overview

This system uses WiFi Channel State Information (CSI) to detect human motion. When a person moves through the WiFi signal path between devices, the channel characteristics change, and these changes are detected and displayed on a web dashboard.

---

## Hardware Setup

### Devices
| Role | Board | Description |
|------|-------|-------------|
| **AP (Access Point)** | FeatherS3D (ESP32-S3) | Creates WiFi network, collects CSI, detects motion |
| **Client 1** | FeatherS3D (ESP32-S3) | Connects to AP, pings continuously |
| **Client 2** | FeatherS3D (ESP32-S3) | Connects to AP, pings continuously |

> **Finding COM ports**: Open Device Manager → Ports (COM & LPT) to see which port each ESP32 is assigned to.

### Physical Arrangement
```
        AP
       /  \
      /    \
Client1    Client2
```
- Motion is detected along the signal paths: Client1↔AP and Client2↔AP
- Best detection occurs when walking perpendicular through a signal path
- Triangle arrangement provides coverage of the enclosed area

### LED Indicators
| Color | Meaning |
|-------|---------|
| Yellow | Starting up |
| Pulsing Red | Disconnected / connecting |
| Pulsing Blue | Calibration Phase 1 (keep still) |
| Pulsing Green | Calibration Phase 2 (wave hand) |
| Dim Blue | Ready, no motion |
| Bright Green | Motion detected! |
| Purple flash | Command received |

---

## Network Architecture

### WiFi Configuration
| Setting | Value |
|---------|-------|
| SSID | `ESP32-AP` |
| Password | `esp32-ap` |
| Channel | 1 |
| Mode | 802.11n HT20 |
| Max Clients | 4 |

### IP Addresses
| Device | IP Address |
|--------|------------|
| AP | 192.168.4.1 |
| Clients | 192.168.4.x (DHCP assigned) |
| PC (when connected) | 192.168.4.x |

### UDP Communication
| Port | Direction | Purpose |
|------|-----------|---------|
| **8848** | AP → PC (broadcast to 192.168.4.255) | Status updates (JSON) every 100ms |
| **8849** | PC → AP (to 192.168.4.1) | Tuning commands |

### Status JSON Format (AP → Dashboard)
```json
{
  "motion": 1,           // 0 or 1
  "rssi": -45,           // Signal strength in dBm
  "pps": 200,            // Packets per second
  "pkts": 12345,         // Total packets received
  "cal": 1,              // Calibration complete (0 or 1)
  "thresh": 5.0,         // Current threshold
  "var": 3.5,            // Current variance (motion metric)
  "amp": 45.2,           // Mean amplitude
  "csi_len": 256,        // CSI buffer length
  "fwi": 1,              // First word invalid flag
  "clients": 3           // Connected WiFi clients (includes PC)
}
```

### Command Format (Dashboard → AP)
```
threshold=5.5      # Set motion threshold
alpha=0.03         # Set smoothing factor
hyst_on=3          # Hysteresis ON count
hyst_off=8         # Hysteresis OFF count
recalibrate        # Restart calibration
```

---

## Motion Detection Algorithm

### Temporal Amplitude Variance (Current Implementation)

This algorithm is **immune to clock drift** because amplitude doesn't change with phase rotation.

#### How It Works
1. **Extract CSI**: For each received packet, extract I/Q values for each subcarrier
2. **Calculate Amplitude**: `amplitude = sqrt(I² + Q²)` for each subcarrier
3. **Skip Edge Subcarriers**: First and last 8 subcarriers are noisy, skip them
4. **Store History**: Keep amplitude values for last 32 packets per subcarrier
5. **Calculate Temporal Variance**: For each subcarrier, calculate variance over time
6. **Sum Variances**: Add up variances across all subcarriers
7. **Scale**: Multiply by 10 for usable threshold range

#### Key Parameters
| Parameter | Value | Description |
|-----------|-------|-------------|
| `TEMPORAL_HISTORY` | 32 | Packets of history (~160ms at 200pps) |
| `NUM_SUBCARRIERS` | 48 | Subcarriers tracked (middle ones) |
| `EDGE_SKIP` | 8 | Subcarriers skipped on each edge |

#### Why Amplitude Instead of Phase?
| Metric | Clock Drift Impact | Stability |
|--------|-------------------|-----------|
| Raw Phase | Catastrophic (~1500 rad/packet) | Unusable |
| Differential Phase | ~1000x reduction | Usable but noisy |
| **Amplitude** | **Immune** | **Most stable** |

---

## Calibration Process

### Two-Phase Calibration
| Phase | Duration | LED | Action Required |
|-------|----------|-----|-----------------|
| 1 - Quiet | ~10 seconds | Pulsing Blue | Keep area completely still |
| 2 - Motion | ~10 seconds | Pulsing Green | Wave hand through signal path |
| Complete | - | White flash → Dim Blue | Ready for detection |

### Calibration Samples
- **CAL_SAMPLES**: 2000 packets per phase
- At 200 pps → ~10 seconds per phase → ~20 seconds total

### Auto-Threshold Calculation
```
auto_threshold = (quiet_level + motion_level) / 2
```
The threshold is set to the midpoint between the quiet baseline and motion level.

---

## Tunable Parameters

### Runtime Adjustable (via Dashboard)
| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| **Smoothing (alpha)** | 0.01 - 0.1 | 0.03 | EMA smoothing factor (lower = smoother) |
| **Threshold** | 0 - 100 | 5.0 | Motion triggers when variance exceeds this |
| **Hysteresis ON** | 1 - 15 | 3 | Consecutive detections to trigger ON |
| **Hysteresis OFF** | 1 - 30 | 8 | Consecutive non-detections to trigger OFF |

### Compile-Time Parameters (in main.c)
| Parameter | Value | Description |
|-----------|-------|-------------|
| `WIFI_CHANNEL` | 1 | WiFi channel (1-13) |
| `PING_INTERVAL_MS` | 10 | Client ping interval (100 pps) |
| `STATUS_INTERVAL_MS` | 100 | Dashboard update rate |
| `TEMPORAL_HISTORY` | 32 | Packet history window |
| `CAL_SAMPLES` | 2000 | Calibration samples per phase |

---

## Running the System

### Prerequisites
- ESP-IDF v5.x or v6.x installed
- Python 3.x with `pyserial` (optional, for restart button)
- PC connected to `ESP32-AP` WiFi network

### Flash Firmware

#### 1. Source ESP-IDF Environment
```powershell
. path/to/esp-idf/export.ps1
```

#### 2. Flash AP
```powershell
cd "active_ap"
idf.py build
idf.py -p COM_AP flash    # Replace COM_AP with your port
```

#### 3. Flash Clients
```powershell
cd "../active_client"
idf.py build
idf.py -p COM_CLIENT1 flash    # Replace with your ports
idf.py -p COM_CLIENT2 flash
```

### Run Dashboard
```powershell
cd "../active_ap"
python web_dashboard.py
```

### Access Dashboard
1. Connect PC WiFi to `ESP32-AP` (password: `esp32-ap`)
2. Open browser to: **http://localhost:8080**

---

## Dashboard Features

### Left Panel
- **Motion Status**: Large "MOTION!" / "NO MOTION" indicator
- **Signal (dBm)**: RSSI from clients
- **Packets/sec**: Current packet rate
- **Total Packets**: Cumulative count
- **Variance**: Current motion metric value
- **Threshold**: Current detection threshold
- **Calibration**: Status (READY or CALIBRATING)
- **Amplitude**: Mean CSI amplitude
- **Connected Clients**: WiFi client count (includes PC)

### Right Panel
- **Motion Activity Chart**: Green bars show motion history
- **Amplitude Variance Chart**: Line graph of variance over time
  - Cyan line: Variance
  - Red line: Threshold
  - Y-axis: Auto-scales to 2× threshold

### Live Tuning Controls
- **Smoothing**: Slider 0.01-0.1
- **Threshold**: Slider 0-100 with text input
- **Hysteresis ON/OFF**: Response timing
- **Recalibrate Button**: Restart calibration
- **Reset Dashboard**: Clear history
- **Restart System**: Reset all ESP32 devices

---

## Project Structure

### Files In Use
```
rv demo/WiFi-Motion-Detection/
├── active_ap/
│   ├── main/
│   │   ├── main.c              # AP firmware (CSI + motion detection)
│   │   ├── led_strip_encoder.c # NeoPixel driver
│   │   └── led_strip_encoder.h
│   └── web_dashboard.py        # Python dashboard server
├── active_client/
│   └── main/
│       ├── main.c              # Client firmware (ping only)
│       ├── led_strip_encoder.c
│       └── led_strip_encoder.h
└── _components/
    └── csi_component.h         # CSI collection functions
```

### Unused Files (Can Be Deleted)
- `active_ap/dashboard/` - Old PyQt dashboard
- `active_ap/camera_streaming.py`
- `active_ap/host_processing_*.py`
- `active_ap/mock_csi_sender.py`
- `active_ap/test_*.py`
- `_components/` (except csi_component.h)
- `udp_client/` folder
- `boards/` folder

---

## Future Enhancements

### Per-Client Motion Tracking
Currently, CSI from all clients is combined. Per-client tracking could provide:
- Which signal path detected motion (localization)
- Different sensitivity per path
- Triangulation with 3+ clients

Implementation notes are in `main.c` at the `motion_state_t` struct.

### Clock Drift Compensation
Hardware sync options:
- GPIO trigger wire between devices
- External precision oscillator (TCXO)

---

## Troubleshooting

| Issue | Possible Cause | Solution |
|-------|----------------|----------|
| No packets received | PC not on ESP32-AP WiFi | Connect PC to ESP32-AP network |
| Variance stuck at 0 | Calibration not complete | Wait for calibration or check client connections |
| 3 clients showing | PC counted as client | Normal - PC is a WiFi client too |
| Erratic variance | Interference on channel | Try different WIFI_CHANNEL |
| Motion not detected | Threshold too high | Lower threshold or recalibrate |
| Constant false positives | Threshold too low | Raise threshold |

---

## Version History

- **Current**: Temporal Amplitude Variance algorithm
- Immune to clock drift
- 10-second calibration phases
- Web dashboard with live tuning
- Two-column responsive layout
