#pragma once

#include <cstdint>

struct Max31865Reading {
    float temperature_c = 0.0f;
    float resistance_ohms = 0.0f;
    uint8_t fault = 0;
    bool valid = false;
};

// MAX31865 connected to SPI0:
//   GP16 = MISO / MAX31865 SDO
//   GP17 = CS
//   GP18 = SCK
//   GP19 = MOSI / MAX31865 SDI
// The driver is configured for a 3-wire PT100 and a 430-ohm reference resistor.
void max31865_init();
bool max31865_read(Max31865Reading &reading);
