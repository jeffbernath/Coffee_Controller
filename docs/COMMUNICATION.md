# Coffee Controller Communication

## Purpose

This document defines communication from the browser GUI to the Raspberry Pi and from the Raspberry Pi to the RP2040 Pico.

The design has one source of truth for communicated I/O:

`config/io_manifest.json`

The Pi reads this file directly. The GUI gets the same manifest from FastAPI. The Pico build automatically generates a C++ header from it. Do not maintain separate I/O-name lists in C++, Python, and JavaScript.

## Architecture

```text
Physical hardware
      ↕
RP2040 Pico / C++
      ↕ USB CDC serial
Raspberry Pi / Python / FastAPI
      ↕ HTTP + WebSocket
HTML / JavaScript GUI
```

The Pico is authoritative for hardware state. A GUI button is a request, not proof that an output changed. The GUI displays the state that the Pico reports back.

## Current I/O

| ID | Name | Type | Direction | Hardware / behavior | Update |
|---:|---|---|---|---|---|
| 1 | `LOAD_CELL_1_G` | load cell | Pico → Pi | Load-cell channel 1 | sample 10 ms, report 100 ms |
| 2 | `LOAD_CELL_2_G` | load cell | Pico → Pi | Load-cell channel 2 | sample 10 ms, report 100 ms |
| 3 | `DIGITAL_INPUT_1` | digital | Pico → Pi | GPIO14, pull-up, active-low | sample 20 ms, report on change |
| 4 | `LED_ENABLE` | digital | Pi → Pico | On-board LED request | report confirmed state |
| 5 | `LED_BLINK_RATE_HZ` | analog control | Pi → Pico | LED blink rate, 0–10 Hz | report confirmed value |
| 6 | `LED_PWM_PERCENT` | PWM | Pi → Pico | GPIO25 LED PWM, 0–100% | report confirmed value |

A blink rate of `0 Hz` means solid ON when `LED_ENABLE` is ON. PWM controls brightness while the LED is in its ON portion of the blink cycle.

## Single Source of Truth

The manifest contains communication and hardware metadata. Example:

```json
{
  "id": 3,
  "name": "DIGITAL_INPUT_1",
  "type": "digital",
  "direction": "input",
  "data_type": "bool",
  "handler": "gpio",
  "pin": 14,
  "active_level": "low",
  "pull": "up",
  "sample_rate_ms": 20,
  "report": "on_change",
  "gui": {
    "label": "Digital Input 1",
    "widget": "status",
    "show": true
  }
}
```

### Important fields

- `id`: permanent numeric identifier. Do not reuse an old ID for a different signal.
- `name`: protocol name used from Pico through GUI.
- `type`: `digital`, `analog`, `pwm`, or `load_cell`.
- `direction`: `input` or `output`, from the Pico's perspective.
- `data_type`: `bool`, `int`, or `float`.
- `handler`: Pico behavior used to implement the I/O.
- `pin`: Pico GPIO when applicable.
- `active_level`: logical active state for digital hardware.
- `pull`: `up`, `down`, or omitted/none.
- `sample_rate_ms`: how often the Pico reads the physical input.
- `report_rate_ms`: how often periodic data is sent to the Pi.
- `report`: `periodic` or `on_change`.
- `min`, `max`, `step`: accepted control range and GUI step.
- `default`: normal startup control value.
- `safe_state`: documented safe value for an output.
- `units`: engineering units shown in the GUI.
- `channel`: hardware-driver channel, used by the two load cells.
- `pwm_frequency_hz`: PWM carrier frequency when applicable.
- `gui`: label and widget information used to build the GUI.

## Generated Pico Definitions

The Pico cannot read JSON directly in this design. During CMake configuration/build:

```text
config/io_manifest.json
        ↓
tools/generate_io.py
        ↓
build/generated/io_manifest.h
        ↓
Pico C++
```

`Pico_Coffee/CMakeLists.txt` has a dependency on both the manifest and generator. Editing the manifest causes the header to be regenerated on the next Pico build.

The generated header must not be edited manually.

## USB Serial

The Pico uses USB CDC through Pico SDK stdio:

```cmake
pico_enable_stdio_uart(Pico_Coffee 0)
pico_enable_stdio_usb(Pico_Coffee 1)
```

The Pi looks for the Pico in this order:

1. `PICO_SERIAL_PORT` environment variable, if explicitly set.
2. `/dev/serial/by-id/*`.
3. USB serial devices with Raspberry Pi VID `0x2E8A`.
4. A fallback `/dev/ttyACM*` / `usbmodem`-style device.

For a permanently installed controller, `/dev/serial/by-id/...` is preferred over relying on `/dev/ttyACM0` always having the same number.

## Serial Protocol

All protocol messages are ASCII and end with a newline (`\n`).

### Pi requests an output

```text
SET,<sequence>,<io_name>,<value>
```

Example:

```text
SET,42,LED_ENABLE,1
SET,43,LED_BLINK_RATE_HZ,2.5
SET,44,LED_PWM_PERCENT,60
```

