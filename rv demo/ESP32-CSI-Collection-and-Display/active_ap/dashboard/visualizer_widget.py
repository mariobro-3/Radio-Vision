import logging
import socket
import subprocess
import time
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Set

import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore, QtGui, QtWidgets


DEFAULT_DETECTION_ON = True
DEFAULT_UDP_IP = "0.0.0.0"
DEFAULT_UDP_PORT = 8848
DEFAULT_RECV_BUFFER = 8192
SOCKET_TIMEOUT = 1.0

QUEUE_LEN = 50
CSI_LEN = 57 * 2
DISP_FRAME_RATE = 10
PLOT_FRESH_INTERVAL = 20

LOG_LEN = 3
TEST_MIN_NUM = 100
DIFF_THRESHOLD = 8  # Increased from 3 to reduce sensitivity
TARGET_NODE = 0
baseline_alpha = 0.95


@dataclass
class VisualizerConfig:
    bind_ip: str = DEFAULT_UDP_IP
    port: int = DEFAULT_UDP_PORT
    recv_bytes: int = DEFAULT_RECV_BUFFER
    refresh_ms: int = PLOT_FRESH_INTERVAL
    detection: bool = DEFAULT_DETECTION_ON
    target_node: int = TARGET_NODE
    camera_command: Optional[Sequence[str]] = None
    camera_cooldown_s: float = 10.0  # Increased from 3.0 to prevent spam
    allow_src: Set[str] = field(default_factory=set)
    deny_src: Set[str] = field(default_factory=set)


