"""ctypes bindings to the RadioVision DSP core (build/librvsim.so).

Complex arrays are passed as numpy complex64 whose memory layout is exactly
interleaved float32 (re, im), which matches the C `rv_cf` struct. This means the
Python validation calls the identical C code that runs on the ESP32-S3.
"""
import os
import ctypes as C
import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
_SO = os.path.join(_HERE, "..", "build", "librvsim.so")

_lib = C.CDLL(os.path.abspath(_SO))

_f32p = C.POINTER(C.c_float)

_lib.rvx_zc.restype = C.c_int
_lib.rvx_zc.argtypes = [C.c_int, C.c_int, _f32p]
_lib.rvx_mseq.restype = C.c_int
_lib.rvx_mseq.argtypes = [C.c_int, C.c_uint, _f32p]
_lib.rvx_process.restype = None
_lib.rvx_process.argtypes = [_f32p, C.c_int, _f32p, C.c_int, C.c_int, _f32p, _f32p]
_lib.rvx_feature.restype = None
_lib.rvx_feature.argtypes = [_f32p, C.c_int, C.c_int, C.c_int, _f32p]
_lib.rvx_zc_root_for_id.restype = C.c_int
_lib.rvx_zc_root_for_id.argtypes = [C.c_uint, C.c_int]
_lib.rvx_det_new.restype = C.c_void_p
_lib.rvx_det_new.argtypes = [C.c_int, C.c_float, C.c_float, C.c_int, C.c_int, C.c_int]
_lib.rvx_det_update.restype = C.c_float
_lib.rvx_det_update.argtypes = [C.c_void_p, _f32p]
_lib.rvx_det_motion.restype = C.c_int
_lib.rvx_det_motion.argtypes = [C.c_void_p]
_lib.rvx_det_phase.restype = C.c_int
_lib.rvx_det_phase.argtypes = [C.c_void_p]
_lib.rvx_det_threshold.restype = C.c_float
_lib.rvx_det_threshold.argtypes = [C.c_void_p]
_lib.rvx_det_free.argtypes = [C.c_void_p]
_lib.rvx_selftest.restype = C.c_float
_lib.rvx_selftest.argtypes = [_f32p]


def _cptr(arr_c64):
    """float* to the start of a complex64 array (viewed as interleaved f32)."""
    a = np.ascontiguousarray(arr_c64, dtype=np.complex64)
    return a, a.view(np.float32).ctypes.data_as(_f32p)


def _fptr(arr_f32):
    a = np.ascontiguousarray(arr_f32, dtype=np.float32)
    return a, a.ctypes.data_as(_f32p)


def zc(N, u):
    out = np.zeros(N, dtype=np.complex64)
    _, p = _cptr(out)
    n = _lib.rvx_zc(N, u, p)
    assert n == N, f"rvx_zc returned {n}"
    return out


def zc_root_for_id(dev_id, N):
    return int(_lib.rvx_zc_root_for_id(dev_id, N))


def mseq(m, seed):
    L = (1 << m) - 1
    out = np.zeros(L, dtype=np.float32)
    _, p = _fptr(out)
    n = _lib.rvx_mseq(m, seed, p)
    assert n == L
    return out


def process_burst(rx_c64, ref_c64, N, R):
    rx, rxp = _cptr(rx_c64)
    ref, refp = _cptr(ref_c64)
    cir = np.zeros(N, dtype=np.complex64)
    _, cirp = _cptr(cir)
    diag = np.zeros(7, dtype=np.float32)
    _, diagp = _fptr(diag)
    _lib.rvx_process(rxp, len(rx), refp, N, R, cirp, diagp)
    d = {
        "sync_offset": int(diag[0]), "cfo": float(diag[1]),
        "peak_snr_db": float(diag[2]), "peak_tap": int(diag[3]),
        "n_data_bits": int(diag[4]), "data_bits": int(diag[5]),
        "ok": int(diag[6]),
    }
    return cir, d


def feature(cir_c64, N, n_taps_used, n_bins):
    cir, cirp = _cptr(cir_c64)
    feat = np.zeros(n_bins, dtype=np.float32)
    _, fp = _fptr(feat)
    _lib.rvx_feature(cirp, N, n_taps_used, n_bins, fp)
    return feat


class Detector:
    def __init__(self, n_feat, ema_alpha=0.02, k_sigma=6.0, cal_target=200,
                 hyst_on=3, hyst_off=8):
        self.h = _lib.rvx_det_new(n_feat, ema_alpha, k_sigma, cal_target,
                                  hyst_on, hyst_off)

    def update(self, feat_f32):
        f, p = _fptr(feat_f32)
        return float(_lib.rvx_det_update(self.h, p))

    @property
    def motion(self): return bool(_lib.rvx_det_motion(self.h))
    @property
    def phase(self): return int(_lib.rvx_det_phase(self.h))
    @property
    def threshold(self): return float(_lib.rvx_det_threshold(self.h))

    def __del__(self):
        try:
            _lib.rvx_det_free(self.h)
        except Exception:
            pass


def selftest():
    sig = np.zeros(4, dtype=np.float32)
    _, p = _fptr(sig)
    return float(_lib.rvx_selftest(p)), sig
