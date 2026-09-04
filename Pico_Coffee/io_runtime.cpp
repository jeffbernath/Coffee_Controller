#include "io_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstring>

#include "hardware/clocks.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

#include "communication.hpp"
#include "io_manifest.h"
#include "load_cells.hpp"
#include "max31865.hpp"

namespace {

struct RuntimeValue {
    float value = 0.0f;
    bool valid = false;
    uint32_t last_sample_ms = 0;
    uint32_t last_report_ms = 0;
};

RuntimeValue g_values[IO_DEFINITION_COUNT];
bool g_first_poll[IO_DEFINITION_COUNT];

bool g_led_enabled = false;
float g_led_blink_hz = 1.0f;
float g_led_pwm_percent = 100.0f;
bool g_led_phase_on = true;
uint32_t g_led_last_toggle_ms = 0;
uint g_led_pwm_slice = 0;
uint16_t g_led_pwm_wrap = 999;
int g_led_pin = -1;

constexpr uint32_t MAX31865_POLL_MS = 250;
Max31865Reading g_max31865_reading{};
bool g_max31865_responding = false;
uint32_t g_max31865_last_sample_ms = 0;

const IoDefinition *find_definition(const char *name, std::size_t *index_out = nullptr)
{
    for (std::size_t i = 0; i < IO_DEFINITION_COUNT; ++i) {
        if (std::strcmp(IO_DEFINITIONS[i].name, name) == 0) {
            if (index_out) {
                *index_out = i;
            }
            return &IO_DEFINITIONS[i];
        }
    }
    return nullptr;
}

const IoDefinition *find_handler(const char *handler, std::size_t *index_out = nullptr)
{
    for (std::size_t i = 0; i < IO_DEFINITION_COUNT; ++i) {
        if (std::strcmp(IO_DEFINITIONS[i].handler, handler) == 0) {
            if (index_out) {
                *index_out = i;
            }
            return &IO_DEFINITIONS[i];
        }
    }
    return nullptr;
}

void copy_error(char *error, std::size_t error_size, const char *message)
{
    if (error && error_size > 0) {
        std::snprintf(error, error_size, "%s", message);
    }
}

bool parse_bool(const char *text, bool &value)
{
    if (!text) {
        return false;
    }
    if (std::strcmp(text, "1") == 0 || std::strcmp(text, "true") == 0 ||
        std::strcmp(text, "TRUE") == 0 || std::strcmp(text, "on") == 0 ||
        std::strcmp(text, "ON") == 0) {
        value = true;
        return true;
    }
    if (std::strcmp(text, "0") == 0 || std::strcmp(text, "false") == 0 ||
        std::strcmp(text, "FALSE") == 0 || std::strcmp(text, "off") == 0 ||
        std::strcmp(text, "OFF") == 0) {
        value = false;
        return true;
    }
    return false;
}

bool parse_float(const char *text, float &value)
{
    if (!text || *text == '\0') {
        return false;
    }
    char *end = nullptr;
    value = std::strtof(text, &end);
    return end && *end == '\0' && std::isfinite(value);
}

void configure_gpio_output(const IoDefinition &definition, float logical_value)
{
    gpio_init(definition.pin);
    gpio_set_dir(definition.pin, GPIO_OUT);
    const bool logical_on = logical_value >= 0.5f;
    const bool physical_high = definition.active_high ? logical_on : !logical_on;
    gpio_put(definition.pin, physical_high);
}

void apply_gpio_output(const IoDefinition &definition, float logical_value)
{
    const bool logical_on = logical_value >= 0.5f;
    const bool physical_high = definition.active_high ? logical_on : !logical_on;
    gpio_put(definition.pin, physical_high);
}

void configure_led_pwm(const IoDefinition &definition)
{
    g_led_pin = definition.pin;
    gpio_set_function(g_led_pin, GPIO_FUNC_PWM);
    g_led_pwm_slice = pwm_gpio_to_slice_num(g_led_pin);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, g_led_pwm_wrap);

    const uint32_t requested_hz = definition.pwm_frequency_hz > 0 ? definition.pwm_frequency_hz : 1000u;
    const float divider = static_cast<float>(clock_get_hz(clk_sys)) /
                          (static_cast<float>(requested_hz) * static_cast<float>(g_led_pwm_wrap + 1u));
    pwm_config_set_clkdiv(&config, std::clamp(divider, 1.0f, 255.0f));
    pwm_init(g_led_pwm_slice, &config, true);
    pwm_set_gpio_level(g_led_pin, 0);
}

