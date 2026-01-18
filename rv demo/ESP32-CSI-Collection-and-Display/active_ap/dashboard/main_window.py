from __future__ import annotations

import logging
from datetime import datetime
from pathlib import Path
from typing import Iterable, List, Optional

from pyqtgraph.Qt import QtCore, QtGui, QtWidgets

from .serial_monitor import SerialDeviceConfig, SerialReaderThread
from .visualizer_widget import CsiVisualizerWidget, VisualizerConfig


class SerialMonitorTab(QtWidgets.QWidget):
    def __init__(self, device: SerialDeviceConfig, parent: Optional[QtWidgets.QWidget] = None) -> None:
        super().__init__(parent)
        self.device = device
        self.log_file = device.log_path.open("a", encoding="utf-8") if device.log_path else None

        layout = QtWidgets.QVBoxLayout(self)
        self.status_label = QtWidgets.QLabel("Disconnected")
        layout.addWidget(self.status_label)

        self.text_edit = QtWidgets.QPlainTextEdit()
        self.text_edit.setReadOnly(True)
        layout.addWidget(self.text_edit, 1)

        buttons = QtWidgets.QHBoxLayout()
        layout.addLayout(buttons)
        self.clear_button = QtWidgets.QPushButton("Clear")
        self.clear_button.clicked.connect(self.text_edit.clear)
        buttons.addWidget(self.clear_button)
        buttons.addStretch()

    def append_line(self, _: str, line: str) -> None:
        self.text_edit.appendPlainText(line)
        if self.log_file:
            self.log_file.write(line + "\n")
            self.log_file.flush()

    def update_status(self, _: str, is_connected: bool, message: str) -> None:
        self.status_label.setText(message)
        color = "green" if is_connected else "red"
        self.status_label.setStyleSheet(f"color: {color};")

    def close(self) -> None:
        if self.log_file:
            try:
                self.log_file.close()
            except OSError:
                pass
            self.log_file = None
        super().close()


class DashboardWindow(QtWidgets.QMainWindow):
    def __init__(
        self,
        visualizer_config: VisualizerConfig,
        device_configs: Iterable[SerialDeviceConfig],
        logs_dir: Path,
        parent: Optional[QtWidgets.QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self.logger = logging.getLogger("dashboard")
        self.visualizer = CsiVisualizerWidget(visualizer_config, logger=logging.getLogger("dashboard.visualizer"))
        self.device_configs = list(device_configs)
        self.logs_dir = logs_dir
        self.serial_threads: List[SerialReaderThread] = []
        self.device_tabs: List[SerialMonitorTab] = []

        splitter = QtWidgets.QSplitter()
        splitter.addWidget(self.visualizer)

        self.tab_widget = QtWidgets.QTabWidget()
        splitter.addWidget(self.tab_widget)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 2)
        self.setCentralWidget(splitter)
        self.quit_shortcut = QtGui.QShortcut(QtGui.QKeySequence("Ctrl+Q"), self)
        self.quit_shortcut.activated.connect(self.close)

        for cfg in self.device_configs:
            tab = SerialMonitorTab(cfg)
            self.tab_widget.addTab(tab, cfg.name)
            reader = SerialReaderThread(cfg)
            reader.line_received.connect(tab.append_line)
            reader.status_changed.connect(tab.update_status)
            reader.start()
            self.serial_threads.append(reader)
            self.device_tabs.append(tab)

        self.visualizer.start()
        self.setWindowTitle("ESP32 CSI Dashboard")
        self.resize(1600, 900)
        self.statusBar().showMessage(f"Logs: {self.logs_dir}")

    @staticmethod
    def prepare_logs_dir(base_dir: Path) -> Path:
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        session_dir = base_dir / timestamp
        session_dir.mkdir(parents=True, exist_ok=True)
        return session_dir

    def closeEvent(self, event: QtCore.QEvent) -> None:  # type: ignore[override]
        self.visualizer.stop()
        for thread in self.serial_threads:
            thread.stop()
            thread.wait(1000)
        for tab in self.device_tabs:
            tab.close()
        super().closeEvent(event)

