#pragma once

// Local scale display + tare button support.
//
// GC9A01 (240x240) uses SPI1 and is intentionally independent of the existing
// MAX31865 on SPI0. The display and button are local to the Pico; they do not
// add or change any GUI/serial protocol I/O.
void scale_display_init();

// Update the local display with the stabilized total scale weight. When the
// scale is not yet available (for example before the first tare), 0.0 is shown.
void scale_display_update(bool scale_available, float total_grams);

// Returns true once for each debounced press of the physical tare button.
bool scale_tare_button_pressed();
