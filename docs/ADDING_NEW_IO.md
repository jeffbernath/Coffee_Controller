# Adding New I/O to Coffee Controller

## Goal

Adding a normal I/O should require editing one source-of-truth file rather than repeating the same definition in C++, Python, JavaScript, and HTML.

The source of truth is:

```text
config/io_manifest.json
```

The normal path is:

```text
Edit io_manifest.json
        ↓
Build / restart
        ↓
Pico C++ definitions generated automatically
Pi reads the manifest directly
GUI reads the manifest from FastAPI
```

## Before Adding an I/O

Choose these properties:

1. Permanent `id`.
2. Unique functional `name`.
3. `type`.
4. `direction` from the Pico's perspective.
5. `data_type`.
6. Hardware `handler`.
7. GPIO/channel information, if applicable.
8. Sample and report behavior for inputs.
9. Range/default/safe values for outputs.
10. GUI widget and label, if it should be visible.

Do not use raw names such as `GPIO14` as the protocol name. Use functional names such as `WATER_LOW`, `PUMP`, or `BOILER_TEMP_C`. GPIO assignment can then change without changing the rest of the application.

## I/O Types Currently Supported by the Manifest

### `digital`

Boolean input or output.

Typical uses:

- switch;
- pressure switch;
- water-level input;
- relay;
- solenoid;
- pump enable.

### `analog`

Floating-point control/measurement value. The current example is the LED blink-rate control. A physical analog input/output may require a Pico ADC or external DAC handler.

### `pwm`

Floating-point PWM setting with a defined range. The current example is LED intensity.

### `load_cell`

Load-cell value in engineering units. The current project defines two channels in grams.

## Direction

Direction is always relative to the Pico:

- `input`: physical machine → Pico → Pi → GUI.
- `output`: GUI/Pi requests → Pico → physical machine.

## Adding a Simple Digital Input

Example: add a door switch on GPIO15 that closes to ground.

Add only this entry to `config/io_manifest.json`:

```json
{
  "id": 7,
  "name": "DOOR_SWITCH",
  "type": "digital",
  "direction": "input",
  "data_type": "bool",
  "handler": "gpio",
  "pin": 15,
  "active_level": "low",
  "pull": "up",
  "sample_rate_ms": 20,
  "report": "on_change",
  "gui": {
    "label": "Door Switch",
    "group": "inputs",
    "widget": "status",
    "true_text": "CLOSED",
    "false_text": "OPEN",
    "show": true
  }
}
```

For a standard `digital` + `input` + `handler: "gpio"`, no new Pico polling code, Python parser code, WebSocket code, or JavaScript parser code is required.

The Pico generic GPIO input handler will:

1. configure GPIO15;
2. apply the pull-up;
3. convert active-low electrical state into logical true/false;
4. sample it every 20 ms;
5. report only when it changes.

The GUI automatically adds the status row because `gui.widget` is `status`.

## Adding a Simple Digital Output

Example: add a basic pump control on GPIO16, active-high, with OFF as the safe/default state.

```json
{
  "id": 8,
  "name": "PUMP",
  "type": "digital",
  "direction": "output",
  "data_type": "bool",
  "handler": "gpio",
  "pin": 16,
  "active_level": "high",
  "default": false,
  "safe_state": false,
  "report": "on_change",
  "gui": {
    "label": "Pump",
    "group": "outputs",
    "widget": "toggle",
    "show": true
  }
}
```

For a basic GPIO output, this produces the full request path without adding special Python or JavaScript code:

```text
GUI toggle
 ↓
POST /api/io/PUMP
 ↓
SET,sequence,PUMP,1
 ↓
Pico generic GPIO handler
 ↓
GPIO16 HIGH
 ↓
IO,PUMP,1
 ↓
GUI confirmed ON
```

### When a digital output needs interlocks

Do not use the generic `gpio` handler for a safety-sensitive output if it requires logic such as:

- only run pump when water is present;
- prevent heater when flow is absent;
- prevent valve operation during a fault.

Instead, give the manifest entry a dedicated handler name, for example:

```json
"handler": "pump_control"
```

Then implement `pump_control` once in Pico C++. The Pi and GUI still remain generic and unchanged.

## Adding an Analog Control

The current LED blink-rate entry is the example:

```json
{
  "id": 5,
  "name": "LED_BLINK_RATE_HZ",
  "type": "analog",
  "direction": "output",
  "data_type": "float",
  "handler": "led_blink_rate",
  "units": "Hz",
  "min": 0.0,
  "max": 10.0,
  "step": 0.1,
  "default": 1.0,
  "safe_state": 0.0,
  "gui": {
    "label": "LED Blink Rate",
    "widget": "slider_number",
    "decimals": 1,
    "show": true
  }
}
```