void apply_led_output()
{
    if (g_led_pin < 0) {
        return;
    }

    const bool visible_on = g_led_enabled && (g_led_blink_hz <= 0.0f || g_led_phase_on);
    const float percent = visible_on ? std::clamp(g_led_pwm_percent, 0.0f, 100.0f) : 0.0f;
    const uint16_t level = static_cast<uint16_t>(std::lround((percent / 100.0f) * g_led_pwm_wrap));
    pwm_set_gpio_level(g_led_pin, level);
}

void poll_digital_input(const IoDefinition &definition, RuntimeValue &runtime, bool first_poll, uint32_t now_ms)
{
    if (!definition.has_pin) {
        return;
    }
    if (!first_poll && definition.sample_rate_ms > 0 && now_ms - runtime.last_sample_ms < definition.sample_rate_ms) {
        return;
    }
    runtime.last_sample_ms = now_ms;

    const bool raw_high = gpio_get(definition.pin) != 0;
    const bool active = definition.active_high ? raw_high : !raw_high;
    const float value = active ? 1.0f : 0.0f;
    const bool changed = !runtime.valid || runtime.value != value;

    runtime.value = value;
    runtime.valid = true;

    if (changed || first_poll) {
        communication_send_io(definition, value);
        runtime.last_report_ms = now_ms;
    }
}

void poll_load_cell(
    const IoDefinition &definition,
    RuntimeValue &runtime,
    uint32_t now_ms,
    bool scale_available,
    float cell_1_grams,
    float cell_2_grams)
{
    if (definition.sample_rate_ms > 0 &&
        now_ms - runtime.last_sample_ms < definition.sample_rate_ms) {
        return;
    }
    runtime.last_sample_ms = now_ms;

    if (!scale_available ||
        (definition.channel != 1 && definition.channel != 2)) {
        runtime.valid = false;
        return;
    }

    const float grams =
        definition.channel == 1 ? cell_1_grams : cell_2_grams;

    runtime.value = grams;
    runtime.valid = true;

    const uint32_t report_ms =
        definition.report_rate_ms > 0
            ? definition.report_rate_ms
            : definition.sample_rate_ms;

    if (report_ms == 0 ||
        now_ms - runtime.last_report_ms >= report_ms) {
        communication_send_io(definition, grams);
        runtime.last_report_ms = now_ms;
    }
}

void poll_max31865_input(
    const IoDefinition &definition,
    RuntimeValue &runtime,
    uint32_t now_ms,
    bool sensor_responding,
    const Max31865Reading &reading)
{
    if (definition.sample_rate_ms > 0 &&
        now_ms - runtime.last_sample_ms < definition.sample_rate_ms) {
        return;
    }
    runtime.last_sample_ms = now_ms;

    float value = 0.0f;
    bool value_valid = sensor_responding;

    if (std::strcmp(definition.handler, "max31865_temperature") == 0) {
        value = reading.temperature_c;
        value_valid = sensor_responding && reading.valid;
    } else if (std::strcmp(definition.handler, "max31865_resistance") == 0) {
        value = reading.resistance_ohms;
        value_valid = sensor_responding && reading.valid;
    } else if (std::strcmp(definition.handler, "max31865_fault") == 0) {
        // The diagnostic/status channel must ALWAYS be published. If the
        // driver itself cannot get far enough to produce a MAX31865 fault
        // byte, publish software diagnostic 0x03 instead of leaving this I/O
        // unavailable. Bits D1:D0 are unused by the MAX31865 hardware fault
        // register, so 0x03 cannot be confused with a real RTD fault.
        value = static_cast<float>(sensor_responding ? reading.fault : 0x03u);
        value_valid = true;
    } else {
        return;
    }

    if (!value_valid) {
        if (runtime.valid) {
            communication_send_io_unavailable(definition);
        }
        runtime.valid = false;
        return;
    }

    runtime.value = value;
    runtime.valid = true;

    const uint32_t report_ms = definition.report_rate_ms > 0
        ? definition.report_rate_ms
        : definition.sample_rate_ms;

    if (report_ms == 0 || now_ms - runtime.last_report_ms >= report_ms) {
        communication_send_io(definition, value);
        runtime.last_report_ms = now_ms;
    }
}

void poll_led(uint32_t now_ms)
{
    if (!g_led_enabled || g_led_blink_hz <= 0.0f) {
        if (!g_led_phase_on) {
            g_led_phase_on = true;
            apply_led_output();
        }
        return;
    }

    const float half_period_ms_f = 500.0f / g_led_blink_hz;
    const uint32_t half_period_ms = static_cast<uint32_t>(std::max(1.0f, half_period_ms_f));
    if (now_ms - g_led_last_toggle_ms >= half_period_ms) {
        g_led_last_toggle_ms = now_ms;
        g_led_phase_on = !g_led_phase_on;
        apply_led_output();
    }
}

