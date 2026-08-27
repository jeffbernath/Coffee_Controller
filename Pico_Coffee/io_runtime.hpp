#pragma once

#include <cstddef>

void io_runtime_init();
void io_runtime_poll();
void io_runtime_report_snapshot();

bool io_runtime_set(const char *name, const char *value_text, char *error, std::size_t error_size);
bool io_runtime_tare_all(char *error, std::size_t error_size);
