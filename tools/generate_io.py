#!/usr/bin/env python3
"""Generate the Pico C++ I/O manifest header from config/io_manifest.json."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def c_ident(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name).upper()


def enum_value(prefix: str, value: str) -> str:
    return f"{prefix}::{c_ident(value)}"


def fnum(value, default=0.0) -> str:
    value = default if value is None else value
    text = f"{float(value):.9g}"
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    items = manifest["io"]

    valid_types = {"digital", "analog", "pwm", "load_cell", "action"}
    valid_dirs = {"input", "output", "command"}
    names = set()
    ids = set()
    for item in items:
        if item["name"] in names:
            raise SystemExit(f"Duplicate I/O name: {item['name']}")
        if item["id"] in ids:
            raise SystemExit(f"Duplicate I/O id: {item['id']}")
        if item["type"] not in valid_types:
            raise SystemExit(f"Unsupported I/O type: {item['type']}")
        if item["direction"] not in valid_dirs:
            raise SystemExit(f"Unsupported direction: {item['direction']}")
        names.add(item["name"])
        ids.add(item["id"])

    lines = [
        "// AUTO-GENERATED FILE. DO NOT EDIT.",
        "// Source: config/io_manifest.json",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "enum class IoType : uint8_t { DIGITAL, ANALOG, PWM, LOAD_CELL, ACTION };",
        "enum class IoDirection : uint8_t { INPUT, OUTPUT, COMMAND };",
        "enum class IoReport : uint8_t { ON_CHANGE, PERIODIC };",
        "enum class IoPull : uint8_t { NONE, UP, DOWN };",
        "",
        "struct IoDefinition {",
        "    uint16_t id;",
        "    const char *name;",
        "    IoType type;",
        "    IoDirection direction;",
        "    const char *data_type;",
        "    const char *handler;",
        "    const char *resource;",
        "    const char *driver;",
        "    int pin;",
        "    bool has_pin;",
        "    bool active_high;",
        "    IoPull pull;",
        "    uint32_t sample_rate_ms;",
        "    uint32_t report_rate_ms;",
        "    IoReport report;",
        "    float min_value;",
        "    float max_value;",
        "    float step;",
        "    float default_value;",
        "    float safe_value;",
        "    int channel;",
        "    uint32_t pwm_frequency_hz;",
        "};",
        "",
        f"constexpr uint32_t IO_PROTOCOL_VERSION = {int(manifest['protocol_version'])}u;",
        f"constexpr uint32_t IO_HEARTBEAT_MS = {int(manifest['serial'].get('heartbeat_ms', 1000))}u;",
        "",
        "enum IoId : uint16_t {",
    ]

    for item in items:
        lines.append(f"    IO_ID_{c_ident(item['name'])} = {int(item['id'])},")
    lines += ["};", "", "constexpr IoDefinition IO_DEFINITIONS[] = {"]

    for item in items:
        pin = int(item.get("pin", -1))
        has_pin = "true" if "pin" in item else "false"
        active_high = "false" if item.get("active_level") == "low" else "true"
        pull = enum_value("IoPull", item.get("pull", "none"))
        report = enum_value("IoReport", item.get("report", "on_change"))
        line = (
            "    {"
            f"{int(item['id'])}, \"{item['name']}\", "
            f"{enum_value('IoType', item['type'])}, "
            f"{enum_value('IoDirection', item['direction'])}, "
            f"\"{item.get('data_type', 'float')}\", "
            f"\"{item.get('handler', '')}\", "
            f"\"{item.get('resource', '')}\", "
            f"\"{item.get('driver', '')}\", "
            f"{pin}, {has_pin}, {active_high}, {pull}, "
            f"{int(item.get('sample_rate_ms', 0))}u, "
            f"{int(item.get('report_rate_ms', 0))}u, "
            f"{report}, "
            f"{fnum(item.get('min'), 0)}, {fnum(item.get('max'), 1)}, {fnum(item.get('step'), 1)}, "
            f"{fnum(item.get('default'), 0)}, {fnum(item.get('safe_state'), 0)}, "
            f"{int(item.get('channel', 0))}, {int(item.get('pwm_frequency_hz', 0))}u"
            "},"
        )
        lines.append(line)

    lines += [
        "};",
        "",
        "constexpr std::size_t IO_DEFINITION_COUNT = sizeof(IO_DEFINITIONS) / sizeof(IO_DEFINITIONS[0]);",
        "",
    ]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))


if __name__ == "__main__":
    main()
