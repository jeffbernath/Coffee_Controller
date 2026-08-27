#include "pico/stdlib.h"

#include "communication.hpp"
#include "io_runtime.hpp"

int main()
{
    stdio_init_all();

    io_runtime_init();
    communication_init();

    while (true) {
        communication_poll();
        io_runtime_poll();
        sleep_ms(1);
    }
}