class CsiVisualizerWidget(QtWidgets.QWidget):
    """Widget that embeds the CSI PyQtGraph visuals and UDP listener."""

    def __init__(self, config: VisualizerConfig, parent: Optional[QtWidgets.QWidget] = None,
                 logger: Optional[logging.Logger] = None) -> None:
        super().__init__(parent)
        self.config = config
        self.logger = logger or logging.getLogger("host_processing")

        self.sock: Optional[socket.socket] = None
        self.warned_timeout = False
        self.last_packet_ts: Optional[float] = None
        self._running = False

        self.node_mac_list: List[str] = []
        self.rssi_que_list: List[np.ndarray] = []
        self.csi_points_list: List[np.ndarray] = []

        self.csi_data_log = []
        self.csi_db_baseline = np.zeros(CSI_LEN)
        self.test_counter = 0
        self.camera_cooldown_s = max(0.0, self.config.camera_cooldown_s)
        self.last_camera_launch = 0.0
        self.camera_processes: List[subprocess.Popen] = []

        self._build_ui()
        self._init_state()

    def _build_ui(self) -> None:
        layout = QtWidgets.QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        self.disp_time = np.array([(x - QUEUE_LEN + 1) / DISP_FRAME_RATE for x in range(QUEUE_LEN)])

        self.plot_layout = pg.LayoutWidget()
        layout.addWidget(self.plot_layout, 3)

        self.pw1 = pg.PlotWidget(name="RSSI")
        self.plot_layout.addWidget(self.pw1, row=0, col=0)
        self.pw1.setLabel('left', 'SNR', units='dB')
        self.pw1.setLabel('bottom', 'Time', units=None)
        self.pw1.setYRange(20, 70)

        self.pw2 = pg.PlotWidget(name="CSI")
        self.plot_layout.addWidget(self.pw2, row=0, col=1)
        self.pw2.setLabel('left', 'CSI', units='dB')
        self.pw2.setLabel('bottom', 'subcarriers [-58,-2] & [2,58]', units=None)
        self.pw2.setXRange(0, CSI_LEN)
        self.pw2.setYRange(20, 70)

        self.rssi_curves = [self.pw1.plot(pen=(0, 3))]
        self.csi_curves = [self.pw2.plot(pen=(0, 3))]
        self.baseline_csi_curve = self.pw2.plot(pen=(10, 3))

        right_panel = QtWidgets.QVBoxLayout()
        layout.addLayout(right_panel, 1)

        self.info_label = QtWidgets.QLabel("Mean Frame Rate: -- FPS")
        right_panel.addWidget(self.info_label)
        right_panel.addStretch()

        self.fps = 0.0
        self.lastupdate = QtCore.QTime.currentTime()

    def _init_state(self) -> None:
        self.rssi_que_list = [np.zeros(QUEUE_LEN)]
        self.csi_points_list = [np.zeros(CSI_LEN)]
        self.node_mac_list = []
        self.csi_data_log = []
        self.csi_db_baseline = np.zeros(CSI_LEN)
        self.test_counter = 0

    # region Lifecycle
    def start(self) -> None:
        if self._running:
            return
        self.sock = self._setup_udp_socket()
        self._running = True
        self.warned_timeout = False
        self._schedule_next_update()
        self.logger.info("Listening for CSI packets on %s:%s (timeout %.1fs)",
                         self.config.bind_ip, self.config.port, SOCKET_TIMEOUT)

    def stop(self) -> None:
        self._running = False
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None
        self._terminate_camera_processes()

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # type: ignore[name-defined]
        self.stop()
        super().closeEvent(event)

    # endregion

    def _schedule_next_update(self) -> None:
        if not self._running:
            return
        QtCore.QTimer.singleShot(self.config.refresh_ms, self._update)

    def _update(self) -> None:
        if not self._running:
            return

        node_id = self._try_receive_packet()
        if node_id >= 0:
            self.rssi_curves[node_id].setData(x=self.disp_time, y=self.rssi_que_list[node_id])
            self.csi_curves[node_id].setData(y=self.csi_points_list[node_id])

            self._calculate_fps()
            self._update_label()

            if self.config.detection and self.config.target_node == node_id:
                self.baseline_csi_curve.setData(y=self.csi_db_baseline)
                if self._crossing_detection(self.csi_points_list[node_id]):
                    self.logger.info("Detection threshold crossed for node %s", node_id)
                    self._trigger_camera()

        self._schedule_next_update()

    def _calculate_fps(self) -> None:
        now = QtCore.QTime.currentTime()
        dt_ms = self.lastupdate.msecsTo(now)
        if dt_ms <= 0:
            dt_ms = 1
        fps = 1000.0 / dt_ms
        self.lastupdate = now
        self.fps = self.fps * 0.9 + fps * 0.1

    def _update_label(self) -> None:
        self.info_label.setText(f"Mean Frame Rate: {self.fps:.3f} FPS")

    def _setup_udp_socket(self) -> socket.socket:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.config.bind_ip, self.config.port))
        sock.settimeout(SOCKET_TIMEOUT)
        return sock

    def _try_receive_packet(self) -> int:
        if not self.sock:
            return -1
        try:
            data, addr = self.sock.recvfrom(self.config.recv_bytes)
        except socket.timeout:
            if not self.warned_timeout:
                self.logger.info("Waiting for CSI packets on %s:%s ...",
                                 self.config.bind_ip, self.config.port)
                self.warned_timeout = True
            return -1
        except OSError as exc:
            self.logger.error("Socket error while receiving CSI: %s", exc)
            return -1

        src_ip, src_port = addr
        if self.config.deny_src and src_ip in self.config.deny_src:
            self.logger.debug("Dropped packet from denied source %s:%s", src_ip, src_port)
            return -1
        if self.config.allow_src and src_ip not in self.config.allow_src:
            self.logger.debug("Dropped packet from non-allowed source %s:%s", src_ip, src_port)
            return -1

        self.warned_timeout = False
        self.last_packet_ts = QtCore.QTime.currentTime().msecsSinceStartOfDay() / 1000.0
        self.logger.debug("Received %d bytes from %s:%s", len(data), src_ip, src_port)

        try:
            rx_ctrl_data, raw_csi_data, node_id = self._parse_data_packet(data)
        except ValueError as exc:
            self.logger.warning("Discarded malformed packet: %s", exc)
            return -1

        # Validate packet format:
        # rx_ctrl_data[2] = sig_mode: 1 = HT (required for quality CSI)
        # rx_ctrl_data[4] = cwb: 0 = 20MHz, 1 = 40MHz (we accept both, prefer 20MHz)
        # rx_ctrl_data[8] = stbc: 0 = no STBC (required for simpler parsing)
        if rx_ctrl_data[2] != 1 or rx_ctrl_data[8] != 0:
            self.logger.debug("Ignoring packet (format mismatch: sig_mod=%s stbc=%s)",
                              rx_ctrl_data[2], rx_ctrl_data[8])
            return -1

        rssi, csi_data = self._cook_csi_data(rx_ctrl_data, raw_csi_data)

        self.rssi_que_list[node_id] = np.roll(self.rssi_que_list[node_id], -1)
        self.rssi_que_list[node_id][-1] = rssi
        self.csi_points_list[node_id] = 10 * np.log10(np.abs(csi_data) ** 2 + 0.1)
        self.logger.debug("Updated CSI buffers for node %s", node_id)
        return node_id

    def _parse_data_packet(self, payload: bytes):
        data_str = payload.decode("ascii", errors="ignore")
        lines = data_str.splitlines()
        node_id = -1
        rx_ctrl_data = None
        raw_csi_data = None
        for idx, line in enumerate(lines):
            if not line:
                continue
            items = line.split(",")

            if "mac =" in items[0]:
                mac_addr = items[0].split("mac =")[-1]
                if mac_addr not in self.node_mac_list:
                    self.node_mac_list.append(mac_addr)
                    node_id = len(self.node_mac_list) - 1
                    self._add_new_node(node_id)
                    self.logger.info("Registered new node %s with id %s", mac_addr, node_id)
                else:
                    node_id = self.node_mac_list.index(mac_addr)

            if items[0] == "rx_ctrl info":
                rx_ctrl_len = self._extract_len(items, idx, lines)
                rx_ctrl_data = self._parse_data_line(lines[idx + 1], rx_ctrl_len)

            if items[0] == "RAW":
                raw_len = self._extract_len(items, idx, lines)
                raw_csi_data = self._parse_data_line(lines[idx + 1], raw_len)

        if node_id < 0:
            raise ValueError("MAC address not found in packet")
        if rx_ctrl_data is None:
            raise ValueError("rx_ctrl payload not parsed")
        if raw_csi_data is None:
            raise ValueError("RAW payload not parsed")
        return rx_ctrl_data, raw_csi_data, node_id

    def _extract_len(self, items: List[str], idx: int, lines: List[str]) -> int:
        tmp_pos = items[1].find("len = ")
        if tmp_pos < 0:
            raise ValueError("header missing length information")
        line_len = int(items[1][tmp_pos + 6:])
        if idx + 1 >= len(lines):
            raise ValueError("payload missing")
        return line_len

    @staticmethod
    def _parse_data_line(line: str, data_len: int) -> List[int]:
        data = line.split(",")
        if data[-1] == "":
            data = data[:-1]
        values = [int(x) for x in data]
        if len(values) != data_len:
            raise ValueError("Unexpected payload length")
        return values

    def _add_new_node(self, node_id: int) -> None:
        if node_id == 0:
            return
        self.rssi_que_list.append(np.zeros(QUEUE_LEN))
        self.csi_points_list.append(np.zeros(CSI_LEN))
        self.rssi_curves.append(self.pw1.plot(pen=(node_id, 3)))
        self.csi_curves.append(self.pw2.plot(pen=(node_id, 3)))

    def _cook_csi_data(self, rx_ctrl_info: List[int], raw_csi_data: List[int]):
        rssi = rx_ctrl_info[0]
        noise_floor = rx_ctrl_info[11]
        cwb = rx_ctrl_info[4]  # Channel bandwidth: 0=20MHz, 1=40MHz
        
        # Convert I/Q pairs to complex values
        raw = [raw_csi_data[2 * i] * 1j + raw_csi_data[2 * i + 1]
               for i in range(int(len(raw_csi_data) / 2))]
        raw_array = np.array(raw)
        
        # Scale by SNR
        snr_db = rssi - noise_floor
        snr_abs = 10 ** (snr_db / 10.0)
        csi_sum = np.sum(np.abs(raw_array) ** 2)
        if csi_sum > 0:
            num_subcarrier = len(raw_array)
            scale = np.sqrt((snr_abs / csi_sum) * num_subcarrier)
            raw_array = raw_array * scale
        
        # Extract subcarriers based on bandwidth
        # 20MHz HT: 64 subcarriers (indices -32 to -1, 0, 1 to 31)
        # 40MHz HT: 128 subcarriers
        if cwb == 0:  # 20 MHz
            # For 20MHz, we have 64 complex samples
            # Subcarriers: -28 to -1 (28) and 1 to 28 (28) are data, rest are pilots/null
            # Use indices 4:32 (subcarriers -28 to -1) and 33:61 (subcarriers 1 to 28)
            if len(raw_array) >= 64:
                # Rearrange: negative subcarriers then positive
                neg_sub = raw_array[32:60]   # subcarriers -32 to -5 (28 values)
                pos_sub = raw_array[4:32]    # subcarriers 4 to 31 (28 values)
                cooked = np.concatenate((neg_sub, pos_sub))
            else:
                cooked = raw_array
        else:  # 40 MHz
            # Original logic for 40MHz
            if len(raw_array) > 64:
                cooked = raw_array[64:]
                cooked = np.concatenate((cooked[-58:-1], cooked[2:59]))
            else:
                cooked = raw_array
        
        # Pad or trim to expected length
        if len(cooked) < CSI_LEN:
            cooked = np.pad(cooked, (0, CSI_LEN - len(cooked)), mode='constant')
        elif len(cooked) > CSI_LEN:
            cooked = cooked[:CSI_LEN]
            
        return snr_db, cooked

    def _crossing_detection(self, new_csi_data: np.ndarray) -> bool:
        self.test_counter += 1
        if len(self.csi_data_log) < LOG_LEN:
            self.csi_data_log.append(new_csi_data)
            return False
        elif self.test_counter < TEST_MIN_NUM:
            self.csi_data_log.pop(0)
            self.csi_data_log.append(new_csi_data)
            self.csi_db_baseline = self.csi_db_baseline * baseline_alpha + \
                new_csi_data * (1 - baseline_alpha)
            return False
        else:
            self.csi_data_log.pop(0)
            self.csi_data_log.append(new_csi_data)
            self.csi_db_baseline = self.csi_db_baseline * baseline_alpha + \
                new_csi_data * (1 - baseline_alpha)

        csi_diff = np.zeros(CSI_LEN)
        for csi in self.csi_data_log:
            csi_diff += (csi - self.csi_db_baseline) / LOG_LEN
        return np.max(np.abs(csi_diff)) > DIFF_THRESHOLD

    def _trigger_camera(self) -> None:
        if not self.config.camera_command:
            self.logger.debug("Camera trigger requested but no command configured.")
            return
        
        # Clean up dead processes
        self._reap_camera_processes()
        
        # Only allow ONE camera instance at a time
        if self.camera_processes:
            self.logger.debug("Camera trigger suppressed - camera already running")
            return
        
        now = time.monotonic()
        if now - self.last_camera_launch < self.camera_cooldown_s:
            self.logger.debug(
                "Camera trigger suppressed due to cooldown (%.1fs remaining)",
                self.camera_cooldown_s - (now - self.last_camera_launch),
            )
            return
        
        try:
            proc = subprocess.Popen(self.config.camera_command)
            self.camera_processes.append(proc)
            self.logger.info("Launching camera script: %s", " ".join(self.config.camera_command))
            self.last_camera_launch = now
        except OSError as exc:
            self.logger.error("Failed to launch camera script: %s", exc)

    def _reap_camera_processes(self) -> None:
        alive = []
        for proc in self.camera_processes:
            if proc.poll() is None:
                alive.append(proc)
        self.camera_processes = alive

    def _terminate_camera_processes(self) -> None:
        for proc in self.camera_processes:
            if proc.poll() is not None:
                continue
            try:
                proc.terminate()
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        self.camera_processes = []

