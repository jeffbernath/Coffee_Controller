# Coffee Controller Starter

Starter repository for the Coffee Controller project.

## Architecture

- Raspberry Pi: FastAPI backend, HTML GUI, networking, logging, updates
- RP2040 Pico: real-time I/O and machine control
- Pi <-> RP2040: USB for normal communications and firmware update
- SWD: recommended production recovery/debug interface

## Repository layout

- `Pi_Coffee/` - Raspberry Pi FastAPI application
- `Pico_Blink/` - RP2040 blink test project
- `system/` - example Linux service and labwc kiosk autostart files

## Raspberry Pi app

Create and activate a virtual environment:

    cd Pi_Coffee
    python3 -m venv .venv
    source .venv/bin/activate
    pip install -r requirements.txt

Start manually:

    uvicorn main:app --host 0.0.0.0 --port 8000

Open from another device on the same network:

    http://<PI-IP>:8000

## RP2040 build

Install prerequisites on Raspberry Pi OS:

    sudo apt install -y cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential git picotool

Clone the Pico SDK:

    cd ~
    git clone https://github.com/raspberrypi/pico-sdk.git
    cd pico-sdk
    git submodule update --init

Build:

    export PICO_SDK_PATH=$HOME/pico-sdk
    cd Pico_Blink
    rm -rf build
    mkdir build
    cd build
    cmake -DPICOTOOL_FORCE_FETCH_FROM_GIT=ON ..
    make -j4

Flash while the Pico is in BOOTSEL mode:

    picotool load -f blink.uf2
    picotool reboot

## Kiosk setup

`system/coffeebar.service` is an example systemd service for the FastAPI backend.

`system/labwc-autostart` contains the Chromium kiosk launch command.

These files assume the Pi username is `jeffreybernath` and the app is installed at:

    /home/jeffreybernath/Coffee_Controller/Pi_Coffee

Change those paths before using on another system.

## Next development steps

1. Split the Pi GUI into HTML/CSS/JavaScript files.
2. Add USB CDC communication between Pi and RP2040.
3. Add RP2040 firmware version reporting.
4. Add an `ENTER_BOOTLOADER` command.
5. Let the Pi automatically update RP2040 firmware with picotool.
6. Keep SWD pads on the production RP2040 PCB for recovery/debugging.
