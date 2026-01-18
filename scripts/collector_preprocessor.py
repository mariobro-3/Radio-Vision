#!/usr/bin/env python3
"""
Real-time ESP32 CSI collector with Hampel filtering and PCA presence detector.
Implements Hernandez thesis pipeline (Thesis p. 150–176):
  1. Amplitude-only CSI (phase discarded per p. 154)
  2. Hampel filter w=50 (p. 161)
  3. PCA reducing 64→10 subcarriers for TinyML-ready features (p. 168)

Usage examples:
  python scripts/collector_preprocessor.py --port 40123 --session evening_walk
"""

from __future__ import annotations

import argparse
import csv
import enum
import logging
import queue
import signal
import socket
import struct
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple

import numpy as np
from sklearn.decomposition import PCA

HEADER_STRUCT = struct.Struct("<I H H Q I 6s b B 8s")
MAGIC = 0x582D4353
MAX_SUBCARRIERS = 64
PCA_COMPONENTS = 10


class Phase(enum.Enum):
    WAIT_EMPTY = "wait_empty"
    CAL_EMPTY = "cal_empty"
    WAIT_PRESENCE = "wait_presence"
    CAL_PRESENCE = "cal_presence"
    RUN = "run"


def mac_to_str(mac_bytes: bytes) -> str:
    return ":".join(f"{b:02X}" for b in mac_bytes)


class HampelFilter:
    """Windowed Hampel filter (Thesis p. 161) applied independently per subcarrier."""

    def __init__(self, window: int = 50, threshold: float = 3.0) -> None:
        self.window = window
        self.threshold = threshold
        self.buffers: List[deque] = [deque(maxlen=window) for _ in range(MAX_SUBCARRIERS)]

    def apply(self, amplitudes: np.ndarray) -> np.ndarray:
        filtered = np.array(amplitudes, dtype=np.float32)
        for idx, value in enumerate(filtered):
            buf = self.buffers[idx]
            buf.append(float(value))
            if len(buf) < self.window:
                continue
            buf_arr = np.fromiter(buf, dtype=np.float32)
            median = np.median(buf_arr)
            mad = np.median(np.abs(buf_arr - median))
            mad = mad if mad > 1e-3 else 1e-3
            if abs(value - median) > self.threshold * 1.4826 * mad:
                filtered[idx] = median
        return filtered


class PCACalibrator:
    """Per-link PCA calibrator storing empty and presence samples."""

    def __init__(self, components: int = PCA_COMPONENTS) -> None:
        self.components = components
        self.empty: List[np.ndarray] = []
        self.presence: List[np.ndarray] = []
        self.pca = PCA(n_components=components, svd_solver="auto", whiten=False)
        self.ready = False
        self.baseline: np.ndarray | None = None
        self.threshold: float = 0.0

    def collect(self, amplitudes: np.ndarray, phase: Phase) -> None:
        vec = amplitudes.astype(np.float32)
        if phase == Phase.CAL_EMPTY:
            self.empty.append(vec)
        elif phase == Phase.CAL_PRESENCE:
            self.presence.append(vec)

    def finalize(self) -> None:
        if not self.empty or not self.presence:
            raise RuntimeError("Insufficient calibration data")
        train = np.vstack([self.empty, self.presence])
        self.pca.fit(train)
        empty_proj = self.pca.transform(np.vstack(self.empty))
        presence_proj = self.pca.transform(np.vstack(self.presence))
        self.baseline = empty_proj.mean(axis=0)
        baseline_scores = np.linalg.norm(empty_proj - self.baseline, axis=1)
        presence_scores = np.linalg.norm(presence_proj - self.baseline, axis=1)
        self.threshold = (baseline_scores.mean() + presence_scores.mean()) / 2.0
        self.ready = True

    def project(self, amplitudes: np.ndarray) -> Tuple[np.ndarray, float, int]:
        if not self.ready or self.baseline is None:
            raise RuntimeError("PCA calibrator not ready")
        proj = self.pca.transform(amplitudes.reshape(1, -1))[0]
        score = float(np.linalg.norm(proj - self.baseline))
        presence = int(score > self.threshold)
        return proj, score, presence


