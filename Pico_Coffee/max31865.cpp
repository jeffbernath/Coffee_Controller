#include "max31865.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>

#include "hardware/spi.h"
#include "pico/stdlib.h"

namespace {

spi_inst_t *const MAX31865_SPI = spi0;
constexpr uint MAX31865_MISO_PIN = 16;
constexpr uint MAX31865_CS_PIN = 17;
constexpr uint MAX31865_SCK_PIN = 18;
constexpr uint MAX31865_MOSI_PIN = 19;
constexpr uint32_t MAX31865_SPI_HZ = 500000;

constexpr float PT100_R0_OHMS = 100.0f;
constexpr float MAX31865_RREF_OHMS = 430.0f;

// IEC 60751 Callendar-Van Dusen coefficients for a standard alpha=0.00385 PT100.
constexpr float CVD_A = 3.9083e-3f;
constexpr float CVD_B = -5.775e-7f;
constexpr float CVD_C = -4.183e-12f;

constexpr uint8_t REG_CONFIG = 0x00;
constexpr uint8_t REG_RTD_MSB = 0x01;
constexpr uint8_t REG_HIGH_FAULT_MSB = 0x03;
constexpr uint8_t REG_LOW_FAULT_MSB = 0x05;
constexpr uint8_t REG_FAULT_STATUS = 0x07;

constexpr uint8_t CONFIG_BIAS = 0x80;
constexpr uint8_t CONFIG_AUTO = 0x40;
constexpr uint8_t CONFIG_3WIRE = 0x10;
constexpr uint8_t CONFIG_FAULT_AUTO = 0x04;
constexpr uint8_t CONFIG_CLEAR_FAULT = 0x02;
constexpr uint8_t CONFIG_FILTER_60HZ = 0x00;

constexpr uint8_t CONFIG_CONTINUOUS_3WIRE_60HZ =
    CONFIG_BIAS | CONFIG_AUTO | CONFIG_3WIRE | CONFIG_FILTER_60HZ;

bool g_initialized = false;
uint32_t g_last_fault_cycle_ms = 0;
constexpr uint32_t FAULT_CYCLE_INTERVAL_MS = 5000;

void select_chip()
{
    gpio_put(MAX31865_CS_PIN, 0);
    busy_wait_us_32(1);
}

void deselect_chip()
{
    busy_wait_us_32(1);
    gpio_put(MAX31865_CS_PIN, 1);
    busy_wait_us_32(1);
}

void write_register(uint8_t address, uint8_t value)
{
    const uint8_t tx[2] = {
        static_cast<uint8_t>(address | 0x80u),
        value,
    };

    select_chip();
    spi_write_blocking(MAX31865_SPI, tx, 2);
    deselect_chip();
}

uint8_t read_register(uint8_t address)
{
    uint8_t value = 0;
    const uint8_t command = static_cast<uint8_t>(address & 0x7Fu);

    select_chip();
    spi_write_blocking(MAX31865_SPI, &command, 1);
    spi_read_blocking(MAX31865_SPI, 0xFF, &value, 1);
    deselect_chip();

    return value;
}

void read_registers(uint8_t start_address, uint8_t *buffer, std::size_t length)
{
    const uint8_t command = static_cast<uint8_t>(start_address & 0x7Fu);

    select_chip();
    spi_write_blocking(MAX31865_SPI, &command, 1);
    spi_read_blocking(MAX31865_SPI, 0xFF, buffer, length);
    deselect_chip();
}

uint16_t resistance_to_threshold_register(float resistance_ohms)
{
    const float ratio = std::clamp(resistance_ohms / MAX31865_RREF_OHMS, 0.0f, 0.999969f);
    const uint16_t raw15 = static_cast<uint16_t>(std::lround(ratio * 32768.0f));
    return static_cast<uint16_t>(raw15 << 1);
}

void write_threshold(uint8_t msb_register, float resistance_ohms)
{
    const uint16_t threshold = resistance_to_threshold_register(resistance_ohms);
    write_register(msb_register, static_cast<uint8_t>(threshold >> 8));
    write_register(static_cast<uint8_t>(msb_register + 1u), static_cast<uint8_t>(threshold & 0xFFu));
}

float pt100_temperature_from_resistance(float resistance_ohms)
{
    // Above 0 C, the C term is zero and the equation can be solved directly.
    if (resistance_ohms >= PT100_R0_OHMS) {
        const float ratio = resistance_ohms / PT100_R0_OHMS;
        const float discriminant = CVD_A * CVD_A - 4.0f * CVD_B * (1.0f - ratio);
        if (discriminant < 0.0f) {
            return NAN;
        }
        return (-CVD_A + std::sqrt(discriminant)) / (2.0f * CVD_B);
    }

    // Below 0 C, solve the complete Callendar-Van Dusen equation iteratively.
    float temperature_c = (resistance_ohms / PT100_R0_OHMS - 1.0f) / CVD_A;
    temperature_c = std::clamp(temperature_c, -200.0f, 0.0f);

    for (int i = 0; i < 8; ++i) {
        const float t = temperature_c;
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float modeled = PT100_R0_OHMS *
            (1.0f + CVD_A * t + CVD_B * t2 + CVD_C * (t - 100.0f) * t3);
        const float derivative = PT100_R0_OHMS *
            (CVD_A + 2.0f * CVD_B * t + CVD_C * (4.0f * t3 - 300.0f * t2));

        if (std::fabs(derivative) < 1e-9f) {
            break;
        }
        temperature_c -= (modeled - resistance_ohms) / derivative;
    }

    return temperature_c;
}

} // namespace

