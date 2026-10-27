/*
 * rv_dsp - RadioVision v2 shared DSP core. See include/rv_dsp.h.
 *
 * Portable C99 (math.h only). Compiled for BOTH the host validator and the
 * ESP32-S3 firmware from the same source, so the on-device numbers are the
 * numbers we validate.
 */
#include "rv_dsp.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================== *
 *  Keyed sounding-code generation
 * ================================================================== */

/* Galois LFSR feedback masks giving maximal-length (2^m-1) sequences.
 * Verified by period check in validate/ (rv_validate.py::test_mseq_period). */
static uint32_t mseq_taps(int m)
{
    switch (m) {
        case 7:  return 0x60u;
        case 8:  return 0xB8u;
        case 9:  return 0x110u;
        case 10: return 0x240u;
        case 11: return 0x500u;
        case 12: return 0xE08u;
        case 13: return 0x1C80u;
        case 14: return 0x3802u;
        case 15: return 0x6000u;
        default: return 0u;
    }
}

int rv_mseq(int m, uint32_t seed, float *out, int out_cap)
{
    if (m < 7 || m > 15 || out == NULL) return 0;
    uint32_t taps = mseq_taps(m);
    uint32_t mask = (m >= 32) ? 0xffffffffu : ((1u << m) - 1u);
    uint32_t reg = seed & mask;
    if (reg == 0u) reg = 1u;
    int L = (1 << m) - 1;
    if (out_cap < L) return 0;
    for (int i = 0; i < L; i++) {
        out[i] = (reg & 1u) ? 1.0f : -1.0f;
        uint32_t lsb = reg & 1u;
        reg >>= 1;
        if (lsb) reg ^= taps;
    }
    return L;
}

int rv_gold(int m, uint32_t seed_a, uint32_t seed_b, int shift,
            float *out, int out_cap)
{
    if (m < 7 || m > 15 || out == NULL) return 0;
    uint32_t taps = mseq_taps(m);
    uint32_t mask = (1u << m) - 1u;
    int L = (1 << m) - 1;
    if (out_cap < L) return 0;

    /* First m-sequence stored as 0.0/1.0 bits in out[]. */
    uint32_t ra = seed_a & mask;
    if (ra == 0u) ra = 1u;
    for (int i = 0; i < L; i++) {
        out[i] = (float)(ra & 1u);
        uint32_t lsb = ra & 1u;
        ra >>= 1;
        if (lsb) ra ^= taps;
    }

    /* Second m-sequence advanced by `shift`, XORed in, mapped to bipolar. */
    uint32_t rb = seed_b & mask;
    if (rb == 0u) rb = 1u;
    int s = shift % L; if (s < 0) s += L;
    for (int i = 0; i < s; i++) {
        uint32_t lsb = rb & 1u; rb >>= 1; if (lsb) rb ^= taps;
    }
    for (int i = 0; i < L; i++) {
        int bbit = (int)(rb & 1u);
        uint32_t lsb = rb & 1u; rb >>= 1; if (lsb) rb ^= taps;
        int a = (out[i] > 0.5f) ? 1 : 0;
        out[i] = (a ^ bbit) ? 1.0f : -1.0f;
    }
    return L;
}

static long rv_gcd(long a, long b) { while (b) { long t = a % b; a = b; b = t; } return a < 0 ? -a : a; }

int rv_zc(int N, int u, rv_cf *out, int out_cap)
{
    if (N < 1 || out == NULL || out_cap < N) return 0;
    if (rv_gcd(u, N) != 1) return 0;
    for (int k = 0; k < N; k++) {
        double q = (N & 1) ? ((double)k * (k + 1)) : ((double)k * k);
        double arg = -M_PI * (double)u * q / (double)N;
        /* reduce for stable trig across libm implementations */
        double red = arg - 2.0 * M_PI * floor(arg / (2.0 * M_PI) + 0.5);
        out[k].re = (float)cos(red);
        out[k].im = (float)sin(red);
    }
    return N;
}

