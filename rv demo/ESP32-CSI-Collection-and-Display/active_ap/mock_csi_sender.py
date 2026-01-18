#!/usr/bin/env python3
"""
Utility to feed synthetic CSI packets into the host visualiser.

It formats UDP datagrams the same way the ESP32 AP does, so you can
exercise `host_processing_pyqt.py` without having the hardware running.
"""

from __future__ import annotations

import argparse
import logging
import socket
import time
from typing import List

import numpy as np

logger = logging.getLogger("mock_csi_sender")

UDP_PORT = 8848
CSI_LEN = 57 * 2  # keep in sync with host_processing_pyqt.py
RAW_CSI_LEN = 64 * 3 * 2  # imaginary & real parts for 3 * 64 subcarriers
RX_CTRL_LEN = 12


def fmt_csv(values: List[int]) -> str:
    return ",".join(str(v) for v in values) + ",\n"


def build_packet(mac: str, rssi_dbm: int, noise_dbm: int, tick: int) -> bytes:
    """
    Build a UDP payload that mimics the firmware trace format.
    """
    lines = []

    lines.append(f"mac ={mac}")
    lines.append(f"rx_ctrl info,len = {RX_CTRL_LEN}")

    #               RSSI, pad, sig-mod, pad, bandwidth, pad, pad, pad, stbc, pad, pad, noise floor
    rx_ctrl = [rssi_dbm, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, noise_dbm]
    lines.append(fmt_csv(rx_ctrl))

    lines.append(f"RAW,len = {RAW_CSI_LEN}")

    # create some fake CSI magnitudes that drift over time
    base = np.linspace(-20, 20, RAW_CSI_LEN)
    wiggle = 5 * np.sin(2 * np.pi * (tick / 50.0) + np.arange(RAW_CSI_LEN) / 12.0)
    raw = np.asarray(base + wiggle, dtype=int)
    lines.append(fmt_csv(raw.tolist()))

    lines.append("")  # blank line between packets

    packet = "\n".join(lines)
    return packet.encode("ascii")


def main() -> None:
    parser = argparse.ArgumentParser(description="Send synthetic CSI packets to the host visualiser.")
    parser.add_argument("--mac", default="ec:da:3b:5b:06:f4", help="Fake MAC address to embed in packets.")
    parser.add_argument("--port", type=int, default=UDP_PORT, help="UDP port to send to (default 8848).")
    parser.add_argument("--host", default="127.0.0.1", help="Host running the visualiser (default localhost).")
    parser.add_argument("--interval", type=float, default=0.1, help="Seconds between packets.")
    parser.add_argument("--count", type=int, default=0, help="Total packets to send (0=run forever).")
    parser.add_argument("--log-level", default="INFO", help="Logging level (DEBUG, INFO, WARNING, ...).")
    parser.add_argument("--preview", action="store_true", help="Log the first 120 characters of each payload.")
    args = parser.parse_args()

    level = getattr(logging, args.log_level.upper(), logging.INFO)
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    logger.info(
        "Sending mock CSI packets to %s:%s every %.2fs (backend buffer len=%s)",
        args.host,
        args.port,
        args.interval,
        RAW_CSI_LEN,
    )

    tick = 0
    try:
        while True:
            rssi = -45 + int(5 * np.sin(tick / 30.0))
            noise = -95 + int(3 * np.cos(tick / 40.0))
            payload = build_packet(args.mac, rssi, noise, tick)
            if args.preview or logger.isEnabledFor(logging.DEBUG):
                preview = payload[:120].decode("ascii", errors="ignore").replace("\n", "\\n")
                logger.debug("tick=%s rssi=%s noise=%s preview=%s", tick, rssi, noise, preview)
            try:
                sock.sendto(payload, (args.host, args.port))
            except OSError as exc:
                logger.error("Failed to send packet %s: %s", tick, exc)
                time.sleep(args.interval)
                continue
            if logger.isEnabledFor(logging.DEBUG):
                logger.debug("Sent packet %s (%s bytes)", tick, len(payload))
            elif tick % 25 == 0:
                logger.info("Sent packet %s (%s bytes)", tick, len(payload))
            tick += 1
            if args.count and tick >= args.count:
                logger.info("Requested packet count reached (%s). Exiting.", args.count)
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        logger.info("Interrupted by user, stopping sender.")


if __name__ == "__main__":
    main()

