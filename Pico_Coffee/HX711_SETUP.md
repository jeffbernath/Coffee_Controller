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

Wire colors are not standardized, so verify the load-cell datasheet before connecting. Both load-cell signals should change in the same direction when weight is added to the platform.

## Tare

The existing action tares both load cells:

    CMD,20,ACTION,TARE_BOTH

The legacy command also still works:

    CMD,20,TARE

Tare averages 10 HX711 conversions for each load cell.

## Scale calibration

The two HX711/load-cell channels are treated as one physical scale. The firmware uses one shared counts-per-gram factor calculated from the combined raw change of both load cells.

1. Make sure the platform is empty.
2. Tare the scale.
3. Put one known calibration weight on the platform.
4. Send one scale calibration command using the known weight in grams.

Example using a 500 g calibration weight:

    CMD,30,ACTION,TARE_BOTH
    CMD,31,CALIBRATE_SCALE,500.0

A successful calibration returns:

    CAL,31,SCALE,725.123456
    ACK,31

The last number is the combined scale counts-per-gram factor.

After calibration, each load-cell channel reports its contribution to the total platform weight. The GUI adds both contributions to display the total weight.

## Power cycles

The runtime scale calibration factor is currently stored in Pico RAM. It must be calibrated again after Pico power is removed or reset. Tare can be performed at any time without losing the current calibration factor.

If you later want a fixed compile-time calibration factor, enter the measured value in `HX711_SCALE_DEFAULT_COUNTS_PER_GRAM` in `load_cells.cpp`. A future version can store the factor in Pico flash automatically.

## Data reporting

After calibration, the existing runtime reports:

    IO,LOAD_CELL_1_G,<contribution_grams>
    IO,LOAD_CELL_2_G,<contribution_grams>

The GUI displays:

    total_weight = LOAD_CELL_1_G + LOAD_CELL_2_G

The existing manifest report interval is 100 ms. The HX711 driver reads new conversions whenever they are ready and applies a small exponential filter before reporting weight.
