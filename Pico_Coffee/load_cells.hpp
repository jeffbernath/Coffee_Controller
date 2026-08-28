#pragma once

#include <cstdint>

// Load-cell abstraction used by io_runtime.
//
// Current hardware implementation: two HX711 ADCs.
//   Channel 1: DOUT = GP2, SCK = GP3
//   Channel 2: DOUT = GP4, SCK = GP5
//
// Tare is held in RAM. Calibration can be supplied below at compile time or
// calculated at runtime with load_cell_calibrate(). Runtime calibration is
// also held in RAM and therefore must be repeated after a power cycle unless
// the compile-time counts/gram value is filled in.
void load_cells_init();

bool load_cell_read_grams(uint8_t channel, float &grams);
bool load_cell_tare(uint8_t channel);
bool load_cell_calibrate(uint8_t channel, float known_grams, float &counts_per_gram);
