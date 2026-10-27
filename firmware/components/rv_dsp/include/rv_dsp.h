/*
 * rv_dsp - RadioVision v2 shared DSP core
 *
 * Portable C99, no ESP-IDF or libc-beyond-math dependencies, so the EXACT same
 * object code that runs on the ESP32-S3 "brain" also compiles on the host for
 * mathematical validation (see validate/). Single-precision float throughout to
 * match the on-device FPU.
 *
 * Pipeline (bistatic active channel sounder):
 *   TX radio continuously replays a known period-N sounding code c[0..N-1].
 *   RX radio captures a burst of R periods of raw IQ.
 *   rv_dsp: sync -> per-period circular correlation -> CIR[r][k]
 *           -> CFO estimate/correct -> coherent CIR average
 *           -> gain-invariant change-detection metric -> calibration/hysteresis.
 *
 * Nothing here allocates; the caller owns all buffers. This keeps it usable from
 * a FreeRTOS task and trivially testable on the host.
 */
#ifndef RV_DSP_H
#define RV_DSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single complex sample. Layout matches ESP-SDR's interleaved I,Q. */
typedef struct { float re, im; } rv_cf;

/* ------------------------------------------------------------------ *
 *  Keyed sounding-code generation
 * ------------------------------------------------------------------ */

/*
 * Maximal-length sequence (m-sequence), length L = 2^m - 1, mapped to bipolar
 * +1.0/-1.0. Galois LFSR. 7 <= m <= 15, seed != 0. The (polynomial implied by
 * m, seed) pair is the key for the real-valued variant.
 * Returns L on success, 0 on bad arguments.
 */
int rv_mseq(int m, uint32_t seed, float *out, int out_cap);

/*
 * Gold code, length L = 2^m - 1, bipolar. XOR of two m-sequences of the same
 * degree with a relative cyclic shift. 'shift' in [0, L-1] selects one of the
 * ~L codes in the family (the key). Returns L or 0.
 */
int rv_gold(int m, uint32_t seed_a, uint32_t seed_b, int shift,
            float *out, int out_cap);

/*
 * Zadoff-Chu sequence, length N, root u with gcd(u, N) = 1. Constant envelope,
 * ideal periodic autocorrelation. Root u is the key for the complex variant.
 * Default RadioVision sounding waveform. Returns N or 0.
 * phase_q controls the quantization used for the discrete argument so host and
 * device agree exactly; pass 0 for the default (continuous).
 */
int rv_zc(int N, int u, rv_cf *out, int out_cap);

/* Deterministic 32-bit integer mix (for deriving a code from a device ID/MAC). */
uint32_t rv_hash32(uint32_t x);

/*
 * Auto-assign a Zadoff-Chu root in [1, N-1] with gcd(root, N) = 1 from a device
 * id (e.g. the low 32 bits of the MAC). This lets a mesh self-assign codes
 * without coordination: every illuminator derives its own root, every sensor
 * separates the simultaneous illuminators by correlation (CDMA). Distinct ZC
 * roots have cross-correlation ~1/sqrt(N), giving ~10*log10(N) dB of isolation.
 */
int rv_zc_root_for_id(uint32_t id, int N);

/* ------------------------------------------------------------------ *
 *  Encrypted DSSS data layer (optional payload riding on the sounder)
 * ------------------------------------------------------------------ */

/* Keystream generator state (xorshift128+ seeded from a 128-bit key). */
typedef struct { uint64_t s0, s1; } rv_keystream;

void rv_keystream_init(rv_keystream *ks, uint64_t key_lo, uint64_t key_hi);
uint64_t rv_keystream_next(rv_keystream *ks);

/*
 * Whiten/encrypt a payload byte buffer in place with the keystream (XOR).
 * Symmetric: call again with the same key to recover.
 */
void rv_payload_crypt(uint8_t *buf, int len, uint64_t key_lo, uint64_t key_hi);

/*
 * Map one data symbol (bit) onto a sounding period by sign (BPSK-DSSS).
 * data_bit in {0,1} -> multiplies the whole period by +1 / -1. This spreads one
 * bit across N chips, giving processing gain 10*log10(N) dB and a noise-like,
 * key-dependent transmission.
 */
void rv_dsss_modulate(const rv_cf *code, int N, int data_bit, rv_cf *period_out);

/* ------------------------------------------------------------------ *
 *  Core sounding DSP
 * ------------------------------------------------------------------ */

/*
 * Circular cross-correlation of one received period (length N) against the
 * reference code (length N), producing an N-tap channel impulse response.
 *   cir[k] = (1/N) * sum_n rx[(n+k) mod N] * conj(ref[n])
 * For an ideal-autocorrelation code this is the channel's tap response.
 */
void rv_cir_period(const rv_cf *rx, const rv_cf *ref, int N, rv_cf *cir);

/* Index of the strongest |cir| tap (the dominant/LOS path). */
int rv_peak_index(const rv_cf *cir, int N);

/*
 * Coarse burst synchronization: slide the reference over the first
 * (search_span) samples of rx and return the offset with maximum correlation
 * magnitude. rx must hold at least N + search_span samples.
 */
int rv_sync_offset(const rv_cf *rx, int rx_len, const rv_cf *ref, int N,
                   int search_span);

/*
 * Robust acquisition: like rv_sync_offset but integrates the correlation
 * magnitude NON-coherently across up to n_periods periods, which lowers the
 * acquisition threshold by ~10*log10(n_periods) dB so the sounder locks even
 * when the signal is well below the noise floor. rx must hold at least
 * search_span + n_periods*N samples.
 */
