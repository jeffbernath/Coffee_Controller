# HX711 Setup for Pico Coffee Controller

## Pico pin assignment

| Load cell | HX711 signal | Pico GPIO | Pico physical pin |
|---|---|---:|---:|
| 1 | DOUT / DT | GP2 | 4 |
| 1 | SCK / CLK | GP3 | 5 |
| 2 | DOUT / DT | GP4 | 6 |
| 2 | SCK / CLK | GP5 | 7 |

Connect HX711 GND to Pico GND. Power the HX711 at a Pico-safe logic voltage; 3.3 V is the simplest choice for a standard HX711 breakout used directly with Pico GPIO.

## Load-cell wiring to each HX711

Typical four-wire load cell wiring uses:

- E+ = excitation positive
- E- = excitation negative
- A+ = signal positive
- A- = signal negative

Wire colors are not standardized, so verify the load-cell datasheet before connecting.

## Tare

The existing action tares both load cells:

    CMD,20,ACTION,TARE_BOTH

The legacy command also still works:

    CMD,20,TARE

Tare averages 10 HX711 conversions for each load cell.

## Calibration

The firmware will not label raw ADC counts as grams. After a fresh build, each channel must be calibrated unless a compile-time calibration factor has been entered in `load_cells.cpp`.

1. Make sure both scales are empty.
2. Tare both scales.
3. Put a known calibration weight on load cell 1.
4. Send a calibration command using the weight in grams.
5. Repeat for load cell 2.

Example using a 500 g calibration weight:

    CMD,30,ACTION,TARE_BOTH
    CMD,31,CALIBRATE,1,500.0
    CMD,32,CALIBRATE,2,500.0

A successful calibration returns a line like:

    CAL,31,1,725.123456
    ACK,31

The last number is the measured counts-per-gram factor.

## Making calibration survive power cycles

Runtime tare and runtime calibration are currently stored in RAM. The tare should normally be performed whenever needed. If you want a calibration factor to be available immediately after every reboot, copy the measured factors into these constants in `load_cells.cpp`:

    HX711_1_DEFAULT_COUNTS_PER_GRAM
    HX711_2_DEFAULT_COUNTS_PER_GRAM

A future version can store calibration factors in Pico flash or have the Raspberry Pi send them automatically during startup.

## Data reporting

After calibration, the existing runtime reports:

    IO,LOAD_CELL_1_G,<grams>
    IO,LOAD_CELL_2_G,<grams>

The existing manifest report interval is 100 ms. The HX711 driver reads new conversions whenever they are ready and applies a small exponential filter before reporting weight.
