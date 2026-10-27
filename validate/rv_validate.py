#!/usr/bin/env python3
"""
RadioVision v2 mathematical validation.

Runs the REAL firmware DSP core (build/librvsim.so, the same rv_dsp.c that
compiles for the ESP32-S3) over the physics simulator and produces quantitative
evidence + figures:

  1. Processing gain          - correlation SNR vs input SNR, vs theory N*R
  2. CIR estimation error     - NMSE of the recovered channel vs SNR
  3. Motion detection ROC/AUC - new coherent detector vs old amplitude variance,
                                under AGC jitter; plus CFO robustness
  4. Interference spectrum    - DSSS sounding PSD vs a Wi-Fi ping approach

Outputs: docs/figures/*.png and docs/validation_metrics.json
"""
import os, json
os.environ.setdefault("MPLCONFIGDIR", "/tmp/rvbuild/mpl")
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import rv_bind as rv
from rv_sim import SoundingConfig, Scene, make_burst, csi_from_paths, C_LIGHT

HERE = os.path.dirname(os.path.abspath(__file__))
FIG = os.path.join(HERE, "..", "docs", "figures")
os.makedirs(FIG, exist_ok=True)
RESULTS = {}


def auc(pos, neg):
    """Area under ROC via the Mann-Whitney U statistic."""
    pos = np.asarray(pos); neg = np.asarray(neg)
    allv = np.concatenate([pos, neg])
    order = allv.argsort()
    ranks = np.empty_like(order, dtype=float)
    ranks[order] = np.arange(1, len(allv) + 1)
    # average ties
    _, inv, counts = np.unique(allv, return_inverse=True, return_counts=True)
    # simple tie handling
    r_pos = ranks[:len(pos)].sum()
    u = r_pos - len(pos) * (len(pos) + 1) / 2.0
    return u / (len(pos) * len(neg))


def roc_curve(pos, neg, n=200):
    lo = min(pos.min(), neg.min()); hi = max(pos.max(), neg.max())
    ths = np.linspace(lo, hi, n)
    tpr = [(pos >= t).mean() for t in ths]
    fpr = [(neg >= t).mean() for t in ths]
    return np.array(fpr), np.array(tpr)


# --------------------------------------------------------------------------
print("== RadioVision v2 validation ==")
sig_val, sig = rv.selftest()
print(f"selftest signature (host, via ctypes) = {sig_val:.6f}")
RESULTS["selftest_signature"] = sig_val
RESULTS["selftest_components"] = {
    "los_tap_mag": float(sig[0]), "cfo_err_rad": float(sig[1]),
    "quiet_metric": float(sig[2]), "motion_metric": float(sig[3])}

cfg = SoundingConfig(N=509, u=25, R=24, fs=40e6, fc=2.44e9)
code = rv.zc(cfg.N, cfg.u)
print(f"waveform: Zadoff-Chu N={cfg.N} u={cfg.u}, fs={cfg.fs/1e6:.0f} MS/s, "
      f"R={cfg.R} periods/burst, lambda={cfg.wavelength*100:.1f} cm, "
      f"range bin={cfg.chip_m:.2f} m")
RESULTS["config"] = {"N": cfg.N, "u": cfg.u, "R": cfg.R, "fs": cfg.fs,
                     "fc": cfg.fc, "proc_gain_dB": 10*np.log10(cfg.N*cfg.R)}

# --------------------------------------------------------------------------
# 1. Processing gain: output correlation SNR vs input per-sample SNR.
# --------------------------------------------------------------------------
print("\n[1] processing gain ...")
scene = Scene(cfg, seed=1)
paths0 = scene.paths_at(0.0, moving=False)
# isolate processing gain on a single line-of-sight path at an INTEGER sample
# delay: the off-peak floor is then pure thermal noise and all the path energy
# lands in one tap, so the measured peak SNR is exactly input + 10log10(N*R)
# once the signal is above the acquisition threshold.
paths_los = [(0.0, 1.0 + 0.0j)]
in_snr = np.arange(-40, 6, 3.0)
meas, theo = [], []
rng = np.random.default_rng(10)
for s in in_snr:
    acc = []
    for _ in range(30):
        rxb = make_burst(code, cfg, paths_los, cfo_hz=0.0, snr_db=float(s),
                         agc_scale=1.0, rng=rng)
        _, d = rv.process_burst(rxb, code, cfg.N, cfg.R)
        acc.append(d["peak_snr_db"])
    meas.append(np.mean(acc))
    theo.append(s + 10*np.log10(cfg.N*cfg.R))