uint32_t rv_hash32(uint32_t x)
{
    /* integer finalizer (splitmix/murmur-style) */
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

int rv_zc_root_for_id(uint32_t id, int N)
{
    if (N < 2) return 1;
    uint32_t h = rv_hash32(id);
    int u = 1 + (int)(h % (uint32_t)(N - 1));   /* in [1, N-1] */
    /* ensure gcd(u, N) == 1 (automatic for prime N) */
    for (int tries = 0; tries < N && rv_gcd(u, N) != 1; tries++) {
        u = 1 + (u % (N - 1));
    }
    return u;
}

/* ================================================================== *
 *  Encrypted DSSS data layer
 * ================================================================== */

void rv_keystream_init(rv_keystream *ks, uint64_t key_lo, uint64_t key_hi)
{
    /* Avoid all-zero state. */
    ks->s0 = key_lo ? key_lo : 0x9E3779B97F4A7C15ull;
    ks->s1 = key_hi ? key_hi : 0xBF58476D1CE4E5B9ull;
}

uint64_t rv_keystream_next(rv_keystream *ks)
{
    /* xorshift128+ */
    uint64_t x = ks->s0;
    uint64_t const y = ks->s1;
    ks->s0 = y;
    x ^= x << 23;
    x ^= x >> 17;
    x ^= y ^ (y >> 26);
    ks->s1 = x;
    return x + y;
}

void rv_payload_crypt(uint8_t *buf, int len, uint64_t key_lo, uint64_t key_hi)
{
    rv_keystream ks;
    rv_keystream_init(&ks, key_lo, key_hi);
    int i = 0;
    while (i < len) {
        uint64_t k = rv_keystream_next(&ks);
        for (int b = 0; b < 8 && i < len; b++, i++) {
            buf[i] ^= (uint8_t)(k >> (8 * b));
        }
    }
}

void rv_dsss_modulate(const rv_cf *code, int N, int data_bit, rv_cf *period_out)
{
    float s = data_bit ? -1.0f : 1.0f;
    for (int k = 0; k < N; k++) {
        period_out[k].re = code[k].re * s;
        period_out[k].im = code[k].im * s;
    }
}

/* ================================================================== *
 *  Core sounding DSP
 * ================================================================== */

void rv_cir_period(const rv_cf *rx, const rv_cf *ref, int N, rv_cf *cir)
{
    float inv = 1.0f / (float)N;
    for (int k = 0; k < N; k++) {
        float sre = 0.0f, sim = 0.0f;
        for (int n = 0; n < N; n++) {
            int idx = n + k;
            if (idx >= N) idx -= N;
            float xr = rx[idx].re, xi = rx[idx].im;
            float rr = ref[n].re, ri = ref[n].im;
            /* rx * conj(ref) */
            sre += xr * rr + xi * ri;
            sim += xi * rr - xr * ri;
        }
        cir[k].re = sre * inv;
        cir[k].im = sim * inv;
    }
}

int rv_peak_index(const rv_cf *cir, int N)
{
    int best = 0;
    float bestp = -1.0f;
    for (int k = 0; k < N; k++) {
        float p = cir[k].re * cir[k].re + cir[k].im * cir[k].im;
        if (p > bestp) { bestp = p; best = k; }
    }
    return best;
}

int rv_sync_offset(const rv_cf *rx, int rx_len, const rv_cf *ref, int N,
                   int search_span)
{
    if (rx_len < N) return 0;
    if (search_span > rx_len - N) search_span = rx_len - N;
    if (search_span < 0) search_span = 0;
    int best = 0;
    float bestm = -1.0f;
    for (int off = 0; off <= search_span; off++) {
        float sre = 0.0f, sim = 0.0f;
        for (int n = 0; n < N; n++) {
            float xr = rx[off + n].re, xi = rx[off + n].im;
            float rr = ref[n].re, ri = ref[n].im;
            sre += xr * rr + xi * ri;
            sim += xi * rr - xr * ri;
        }
        float mag = sre * sre + sim * sim;
        if (mag > bestm) { bestm = mag; best = off; }
    }
    return best;
}

int rv_sync_offset_multi(const rv_cf *rx, int rx_len, const rv_cf *ref, int N,
                         int search_span, int n_periods)
{
    if (n_periods < 1) n_periods = 1;
    if (rx_len < n_periods * N) return rv_sync_offset(rx, rx_len, ref, N, search_span);
    if (search_span > rx_len - n_periods * N) search_span = rx_len - n_periods * N;
    if (search_span < 0) search_span = 0;
    int best = 0;
    float bestm = -1.0f;
    for (int off = 0; off <= search_span; off++) {
        float score = 0.0f;
        for (int j = 0; j < n_periods; j++) {
            const rv_cf *w = &rx[off + j * N];
            float sre = 0.0f, sim = 0.0f;
            for (int n = 0; n < N; n++) {
                float xr = w[n].re, xi = w[n].im;
                float rr = ref[n].re, ri = ref[n].im;
                sre += xr * rr + xi * ri;
                sim += xi * rr - xr * ri;
            }
            score += sre * sre + sim * sim;   /* non-coherent sum of |corr|^2 */
        }
        if (score > bestm) { bestm = score; best = off; }
    }
    return best;
}

bool rv_process_burst(const rv_cf *rx, int rx_len, const rv_cf *ref,
                      int N, int R, rv_cf *cir_out, rv_cf *scratch,
                      rv_burst_diag *out)
{
    if (rx == NULL || ref == NULL || cir_out == NULL || scratch == NULL) return false;
    if (N < 4 || R < 1) return false;
    if (rx_len < 2 * N) return false;

    /* The sounding signal is periodic with period N, so an aligned window exists
     * within any single period. Keep one period of slack for the sync search and
     * extract R clean periods after it. */
    while (R > 1 && ((R + 1) * N > rx_len)) R--;
    int span = rx_len - R * N;
    if (span > N) span = N;
    if (span < 0) span = 0;

    int nsync = R < 4 ? R : 4;                      /* non-coherent acq. depth */
    int p0 = rv_sync_offset_multi(rx, rx_len, ref, N, span, nsync);
    if (p0 + R * N > rx_len) p0 = rx_len - R * N;   /* safety, should not trigger */

    for (int r = 0; r < R; r++)
        rv_cir_period(&rx[p0 + r * N], ref, N, &scratch[r * N]);

    int peak = rv_peak_index(&scratch[0], N);

    /* CFO: phase increment between consecutive periods, accumulated over the
     * significant channel taps only (power > 5% of peak). Using every tap would
     * let the many noise-only taps dominate on a single-path channel; using just
     * the peak wastes a rich channel's energy. The threshold adapts to both.
     * Valid for pilot (sensing) bursts; DSSS data mode uses pilot periods. */
    float peakpow = scratch[peak].re * scratch[peak].re + scratch[peak].im * scratch[peak].im;
    float cfo_thr = 0.05f * peakpow;
    double sre = 0.0, sim = 0.0;
    for (int k = 0; k < N; k++) {
        float p0 = scratch[k].re * scratch[k].re + scratch[k].im * scratch[k].im;
        if (p0 < cfo_thr) continue;
        for (int r = 0; r + 1 < R; r++) {
            rv_cf a = scratch[r * N + k];
            rv_cf b = scratch[(r + 1) * N + k];
            sre += (double)b.re * a.re + (double)b.im * a.im;   /* b * conj(a) */
            sim += (double)b.im * a.re - (double)b.re * a.im;
        }
    }
    float cfo = (R > 1) ? (float)atan2(sim, sre) : 0.0f;

    /* Coherent average with CFO derotation and DSSS data-sign removal,
     * referenced to period 0's peak phasor. */
    rv_cf pk0 = scratch[peak];
    for (int k = 0; k < N; k++) { cir_out[k].re = 0.0f; cir_out[k].im = 0.0f; }
    int bits = 0, nb = 0;
    for (int r = 0; r < R; r++) {
        float ph = -cfo * (float)r;
        float cr = cosf(ph), ci = sinf(ph);
        rv_cf pk = scratch[r * N + peak];
        float pr = pk.re * cr - pk.im * ci;      /* peak derotated */
        float pi = pk.re * ci + pk.im * cr;
        float dot = pr * pk0.re + pi * pk0.im;   /* vs period-0 peak */
        int bit = (dot < 0.0f) ? 1 : 0;
        float sgn = bit ? -1.0f : 1.0f;
        if (nb < 32) bits |= (bit << nb);
        nb++;
        for (int k = 0; k < N; k++) {
            rv_cf v = scratch[r * N + k];
            float rr = (v.re * cr - v.im * ci) * sgn;
            float ii = (v.re * ci + v.im * cr) * sgn;
            cir_out[k].re += rr;
            cir_out[k].im += ii;
        }
    }
    float invR = 1.0f / (float)R;
    for (int k = 0; k < N; k++) { cir_out[k].re *= invR; cir_out[k].im *= invR; }

    if (out) {
        float peakp = cir_out[peak].re * cir_out[peak].re + cir_out[peak].im * cir_out[peak].im;
        double noise = 0.0; int cnt = 0; int guard = 2;
        for (int k = 0; k < N; k++) {
            int dd = k - peak; if (dd < 0) dd = -dd;
            if (dd <= guard || dd >= N - guard) continue;
            noise += (double)cir_out[k].re * cir_out[k].re + (double)cir_out[k].im * cir_out[k].im;
            cnt++;
        }
        float npow = (cnt > 0) ? (float)(noise / cnt) : 1e-9f;
        out->sync_offset = p0;
        out->cfo_rad_per_sym = cfo;
        out->peak_tap = peak;
        out->peak_snr_db = 10.0f * log10f((peakp + 1e-20f) / (npow + 1e-20f));
        out->data_bits = bits;
        out->n_data_bits = nb;
    }

    /* Gain- and phase-invariant normalization: divide the whole CIR by the
     * dominant (line-of-sight) tap. The direct TX->RX coupling is constant, so
     * referencing to it removes the per-burst AGC scale AND any residual common
     * phase (CFO/LO drift) in one step. After this cir[peak] == 1+0j and motion
     * shows up only as genuine change in the other taps. */
    {
        rv_cf ref = cir_out[peak];
        float rp = ref.re * ref.re + ref.im * ref.im;
        if (rp > 1e-20f) {
            float inv = 1.0f / rp;
            for (int k = 0; k < N; k++) {
                float ar = cir_out[k].re, ai = cir_out[k].im;
                /* (a / ref) = a * conj(ref) / |ref|^2 */
                cir_out[k].re = (ar * ref.re + ai * ref.im) * inv;
                cir_out[k].im = (ai * ref.re - ar * ref.im) * inv;
            }
        }
    }
    return true;
}

/* ================================================================== *
 *  Timing- & gain-invariant feature: |H[m]| of the time-gated CIR
 * ================================================================== */

void rv_cir_feature(const rv_cf *cir, int N, int n_taps_used, int n_bins,
                    float *feat_out)
{
    if (n_taps_used > N) n_taps_used = N;
    if (n_bins > RV_MAX_FEAT) n_bins = RV_MAX_FEAT;

    double sum = 0.0;
    for (int m = 0; m < n_bins; m++) {
        double hr = 0.0, hi = 0.0;
        for (int k = 0; k < n_taps_used; k++) {
            double ang = -2.0 * M_PI * (double)m * (double)k / (double)N;
            double c = cos(ang), s = sin(ang);
            hr += (double)cir[k].re * c - (double)cir[k].im * s;
            hi += (double)cir[k].re * s + (double)cir[k].im * c;
        }
        float mag = (float)sqrt(hr * hr + hi * hi);
        feat_out[m] = mag;
        sum += mag;
    }
    /* mean-normalize -> invariant to a common amplitude scale (AGC/distance) */
    float inv = (sum > 1e-20) ? (float)(n_bins / sum) : 1.0f;
    for (int m = 0; m < n_bins; m++) feat_out[m] *= inv;
}

/* ================================================================== *
 *  Motion detector
 * ================================================================== */

void rv_detector_init(rv_detector *d, int n_feat, float ema_alpha,
                      float k_sigma, int cal_target,
                      int hyst_on, int hyst_off)
{
    memset(d, 0, sizeof(*d));
    if (n_feat > RV_MAX_FEAT) n_feat = RV_MAX_FEAT;
    d->n_feat = n_feat;
    d->ema_alpha = ema_alpha;
    d->k_sigma = k_sigma;
    d->cal_target = cal_target;
    d->hyst_on = hyst_on;
    d->hyst_off = hyst_off;
    d->phase = RV_CAL_QUIET;
    d->ema_init = false;
}

void rv_detector_recalibrate(rv_detector *d)
{
    d->phase = RV_CAL_QUIET;
    d->cal_count = 0;
    d->cal_sum = 0.0;
    d->cal_sumsq = 0.0;
    d->on_count = 0;
    d->off_count = 0;
    d->motion = false;
    /* keep ema so the static scene estimate is not thrown away */
}

float rv_detector_update(rv_detector *d, const float *feat)
{
    int M = d->n_feat;
    if (!d->ema_init) {
        for (int m = 0; m < M; m++) d->ema[m] = feat[m];
        d->ema_init = true;
    }

    double dev = 0.0, en = 0.0;
    for (int m = 0; m < M; m++) {
        float e = feat[m] - d->ema[m];
        dev += (double)e * e;
        en  += (double)d->ema[m] * d->ema[m];
    }
    float metric = (float)(dev / (en + 1e-12));

    /* Track the static scene. Adapt slowly; slow further during motion so a
     * real target is not absorbed, but never fully freeze so furniture moves
     * eventually re-baseline. */
    float a = d->ema_alpha;
    if (d->motion) a *= 0.1f;
    for (int m = 0; m < M; m++) {
        d->ema[m] += a * (feat[m] - d->ema[m]);
    }

    d->last_metric = metric;
    d->last_static_energy = (float)en;

    if (d->phase == RV_CAL_QUIET) {
        d->cal_sum += metric;
        d->cal_sumsq += (double)metric * metric;
        d->cal_count++;
        if (d->cal_count >= d->cal_target) {
            double mean = d->cal_sum / d->cal_count;
            double var = d->cal_sumsq / d->cal_count - mean * mean;
            if (var < 0.0) var = 0.0;
            d->threshold = (float)(mean + (double)d->k_sigma * sqrt(var));
            d->phase = RV_CAL_READY;
        }
        return metric;
    }

    bool inst = metric > d->threshold;
    if (inst) { d->on_count++; d->off_count = 0; }
    else      { d->off_count++; d->on_count = 0; }
    if (!d->motion && d->on_count >= d->hyst_on) d->motion = true;
    else if (d->motion && d->off_count >= d->hyst_off) d->motion = false;
    return metric;
}

/* ================================================================== *
 *  Radix-2 FFT (delay-Doppler diagnostics)
 * ================================================================== */

void rv_fft(rv_cf *x, int len, int inv)
{
    if (len < 2) return;
    /* bit reversal */
    for (int i = 1, j = 0; i < len; i++) {
        int bit = len >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { rv_cf t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int seglen = 2; seglen <= len; seglen <<= 1) {
        double ang = (inv ? 2.0 : -2.0) * M_PI / seglen;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int s = 0; s < len; s += seglen) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < seglen / 2; k++) {
                rv_cf u = x[s + k];
                rv_cf v = x[s + k + seglen / 2];
                float vr = v.re * cr - v.im * ci;
                float vi = v.re * ci + v.im * cr;
                x[s + k].re = u.re + vr;
                x[s + k].im = u.im + vi;
                x[s + k + seglen / 2].re = u.re - vr;
                x[s + k + seglen / 2].im = u.im - vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
    if (inv) {
        float s = 1.0f / (float)len;
        for (int i = 0; i < len; i++) { x[i].re *= s; x[i].im *= s; }
    }
}

/* ================================================================== *
 *  Cross-platform self test
 * ================================================================== */

#define ST_N 63
#define ST_R 4
#define ST_RP (ST_R + 1)        /* periods actually synthesized (1 slack period) */
#define ST_OFF 5
static rv_cf st_code[ST_N];
static rv_cf st_rx[ST_OFF + ST_N * ST_RP + 4];
static rv_cf st_scratch[ST_N * ST_R];
static rv_cf st_cir[ST_N];

/* deterministic ~N(0,1) from keystream (sum of 4 uniforms, centered) */
static float st_noise(rv_keystream *ks)
{
    uint64_t v = rv_keystream_next(ks);
    double s = 0.0;
    for (int i = 0; i < 4; i++) {
        s += (double)((v >> (i * 16)) & 0xffff) / 65535.0;
    }
    return (float)((s - 2.0) * 0.5);
}

/* Build one burst for a given 3-tap channel and CFO, write into st_rx. */
static void st_make_burst(const rv_cf ch[3], const int tap[3], float cfo,
                          float noise_amp, rv_keystream *ks)
{
    for (int i = 0; i < ST_OFF; i++) { st_rx[i].re = st_noise(ks) * noise_amp; st_rx[i].im = st_noise(ks) * noise_amp; }
    for (int r = 0; r < ST_RP; r++) {
        float ph = cfo * (float)r;
        float cr = cosf(ph), ci = sinf(ph);
        for (int n = 0; n < ST_N; n++) {
            /* circular convolution of code with channel */
            float yr = 0.0f, yi = 0.0f;
            for (int t = 0; t < 3; t++) {
                int idx = n - tap[t]; while (idx < 0) idx += ST_N;
                float xr = st_code[idx].re, xi = st_code[idx].im;
                yr += xr * ch[t].re - xi * ch[t].im;
                yi += xr * ch[t].im + xi * ch[t].re;
            }
            /* apply per-period CFO phase and add noise */
            float rr = yr * cr - yi * ci + st_noise(ks) * noise_amp;
            float ii = yr * ci + yi * cr + st_noise(ks) * noise_amp;
            int o = ST_OFF + r * ST_N + n;
            st_rx[o].re = rr; st_rx[o].im = ii;
        }
    }
}

float rv_selftest(float *sig, int sig_len)
{
    rv_keystream ks;
    rv_keystream_init(&ks, 0xA5A5A5A5A5A5A5A5ull, 0x0123456789ABCDEFull);

    rv_zc(ST_N, 5, st_code, ST_N);

    const int tap[3] = { 2, 7, 13 };
    rv_cf ch_quiet[3] = { {1.0f, 0.0f}, {0.4f, -0.2f}, {0.2f, 0.1f} };
    float cfo = 0.15f;             /* radians/period */
    float namp = 0.01f;

    /* one burst -> CIR + diagnostics */
    rv_burst_diag diag;
    st_make_burst(ch_quiet, tap, cfo, namp, &ks);
    rv_process_burst(st_rx, ST_OFF + ST_N * ST_RP, st_code, ST_N, ST_R,
                     st_cir, st_scratch, &diag);

    /* CFO estimate error and recovered dominant-path magnitude (sync aligns the
     * strongest tap to index peak_tap; here that is the unit LOS tap). */
    float cfo_err = fabsf(diag.cfo_rad_per_sym - cfo);
    int pk = diag.peak_tap;
    float los = sqrtf(st_cir[pk].re * st_cir[pk].re + st_cir[pk].im * st_cir[pk].im);

    /* detector on the timing/gain-invariant feature: calibrate on quiet bursts,
     * then perturb a reflector (motion). static: rv_detector is large; never
     * place it on a small embedded task stack. */
    #define ST_TAPS 16
    #define ST_BINS 32
    static rv_detector det;
    static float st_feat[ST_BINS];
    rv_detector_init(&det, ST_BINS, 0.05f, 6.0f, 8, 2, 3);
    float quiet_metric = 0.0f;
    for (int b = 0; b < 12; b++) {
        st_make_burst(ch_quiet, tap, cfo, namp, &ks);
        rv_process_burst(st_rx, ST_OFF + ST_N * ST_RP, st_code, ST_N, ST_R,
                         st_cir, st_scratch, &diag);
        rv_cir_feature(st_cir, ST_N, ST_TAPS, ST_BINS, st_feat);
        quiet_metric = rv_detector_update(&det, st_feat);
    }
    rv_cf ch_motion[3] = { {1.0f, 0.0f}, {0.4f, -0.2f}, {0.6f, 0.35f} };
    st_make_burst(ch_motion, tap, cfo, namp, &ks);
    rv_process_burst(st_rx, ST_OFF + ST_N * ST_RP, st_code, ST_N, ST_R,
                     st_cir, st_scratch, &diag);
    rv_cir_feature(st_cir, ST_N, ST_TAPS, ST_BINS, st_feat);
    float motion_metric = rv_detector_update(&det, st_feat);

    if (sig && sig_len > 0) sig[0] = los;
    if (sig && sig_len > 1) sig[1] = cfo_err;
    if (sig && sig_len > 2) sig[2] = quiet_metric;
    if (sig && sig_len > 3) sig[3] = motion_metric;

    /* single scalar signature (host vs device must match within 1e-3) */
    return los + 10.0f * cfo_err + quiet_metric + 0.001f * motion_metric;
}
