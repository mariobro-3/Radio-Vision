"""
rv_sim - physics-based bistatic channel simulator for RadioVision v2.

Two ESP32-S3 radios: one replays a period-N sounding code at fs samples/s near
2.44 GHz, the other captures raw IQ. The scene is a sum of propagation paths:
a strong TX->RX line-of-sight coupling, a few static reflectors (walls), and an
optional moving person whose bistatic path length changes over time (this is
what we must detect). The simulator reproduces what the RX ADC would deliver,
including thermal noise, a carrier-frequency offset between the two unsynchronized
crystals, and random per-burst AGC gain scaling.

Units are physical (metres, seconds, Hz) so the numbers mean something.
"""
import numpy as np

C_LIGHT = 299_792_458.0


class SoundingConfig:
    def __init__(self, N=509, u=25, R=24, fs=40e6, fc=2.44e9):
        self.N = N              # code length (chips/samples per period)
        self.u = u              # Zadoff-Chu root (the "key")
        self.R = R              # periods integrated per burst
        self.fs = fs            # sample / chip rate (Hz)
        self.fc = fc            # carrier (Hz)

    @property
    def wavelength(self):
        return C_LIGHT / self.fc

    @property
    def chip_m(self):
        """bistatic path-length resolution per tap (m)."""
        return C_LIGHT / self.fs

    @property
    def burst_len(self):
        """samples synthesized per burst (R+1 periods: 1 slack for sync)."""
        return (self.R + 1) * self.N


def _frac_roll(code, delay_samples):
    """Circularly delay `code` by a possibly fractional number of samples."""
    d0 = int(np.floor(delay_samples))
    frac = delay_samples - d0
    a = np.roll(code, d0)
    b = np.roll(code, d0 + 1)
    return (1.0 - frac) * a + frac * b


class Path:
    __slots__ = ("length_m", "gain", "name")

    def __init__(self, length_m, gain, name=""):
        self.length_m = length_m     # bistatic path length (m)
        self.gain = complex(gain)    # complex reflection/coupling coefficient
        self.name = name

    def delay_samples(self, cfg):
        return self.length_m / C_LIGHT * cfg.fs

    def phasor(self, cfg):
        # carrier phase accrued over the path length
        return np.exp(-1j * 2 * np.pi * self.length_m / cfg.wavelength)


class Scene:
    """A static background plus an optional moving person."""

    def __init__(self, cfg, seed=0):
        self.cfg = cfg
        self.rng = np.random.default_rng(seed)
        # Line-of-sight TX->RX coupling: strong, near tap 0.
        self.static = [Path(3.0, 1.0 + 0.0j, "LOS")]
        # A handful of static reflectors (walls/furniture), -6..-16 dB, random phase.
        for i, (Lm, dB) in enumerate([(12.0, -7), (26.0, -11), (41.0, -15)]):
            amp = 10 ** (dB / 20.0)
            ph = self.rng.uniform(0, 2 * np.pi)
            self.static.append(Path(Lm, amp * np.exp(1j * ph), f"wall{i}"))
        # Person parameters (set when moving).
        self.person_dB = -20.0        # reflectivity relative to LOS
        self.person_L0 = 18.0         # starting bistatic path length (m)
        self.person_speed = 1.0       # m/s (radial, bistatic)

    def paths_at(self, t_s, moving):
        """Return the list of (delay_samples, complex_gain) at time t_s."""
        cfg = self.cfg
        out = []
        for p in self.static:
            out.append((p.delay_samples(cfg), p.gain * p.phasor(cfg)))
        if moving:
            L = self.person_L0 + 2.0 * self.person_speed * t_s  # bistatic change
            amp = 10 ** (self.person_dB / 20.0)
            ph = np.exp(-1j * 2 * np.pi * L / cfg.wavelength)
            dly = L / C_LIGHT * cfg.fs
            out.append((dly, amp * ph))
        return out


