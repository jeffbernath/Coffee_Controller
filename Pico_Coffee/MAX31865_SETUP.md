# MAX31865 + 3-Wire PT100 Setup

## Pico wiring

| MAX31865 breakout | Raspberry Pi Pico | Purpose |
|---|---:|---|
| VIN / 3V3 | 3V3(OUT) | Module power |
| GND | GND | Common ground |
| SDO | GP16 | SPI0 MISO |
| CS | GP17 | Chip select |
| SCK / CLK | GP18 | SPI0 clock |
| SDI | GP19 | SPI0 MOSI |
| RDY / DRDY | Not connected | Not used by this driver |

The Pico firmware uses SPI mode 1 at 500 kHz. GPIO 2-5 remain dedicated to the two HX711 load-cell channels, GPIO 14 remains the existing digital input, and GPIO 25 remains the onboard LED.

## 3-wire PT100 wiring

A 3-wire PT100 normally has two same-color wires connected to the same end of the RTD element and one different-color wire connected to the other end.

1. Use a multimeter if necessary to identify the paired wires. The two same-end wires measure only the lead resistance between each other; either of those wires to the third wire measures roughly the PT100 resistance plus lead resistance.
2. Connect the two same-end wires to **F+ (FORCE+)** and **RTD+ (RTDIN+)**.
3. Connect the third wire to the negative side, **F- / RTD-** according to the breakout's 3-wire terminal arrangement.
4. Configure the breakout itself for **3-wire** operation. On an Adafruit MAX31865 breakout, close the 2/3-wire jumper, cut the small trace between the two-way jumper above Rref, and close the side marked 3. Follow the silkscreen/manual for other breakout brands.

## Driver assumptions

- Sensor: 3-wire PT100, IEC 60751 alpha = 0.00385
- PT100 nominal resistance: 100 ohms at 0 C
- MAX31865 reference resistor: 430 ohms
- Mains rejection: 60 Hz
- Reported values: temperature in C, RTD resistance in ohms, MAX31865 fault byte

If your MAX31865 board uses a reference resistor other than 430 ohms, change `MAX31865_RREF_OHMS` in `max31865.cpp` to the actual installed reference resistor value.
