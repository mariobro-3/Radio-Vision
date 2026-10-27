/* Debug a single channel-sounding burst through the public API. */
#include "rv_dsp.h"
#include <stdio.h>
#include <math.h>

enum { N = 63, R = 4, RP = R + 1, OFF = 5 };
static rv_cf code[N], rx[OFF + N * RP + 8], scratch[N * R], cir[N];

int main(void)
{
    rv_zc(N, 5, code, N);
    const int tap[3] = { 2, 7, 13 };
    rv_cf ch[3] = { {1.0f,0.0f}, {0.4f,-0.2f}, {0.2f,0.1f} };
    float cfo = 0.15f;

    for (int i = 0; i < OFF; i++) { rx[i].re = 0; rx[i].im = 0; }
    for (int r = 0; r < RP; r++) {
        float ph = cfo * (float)r, cr = cosf(ph), ci = sinf(ph);
        for (int n = 0; n < N; n++) {
            float yr = 0, yi = 0;
            for (int t = 0; t < 3; t++) {
                int idx = n - tap[t]; while (idx < 0) idx += N;
                yr += code[idx].re * ch[t].re - code[idx].im * ch[t].im;
                yi += code[idx].re * ch[t].im + code[idx].im * ch[t].re;
            }
            int o = OFF + r * N + n;
            rx[o].re = yr * cr - yi * ci;
            rx[o].im = yr * ci + yi * cr;
        }
    }

    rv_burst_diag d;
    int ok = rv_process_burst(rx, OFF + N * RP, code, N, R, cir, scratch, &d);
    printf("ok=%d sync_off=%d (expect %d) peak_tap=%d cfo=%.4f (expect %.4f) snr=%.1f dB\n",
           ok, d.sync_offset, OFF, d.peak_tap, d.cfo_rad_per_sym, cfo, d.peak_snr_db);
    printf("CIR taps of interest:\n");
    for (int k = 0; k < N; k++) {
        float m = sqrtf(cir[k].re * cir[k].re + cir[k].im * cir[k].im);
        if (m > 0.05f)
            printf("  tap %2d: %+.3f %+.3fj  |.|=%.3f\n", k, cir[k].re, cir[k].im, m);
    }
    return 0;
}