### Pico accepts a request

```text
ACK,<sequence>
```

Example:

```text
ACK,42
```

The Pico also reports the actual confirmed I/O value:

```text
IO,LED_ENABLE,1
```

The GUI is updated from this `IO` report, not just from the button press.

### Pico rejects a request

```text
ERR,<sequence>,<target>,<reason>
```

Examples:

```text
ERR,42,LED_ENABLE,INVALID_BOOL
ERR,43,LED_BLINK_RATE_HZ,OUT_OF_RANGE
ERR,44,DIGITAL_INPUT_1,NOT_OUTPUT
```

### Pico reports an I/O

```text
IO,<io_name>,<value>
```

Examples:

```text
IO,DIGITAL_INPUT_1,1
IO,LOAD_CELL_1_G,18.420
IO,LED_PWM_PERCENT,60.000
```

### I/O unavailable

If a logical I/O exists but its hardware driver is not configured:

```text
IO_STATUS,<io_name>,UNAVAILABLE
```

The initial load-cell driver uses this behavior so an unconfigured load cell is not falsely displayed as `0.00 g`.

### Heartbeat

The Pico sends:

```text
HB,<pico_uptime_ms>
```

The default heartbeat rate is defined in the manifest as 1000 ms. The Pi marks the Pico offline when communication exceeds the manifest timeout.

### Handshake and synchronization

When the Pi opens the serial port it sends:

```text
CMD,0,SYNC
```

The Pico replies with:

```text
ACK,0
HELLO,<protocol_version>,<firmware_version>
```

and then reports a snapshot of its current I/O states.

This is important because the Pi may start after the Pico or reconnect after a USB interruption.

### Tare

The GUI's `TARE BOTH` button causes the Pi to send:

```text
CMD,<sequence>,TARE,ALL
```

The Pico asks both load-cell driver channels to tare and returns `ACK` or `ERR`.

## Pi Software

### `Pi_Coffee/pico/registry.py`

Loads and validates values against `config/io_manifest.json`. It determines whether an I/O is writable, its data type, and its allowed range.

### `Pi_Coffee/pico/manager.py`

Owns the USB serial connection. It:

- discovers the serial port;
- reconnects automatically;
- performs the protocol handshake;
- receives Pico messages;
- stores the latest confirmed values;
- sends generic `SET` requests;
- sends tare requests;
- tracks Pico online/offline state;
- places I/O events onto a queue for FastAPI.

No GUI code opens a serial port directly.

### `Pi_Coffee/main.py`

FastAPI provides:

- `GET /api/io/manifest` — same I/O manifest used by the Pico build;
- `GET /api/status` — latest Pico state and I/O values;
- `POST /api/io/{io_name}` — generic output request;
- `POST /api/tare` — tare both load cells;
- `WS /ws` — live I/O/status updates to the GUI.

## Browser / GUI

`Pi_Coffee/static/js/app.js` loads the manifest and builds the test controls from the `gui` fields.

Current widgets:

- `value`: load-cell readout;
- `status`: digital input indication;
- `toggle`: digital output request;
- `slider_number`: slider plus editable numerical field for analog/PWM controls.

The blink-rate and PWM controls therefore do not require hard-coded min/max values in JavaScript. Those values come from the manifest.

## LED Request Path

```text
User presses TURN ON
      ↓
JavaScript POST /api/io/LED_ENABLE
      ↓
FastAPI / PicoManager validates manifest
      ↓
SET,sequence,LED_ENABLE,1
      ↓ USB
Pico validates direction/range/handler
      ↓
Pico updates LED behavior
      ↓
IO,LED_ENABLE,1
ACK,sequence
      ↓ USB
Pi updates state
      ↓ WebSocket
GUI displays confirmed ON state
```

This is the same request/confirmation pattern intended for a future pump output.

## Load Cells

There are two complete logical channels now:

- `LOAD_CELL_1_G`, channel 1
- `LOAD_CELL_2_G`, channel 2

The communication, sample/report scheduling, Pi state, WebSocket, and GUI are implemented.

The physical ADC was not finalized in the project. `Pico_Coffee/load_cells.cpp` therefore provides weak hardware hooks:

```cpp
bool coffee_load_cell_hw_read_grams(uint8_t channel, float &grams);
bool coffee_load_cell_hw_tare(uint8_t channel);
```

The final HX711, NAU7802, or other load-cell ADC driver should provide strong implementations of these functions. No communication, Pi, or GUI files need to change when that driver is added.

Until the driver exists, the GUI intentionally displays the load-cell channels as unavailable.

## Safe-State Responsibility

`safe_state` is defined in the manifest, but machine safety must ultimately be enforced by Pico C++ hardware logic. The Pi and GUI must not be the only mechanism that turns a hazardous output off.

For simple GPIO outputs, the generic Pico `gpio` handler can initialize and drive the pin directly. Outputs requiring interlocks should receive a dedicated Pico handler that checks those interlocks before changing the hardware and before reporting confirmation.
