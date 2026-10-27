/*
 * rv_shim - host-only flat C API over rv_dsp for the Python/ctypes validator.
 * Keeps ctypes free of struct-layout concerns: complex arrays are passed as
 * interleaved float32 (re, im, re, im, ...), matching numpy complex64 memory.
 * This file is NOT part of the firmware.
 */
#include "rv_dsp.h"
#include <stdlib.h>
#include <string.h>

/* ---- waveform ---- */
int rvx_zc(int N, int u, float *out /* 2N floats */)
{
    return rv_zc(N, u, (rv_cf *)out, N);
}
int rvx_mseq(int m, unsigned seed, float *out /* L floats, real */)
{
    return rv_mseq(m, seed, out, (1 << m) - 1);
}
int rvx_gold(int m, unsigned sa, unsigned sb, int shift, float *out)
{
    return rv_gold(m, sa, sb, shift, out, (1 << m) - 1);
}
int rvx_zc_root_for_id(unsigned id, int N) { return rv_zc_root_for_id(id, N); }

/* ---- burst processing ----
 * diag_out (7 floats): sync_offset, cfo_rad_per_sym, peak_snr_db, peak_tap,
 *                      n_data_bits, data_bits(lo 24), ok
 */
void rvx_process(const float *rx, int rx_len, const float *ref, int N, int R,
                 float *cir_out /* 2N floats */, float *diag_out)
{
    rv_cf *scratch = (rv_cf *)malloc((size_t)R * N * sizeof(rv_cf));
    rv_burst_diag d;
    memset(&d, 0, sizeof(d));
    int ok = scratch ? rv_process_burst((const rv_cf *)rx, rx_len,
                                        (const rv_cf *)ref, N, R,
                                        (rv_cf *)cir_out, scratch, &d) : 0;
    free(scratch);
    if (diag_out) {
        diag_out[0] = (float)d.sync_offset;
        diag_out[1] = d.cfo_rad_per_sym;
        diag_out[2] = d.peak_snr_db;
        diag_out[3] = (float)d.peak_tap;
        diag_out[4] = (float)d.n_data_bits;
        diag_out[5] = (float)d.data_bits;
        diag_out[6] = (float)ok;
    }
}

/* ---- timing/gain-invariant feature ---- */
void rvx_feature(const float *cir /* 2N */, int N, int n_taps_used, int n_bins,
                 float *feat_out /* n_bins */)
{
    rv_cir_feature((const rv_cf *)cir, N, n_taps_used, n_bins, feat_out);
}

/* ---- detector handle ---- */
void *rvx_det_new(int n_feat, float ema_alpha, float k_sigma, int cal_target,
                  int hyst_on, int hyst_off)
{
    rv_detector *d = (rv_detector *)malloc(sizeof(rv_detector));
    if (d) rv_detector_init(d, n_feat, ema_alpha, k_sigma, cal_target, hyst_on, hyst_off);
    return d;
}
float rvx_det_update(void *h, const float *feat)
{
    return rv_detector_update((rv_detector *)h, feat);
}
void rvx_det_recal(void *h) { rv_detector_recalibrate((rv_detector *)h); }
int   rvx_det_motion(void *h)   { return ((rv_detector *)h)->motion ? 1 : 0; }
int   rvx_det_phase(void *h)    { return (int)((rv_detector *)h)->phase; }
float rvx_det_metric(void *h)   { return ((rv_detector *)h)->last_metric; }
float rvx_det_threshold(void *h){ return ((rv_detector *)h)->threshold; }
void  rvx_det_free(void *h)     { free(h); }

/* ---- selftest passthrough (for host/device identity) ---- */
float rvx_selftest(float *sig4) { return rv_selftest(sig4, 4); }
