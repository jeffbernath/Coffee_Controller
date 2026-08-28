from __future__ import annotations

import glob
import os
import queue
import threading
import time
from typing import Any

import serial
from serial.tools import list_ports

from .registry import IoRegistry


class PicoManager:
    """Owns the Pi side of the USB CDC connection to the Pico."""

    def __init__(self, registry: IoRegistry):
        self.registry = registry
        self.events: queue.Queue[dict[str, Any]] = queue.Queue()

        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._serial: serial.Serial | None = None
        self._sequence = 1

        self._online = False
        self._hello_seen = False
        self._last_message_monotonic = 0.0
        self._port: str | None = None
        self._last_error: str | None = None
        self._firmware_version: str | None = None
        self._last_sync_monotonic = 0.0
        self._scale_counts_per_gram: float | None = None

        self._io: dict[str, Any] = {item["name"]: None for item in registry.items}
        self._available: dict[str, bool] = {item["name"]: True for item in registry.items}
        for item in registry.items:
            if item.get("type") == "load_cell":
                self._available[item["name"]] = False

        self._timeout_seconds = float(registry.serial.get("offline_timeout_ms", 5000)) / 1000.0
        self._baud_rate = int(registry.serial.get("baud_rate", 115200))

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="pico-usb", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._close_serial()
        if self._thread:
            self._thread.join(timeout=2.0)

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "pico_online": self._online,
                "serial_port": self._port,
                "firmware_version": self._firmware_version,
                "last_error": self._last_error,
                "io": dict(self._io),
                "available": dict(self._available),
                "scale_counts_per_gram": self._scale_counts_per_gram,
            }

    def set_output(self, name: str, value: Any) -> int:
        normalized = self.registry.normalize_output_value(name, value)
        sequence = self._next_sequence()
        self._send_line(f"SET,{sequence},{name},{self.registry.to_wire_value(name, normalized)}")
        return sequence

    def run_action(self, name: str) -> int:
        definition = self.registry.get(name)

        if not definition:
            raise ValueError("UNKNOWN_ACTION")

        if definition.get("type") != "action":
            raise ValueError("NOT_ACTION")

        if definition.get("direction") != "command":
            raise ValueError("NOT_COMMAND")

        sequence = self._next_sequence()

        self._send_line(
            f"CMD,{sequence},ACTION,{name}"
        )

        return sequence

    def tare_all(self) -> int:
        sequence = self._next_sequence()
        self._send_line(f"CMD,{sequence},ACTION,TARE_BOTH")
        return sequence

    def calibrate_scale(self, known_grams: float) -> int:
        if known_grams <= 0:
            raise ValueError("KNOWN_WEIGHT_MUST_BE_POSITIVE")

        sequence = self._next_sequence()
        self._send_line(f"CMD,{sequence},CALIBRATE_SCALE,{known_grams:.3f}")
        return sequence

    def _next_sequence(self) -> int:
        with self._lock:
            sequence = self._sequence
            self._sequence = 1 if self._sequence >= 2_000_000_000 else self._sequence + 1
            return sequence

    def _set_online(self, online: bool) -> None:
        changed = False
        with self._lock:
            if self._online != online:
                self._online = online
                changed = True
        if changed:
            self.events.put({"type": "pico_status", "online": online, "port": self._port})

    def _send_line(self, line: str, require_online: bool = True) -> None:
        with self._write_lock:
            connection = self._serial
            if connection is None or not connection.is_open:
                raise ConnectionError("Pico serial port is not connected")
            if require_online and not self._online:
                raise ConnectionError("Pico has not completed protocol handshake")
            connection.write((line + "\n").encode("ascii"))
            connection.flush()

    def _find_port(self) -> str | None:
        configured = os.environ.get("PICO_SERIAL_PORT")
        if configured:
            return configured

        by_id = sorted(glob.glob("/dev/serial/by-id/*"))
        for path in by_id:
            lower = path.lower()
            if "pico" in lower or "raspberry_pi" in lower or "rp2040" in lower or "rp2350" in lower:
                return path

        ports = list(list_ports.comports())
        for port in ports:
            if port.vid == 0x2E8A:  # Raspberry Pi RP2040/RP2350 USB VID
                return port.device

        if len(by_id) == 1:
            return by_id[0]

        for port in ports:
            device_lower = port.device.lower()
            if "ttyacm" in device_lower or "usbmodem" in device_lower:
                return port.device
        return None

    def _open_serial(self, port: str) -> None:
        self._serial = serial.Serial(port, self._baud_rate, timeout=0.2, write_timeout=1.0)
        self._serial.reset_input_buffer()
        self._port = port
        self._hello_seen = False
        self._last_message_monotonic = time.monotonic()
        self._set_online(False)
        self._send_line("CMD,0,SYNC", require_online=False)
        self._last_sync_monotonic = time.monotonic()

    def _close_serial(self) -> None:
        connection = self._serial
        self._serial = None
        if connection is not None:
            try:
                connection.close()
            except Exception:
                pass
        self._hello_seen = False
        self._set_online(False)

    def _handle_line(self, line: str) -> None:
        parts = [part.strip() for part in line.split(",")]
        if not parts:
            return

        message_type = parts[0]
        now = time.monotonic()

        if message_type == "HELLO" and len(parts) >= 3:
            protocol_version = int(parts[1])
            firmware_version = parts[2]
            with self._lock:
                self._firmware_version = firmware_version
                self._last_message_monotonic = now
                self._last_error = None if protocol_version == self.registry.protocol_version else "PROTOCOL_VERSION_MISMATCH"
            self._hello_seen = True
            self._set_online(True)
            self.events.put({
                "type": "hello",
                "protocol_version": protocol_version,
                "firmware_version": firmware_version,
            })
            return

        if message_type == "HB" and len(parts) >= 2:
            with self._lock:
                self._last_message_monotonic = now
            return

        if message_type == "IO" and len(parts) >= 3:
            name = parts[1]
            if name not in self.registry.by_name:
                return
            value = self.registry.parse_wire_value(name, parts[2])
            with self._lock:
                self._io[name] = value
                self._available[name] = True
                self._last_message_monotonic = now
            self.events.put({"type": "io", "name": name, "value": value, "available": True})
            return

        if message_type == "IO_STATUS" and len(parts) >= 3:
            name = parts[1]
            if name not in self.registry.by_name:
                return
            status = parts[2]
            available = status != "UNAVAILABLE"
            with self._lock:
                self._available[name] = available
                self._last_message_monotonic = now
            self.events.put({"type": "io_status", "name": name, "available": available, "status": status})
            return

        if message_type == "CAL" and len(parts) >= 4 and parts[2] == "SCALE":
            try:
                counts_per_gram = float(parts[3])
            except ValueError:
                return
            with self._lock:
                self._scale_counts_per_gram = counts_per_gram
                self._last_message_monotonic = now
            self.events.put({
                "type": "calibration",
                "sequence": parts[1],
                "counts_per_gram": counts_per_gram,
            })
            return

        if message_type == "ACK" and len(parts) >= 2:
            with self._lock:
                self._last_message_monotonic = now
            self.events.put({"type": "ack", "sequence": parts[1]})
            return

        if message_type == "ERR" and len(parts) >= 4:
            with self._lock:
                self._last_message_monotonic = now
                self._last_error = f"{parts[2]}: {parts[3]}"
            self.events.put({
                "type": "command_error",
                "sequence": parts[1],
                "target": parts[2],
                "error": parts[3],
            })
            return

    def _run(self) -> None:
        while not self._stop.is_set():
            if self._serial is None:
                port = self._find_port()
                if not port:
                    self._port = None
                    self._set_online(False)
                    self._stop.wait(1.0)
                    continue
                try:
                    self._open_serial(port)
                except Exception as exc:
                    with self._lock:
                        self._last_error = str(exc)
                    self._close_serial()
                    self._stop.wait(1.0)
                    continue

            try:
                raw = self._serial.readline() if self._serial else b""
                if raw:
                    line = raw.decode("ascii", errors="replace").strip()
                    if line:
                        self._handle_line(line)

                now = time.monotonic()
                if not self._hello_seen and now - self._last_sync_monotonic >= 1.0:
                    self._send_line("CMD,0,SYNC", require_online=False)
                    self._last_sync_monotonic = now

                with self._lock:
                    last_message = self._last_message_monotonic
                if self._hello_seen and now - last_message > self._timeout_seconds:
                    raise TimeoutError("Pico heartbeat timed out")
            except Exception as exc:
                with self._lock:
                    self._last_error = str(exc)
                self._close_serial()
                self._stop.wait(0.5)