meas = np.array(meas); theo = np.array(theo)
RESULTS["processing_gain_dB_theory"] = float(10*np.log10(cfg.N*cfg.R))
# measured gain where acquisition is reliable (above the sync threshold)
gain = meas - in_snr
gain_meas = float(np.median(gain[(in_snr >= -16) & (in_snr <= 0)]))
# acquisition threshold: lowest input SNR where the measured peak SNR is still
# within 3 dB of the theory line (|measured - (input + full)| <= 3).
full = RESULTS["processing_gain_dB_theory"]
on_line = in_snr[np.abs(meas - theo) <= 3.0]
RESULTS["processing_gain_dB_measured"] = gain_meas
RESULTS["acquisition_threshold_sample_snr_dB"] = float(on_line.min()) if len(on_line) else None
print(f"    acquisition threshold (sample SNR) = {RESULTS['acquisition_threshold_sample_snr_dB']} dB")
print(f"    theoretical gain 10log10(N*R) = {RESULTS['processing_gain_dB_theory']:.1f} dB")
print(f"    measured gain (median)        = {gain_meas:.1f} dB")

plt.figure(figsize=(7, 4.5))
plt.plot(in_snr, meas, "o-", label="measured output peak SNR")
plt.plot(in_snr, theo, "k--", label=f"input + 10log10(N·R) = +{RESULTS['processing_gain_dB_theory']:.0f} dB")
plt.axhline(0, color="r", lw=0.8, ls=":", label="0 dB (detection floor)")
plt.xlabel("input per-sample SNR (dB)"); plt.ylabel("recovered CIR peak SNR (dB)")
plt.title("Spread-spectrum processing gain (TX can sit far below noise)")
plt.legend(); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig(os.path.join(FIG, "fig1_processing_gain.png"), dpi=110); plt.close()

# --------------------------------------------------------------------------
# 2. CIR estimation NMSE vs SNR
# --------------------------------------------------------------------------
print("\n[2] CIR NMSE vs SNR ...")
# noiseless reference CIR (aligned the same way the DSP aligns it)
rx_clean = make_burst(code, cfg, paths0, snr_db=120.0,
                      rng=np.random.default_rng(0))
cir_ref, _ = rv.process_burst(rx_clean, code, cfg.N, cfg.R)
ref_energy = np.sum(np.abs(cir_ref)**2)
snr_axis = np.arange(-30, 21, 3.0)
nmse = []
rng = np.random.default_rng(11)
for s in snr_axis:
    errs = []
    for _ in range(30):
        rxb = make_burst(code, cfg, paths0, snr_db=float(s), rng=rng)
        cir, _ = rv.process_burst(rxb, code, cfg.N, cfg.R)
        errs.append(np.sum(np.abs(cir - cir_ref)**2) / ref_energy)
    nmse.append(10*np.log10(np.mean(errs)))
nmse = np.array(nmse)
RESULTS["cir_nmse_dB"] = {f"{int(s)}dB": float(v) for s, v in zip(snr_axis, nmse)}
plt.figure(figsize=(7, 4.5))
plt.plot(snr_axis, nmse, "o-")
plt.xlabel("input per-sample SNR (dB)"); plt.ylabel("CIR NMSE (dB)")
plt.title("Channel impulse response estimation error")
plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig(os.path.join(FIG, "fig2_cir_nmse.png"), dpi=110); plt.close()
print(f"    NMSE at 0 dB input = {nmse[np.where(snr_axis==0)[0][0]]:.1f} dB")

# --------------------------------------------------------------------------
# 3. Motion ROC/AUC: new coherent change detector vs old amplitude variance
# --------------------------------------------------------------------------
print("\n[3] motion ROC/AUC ...")

