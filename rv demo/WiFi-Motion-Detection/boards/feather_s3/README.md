# FeatherS3D board profile

This profile captures the tweaks needed to run the ESP32-CSI-Collection-and-Display
firmware on the Unexpected Maker **FeatherS3D** (ESP32-S3) board.

## Hardware recap

- ESP32-S3 dual Xtensa LX7 cores @ 240 MHz, 16 MB QSPI flash, 8 MB Octal PSRAM
- Native USB (USB-Serial/JTAG) for flashing + console
- On-board LiPo charger, fuel gauge, dual STEMMA QT connectors, castellated pads  
  See the [official FeatherS3D product page](https://esp32s3.com/feathers3d.html) for
  a full pinout and schematic.

## How the profile is wired in

1. Each `CMakeLists.txt` will look at the `CSI_BOARD` environment variable (defaulting
   to `feather_s3`) and automatically:
   - set `IDF_TARGET=esp32s3`
   - append `boards/feather_s3/sdkconfig.defaults` to the build so ESP-IDF generates
     an `sdkconfig` that matches the board (flash, PSRAM, USB console).
2. The defaults enable the Octal PSRAM, bump Wi-Fi buffer counts for CSI throughput
   and switch logging to the native USB/JTAG serial port, which is exposed over the
   board’s USB-C connector.

## Usage

```powershell
# PowerShell
$env:CSI_BOARD = "feather_s3"
idf.py set-target esp32s3 build flash monitor
```

or with POSIX shells:

```bash
CSI_BOARD=feather_s3 idf.py set-target esp32s3 build flash monitor
```

Running `idf.py set-target esp32s3` once per build tree is required so that the
generated `sdkconfig` matches the target. After that, the new defaults will keep
flash/PSRAM/USB settings in sync with the FeatherS3D hardware. Adjust Wi-Fi
credentials and CSI filters via `idf.py menuconfig` as usual.

