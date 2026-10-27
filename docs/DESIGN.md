# RadioVision v2 - design

## Idea in one paragraph

Two ESP32-S3 boards form a **bistatic spread-spectrum channel sounder**. One board
continuously transmits a known, keyed, constant-envelope waveform; the other
captures raw IQ and recovers the **channel impulse response (CIR)** by correlating
against the known code. A person moving through the link perturbs the channel;
we detect that as a change in a timing- and gain-invariant feature of the CIR.
This replaces the original design's passive Wi-Fi-CSI amplitude variance, and it
directly serves the three goals: a specific/keyed waveform, minimal interference
(processing gain lets us transmit far below the noise floor), and no ESP-NOW/ping
flood.

## Why active sounding beats passive CSI

| | old (passive CSI) | v2 (active sounding) |
|---|---|---|
| excitation | whatever Wi-Fi traffic exists (200 ping/s flood) | a chosen, keyed waveform |
| what you recover | 64 noisy sub-carriers, AGC-scaled | full complex CIR with processing gain |
| gain handling | none (AGC corrupts variance) | gain-invariant feature |
| interference | continuous broadband, high duty | ~30 dB lower PSD for same SNR |
| security | none | keyed code = noise-like to others |
| detect below noise | no | yes (+41 dB processing gain) |

## Waveform

Default: **Zadoff-Chu**, length `N = 509` (prime), root `u` = the key. ZC is
constant-envelope (good for the PA and the 10-bit DAC) and has an ideal periodic
autocorrelation, so one period correlated against the reference gives the CIR
directly (measured peak-to-sidelobe > 160 dB, numerically exact). Alternatives in
`rv_dsp`: maximal-length m-sequences (real BPSK, provably -1 off-peak
autocorrelation) and Gold codes (large keyable family).

- **Keying / "encryption":** the ZC root (or the Gold-code selection, or an
  m-sequence seed/polynomial) is a shared secret. Without it the transmission is
  noise-like and cannot be despread. `rv_dsp` also includes an xorshift128+
  keystream (`rv_payload_crypt`) and BPSK-DSSS data modulation
  (`rv_dsss_modulate`) so the sounding waveform can carry an encrypted data
  payload at `fs/N` bits/s (~78 kb/s at 40 MS/s, N=509) while still sounding the
  channel - one transmission does both.
- **Minimal interference:** the receiver gains `10*log10(N*R)` dB of processing
  gain, so the transmitter can run far below what any narrowband link would need.
  For an equal detection SNR the DSSS average PSD sits ~30 dB below a Wi-Fi ping
  approach (see [VALIDATION](VALIDATION.md)), i.e. close to the noise floor.

## Receive DSP (`rv_dsp`, portable C, runs on host and device)

1. **Acquire** (`rv_sync_offset_multi`): non-coherent correlation over several
   periods locks onto the signal even tens of dB below the noise floor.
2. **CIR per period** (`rv_cir_period`): circular matched filter -> N taps.
3. **CFO estimate/remove**: the phase advance between periods, accumulated over
   the significant taps, is the carrier-frequency offset between the two
   crystals; derotate so the periods combine coherently.
4. **Coherent average** over R periods (processing gain), with DSSS data-sign
   removal referenced to the first period.
5. **Feature** (`rv_cir_feature`): time-gate the CIR to its delay support, take a
   short DFT, and mean-normalize the magnitudes -> `|H[m]|`. This is **invariant
   to the per-burst sampling-clock (timing) offset** (a delay is a pure phase
   slope, so `|H|` is unchanged) and **invariant to gain** (mean-normalized).
6. **Detect** (`rv_detector`): deviation of `|H|` from an adaptive static-scene
   estimate (slow EMA), thresholded at quiet-mean + k*sigma with hysteresis.

The timing-invariant feature is the key robustness choice: two free-running
crystals give an essentially random sub-sample delay every burst, which wrecks a
naive time-domain CIR comparison (AUC ~0.55) but leaves `|H|` intact.

## Scaling to more devices (multistatic CDMA mesh)

The keyed waveform is also what makes the system scale. Give each illuminator a
distinct code and they can all transmit **at once, on the same channel**; a sensor
separates them by correlation (code-division multiplexing), recovering one
independent channel - one **link** - per illuminator from a single capture.

- **Auto code assignment.** Each illuminator derives its Zadoff-Chu root from its
  MAC (`rv_zc_root_for_id`), so a mesh self-assigns near-orthogonal codes with no
  coordination. Plug in a board and it is a new code = a new link.
- **Isolation.** Distinct ZC roots have cross-correlation `1/sqrt(N)`, i.e.
  `10*log10(N)` dB of separation - **measured 27.1 dB at N=509, matching theory.**
  Each extra illuminator adds a `~1/N` interference floor to the others' links.
- **Graceful degradation.** Link detection AUC falls slowly as the channel fills:
  0.97 (1 illuminator) -> 0.75 (8 sharing the channel), in
  [VALIDATION](VALIDATION.md). Longer codes (larger N) buy back isolation.
- **What more devices give you.** Each TX-RX pair is a bistatic link with its own
  geometry; a moving target perturbs the links whose ellipse passes through it, so
  more links mean wider coverage and the basis for **localization** (which links
  fired), not just a yes/no. Sensors fuse links by OR for presence, or keep them
  separate for position.

Roles are independent: any mix of TX-only illuminators and RX sensors works. The
firmware picks the role and (for RX) the list of code roots to track in menuconfig.

## Two ways to run the radio

- **Route B - direct PHY (headline, ESP-only):** each board drives its own radio
  through the undocumented dump engine (`rv_radio`, see
  [UNDOCUMENTED_PHY](UNDOCUMENTED_PHY.md)). TX board loops the code; RX board
  captures + runs `rv_dsp`. No PC, no bridge. `firmware/rv_sounder` (role chosen
  in menuconfig).
- **Route A - brain drives a stock ESP-SDR board over UART:** if you prefer to
  run unmodified ESP-SDR firmware on the radios, a controller ESP speaks the
  documented ESP-SDR serial protocol (`FREQ`, `CAP20`, `REPLAY20`, `RELEASE`,
  2 Mbaud) to set up TX replay and pull RX bursts, then runs the same `rv_dsp`.
  This is the "use an ESP to drive an ESP" option; it needs no new radio code.

Both routes use the identical, validated `rv_dsp` core.

## Hardware topology

Reuses the original 3x FeatherS3 idea:

```
 TX board (illuminator) ......)))  ((...... RX board (sensor + rv_dsp -> motion)
         loops keyed ZC                 captures IQ bursts, detects
                      \   person   /
                       \   here   /
```

A wired clock/GPIO sync between the two boards is optional; with it, the coherent
path reaches AUC ~1.0 (the synchronized operating point in VALIDATION). Without
it, the timing-invariant feature still works.
