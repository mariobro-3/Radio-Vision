# Audit of the original Radio-Vision (WiFi CSI motion detection)

Repo audited: `mariobro-3/Radio-Vision` @ `287b1a9` (the `rv demo/WiFi-Motion-Detection`
tree). It is a fork of StevenMHernandez/ESP32-CSI-Tool with a custom AP firmware,
a ping-only client, and a Python web dashboard. The concept is sound; the issues
below are why it is unreliable in practice and why v2 replaces the sensing method
rather than patching it.

Severity: **H** breaks reliability, **M** meaningful, **L** minor.

## 1. Signal-processing / math

- **(H) The motion metric is not gain-invariant.** `extract_csi_features()`
  sums the temporal variance of raw sub-carrier amplitude and scales by 10;
  `update_motion_detection()` thresholds it against an absolute number
  (`cfg_min_threshold`, default 5.0, plus `BASE_THRESHOLD` 8 %). Raw ESP32 CSI
  amplitude is scaled by the receiver AGC, which steps whenever the RSSI changes
  (someone moves, a client re-associates, interference). Variance scales with
  amplitude squared, so an AGC step looks exactly like motion. Our simulation
  reproduces this: under 3 dB of AGC jitter the amplitude-variance metric drops
  to **AUC 0.565** (near chance). Modern CSI stacks (espressif/esp-csi,
  ESPectre) use gain-invariant features (coefficient of variation, std/mean) for
  precisely this reason.

- **(H) No AGC/gain compensation.** `csi_component.h` sets `manu_scale = 0`
  (automatic scaling) and the firmware never reads the gain state. espressif's
  current example compensates every frame with `esp_csi_gain_ctrl_*`
  (`fft_gain`, `agc_gain`). Without it the amplitude baseline wanders.

- **(M) Sub-carrier handling is ad hoc and mixes incomparable bins.** The code
  does `num_subcarriers = (len - start_byte)/2`, clamps to 64, trims a fixed 8
  bins per edge, and treats the buffer as a flat array. The actual HT20 CSI
  buffer concatenates LLTF and HT-LTF fields with their own index maps and
  contains null (DC) and pilot sub-carriers that carry no channel information.
  Averaging amplitude/variance across that mixture dilutes the useful bins.

- **(M) Calibration threshold is a 2-point midpoint.** `auto_threshold =
  (quiet_level + motion_level)/2`. There is no notion of a false-alarm rate;
  the threshold depends entirely on how vigorously the installer waves during
  phase 2. A statistics-based threshold (quiet mean + k·σ, or a quiet
  percentile) is both unsupervised and tied to a target false-alarm rate.

- **(M) The time window is packet-count based, not time based.**
  `TEMPORAL_HISTORY = 32` is described as "160 ms at 200 pps". If the packet
  rate drops (interference, a client drops), the effective window silently
  changes and so does the metric, with no re-normalization.

- **(L) Phase is discarded.** Amplitude-only is a *reasonable* choice given
  unsynchronized radios (raw CSI phase is destroyed by CFO/sampling-time
  offset), but it throws away most of the channel information. v2 keeps the
  coherent channel by controlling the probe waveform and estimating/removing
  the CFO.

## 2. Assumptions

- **(H) "Amplitude is immune to clock drift" is used to justify discarding all
  coherent processing.** True for *raw* phase, but it leaves the system with a
  low-information feature and the gain problem above. The right fix is not "use
  amplitude", it is "control the waveform so the coherent channel is
  recoverable" (v2).
- **(M) All clients' CSI is merged into one global `motion_state_t`.** Different
  links have different SNR and geometry; merging them couples a noisy path into
  a clean one and precludes localization. (The code comments acknowledge this as
  future work.)
- **(M) Flooding the channel with 200 pings/s is assumed acceptable.** It is a
  continuous, full-power, broadband load on one Wi-Fi channel and is the
  opposite of the "minimize interference" goal. v2's spread-spectrum sounding
  sits ~30 dB lower in average PSD for the same detection SNR (see VALIDATION).
- **(L) MCS2 AP TX rate is set to "stabilize CSI", but CSI is measured on the
  client→AP uplink,** whose rate the AP setting does not control.

## 3. ESP-IDF implementation

- **(H) The board profile is never applied.** `active_ap/CMakeLists.txt` does
  `list(APPEND EXTRA_SDKCONFIG_DEFAULTS .../boards/feather_s3/sdkconfig.defaults)`,
  but ESP-IDF's `project.cmake` reads `SDKCONFIG_DEFAULTS`, not
  `EXTRA_SDKCONFIG_DEFAULTS` (0 references in the IDF tree). Moreover no
  `sdkconfig.defaults` file exists under `boards/feather_s3/` at all. So PSRAM,
  Wi-Fi buffer counts, and the USB console settings the README claims are
  "wired in" are silently ignored; the build uses plain target defaults.
- **(M) Unused managed dependency.** `idf_component.yml` pulls
  `espressif/mdns ^1.0.9`; mdns is not used in the active AP/client path (the
  README even lists it as removable). It bloats the build and the dependency
  surface.
- **(M) The stock `_wifi_csi_cb` in `csi_component.h` reads a fixed 128 bytes**
  (`for (i=0;i<128;i++) printf(... my_ptr[i])`) regardless of `data->len`. The
  AP uses its own callback so it is not hit in this build, but the shared
  component ships an out-of-bounds read for short frames.
- **(L) Per-packet `malloc`/`free` in the CSI callback at ~200 pps** churns the
  heap from the Wi-Fi task. A pre-allocated ring would be steadier.
- **(L) Windows-only operational glue.** `start_demo.ps1`, COM-port assumptions,
  and DTR-reset logic are PowerShell/Windows specific.

## What this means for v2

Points 1 and 2 are not individually hard to patch, but together they say the
*method* (passive amplitude variance on uncontrolled Wi-Fi traffic with AGC) is
the ceiling. v2 changes the method: transmit a known, keyed, constant-envelope
sounding waveform and recover the full complex channel impulse response with
processing gain, then detect motion as a gain- and CFO-invariant coherent change.
The old gain/threshold/window problems disappear by construction, and the result
is validated quantitatively in [VALIDATION.md](VALIDATION.md).