bool apply_special_output(const IoDefinition &definition, float value)
{
    if (std::strcmp(definition.handler, "led_enable") == 0) {
        g_led_enabled = value >= 0.5f;
        g_led_phase_on = true;
        g_led_last_toggle_ms = to_ms_since_boot(get_absolute_time());
        apply_led_output();
        return true;
    }

    if (std::strcmp(definition.handler, "led_blink_rate") == 0) {
        g_led_blink_hz = value;
        g_led_phase_on = true;
        g_led_last_toggle_ms = to_ms_since_boot(get_absolute_time());
        apply_led_output();
        return true;
    }

    if (std::strcmp(definition.handler, "led_pwm") == 0) {
        g_led_pwm_percent = value;
        apply_led_output();
        return true;
    }

    return false;
}

} // namespace

void io_runtime_init()
{
    load_cells_init();
    max31865_init();

    for (std::size_t i = 0; i < IO_DEFINITION_COUNT; ++i) {
        g_first_poll[i] = true;
        const IoDefinition &definition = IO_DEFINITIONS[i];

        if (definition.direction == IoDirection::INPUT && definition.type == IoType::DIGITAL &&
            definition.has_pin && std::strcmp(definition.handler, "gpio") == 0) {
            gpio_init(definition.pin);
            gpio_set_dir(definition.pin, GPIO_IN);
            if (definition.pull == IoPull::UP) {
                gpio_pull_up(definition.pin);
            } else if (definition.pull == IoPull::DOWN) {
                gpio_pull_down(definition.pin);
            } else {
                gpio_disable_pulls(definition.pin);
            }
        }

        if (definition.direction == IoDirection::OUTPUT) {
            g_values[i].value = definition.default_value;
            g_values[i].valid = true;

            if (definition.type == IoType::DIGITAL && definition.has_pin &&
                std::strcmp(definition.handler, "gpio") == 0) {
                configure_gpio_output(definition, definition.default_value);
            }

            if (std::strcmp(definition.handler, "led_pwm") == 0 && definition.has_pin) {
                configure_led_pwm(definition);
            }
        }
    }

    std::size_t index = 0;
    if (const IoDefinition *definition = find_handler("led_enable", &index)) {
        g_led_enabled = definition->default_value >= 0.5f;
        g_values[index].value = definition->default_value;
    }
    if (const IoDefinition *definition = find_handler("led_blink_rate", &index)) {
        g_led_blink_hz = definition->default_value;
        g_values[index].value = definition->default_value;
    }
    if (const IoDefinition *definition = find_handler("led_pwm", &index)) {
        g_led_pwm_percent = definition->default_value;
        g_values[index].value = definition->default_value;
    }

    g_led_phase_on = true;
    g_led_last_toggle_ms = to_ms_since_boot(get_absolute_time());
    apply_led_output();
}

void io_runtime_poll()
{
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    // Read both HX711 channels together so their reported values always come
    // from the same stabilized combined-scale calculation.
    float cell_1_grams = 0.0f;
    float cell_2_grams = 0.0f;
    const bool scale_available =
        load_cells_read_stabilized_pair(cell_1_grams, cell_2_grams);

    // Sample the MAX31865 at 4 Hz, then share the cached reading across all
    // three manifest values. This avoids unnecessary SPI traffic in the 1 ms
    // main loop and keeps the load-cell polling responsive.
    if (g_max31865_last_sample_ms == 0 ||
        now_ms - g_max31865_last_sample_ms >= MAX31865_POLL_MS) {
        g_max31865_last_sample_ms = now_ms;
        g_max31865_responding = max31865_read(g_max31865_reading);
    }

    for (std::size_t i = 0; i < IO_DEFINITION_COUNT; ++i) {
        const IoDefinition &definition = IO_DEFINITIONS[i];
        RuntimeValue &runtime = g_values[i];

        if (definition.direction != IoDirection::INPUT) {
            continue;
        }

        if (definition.type == IoType::DIGITAL && std::strcmp(definition.handler, "gpio") == 0) {
            poll_digital_input(definition, runtime, g_first_poll[i], now_ms);
        } else if (definition.type == IoType::LOAD_CELL &&
                   std::strcmp(definition.handler, "load_cell") == 0) {
            poll_load_cell(
                definition,
                runtime,
                now_ms,
                scale_available,
                cell_1_grams,
                cell_2_grams);
        } else if (definition.type == IoType::ANALOG &&
                   std::strcmp(definition.driver, "max31865") == 0) {
            poll_max31865_input(
                definition,
                runtime,
                now_ms,
                g_max31865_responding,
                g_max31865_reading);
        }

        g_first_poll[i] = false;
    }

    poll_led(now_ms);
}

