#pragma once

#include <cstddef>
#include <cstdint>

void io_runtime_init();
void io_runtime_poll();
void io_runtime_report_snapshot();

bool io_runtime_set(const char *name, const char *value_text, char *error, std::size_t error_size);
bool io_runtime_tare_all(char *error, std::size_t error_size);
bool io_runtime_calibrate_load_cell(uint8_t channel, float known_grams, float &counts_per_gram, char *error, std::size_t error_size);

bool io_runtime_action(const char *name,char *error,std::size_t error_size);