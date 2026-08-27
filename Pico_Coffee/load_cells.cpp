#include "load_cells.hpp"

bool __attribute__((weak)) coffee_load_cell_hw_read_grams(uint8_t channel, float &grams)
{
    (void)channel;
    (void)grams;
    return false;
}

bool __attribute__((weak)) coffee_load_cell_hw_tare(uint8_t channel)
{
    (void)channel;
    return false;
}

bool load_cell_read_grams(uint8_t channel, float &grams)
{
    return coffee_load_cell_hw_read_grams(channel, grams);
}

bool load_cell_tare(uint8_t channel)
{
    return coffee_load_cell_hw_tare(channel);
}