def cir_metric_stream(scene, n_cal, n_test, snr_db, cfo_hz, agc_sigma_dB,
                      person_dB, seed, timing_jit=0.25):
    """Return (quiet_scores, motion_scores) from the REAL C CIR + a fixed
    quiet baseline (mirrors the on-device coherent change metric).

    timing_jit: std (samples) of the per-burst sampling-clock offset, a realistic
    quiet-scene nuisance so the ROC is not trivially perfect."""
    rng = np.random.default_rng(seed)
    scene.person_dB = person_dB

    def agc():
        return 10 ** (rng.normal(0, agc_sigma_dB) / 20.0) if agc_sigma_dB > 0 else 1.0
    def toff():
        return rng.normal(0, timing_jit) if timing_jit > 0 else 0.0

    TAPS, BINS = 64, 64   # CIR delay support used, frequency bins in the feature

    def feat_of(cir):
        return rv.feature(cir, cfg.N, TAPS, BINS)

    # calibration (quiet) -> baseline feature
    cals = []
    for i in range(n_cal):
        rxb = make_burst(code, cfg, scene.paths_at(0.0, False), cfo_hz=cfo_hz,
                         snr_db=snr_db, agc_scale=agc(), timing_off=toff(), rng=rng)
        cir, _ = rv.process_burst(rxb, code, cfg.N, cfg.R)
        cals.append(feat_of(cir))
    baseline = np.mean(cals, axis=0)
    base_e = np.sum(baseline**2) + 1e-12

    def metric(cir):
        f = feat_of(cir)
        return float(np.sum((f - baseline)**2) / base_e)

    quiet, motion = [], []
    for i in range(n_test):
        rxb = make_burst(code, cfg, scene.paths_at(0.0, False), cfo_hz=cfo_hz,
                         snr_db=snr_db, agc_scale=agc(), timing_off=toff(), rng=rng)
        cir, _ = rv.process_burst(rxb, code, cfg.N, cfg.R)
        quiet.append(metric(cir))
        t = (i + 1) * 0.1   # person moves between bursts (100 ms cadence)
        rxb = make_burst(code, cfg, scene.paths_at(t, True), cfo_hz=cfo_hz,
                         snr_db=snr_db, agc_scale=agc(), timing_off=toff(), rng=rng)
        cir, _ = rv.process_burst(rxb, code, cfg.N, cfg.R)
        motion.append(metric(cir))
    return np.array(quiet), np.array(motion), baseline


def csi_metric_stream(scene, n_cal, n_test, snr_db, agc_sigma_dB, person_dB, seed):
    """Old-style amplitude-variance metric on emulated Wi-Fi CSI (NOT gain
    normalized, as in the original firmware)."""
    rng = np.random.default_rng(seed + 777)
    scene.person_dB = person_dB

    def agc():
        return 10 ** (rng.normal(0, agc_sigma_dB) / 20.0) if agc_sigma_dB > 0 else 1.0

    def toff():
        return rng.normal(0, 0.25)
    cals = []
    for i in range(n_cal):
        H = csi_from_paths(cfg, scene.paths_at(0.0, False), snr_db=snr_db,
                           agc_scale=agc(), timing_off=toff(), rng=rng)
        cals.append(np.abs(H))
    base_amp = np.mean(cals, axis=0)

    def metric(H):
        return float(np.sum((np.abs(H) - base_amp) ** 2))  # absolute, not normalized

    quiet, motion = [], []
    for i in range(n_test):
        H = csi_from_paths(cfg, scene.paths_at(0.0, False), snr_db=snr_db,
                           agc_scale=agc(), timing_off=toff(), rng=rng)
        quiet.append(metric(H))
        t = (i + 1) * 0.1
        H = csi_from_paths(cfg, scene.paths_at(t, True), snr_db=snr_db,
                           agc_scale=agc(), timing_off=toff(), rng=rng)
        motion.append(metric(H))
    return np.array(quiet), np.array(motion)


NCAL, NTEST = 120, 250
scene = Scene(cfg, seed=2)
JIT = 0.25   # free-running-crystal per-burst sub-sample timing jitter (samples)

