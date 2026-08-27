#pragma once

#include "io_manifest.h"

void communication_init();
void communication_poll();
void communication_send_heartbeat();
void communication_send_hello();
void communication_send_io(const IoDefinition &definition, float value);
void communication_send_io_unavailable(const IoDefinition &definition);