The GUI automatically creates:

- a slider;
- a numerical value field;
- an editable numeric input;
- the engineering unit;
- confirmed Pico value display.

The Pi automatically rejects values below `min` or above `max`.

A new analog behavior still needs a Pico handler if it changes machine behavior. The communication plumbing does not need to change.

## Adding PWM

The current LED PWM example is:

```json
{
  "id": 6,
  "name": "LED_PWM_PERCENT",
  "type": "pwm",
  "direction": "output",
  "data_type": "float",
  "handler": "led_pwm",
  "pin": 25,
  "units": "%",
  "pwm_frequency_hz": 1000,
  "min": 0.0,
  "max": 100.0,
  "step": 1.0,
  "default": 100.0,
  "safe_state": 0.0,
  "gui": {
    "label": "LED Intensity",
    "widget": "slider_number",
    "decimals": 0,
    "show": true
  }
}
```

A PWM output should define the carrier frequency and user-facing value range separately. In this example, the user works in percent while the Pico converts that percentage into a hardware PWM level.

## Adding a Load Cell

The two logical load cells are already in the manifest. Their hardware interface is intentionally isolated.

The communication system calls:

```cpp
bool load_cell_read_grams(uint8_t channel, float &grams);
bool load_cell_tare(uint8_t channel);
```

Those functions delegate to weak hardware hooks in `Pico_Coffee/load_cells.cpp`.

When the ADC is finalized, add a new C++ source file for the selected hardware and provide strong functions:

```cpp
bool coffee_load_cell_hw_read_grams(uint8_t channel, float &grams)
{
    // Read channel 1 or 2 from the selected ADC.
    // Apply tare/calibration/filtering.
    // Return true only when grams is a valid measurement.
}

bool coffee_load_cell_hw_tare(uint8_t channel)
{
    // Capture/update the zero offset for the selected channel.
}
```

Then add that source file to `Pico_Coffee/CMakeLists.txt`.

The following do not change:

- serial protocol;
- Pi manager;
- FastAPI routes;
- WebSocket handling;
- GUI load-cell cards;
- load-cell names.

This is why the ADC choice can be finalized later without redesigning the communication system.

## Sample Rate vs Report Rate

These are intentionally separate.

Example:

```json
"sample_rate_ms": 10,
"report_rate_ms": 100
```

means:

```text
Pico reads every 10 ms
Pico can use those readings for local control/filtering
Pico reports to Pi every 100 ms
```

Do not increase USB traffic just because a control loop requires a fast sensor sample.

For a digital switch, use:

```json
"sample_rate_ms": 20,
"report": "on_change"
```

so the Pico can poll/debounce frequently while only communicating a new value when the state changes.

## Choosing `default` and `safe_state`

`default` is the normal startup value used by the current runtime.

`safe_state` documents the state the output must take during a safety condition or communication failure. A safety-critical handler must explicitly enforce that behavior in Pico C++.

Typical examples:

```json
"default": false,
"safe_state": false
```

for a pump, valve, heater, or relay that must be OFF until intentionally enabled.

## GUI Widgets

The current generic GUI supports:

| Widget | Use |
|---|---|
| `value` | numeric sensor / load-cell display |
| `status` | digital input state |
| `toggle` | digital output request |
| `slider_number` | analog or PWM output, with slider and editable number |

Set:

```json
"show": false
```

when the I/O should be communicated but should not appear in the current test GUI.

## Build Process

There is no separate manual generator step required for a normal Pico build.

The root VS Code `Pico: Build` task runs CMake. CMake sees the dependency:

```text
io_manifest.json → generate_io.py → io_manifest.h
```

and regenerates the header when necessary.

If you want to inspect the generated file manually, run from the project root:

```bash
python3 tools/generate_io.py config/io_manifest.json /tmp/io_manifest.h
```

Do not edit `/tmp/io_manifest.h` or the generated build copy.

## Recommended New-I/O Workflow

For most new I/O:

1. Add one entry to `config/io_manifest.json`.
2. Choose an existing generic handler where possible.
3. Build the Pico.
4. Restart/deploy the Pi app so it reloads the manifest.
5. Verify the new I/O appears in `/api/io/manifest` and the GUI.
6. Test the real electrical input/output.

Only add C++ code when the physical device or machine behavior needs a new handler/driver.

### What should normally NOT require editing

A normal new I/O should not require adding its name to:

- `main.py`;
- `manager.py`;
- `registry.py`;
- `app.js` protocol parsing;
- a Python list of known I/O;
- a JavaScript list of known I/O.

That is the main purpose of the manifest architecture.