# (a) free-running crystals (realistic default), new detector
q_new, m_new, _ = cir_metric_stream(scene, NCAL, NTEST, snr_db=20, cfo_hz=0.0,
                                    agc_sigma_dB=0.0, person_dB=-20, seed=100,
                                    timing_jit=JIT)
auc_new_free = auc(m_new, q_new)

# (b) with AGC jitter (sigma 3 dB) - new vs old
q_new2, m_new2, _ = cir_metric_stream(scene, NCAL, NTEST, snr_db=20, cfo_hz=0.0,
                                      agc_sigma_dB=3.0, person_dB=-20, seed=101,
                                      timing_jit=JIT)
auc_new_agc = auc(m_new2, q_new2)
q_old, m_old = csi_metric_stream(scene, NCAL, NTEST, snr_db=20,
                                 agc_sigma_dB=3.0, person_dB=-20, seed=101)
auc_old_agc = auc(m_old, q_old)

# (c) with 2 kHz CFO (new detector; CFO is estimated & removed on-device)
q_new3, m_new3, _ = cir_metric_stream(scene, NCAL, NTEST, snr_db=20, cfo_hz=2000.0,
                                      agc_sigma_dB=0.0, person_dB=-20, seed=102,
                                      timing_jit=JIT)
auc_new_cfo = auc(m_new3, q_new3)

# (d) synchronized clock (shared/GPIO sync -> no per-burst timing jitter): the
# coherent limit of the same pipeline.
q_syn, m_syn, _ = cir_metric_stream(scene, NCAL, NTEST, snr_db=20, cfo_hz=0.0,
                                    agc_sigma_dB=0.0, person_dB=-20, seed=103,
                                    timing_jit=0.0)
auc_new_sync = auc(m_syn, q_syn)

RESULTS["auc"] = {
    "new_freerun_xtal": float(auc_new_free),
    "new_freerun_agc3dB": float(auc_new_agc),
    "old_csi_agc3dB": float(auc_old_agc),
    "new_freerun_cfo2kHz": float(auc_new_cfo),
    "new_synced_clock": float(auc_new_sync),
}
for k, v in RESULTS["auc"].items():
    print(f"    AUC {k:20s} = {v:.3f}")

plt.figure(figsize=(7, 5.2))
for (q, m, lbl) in [(q_syn, m_syn, f"new, synced clock (AUC {auc_new_sync:.3f})"),
                    (q_new, m_new, f"new, free-run xtals (AUC {auc_new_free:.3f})"),
                    (q_new2, m_new2, f"new, free-run +AGC 3dB (AUC {auc_new_agc:.3f})"),
                    (q_new3, m_new3, f"new, free-run +CFO 2kHz (AUC {auc_new_cfo:.3f})"),
                    (q_old, m_old, f"old amp-var +AGC 3dB (AUC {auc_old_agc:.3f})")]:
    fpr, tpr = roc_curve(m, q)
    plt.plot(fpr, tpr, label=lbl)
plt.plot([0, 1], [0, 1], "k:", lw=0.8)
plt.xlabel("false positive rate"); plt.ylabel("true positive rate")
plt.title("Motion detection ROC  (person reflectivity -20 dB vs LOS, single burst)")
plt.legend(loc="lower right"); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig(os.path.join(FIG, "fig3_roc.png"), dpi=110); plt.close()

# AUC vs person reflectivity (sensitivity curve)
print("    sensitivity sweep ...")
dmins = np.arange(-34, -9, 3.0)
auc_curve = []
for dB in dmins:
    q, m, _ = cir_metric_stream(scene, 80, 150, snr_db=20, cfo_hz=0.0,
                                agc_sigma_dB=0.0, person_dB=float(dB), seed=200)
    auc_curve.append(auc(m, q))
