import argparse
import logging
import os
import sys
from pathlib import Path

REQUIRED_QT_LIB = "PyQt6"

if "PYQTGRAPH_QT_LIB" not in os.environ:
    try:
        __import__(REQUIRED_QT_LIB)
    except ImportError as exc:
        raise RuntimeError(
            "PyQt6 is required for the CSI dashboard. "
            "Install it with 'pip install PyQt6' inside the ESP-IDF environment."
        ) from exc
    os.environ["PYQTGRAPH_QT_LIB"] = REQUIRED_QT_LIB

import pyqtgraph as pg
from pyqtgraph.Qt import QtWidgets

from dashboard.main_window import DashboardWindow
from dashboard.serial_monitor import SerialDeviceConfig
from dashboard.visualizer_widget import (
    VisualizerConfig,
    DEFAULT_DETECTION_ON,
    DEFAULT_RECV_BUFFER,
    DEFAULT_UDP_IP,
    DEFAULT_UDP_PORT,
    PLOT_FRESH_INTERVAL,
    TARGET_NODE,
)

QT_BACKEND = pg.Qt.QT_LIB
logger = logging.getLogger("host_processing")

DEFAULT_CAMERA_SCRIPT = os.path.join(os.path.dirname(__file__), "camera_streaming.py")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Visualise CSI packets from the ESP32 soft-AP or a mock sender."
    )
    parser.add_argument("--bind", default=DEFAULT_UDP_IP,
                        help="IPv4 address to bind the UDP listener to (default: %(default)s).")
    parser.add_argument("--port", type=int, default=DEFAULT_UDP_PORT,
                        help="UDP port to bind (default: %(default)s).")
    parser.add_argument("--recv-bytes", type=int, default=DEFAULT_RECV_BUFFER,
                        help="UDP receive buffer size.")
    parser.add_argument("--refresh-ms", type=int, default=PLOT_FRESH_INTERVAL,
                        help="Plot refresh interval in milliseconds.")
    parser.add_argument("--log-level", default="INFO",
                        help="Root logging level (DEBUG, INFO, WARNING, ...).")
    parser.add_argument("--detection", dest="detection", action="store_true",
                        default=DEFAULT_DETECTION_ON, help="Enable motion detection trigger.")
    parser.add_argument("--no-detection", dest="detection", action="store_false",
                        help="Disable motion detection trigger.")
    parser.add_argument("--target-node", type=int, default=TARGET_NODE,
                        help="Node index to monitor for motion detection.")
    parser.add_argument("--camera-script", default=DEFAULT_CAMERA_SCRIPT,
                        help="Script to launch when detection fires.")
    parser.add_argument("--python-exe", default=sys.executable,
                        help="Python interpreter used to launch the camera script.")
    parser.add_argument("--disable-camera", action="store_true",
                        help="Skip launching the camera script even when detection fires.")
    parser.add_argument("--ap-port", default="COM19",
                        help="Serial port for the active_ap device (set empty to skip).")
    parser.add_argument("--client-port", default="",
                        help="Serial port for the active_client device (set empty to skip).")
    parser.add_argument("--udp-port", default="COM17",
                        help="Serial port for the udp_client device (set empty to skip).")
    parser.add_argument("--serial-baud", type=int, default=115200,
                        help="Baud rate used for all serial monitors.")
    parser.add_argument("--logs-root", default=os.path.join(os.path.dirname(__file__), "..", "logs", "csi_dashboard"),
                        help="Base directory where dashboard logs will be stored.")
    parser.add_argument("--allow-src", action="append",
                        help="Only accept UDP packets from these IPv4 addresses (repeatable).")
    parser.add_argument("--deny-src", action="append",
                        help="Drop UDP packets from these IPv4 addresses (repeatable).")
    return parser.parse_args()


def configure_logging(level_name: str) -> None:
    level = getattr(logging, level_name.upper(), logging.INFO)
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        datefmt="%H:%M:%S",
    )
    logger.info("Log level set to %s", logging.getLevelName(level))


def build_visualizer_config(args: argparse.Namespace) -> VisualizerConfig:
    camera_command = None
    if args.disable_camera:
        logger.info("Camera trigger disabled via CLI flag.")
    else:
        script_path = os.path.abspath(args.camera_script) if args.camera_script else None
        if script_path and os.path.exists(script_path):
            camera_command = [args.python_exe, script_path]
            logger.info("Camera trigger command: %s", " ".join(camera_command))
        elif script_path:
            logger.warning("Camera script %s not found; trigger disabled.", script_path)

    allow_src = set(args.allow_src or [])
    deny_src = set(args.deny_src or [])
    if allow_src:
        logger.info("Allowed source IPs: %s", ", ".join(sorted(allow_src)))
    if deny_src:
        logger.info("Denied source IPs: %s", ", ".join(sorted(deny_src)))

    cfg = VisualizerConfig(
        bind_ip=args.bind,
        port=args.port,
        recv_bytes=args.recv_bytes,
        refresh_ms=max(1, args.refresh_ms),
        detection=args.detection,
        target_node=args.target_node,
        camera_command=camera_command,
        allow_src=allow_src,
        deny_src=deny_src,
    )
    logger.info(
        "Detection %s for node %s (threshold=%.1f, baseline alpha=%.2f)",
        "enabled" if args.detection else "disabled",
        args.target_node,
        3.0,
        0.95,
    )
    return cfg


def build_device_configs(args: argparse.Namespace, logs_dir: Path) -> list[SerialDeviceConfig]:
    devices: list[SerialDeviceConfig] = []
    if args.ap_port:
        devices.append(SerialDeviceConfig("active_ap", args.ap_port, args.serial_baud,
                                          logs_dir / f"active_ap_{args.ap_port}.log"))
    if args.client_port:
        devices.append(SerialDeviceConfig("active_client", args.client_port, args.serial_baud,
                                          logs_dir / f"active_client_{args.client_port}.log"))
    if args.udp_port:
        devices.append(SerialDeviceConfig("udp_client", args.udp_port, args.serial_baud,
                                          logs_dir / f"udp_client_{args.udp_port}.log"))
    return devices


def main() -> None:
    args = parse_args()
    configure_logging(args.log_level)
    logger.info("Qt backend in use: %s", QT_BACKEND)

    config = build_visualizer_config(args)

    app = QtWidgets.QApplication(sys.argv)
    logs_root = Path(args.logs_root).expanduser().resolve()
    session_dir = DashboardWindow.prepare_logs_dir(logs_root)

    device_configs = build_device_configs(args, session_dir)

    window = DashboardWindow(config, device_configs, session_dir)
    window.show()
    exit_code = app.exec()
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
