from __future__ import annotations

import logging
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import serial
from pyqtgraph.Qt import QtCore


@dataclass
class SerialDeviceConfig:
    name: str
    port: str
    baud: int = 115200
    log_path: Optional[Path] = None


class SerialReaderThread(QtCore.QThread):
    line_received = QtCore.pyqtSignal(str, str)  # device name, line text
    status_changed = QtCore.pyqtSignal(str, bool, str)  # device name, is_connected, message

    def __init__(self, config: SerialDeviceConfig, logger: Optional[logging.Logger] = None) -> None:
        super().__init__()
        self.config = config
        self.logger = logger or logging.getLogger(f"dashboard.serial.{config.name}")
        self._stop_event = threading.Event()
        self._serial: Optional[serial.Serial] = None

    def stop(self) -> None:
        self._stop_event.set()
        if self._serial:
            try:
                if self._serial.is_open:
                    self._serial.close()
            except serial.SerialException:
                pass
            finally:
                self._serial = None

    def run(self) -> None:
        try:
            self._serial = serial.Serial(
                self.config.port,
                self.config.baud,
                timeout=0.2,
                write_timeout=0.2,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                bytesize=serial.EIGHTBITS,
            )
            self.status_changed.emit(self.config.name, True, f"Connected to {self.config.port}")
            self.logger.info("Connected to %s @ %s", self.config.port, self.config.baud)
        except serial.SerialException as exc:
            self.status_changed.emit(self.config.name, False, str(exc))
            self.logger.error("Failed to open %s: %s", self.config.port, exc)
            return

        buffer: bytes = b""
        while not self._stop_event.is_set():
            try:
                chunk = self._serial.read(self._serial.in_waiting or 1)
                if not chunk:
                    continue
                buffer += chunk
                if b"\n" in buffer:
                    lines = buffer.split(b"\n")
                    buffer = lines.pop()
                    for raw in lines:
                        text = raw.decode("utf-8", errors="ignore").rstrip("\r")
                        self.line_received.emit(self.config.name, text)
            except serial.SerialException as exc:
                self.logger.error("Serial error on %s: %s", self.config.port, exc)
                self.status_changed.emit(self.config.name, False, str(exc))
                break

        if self._serial and self._serial.is_open:
            try:
                self._serial.close()
            except serial.SerialException:
                pass
        self.logger.info("Disconnected from %s", self.config.port)
        self.status_changed.emit(self.config.name, False, "Disconnected")

