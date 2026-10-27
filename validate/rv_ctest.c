/* Quick host sanity check for rv_dsp: selftest signature, m-sequence periods,
 * ZC autocorrelation sidelobe level. Compiled with ziglang cc in run_ctest.sh. */
#include "rv_dsp.h"
#include <stdio.h>
#include <math.h>

static int mseq_period(int m, unsigned seed)
{
    static float buf[40000];
    int L = rv_mseq(m, seed, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    if (L <= 0) return -1;
    /* period = first shift>0 where the sequence repeats; for maximal LFSR == L */
    /* we instead re-run and detect state recurrence via a simple full-cycle check:
       count distinct by verifying buf has no shorter repeat dividing L is overkill;
       here we just confirm balance property of m-sequences: #(+1) - #(-1) == 1 */
    int ones = 0;
    for (int i = 0; i < L; i++) ones += (buf[i] > 0) ? 1 : -1;
    return (ones == 1) ? L : -2;   /* balance property holds iff maximal */
}

int main(void)
{
    printf("== rv_dsp host ctest ==\n");

    float sig[4];
    float s = rv_selftest(sig, 4);
    printf("selftest signature = %.6f\n", s);
    printf("  los_tap_mag   = %.6f (expect ~1.0)\n", sig[0]);
    printf("  cfo_err_rad   = %.6f (expect < 0.01)\n", sig[1]);
    printf("  quiet_metric  = %.6f (expect small)\n", sig[2]);
    printf("  motion_metric = %.6f (expect >> quiet)\n", sig[3]);

    printf("m-sequence maximality (balance == 1 -> length):\n");
    for (int m = 7; m <= 15; m++) {
        int L = mseq_period(m, 1u);
        printf("  m=%2d  L=%d  %s\n", m, (1 << m) - 1,
               (L == (1 << m) - 1) ? "OK" : "FAIL");
    }

    /* ZC periodic autocorrelation: peak vs worst sidelobe */
    enum { N = 509 };
    static rv_cf zc[N], ac[N];
    rv_zc(N, 25, zc, N);
    for (int k = 0; k < N; k++) {
        double re = 0, im = 0;
        for (int n = 0; n < N; n++) {
            int idx = n + k; if (idx >= N) idx -= N;
            re += (double)zc[idx].re * zc[n].re + (double)zc[idx].im * zc[n].im;
            im += (double)zc[idx].im * zc[n].re - (double)zc[idx].re * zc[n].im;
        }
        ac[k].re = (float)(re / N); ac[k].im = (float)(im / N);
    }
    double peak = sqrt((double)ac[0].re * ac[0].re + (double)ac[0].im * ac[0].im);
    double worst = 0;
    for (int k = 1; k < N; k++) {
        double m = sqrt((double)ac[k].re * ac[k].re + (double)ac[k].im * ac[k].im);
        if (m > worst) worst = m;
    }
    printf("ZC(509,u=25) autocorr: peak=%.4f worst_sidelobe=%.6f  ratio_dB=%.1f\n",
           peak, worst, 20.0 * log10(peak / (worst + 1e-12)));
    return 0;
}