def make_burst(code, cfg, paths, cfo_hz=0.0, snr_db=30.0, agc_scale=1.0,
               preroll=7, timing_off=0.0, rng=None):
    """
    Synthesize one capture burst (complex64) for the given path list.

    cfo_hz     : carrier frequency offset between the two crystals (Hz).
    snr_db     : LOS-to-noise ratio at the ADC (per sample).
    agc_scale  : multiplicative gain applied to the whole burst (AGC jitter).
    timing_off : common fractional-sample delay from the TX/RX sampling-clock
                 offset (ppm mismatch between crystals). Drifts burst to burst
                 and is the dominant real-world quiet-scene nuisance.
    """
    if rng is None:
        rng = np.random.default_rng()
    N, R = cfg.N, cfg.R
    nper = R + 1
    # one clean period = sum of delayed, scaled copies of the code
    base = np.zeros(N, dtype=np.complex128)
    for dly, gain in paths:
        base += gain * _frac_roll(code.astype(np.complex128), dly + timing_off)

    # CFO phase advance per sample, continuous across the burst
    sym_per = N / cfg.fs
    dphi = 2 * np.pi * cfo_hz * sym_per  # phase advance per period
    sig = np.empty(nper * N, dtype=np.complex128)
    for p in range(nper):
        sig[p * N:(p + 1) * N] = base * np.exp(1j * dphi * p)

    # noise referenced to LOS power (gain ~1 for LOS)
    los_pwr = 1.0
    npow = los_pwr / (10 ** (snr_db / 10.0))
    nstd = np.sqrt(npow / 2.0)
    sig += nstd * (rng.standard_normal(sig.shape) + 1j * rng.standard_normal(sig.shape))

    # random pre-roll (unknown capture start) + AGC scaling
    pre = nstd * (rng.standard_normal(preroll) + 1j * rng.standard_normal(preroll))
    burst = np.concatenate([pre, sig]) * agc_scale
    return burst.astype(np.complex64)


def make_burst_multi(codes, cfg, paths_list, snr_db=20.0, cfo_list=None,
                     timing_off=0.0, agc_scale=1.0, preroll=7, rng=None):
    """
    Synthesize one RX capture when K illuminators transmit simultaneously on the
    same channel, each with its own code and its own channel (multistatic CDMA).
    `codes` and `paths_list` are length-K lists. Returns complex64.

    The receiver sees the sum of all K coded signals plus noise; correlating
    against one code separates that illuminator's channel, the others appearing as
    a ~1/sqrt(N) cross-correlation floor.
    """
    if rng is None:
        rng = np.random.default_rng()
    N, R = cfg.N, cfg.R
    nper = R + 1
    if cfo_list is None:
        cfo_list = [0.0] * len(codes)

    sig = np.zeros(nper * N, dtype=np.complex128)
    for code, paths, cfo_hz in zip(codes, paths_list, cfo_list):
        base = np.zeros(N, dtype=np.complex128)
        for dly, gain in paths:
            base += gain * _frac_roll(code.astype(np.complex128), dly + timing_off)
        dphi = 2 * np.pi * cfo_hz * (N / cfg.fs)
        for p in range(nper):
            sig[p * N:(p + 1) * N] += base * np.exp(1j * dphi * p)

    # noise referenced to a single illuminator's LOS power (=1)
    npow = 1.0 / (10 ** (snr_db / 10.0))
    nstd = np.sqrt(npow / 2.0)
    sig += nstd * (rng.standard_normal(sig.shape) + 1j * rng.standard_normal(sig.shape))
    pre = nstd * (rng.standard_normal(preroll) + 1j * rng.standard_normal(preroll))
    return (np.concatenate([pre, sig]) * agc_scale).astype(np.complex64)


def csi_from_paths(cfg, paths, n_sub=52, snr_db=30.0, agc_scale=1.0,
                   timing_off=0.0, rng=None):
    """
    Emulate ESP32 Wi-Fi CSI for the SAME physical channel, for a fair comparison
    with the old amplitude-variance detector: the frequency response sampled on
    n_sub subcarriers, with AGC scaling and noise. Returns complex64[n_sub].
    """
    if rng is None:
        rng = np.random.default_rng()
    # subcarrier spacing over the occupied band
    k = np.arange(n_sub) - n_sub // 2
    H = np.zeros(n_sub, dtype=np.complex128)
    for dly, gain in paths:
        # delay (samples) -> phase slope across subcarriers
        H += gain * np.exp(-1j * 2 * np.pi * k * (dly + timing_off) / cfg.N)
    npow = 1.0 / (10 ** (snr_db / 10.0))
    nstd = np.sqrt(npow / 2.0)
    H += nstd * (rng.standard_normal(n_sub) + 1j * rng.standard_normal(n_sub))
    return (H * agc_scale).astype(np.complex64)