@dataclass
class LinkState:
    mac: str
    label: str
    hampel: HampelFilter = field(default_factory=HampelFilter)
    calibrator: PCACalibrator = field(default_factory=PCACalibrator)


class SessionWriter:
    """Thread-safe CSV writer streaming synchronized samples."""

    def __init__(self, output_path: Path) -> None:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        self.file = output_path.open("w", newline="", encoding="utf-8")
        self.writer = csv.writer(self.file)
        header = [
            "host_ts",
            "device_ts_us",
            "seq",
            "mac",
            "label",
            "rssi",
            "channel",
            "phase",
            "presence_flag",
            "presence_score",
        ]
        header += [f"pc{i+1}" for i in range(PCA_COMPONENTS)]
        header += [f"amp_{i}" for i in range(MAX_SUBCARRIERS)]
        self.writer.writerow(header)
        self.lock = threading.Lock()

    def write(self, row: List[float | str | int]) -> None:
        with self.lock:
            self.writer.writerow(row)
            self.file.flush()

    def close(self) -> None:
        self.file.close()


class WiFiSonarCollector:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.queue: queue.Queue = queue.Queue(maxsize=8192)
        self.links: Dict[str, LinkState] = {}
        self.phase = Phase.WAIT_EMPTY
        self.phase_lock = threading.Lock()
        self.stop_event = threading.Event()
        timestamp = datetime.utcnow().strftime("%Y%m%d_%H%M%S")
        session_name = args.session or f"session_{timestamp}"
        self.writer = SessionWriter(Path(args.output_dir) / f"{session_name}.csv")
        self.listener_thread = threading.Thread(target=self._udp_listener, name="udp_listener", daemon=True)
        self.processor_thread = threading.Thread(target=self._processor, name="processor", daemon=True)
        self.calibration_done = threading.Event()

    def start(self) -> None:
        self.listener_thread.start()
        self.processor_thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.listener_thread.join(timeout=1.0)
        self.processor_thread.join(timeout=1.0)
        self.writer.close()

    def _udp_listener(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.args.bind_ip, self.args.port))
        logging.info("Listening on %s:%d", self.args.bind_ip, self.args.port)
        sock.settimeout(1.0)
        while not self.stop_event.is_set():
            try:
                data, addr = sock.recvfrom(4096)
                host_ts = time.time()
                self.queue.put_nowait((host_ts, data, addr))
            except queue.Full:
                logging.warning("Processing backlog; dropping packet")
            except socket.timeout:
                continue
            except OSError as exc:
                logging.error("Socket error: %s", exc)
                break
        sock.close()

    def _processor(self) -> None:
        while not self.stop_event.is_set():
            try:
                host_ts, datagram, addr = self.queue.get(timeout=0.5)
            except queue.Empty:
                continue

            parsed = self._parse_datagram(datagram)
            if not parsed:
                continue
            header, amplitudes = parsed
            mac = header["mac"]
            link = self.links.setdefault(mac, LinkState(mac=mac, label=header["label"]))

            filtered = link.hampel.apply(amplitudes)
            phase = self.current_phase()
            link.calibrator.collect(filtered, phase)

            presence_flag = 0
            presence_score = 0.0
            pcs = np.zeros(PCA_COMPONENTS, dtype=np.float32)

            if link.calibrator.ready:
                pcs, presence_score, presence_flag = link.calibrator.project(filtered)

            row = [
                host_ts,
                header["device_ts"],
                header["seq"],
                mac,
                link.label,
                header["rssi"],
                header["channel"],
                phase.value,
                presence_flag,
                presence_score,
            ]
            row += pcs.tolist()
            row += filtered.tolist()
            self.writer.write(row)

    def _parse_datagram(self, data: bytes) -> Tuple[Dict[str, object], np.ndarray] | None:
        if len(data) < HEADER_STRUCT.size:
            return None
        (
            magic,
            version,
            amp_len,
            device_ts,
            seq,
            mac_bytes,
            rssi,
            channel,
            label_bytes,
        ) = HEADER_STRUCT.unpack_from(data)
        if magic != MAGIC:
            logging.debug("Magic mismatch: %#x", magic)
            return None
        float_count = min(amp_len, MAX_SUBCARRIERS)
        expected = HEADER_STRUCT.size + amp_len * 4
        if len(data) < expected:
            logging.warning("Truncated CSI frame (expected %d, got %d)", expected, len(data))
            return None
        amplitudes = np.zeros(MAX_SUBCARRIERS, dtype=np.float32)
        payload = np.frombuffer(data, dtype="<f4", offset=HEADER_STRUCT.size, count=amp_len)
        amplitudes[:float_count] = payload[:float_count]
        header = {
            "mac": mac_to_str(mac_bytes),
            "label": label_bytes.decode("ascii", errors="ignore").strip("\x00") or "STA",
            "device_ts": device_ts,
            "seq": seq,
            "rssi": rssi,
            "channel": channel,
        }
        return header, amplitudes

    def current_phase(self) -> Phase:
        with self.phase_lock:
            return self.phase

    def set_phase(self, phase: Phase) -> None:
        with self.phase_lock:
            self.phase = phase

    def run_calibration(self) -> None:
        input("Clear the room, then press Enter to capture EMPTY baseline...")
        self.set_phase(Phase.CAL_EMPTY)
        logging.info("Capturing empty baseline for %d s", self.args.cal_empty_sec)
        time.sleep(self.args.cal_empty_sec)

        self.set_phase(Phase.WAIT_PRESENCE)
        input("Introduce target motion (walk/fall) then press Enter to capture PRESENCE...")
        self.set_phase(Phase.CAL_PRESENCE)
        logging.info("Capturing presence data for %d s", self.args.cal_presence_sec)
        time.sleep(self.args.cal_presence_sec)

        # Finalize PCA for all links observed during calibration.
        failures = 0
        for mac, link in self.links.items():
            try:
                link.calibrator.finalize()
                logging.info("PCA ready for %s (%s)", link.label, mac)
            except RuntimeError:
                failures += 1
                logging.warning("Calibration incomplete for %s (%s)", link.label, mac)
        self.set_phase(Phase.RUN)
        self.calibration_done.set()
        logging.info("Calibration complete (failures=%d). Streaming in RUN phase.", failures)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WiFi sonar collector with PCA preprocessing")
    parser.add_argument("--bind-ip", default="0.0.0.0", help="Local bind IP (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=40123, help="UDP port to listen on (default 40123)")
    parser.add_argument("--session", default="", help="Custom session name used for CSV filenames")
    parser.add_argument("--output-dir", default="data/raw", help="Directory where CSV data is written")
    parser.add_argument("--cal-empty-sec", type=int, default=20, help="Seconds of empty-room calibration")
    parser.add_argument("--cal-presence-sec", type=int, default=20, help="Seconds of presence calibration")
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    logging.basicConfig(level=getattr(logging, args.log_level), format="%(asctime)s | %(levelname)s | %(message)s")

    collector = WiFiSonarCollector(args)

    def shutdown(signum=None, frame=None) -> None:
        logging.info("Stopping collector...")
        collector.stop()
        raise SystemExit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    collector.start()
    try:
        collector.run_calibration()
        logging.info("Entering run phase. Press Ctrl+C to stop.")
        while not collector.stop_event.is_set():
            time.sleep(1.0)
    except KeyboardInterrupt:
        shutdown()


if __name__ == "__main__":
    main()

