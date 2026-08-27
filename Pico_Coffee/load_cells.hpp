#pragma once

#include <cstdint>

// Communication-facing load-cell API. The communication stack does not need
// to know whether the final hardware is HX711, NAU7802, or another ADC.
bool load_cell_read_grams(uint8_t channel, float &grams);
bool load_cell_tare(uint8_t channel);

// Hardware hooks. Add a strong implementation in the final ADC driver.
// The weak defaults in load_cells.cpp return false so an unconfigured load
// cell is never presented to the GUI as a real zero reading.
bool coffee_load_cell_hw_read_grams(uint8_t channel, float &grams);
bool coffee_load_cell_hw_tare(uint8_t channel);
