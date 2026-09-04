# GC9A01 Scale Display Wiring

This firmware drives a 240 x 240 GC9A01/GC9A01A SPI display from **SPI1** so
it does not share the existing MAX31865 SPI0 bus.

## Pico to GC9A01

| Pico GPIO | Pico physical pin | GC9A01 signal | Purpose |
|---|---:|---|---|
| 3V3(OUT) | 36 | VCC | Display logic power (for a 3.3 V module) |
| GND | any GND | GND | Ground |
| GP8 | 11 | DC / D-C | Data/command select |
| GP9 | 12 | CS | Chip select |
| GP10 | 14 | SCL / CLK / SCK | SPI1 clock |
| GP11 | 15 | SDA / DIN / MOSI | SPI1 data from Pico to display |
| GP12 | 16 | RST / RES | Display reset |
| GP13 | 17 | BL / BLK | Backlight control, **only if this pin is a logic-level enable on your module** |

No MISO connection is required.

### Backlight caution

GC9A01 breakout boards are not all wired the same. The firmware drives GP13
HIGH after display initialization. Use GP13 only when the module's BL/BLK pin
is a logic-level backlight-enable input. If BL/BLK is the raw LED supply pin,
power it from the appropriate supply/current-limited circuit instead and leave
GP13 disconnected.

## Physical tare button

| Pico GPIO | Pico physical pin | Connection |
|---|---:|---|
| GP6 | 9 | One side of normally-open momentary tare switch |
| GND | any GND | Other side of tare switch |

GP6 uses the Pico's internal pull-up. Pressing the switch shorts GP6 to GND.
The firmware debounces the switch and tares both HX711 channels locally. No GUI
or serial-protocol change is required.

## Existing connections left unchanged

- HX711 #1: GP2 DOUT, GP3 SCK
- HX711 #2: GP4 DOUT, GP5 SCK
- MAX31865 SPI0: GP16 MISO, GP17 CS, GP18 SCK, GP19 MOSI
- Existing manifest digital input: GP14
