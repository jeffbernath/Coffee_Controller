from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class IoRegistry:
    """Loads the single source-of-truth I/O manifest used by Pi and Pico."""

    def __init__(self, manifest_path: Path):
        self.manifest_path = Path(manifest_path)
        self.manifest = json.loads(self.manifest_path.read_text())
        self.protocol_version = int(self.manifest["protocol_version"])
        self.serial = self.manifest.get("serial", {})
        self.items = self.manifest["io"]
        self.by_name = {item["name"]: item for item in self.items}

        if len(self.by_name) != len(self.items):
            raise ValueError("Duplicate I/O names in io_manifest.json")

    def get(self, name: str) -> dict[str, Any] | None:
        return self.by_name.get(name)

    def parse_wire_value(self, name: str, value: str) -> bool | int | float | str:
        definition = self.by_name[name]
        data_type = definition.get("data_type", "float")
        if data_type == "bool":
            return value.strip().lower() in {"1", "true", "on"}
        if data_type == "int":
            # Accept both canonical integer wire values ("3") and legacy
            # float-formatted integer values ("3.000").
            return int(float(value))
        if data_type == "float":
            return float(value)
        return value

    def normalize_output_value(self, name: str, value: Any) -> bool | int | float:
        definition = self.by_name.get(name)
        if not definition:
            raise ValueError("UNKNOWN_IO")
        if definition.get("direction") != "output":
            raise ValueError("NOT_OUTPUT")

        data_type = definition.get("data_type", "float")
        if data_type == "bool":
            if isinstance(value, bool):
                normalized: bool | int | float = value
            elif isinstance(value, (int, float)) and value in (0, 1):
                normalized = bool(value)
            else:
                raise ValueError("INVALID_BOOL")
        elif data_type == "int":
            normalized = int(value)
        else:
            normalized = float(value)

        if data_type != "bool":
            minimum = definition.get("min")
            maximum = definition.get("max")
            if minimum is not None and normalized < minimum:
                raise ValueError("OUT_OF_RANGE")
            if maximum is not None and normalized > maximum:
                raise ValueError("OUT_OF_RANGE")

        return normalized

    def to_wire_value(self, name: str, value: Any) -> str:
        definition = self.by_name[name]
        if definition.get("data_type") == "bool":
            return "1" if bool(value) else "0"
        return str(value)