RESULTS["auc_vs_person_dB"] = {f"{int(d)}": float(a) for d, a in zip(dmins, auc_curve)}
plt.figure(figsize=(7, 4.5))
plt.plot(dmins, auc_curve, "o-")
plt.axhline(0.5, color="r", ls=":", lw=0.8, label="chance")
plt.xlabel("person reflectivity relative to LOS (dB)"); plt.ylabel("detection AUC")
plt.title("Sensitivity: AUC vs how weak the moving reflection is")
plt.legend(); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig(os.path.join(FIG, "fig5_sensitivity.png"), dpi=110); plt.close()

# --------------------------------------------------------------------------
# 4. Interference spectrum: DSSS sounding vs Wi-Fi ping, for EQUAL detection SNR
# --------------------------------------------------------------------------
print("\n[4] interference spectrum ...")
# Target: a per-estimate SNR of 20 dB at the sensor.
target_snr_dB = 20.0
# DSSS gets +10log10(N*R) processing gain, so needs that much less Es/N0.
dsss_sample_snr_dB = target_snr_dB - 10*np.log10(cfg.N*cfg.R)
# A Wi-Fi/CSI ping gets no despreading gain beyond its pilots; model it needing
# the full target SNR per packet, 20 MHz wide, ~4% duty (200 pkt/s * 200 us).
ping_bw = 20e6; dsss_bw = 40e6
ping_duty = 200 * 200e-6
# Represent each as a PSD (relative dB) over a 60 MHz span, noise floor at 0 dB.
f = np.linspace(-30e6, 30e6, 2000)
def band(f, bw, center=0.0):
    return (np.abs(f - center) <= bw/2).astype(float)
# average PSD (linear) = total_power / bandwidth ; total power set by required SNR
dsss_lin = 10**(dsss_sample_snr_dB/10.0)      # ~ per-sample power rel. to noise
ping_lin = 10**(target_snr_dB/10.0) * ping_duty
dsss_psd = dsss_lin / dsss_bw * band(f, dsss_bw)
ping_psd = ping_lin / ping_bw * band(f, ping_bw)
noise_psd = 1.0 / dsss_bw  # reference
dsss_psd_dB = 10*np.log10(dsss_psd/noise_psd + 1e-9)
ping_psd_dB = 10*np.log10(ping_psd/noise_psd + 1e-9)
RESULTS["interference"] = {
    "target_detection_snr_dB": target_snr_dB,
    "dsss_required_sample_snr_dB": float(dsss_sample_snr_dB),
    "dsss_avg_psd_dB_rel_noise": float(np.max(dsss_psd_dB)),
    "ping_avg_psd_dB_rel_noise": float(np.max(ping_psd_dB)),
    "psd_advantage_dB": float(np.max(ping_psd_dB) - np.max(dsss_psd_dB)),
}
print(f"    DSSS needs only {dsss_sample_snr_dB:.1f} dB/sample at RX "
      f"(processing gain {10*np.log10(cfg.N*cfg.R):.0f} dB)")
print(f"    DSSS in-band PSD = {np.max(dsss_psd_dB):+.1f} dB vs noise; "
      f"ping PSD = {np.max(ping_psd_dB):+.1f} dB; "
      f"advantage {RESULTS['interference']['psd_advantage_dB']:.1f} dB")
plt.figure(figsize=(7, 4.5))
plt.plot(f/1e6, dsss_psd_dB, label="RadioVision DSSS (40 MHz, continuous, keyed)")
plt.plot(f/1e6, ping_psd_dB, label="Wi-Fi ping flood (20 MHz, 4% duty)")
plt.axhline(0, color="k", ls=":", lw=0.8, label="ambient noise floor")
plt.xlabel("offset from carrier (MHz)"); plt.ylabel("avg PSD relative to noise (dB)")
plt.title("Spectral footprint for equal detection SNR (lower = less interference)")
plt.legend(); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig(os.path.join(FIG, "fig4_interference_psd.png"), dpi=110); plt.close()

# --------------------------------------------------------------------------
# 5. Multistatic scaling: many coded illuminators separated by correlation (CDMA)
# --------------------------------------------------------------------------
print("\n[5] multistatic CDMA scaling ...")
from rv_sim import make_burst_multi

# auto-assigned, distinct ZC roots from device ids (the real rv_zc_root_for_id)
def assign_roots(k):
    roots, i = [], 0
    while len(roots) < k:
        r = rv.zc_root_for_id(i, cfg.N); i += 1
        if r not in roots:
            roots.append(r)
    return roots

