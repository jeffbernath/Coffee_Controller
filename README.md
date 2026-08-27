# Coffee Controller

Coffee Controller uses a Raspberry Pi for the FastAPI/web GUI and an RP2040 Pico for real-time I/O and machine control.

## Architecture

- Raspberry Pi: Python, FastAPI, HTML/CSS/JavaScript, networking, logging and higher-level control.
- RP2040 Pico: C++ using Pico SDK, physical I/O, real-time behavior and interlocks.
- Pi ↔ Pico: USB CDC serial for normal communication.
- Single I/O source of truth: `config/io_manifest.json`.

## Current communication test I/O

- Two logical load-cell inputs (`LOAD_CELL_1_G`, `LOAD_CELL_2_G`).
- One digital input on GPIO14, active-low with pull-up.
- On-board LED ON/OFF request.
- LED blink-rate analog control, 0–10 Hz.
- LED PWM intensity control, 0–100%.

The load-cell communication path is complete, but the final physical load-cell ADC driver is intentionally not selected in code yet. Until that driver is added, the GUI marks both load cells unavailable instead of displaying a false zero.

## Documentation

- `docs/COMMUNICATION.md` — complete Pi ↔ Pico ↔ GUI protocol and architecture.
- `docs/ADDING_NEW_IO.md` — how to add new I/O using the shared manifest.

## Raspberry Pi app

```bash
cd Pi_Coffee
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000
```

Open:

```text
http://<PI-IP>:8000
```

## Pico build

The Pico build automatically generates its C++ I/O table from `config/io_manifest.json`.

With the Pico SDK environment configured:

```bash
cmake -S Pico_Coffee -B Pico_Coffee/build
cmake --build Pico_Coffee/build --parallel 4
```

The root VS Code `Pico: Build` and `Pico: Build + Program` tasks use the same build path.