int rv_sync_offset_multi(const rv_cf *rx, int rx_len, const rv_cf *ref, int N,
                         int search_span, int n_periods);

/*
 * Process one capture burst into a single CFO-corrected, coherently averaged
 * CIR plus diagnostics. Scratch must hold at least R*N rv_cf.
 *
 *   rx       : captured IQ, at least sync_span + R*N samples
 *   rx_len   : length of rx in complex samples
 *   ref      : reference code, N samples (the period the TX is replaying)
 *   N, R     : period length and number of periods to integrate
 *   cir_out  : N taps, the averaged complex CIR for this burst
 *   scratch  : R*N rv_cf workspace
 *   out      : filled diagnostics (may be NULL)
 * Returns true on success (sync found, enough samples).
 */
typedef struct {
    int    sync_offset;    /* sample index of first integrated period        */
    float  cfo_rad_per_sym;/* estimated carrier phase advance per period      */
    float  peak_snr_db;    /* dominant-tap power over off-peak noise          */
    int    peak_tap;       /* dominant-tap index                              */
    int    data_bits;      /* recovered DSSS bits (LSB first), if used        */
    int    n_data_bits;    /* count of recovered bits                         */
} rv_burst_diag;

bool rv_process_burst(const rv_cf *rx, int rx_len, const rv_cf *ref,
                      int N, int R, rv_cf *cir_out, rv_cf *scratch,
                      rv_burst_diag *out);

/* ------------------------------------------------------------------ *
 *  Motion detector (gain-invariant coherent change detection)
 * ------------------------------------------------------------------ */

#define RV_MAX_TAPS 1024
#define RV_MAX_FEAT 128     /* max frequency-response bins in the feature     */

/*
 * Timing-invariant, gain-invariant detection feature.
 *
 * Two free-running crystals give a per-burst sub-sample timing offset that is
 * effectively random in [0,1). In the time domain that reshuffles the CIR taps
 * and swamps a weak moving target. The magnitude of the channel FREQUENCY
 * response, |H[m]|, is invariant to any common delay (a delay is a pure phase
 * slope), so we detect on it instead. We first time-gate the CIR to the first
 * `n_taps_used` taps (the channel's delay support) to reject the noise-only
 * taps, take a short DFT to `n_bins` frequency points, and mean-normalize the
 * magnitudes (gain-invariant). Writes feat_out[0..n_bins-1].
 */
void rv_cir_feature(const rv_cf *cir, int N, int n_taps_used, int n_bins,
                    float *feat_out);

typedef enum {
    RV_CAL_QUIET = 0,   /* collecting quiet baseline                         */
    RV_CAL_READY        /* detection active                                  */
} rv_cal_phase;

typedef struct {
    int          n_feat;          /* feature length (<= RV_MAX_FEAT)         */
    float        ema_alpha;       /* static-scene tracking rate (slow)       */
    float        ema[RV_MAX_FEAT];/* adaptive static feature estimate        */
    bool         ema_init;

    /* calibration */
    rv_cal_phase phase;
    int          cal_count;
    int          cal_target;
    double       cal_sum;         /* running sum of quiet metric             */
    double       cal_sumsq;       /* running sum of quiet metric^2           */
    float        threshold;       /* decision threshold on the metric        */
    float        k_sigma;         /* threshold = mean + k_sigma * std        */

    /* hysteresis */
    int          hyst_on, hyst_off;
    int          on_count, off_count;
    bool         motion;

    /* last outputs (for reporting) */
    float        last_metric;
    float        last_static_energy;
} rv_detector;

/* Configure a detector. n_feat = feature length (frequency bins).
 * ema_alpha ~ 0.02, k_sigma ~ 6, cal_target ~ 200. */
void rv_detector_init(rv_detector *d, int n_feat, float ema_alpha,
                      float k_sigma, int cal_target,
                      int hyst_on, int hyst_off);

/*
 * Feed one burst feature (from rv_cir_feature). Returns the motion metric and
 * updates calibration / motion state. The metric is
 *   metric = sum_m (feat_m - ema_m)^2  /  (sum_m ema_m^2 + eps)
 * i.e. deviation of the frequency-response-magnitude profile from the adaptive
 * static estimate. Timing- and gain-invariant by construction.
 */
float rv_detector_update(rv_detector *d, const float *feat);

/* Restart calibration (e.g. after a frequency or scene change). */
void rv_detector_recalibrate(rv_detector *d);

/* ------------------------------------------------------------------ *
 *  Small FFT (radix-2) used for optional delay-Doppler diagnostics
 * ------------------------------------------------------------------ */

/* In-place radix-2 FFT, len must be a power of two, inverse if inv != 0. */
void rv_fft(rv_cf *x, int len, int inv);

/* ------------------------------------------------------------------ *
 *  Cross-platform self test
 * ------------------------------------------------------------------ */

/*
 * Deterministic end-to-end exercise of the core on a fixed synthetic channel.
 * Returns a scalar signature; host and device must agree to within 1e-3. Used
 * to prove the on-device build is numerically identical to the validated host
 * build. Writes a few component values into sig[0..3] if sig != NULL.
 */
float rv_selftest(float *sig, int sig_len);

#ifdef __cplusplus
}
#endif

#endif /* RV_DSP_H */