# (a) code isolation: circular cross-correlation of two auto-assigned ZC codes,
# relative to the autocorrelation peak. This is a property of the waveforms, so
# measure it directly on the real C-generated codes.
def circ_corr(b, a):
    return np.fft.ifft(np.fft.fft(b) * np.conj(np.fft.fft(a)))
rootsAB = assign_roots(2)
codeA = rv.zc(cfg.N, rootsAB[0]).astype(np.complex128)
codeB = rv.zc(cfg.N, rootsAB[1]).astype(np.complex128)
auto_peak = np.abs(circ_corr(codeA, codeA)[0])          # == N for ZC
cross = circ_corr(codeB, codeA)
cross_rel = float(np.mean(np.abs(cross) ** 2) / auto_peak ** 2)
iso_db = float(-10 * np.log10(cross_rel))
RESULTS["cdma_isolation_dB_measured"] = iso_db
RESULTS["cdma_isolation_dB_theory"] = float(10 * np.log10(cfg.N))
print(f"    ZC code isolation: {iso_db:.1f} dB measured vs {10*np.log10(cfg.N):.1f} dB "
      f"theory (10log10 N)")

# (b) detection on link 1 as K illuminators transmit at once
def multi_auc(K, n_cal=60, n_test=120, person_dB=-16, seed=400):
    rng = np.random.default_rng(seed)
    roots = assign_roots(K)
    codes = [rv.zc(cfg.N, r) for r in roots]
    # each illuminator -> its own static channel (LOS + 2 walls)
    scenes = [Scene(cfg, seed=500 + i) for i in range(K)]
    for s in scenes:
        s.person_dB = person_dB
    TAPS, BINS = 64, 64

    def paths(k, t, moving):
        return scenes[k].paths_at(t, moving if k == 0 else False)

    def burst(t, moving):
        pl = [paths(k, t, moving) for k in range(K)]
        return make_burst_multi(codes, cfg, pl, snr_db=20.0,
                                timing_off=rng.normal(0, 0.25), rng=rng)

    def feat(rxb):
        cir, _ = rv.process_burst(rxb, codes[0], cfg.N, cfg.R)   # link 1
        return rv.feature(cir, cfg.N, TAPS, BINS)

    base = np.mean([feat(burst(0.0, False)) for _ in range(n_cal)], axis=0)
    be = np.sum(base ** 2) + 1e-12
    q = [float(np.sum((feat(burst(0.0, False)) - base) ** 2) / be) for _ in range(n_test)]
    m = [float(np.sum((feat(burst((i + 1) * 0.1, True)) - base) ** 2) / be)
         for i in range(n_test)]
    return auc(np.array(m), np.array(q))

Ks = [1, 2, 3, 4, 6, 8]
auc_K = [multi_auc(K) for K in Ks]
RESULTS["auc_vs_num_illuminators"] = {str(K): float(a) for K, a in zip(Ks, auc_K)}
for K, a in zip(Ks, auc_K):
    print(f"    K={K} illuminators (link-1 AUC) = {a:.3f}")

plt.figure(figsize=(7, 4.5))
plt.plot(Ks, auc_K, "o-")
plt.axhline(0.5, color="r", ls=":", lw=0.8, label="chance")
plt.xlabel("simultaneous coded illuminators sharing the channel")
plt.ylabel("link-1 detection AUC")
plt.title(f"Multistatic CDMA scaling (code isolation {iso_db:.0f} dB)\n"
          "all transmit at once; sensors separate them by correlation")
plt.ylim(0.45, 1.02); plt.legend(); plt.grid(alpha=0.3); plt.tight_layout()
plt.savefig(os.path.join(FIG, "fig6_scaling.png"), dpi=110); plt.close()

# --------------------------------------------------------------------------
with open(os.path.join(HERE, "..", "docs", "validation_metrics.json"), "w") as fp:
    json.dump(RESULTS, fp, indent=2)
print("\nwrote docs/validation_metrics.json and docs/figures/*.png")