void max31865_init()
{
    spi_init(MAX31865_SPI, MAX31865_SPI_HZ);
    spi_set_format(MAX31865_SPI, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    gpio_set_function(MAX31865_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(MAX31865_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(MAX31865_MOSI_PIN, GPIO_FUNC_SPI);

    gpio_init(MAX31865_CS_PIN);
    gpio_set_dir(MAX31865_CS_PIN, GPIO_OUT);
    gpio_put(MAX31865_CS_PIN, 1);

    sleep_ms(5);

    // Configure broad PT100 physical-range thresholds so open/short conditions
    // produce a useful fault indication without limiting normal boiler readings.
    write_threshold(REG_LOW_FAULT_MSB, 15.0f);
    write_threshold(REG_HIGH_FAULT_MSB, 410.0f);

    write_register(REG_CONFIG, CONFIG_CONTINUOUS_3WIRE_60HZ | CONFIG_CLEAR_FAULT);

    // VBIAS startup can require up to 10 ms; the first continuous conversion
    // then takes approximately one single-conversion period.
    sleep_ms(70);

    const uint8_t config = read_register(REG_CONFIG);
    g_initialized = (config & (CONFIG_BIAS | CONFIG_AUTO | CONFIG_3WIRE | 0x01u)) ==
                    CONFIG_CONTINUOUS_3WIRE_60HZ;
}

bool max31865_read(Max31865Reading &reading)
{
    reading = {};

    if (!g_initialized) {
        return false;
    }

    uint8_t rtd_bytes[2] = {0, 0};
    read_registers(REG_RTD_MSB, rtd_bytes, 2);

    const uint16_t register_value =
        static_cast<uint16_t>((static_cast<uint16_t>(rtd_bytes[0]) << 8) | rtd_bytes[1]);
    const uint16_t raw_rtd = static_cast<uint16_t>(register_value >> 1);
    const bool rtd_fault_flag = (register_value & 0x0001u) != 0;

    uint8_t fault = read_register(REG_FAULT_STATUS);

    // Periodically run the MAX31865's full cable fault-detection cycle. The
    // current conversion is captured first, then continuous conversion is
    // restarted. By the next 250 ms application sample, a fresh conversion is
    // ready.
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (g_last_fault_cycle_ms == 0 ||
        now_ms - g_last_fault_cycle_ms >= FAULT_CYCLE_INTERVAL_MS) {
        g_last_fault_cycle_ms = now_ms;
        write_register(REG_CONFIG, CONFIG_BIAS | CONFIG_3WIRE | CONFIG_CLEAR_FAULT);
        write_register(REG_CONFIG, CONFIG_BIAS | CONFIG_3WIRE | CONFIG_FAULT_AUTO);
        sleep_us(1000);  // automatic cycle is <= 600 us per the data sheet
        fault = static_cast<uint8_t>(fault | read_register(REG_FAULT_STATUS));
        write_register(REG_CONFIG, CONFIG_CONTINUOUS_3WIRE_60HZ);
    }

    reading.fault = fault;
    reading.resistance_ohms =
        (static_cast<float>(raw_rtd) * MAX31865_RREF_OHMS) / 32768.0f;
    reading.temperature_c = pt100_temperature_from_resistance(reading.resistance_ohms);

    const bool plausible_register = raw_rtd > 0u && raw_rtd < 32767u;
    const bool plausible_temperature = std::isfinite(reading.temperature_c) &&
                                       reading.temperature_c >= -200.0f &&
                                       reading.temperature_c <= 850.0f;

    reading.valid = !rtd_fault_flag && fault == 0u && plausible_register && plausible_temperature;
    return true;
}
