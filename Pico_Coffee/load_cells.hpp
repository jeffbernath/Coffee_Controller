#pragma once

#include <cstdint>

// Load-cell abstraction used by io_runtime.
//
// Hardware: two HX711 ADCs supporting one physical scale platform.
//   Channel 1: DOUT = GP2, SCK = GP3
//   Channel 2: DOUT = GP4, SCK = GP5
//
// Each channel reports its contribution to the total weight. Both channels use
// one shared scale calibration factor, calculated from the sum of both HX711
// raw readings while a known weight is on the platform.
void load_cells_init();

bool load_cell_read_grams(uint8_t channel, float &grams);
bool load_cell_tare(uint8_t channel);
bool load_cells_calibrate_scale(float known_grams, float &counts_per_gram);
