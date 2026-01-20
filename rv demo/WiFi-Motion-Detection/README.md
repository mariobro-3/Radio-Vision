# WiFi Motion Detection System

A WiFi-based motion detection system using Channel State Information (CSI) from ESP32-S3 devices. Detects human presence by analyzing how movement affects WiFi signal propagation.

## Quick Start

### Hardware Required
- 3x FeatherS3D boards (ESP32-S3)
- USB cables for programming

### Setup
```powershell
# 1. Source ESP-IDF
. C:\Users\laser\esp-idf\export.ps1

# 2. Flash AP (COM19)
cd "WiFi-Motion-Detection/active_ap"
idf.py build && idf.py -p COM19 flash

# 3. Flash Clients (COM15, COM17)
cd "../active_client"
idf.py build && idf.py -p COM15 flash && idf.py -p COM17 flash

# 4. Run Dashboard
cd "../active_ap"
python web_dashboard.py
```

### Access Dashboard
1. Connect PC WiFi to `ESP32-AP` (password: `esp32-ap`)
2. Open browser: **http://localhost:8080**
3. Wait ~20 seconds for calibration to complete

---

## System Architecture

```
Client1 (COM15) ──ping──┐
                        ├──> AP (COM19) ──UDP──> Dashboard (localhost:8080)
Client2 (COM17) ──ping──┘        │
                                 ↓
                         CSI extraction
                         Motion detection
                         NeoPixel feedback
```

### How It Works
1. **Clients** continuously ping the AP at 100 pings/sec each
2. **AP** extracts CSI from received packets
3. **Temporal Amplitude Variance** algorithm detects motion
4. **Dashboard** displays real-time status via UDP broadcast

---

## Motion Detection Algorithm

Uses **Temporal Amplitude Variance** - immune to clock drift between devices.

| Step | Description |
|------|-------------|
| 1 | Extract I/Q values from CSI buffer |
| 2 | Calculate amplitude: `sqrt(I² + Q²)` per subcarrier |
| 3 | Track amplitude history over 32 packets (~160ms) |
| 4 | Calculate variance over time for each subcarrier |
| 5 | Sum variances across subcarriers = motion metric |
| 6 | Compare to threshold → motion detected |

### Why Amplitude?
- **Phase-based methods** are destroyed by clock drift between unsynchronized devices
- **Amplitude** is immune - rotating phase doesn't change vector length

---

## Calibration

Two-phase calibration runs on startup (~20 seconds total):

| Phase | LED | Duration | Action |
|-------|-----|----------|--------|
| 1 - Quiet | Pulsing Blue | ~10 sec | Keep area still |
| 2 - Motion | Pulsing Green | ~10 sec | Wave hand through signal path |
| Ready | Dim Blue | - | Detection active |

Threshold is auto-calculated as midpoint between quiet and motion levels.

---

## Network Configuration

| Setting | Value |
|---------|-------|
| SSID | `ESP32-AP` |
| Password | `esp32-ap` |
| Channel | 1 |
| AP IP | 192.168.4.1 |

### UDP Ports
| Port | Purpose |
|------|---------|
| 8848 | Status broadcast (AP → Dashboard) |
| 8849 | Commands (Dashboard → AP) |

---

## Dashboard Features

### Live Metrics
- Motion status (big indicator)
- RSSI, packets/sec, total packets
- Variance and threshold values
- Calibration status

### Charts
- Motion activity (green bars)
- Variance over time (with threshold line)

### Tuning Controls
- **Smoothing**: 0.01-0.1 (lower = smoother)
- **Threshold**: 0-100 (motion triggers above this)
- **Hysteresis ON/OFF**: Response timing
- **Recalibrate**: Restart calibration

---

## LED Status Guide

| Color | Meaning |
|-------|---------|
| Yellow | Starting up |
| Pulsing Red | Disconnected |
| Pulsing Blue | Calibration Phase 1 |
| Pulsing Green | Calibration Phase 2 |
| Dim Blue | Ready, no motion |
| Bright Green | Motion detected |
| Purple flash | Command received |

---

## Project Structure

### Active Files
```
├── active_ap/
│   ├── main/main.c          # AP firmware
│   └── web_dashboard.py     # Web dashboard
├── active_client/
│   └── main/main.c          # Client firmware  
├── _components/
│   └── csi_component.h      # CSI functions
└── SETUP_DOCUMENTATION.md   # Detailed docs
```

### Unused (can delete)
- `active_ap/dashboard/` (old PyQt)
- `active_ap/host_processing_*.py`
- `active_ap/test_*.py`
- `udp_client/`

---

## Detailed Documentation

See **[SETUP_DOCUMENTATION.md](SETUP_DOCUMENTATION.md)** for:
- Complete parameter reference
- Algorithm details
- Troubleshooting guide
- Future enhancement notes

---

## Credits

**Built by Cole Studer** — X: [@photoncmndr](https://x.com/photoncmndr)

Based on [ESP32-CSI-Tool](https://github.com/StevenMHernandez/ESP32-CSI-Tool)

Modified for:
- FeatherS3D (ESP32-S3) support
- Temporal amplitude variance algorithm
- Web-based dashboard
- Two-phase calibration
- Real-time tuning