bool io_runtime_set(const char *name, const char *value_text, char *error, std::size_t error_size)
{
    std::size_t index = 0;
    const IoDefinition *definition = find_definition(name, &index);
    if (!definition) {
        copy_error(error, error_size, "UNKNOWN_IO");
        return false;
    }
    if (definition->direction != IoDirection::OUTPUT) {
        copy_error(error, error_size, "NOT_OUTPUT");
        return false;
    }

    float value = 0.0f;
    if (definition->data_type && std::strcmp(definition->data_type, "bool") == 0) {
        bool bool_value = false;
        if (!parse_bool(value_text, bool_value)) {
            copy_error(error, error_size, "INVALID_BOOL");
            return false;
        }
        value = bool_value ? 1.0f : 0.0f;
    } else if (!parse_float(value_text, value)) {
        copy_error(error, error_size, "INVALID_NUMBER");
        return false;
    }

    if (value < definition->min_value || value > definition->max_value) {
        copy_error(error, error_size, "OUT_OF_RANGE");
        return false;
    }

    bool applied = false;
    if (definition->type == IoType::DIGITAL && definition->has_pin &&
        std::strcmp(definition->handler, "gpio") == 0) {
        apply_gpio_output(*definition, value);
        applied = true;
    } else {
        applied = apply_special_output(*definition, value);
    }

    if (!applied) {
        copy_error(error, error_size, "OUTPUT_HANDLER_NOT_IMPLEMENTED");
        return false;
    }

    g_values[index].value = value;
    g_values[index].valid = true;
    communication_send_io(*definition, value);
    return true;
}

bool io_runtime_tare_all(char *error, std::size_t error_size)
{
    bool found = false;
    bool all_ok = true;

    for (const IoDefinition &definition : IO_DEFINITIONS) {
        if (definition.type != IoType::LOAD_CELL || definition.direction != IoDirection::INPUT) {
            continue;
        }
        found = true;
        if (!load_cell_tare(static_cast<uint8_t>(definition.channel))) {
            all_ok = false;
        }
    }

    if (!found) {
        copy_error(error, error_size, "NO_LOAD_CELLS");
        return false;
    }
    if (!all_ok) {
        copy_error(error, error_size, "LOAD_CELL_DRIVER_UNAVAILABLE");
        return false;
    }
    return true;
}


bool io_runtime_calibrate_scale(
    float known_grams,
    float &counts_per_gram,
    char *error,
    std::size_t error_size)
{
    if (!std::isfinite(known_grams) || known_grams <= 0.0f) {
        copy_error(error, error_size, "INVALID_CALIBRATION_WEIGHT");
        return false;
    }

    if (!load_cells_calibrate_scale(known_grams, counts_per_gram)) {
        copy_error(error, error_size, "CALIBRATION_FAILED_TARE_FIRST");
        return false;
    }

    return true;
}

bool io_runtime_action(
    const char *name,
    char *error,
    std::size_t error_size)
{
    std::size_t index = 0;

    const IoDefinition *definition =
        find_definition(name, &index);

    if (!definition) {
        copy_error(
            error,
            error_size,
            "UNKNOWN_ACTION");
        return false;
    }

    if (definition->type != IoType::ACTION) {
        copy_error(
            error,
            error_size,
            "NOT_ACTION");
        return false;
    }

    if (definition->direction != IoDirection::COMMAND) {
        copy_error(
            error,
            error_size,
            "NOT_COMMAND");
        return false;
    }

    //
    // TARE BOTH
    //
    if (std::strcmp(
            definition->handler,
            "tare_all") == 0) {

        return io_runtime_tare_all(
            error,
            error_size);
    }

    copy_error(
        error,
        error_size,
        "ACTION_HANDLER_NOT_IMPLEMENTED");

    return false;
}

void io_runtime_report_snapshot()
{
    for (std::size_t i = 0; i < IO_DEFINITION_COUNT; ++i) {
        const IoDefinition &definition = IO_DEFINITIONS[i];
        const RuntimeValue &runtime = g_values[i];
        if (runtime.valid) {
            communication_send_io(definition, runtime.value);
        } else if (definition.type == IoType::LOAD_CELL ||
                   std::strcmp(definition.driver, "max31865") == 0) {
            communication_send_io_unavailable(definition);
        }
    }
}
