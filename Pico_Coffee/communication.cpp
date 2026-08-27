#include "communication.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "io_runtime.hpp"

namespace {

char g_rx_buffer[160];
std::size_t g_rx_length = 0;
uint32_t g_last_heartbeat_ms = 0;

void send_ack(const char *sequence)
{
    std::printf("ACK,%s\n", sequence ? sequence : "0");
}

void send_error(const char *sequence, const char *target, const char *error)
{
    std::printf("ERR,%s,%s,%s\n",
                sequence ? sequence : "0",
                target ? target : "UNKNOWN",
                error ? error : "UNKNOWN_ERROR");
}

void handle_command(char *line)
{
    char *save = nullptr;
    char *verb = strtok_r(line, ",", &save);

    if (!verb) {
        return;
    }

    // Legacy/manual convenience command retained from the original project.
    if (std::strcmp(verb, "BOOTSEL") == 0) {
        stdio_flush();
        sleep_ms(50);
        reset_usb_boot(0, 0);
        return;
    }

    //
    // SET command
    //
    // Example:
    // SET,15,LED_ENABLE,1
    //
    if (std::strcmp(verb, "SET") == 0) {
        char *sequence = strtok_r(nullptr, ",", &save);
        char *name = strtok_r(nullptr, ",", &save);
        char *value = strtok_r(nullptr, ",", &save);

        if (!sequence || !name || !value) {
            send_error(sequence, name, "BAD_FORMAT");
            return;
        }

        char error[64] = {0};

        if (!io_runtime_set(
                name,
                value,
                error,
                sizeof(error))) {
            send_error(sequence, name, error);
            return;
        }

        send_ack(sequence);
        return;
    }

    //
    // CMD command
    //
    // Examples:
    //
    // CMD,20,SYNC
    // CMD,21,ACTION,TARE_BOTH
    // CMD,22,BOOTSEL
    //
    if (std::strcmp(verb, "CMD") == 0) {
        char *sequence = strtok_r(nullptr, ",", &save);
        char *command = strtok_r(nullptr, ",", &save);
        char *target = strtok_r(nullptr, ",", &save);

        if (!sequence || !command) {
            send_error(sequence, command, "BAD_FORMAT");
            return;
        }

        //
        // Synchronize the complete Pico state with the Pi.
        //
        if (std::strcmp(command, "SYNC") == 0) {
            send_ack(sequence);

            communication_send_hello();
            io_runtime_report_snapshot();

            return;
        }

        //
        // Generic action command.
        //
        // Example:
        //
        // CMD,52,ACTION,TARE_BOTH
        //
        // target = TARE_BOTH
        //
        // io_runtime_action() looks up the action in
        // io_manifest.json and runs the associated handler.
        //
        if (std::strcmp(command, "ACTION") == 0) {
            if (!target) {
                send_error(
                    sequence,
                    "ACTION",
                    "BAD_FORMAT");
                return;
            }

            char error[64] = {0};

            if (!io_runtime_action(
                    target,
                    error,
                    sizeof(error))) {

                send_error(
                    sequence,
                    target,
                    error);

                return;
            }

            send_ack(sequence);
            return;
        }

        //
        // Legacy TARE command.
        //
        // This can be removed later after the GUI has been
        // converted completely to:
        //
        // CMD,<sequence>,ACTION,TARE_BOTH
        //
        if (std::strcmp(command, "TARE") == 0) {
            char error[64] = {0};

            if (!io_runtime_tare_all(
                    error,
                    sizeof(error))) {

                send_error(
                    sequence,
                    target ? target : "TARE",
                    error);

                return;
            }

            send_ack(sequence);
            return;
        }

        //
        // Reboot the Pico into BOOTSEL mode.
        //
        if (std::strcmp(command, "BOOTSEL") == 0) {
            send_ack(sequence);

            stdio_flush();
            sleep_ms(50);

            reset_usb_boot(0, 0);
            return;
        }

        send_error(
            sequence,
            command,
            "UNKNOWN_COMMAND");

        return;
    }
}

} // namespace


void communication_init()
{
    g_last_heartbeat_ms =
        to_ms_since_boot(get_absolute_time());
}


void communication_poll()
{
    int ch = 0;

    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {

            if (g_rx_length > 0) {

                g_rx_buffer[g_rx_length] = '\0';

                handle_command(g_rx_buffer);

                g_rx_length = 0;
            }

            continue;
        }

        if (g_rx_length < sizeof(g_rx_buffer) - 1) {

            g_rx_buffer[g_rx_length++] =
                static_cast<char>(ch);

        } else {

            // Overflow protection.
            // Throw away the malformed command.
            g_rx_length = 0;
        }
    }

    const uint32_t now_ms =
        to_ms_since_boot(get_absolute_time());

    if (now_ms - g_last_heartbeat_ms >=
        IO_HEARTBEAT_MS) {

        g_last_heartbeat_ms = now_ms;

        communication_send_heartbeat();
    }
}


void communication_send_heartbeat()
{
    std::printf(
        "HB,%lu\n",
        static_cast<unsigned long>(
            to_ms_since_boot(
                get_absolute_time())));
}


void communication_send_hello()
{
    std::printf(
        "HELLO,%lu,0.2.0\n",
        static_cast<unsigned long>(
            IO_PROTOCOL_VERSION));
}


void communication_send_io(
    const IoDefinition &definition,
    float value)
{
    if (definition.data_type &&
        std::strcmp(
            definition.data_type,
            "bool") == 0) {

        std::printf(
            "IO,%s,%d\n",
            definition.name,
            value >= 0.5f ? 1 : 0);

    } else {

        std::printf(
            "IO,%s,%.3f\n",
            definition.name,
            static_cast<double>(value));
    }
}


void communication_send_io_unavailable(
    const IoDefinition &definition)
{
    std::printf(
        "IO_STATUS,%s,UNAVAILABLE\n",
        definition.name);
}