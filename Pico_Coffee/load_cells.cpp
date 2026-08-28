#include "load_cells.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

namespace {

// -----------------------------------------------------------------------------
// HX711 pin assignment
// -----------------------------------------------------------------------------
constexpr uint HX711_1_DOUT_PIN = 2;
constexpr uint HX711_1_SCK_PIN  = 3;
constexpr uint HX711_2_DOUT_PIN = 4;
constexpr uint HX711_2_SCK_PIN  = 5;

// One calibration factor is used for the complete two-load-cell scale.
// If no valid value exists in flash, the scale requires calibration.
constexpr float HX711_SCALE_DEFAULT_COUNTS_PER_GRAM = 0.0f;

constexpr uint32_t HX711_READY_TIMEOUT_MS = 500;
constexpr unsigned HX711_TARE_SAMPLES = 10;
constexpr unsigned HX711_CALIBRATION_SAMPLES = 10;
constexpr float HX711_FILTER_ALPHA = 0.25f;
constexpr float MIN_VALID_CALIBRATION = 0.0001f;

// -----------------------------------------------------------------------------
// Nonvolatile calibration storage
//
// The final 4 KB sector of the Pico's external flash is reserved for scale
// calibration. Programming a normal UF2 generally leaves this sector alone
// unless the image grows into it or the entire flash is erased.
// -----------------------------------------------------------------------------
constexpr uint32_t CALIBRATION_MAGIC = 0x5343414Cu;   // "SCAL"
constexpr uint32_t CALIBRATION_VERSION = 1u;
constexpr uint32_t CALIBRATION_CHECK_XOR = 0xA5C35A3Cu;
constexpr uint32_t CALIBRATION_FLASH_OFFSET =
    PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;

static_assert((CALIBRATION_FLASH_OFFSET % FLASH_SECTOR_SIZE) == 0,
              "Calibration flash offset must be sector aligned");

struct CalibrationRecord {
    uint32_t magic;
    uint32_t version;
    float counts_per_gram;
    uint32_t checksum;
};

struct FlashProgramParams {
    uint32_t offset;
    const uint8_t *data;
};

struct Hx711State {
    uint dout_pin;
    uint sck_pin;
    bool initialized = false;

    int32_t tare_offset = 0;
    bool tare_valid = false;

    float last_grams = 0.0f;
    bool last_grams_valid = false;

    float filtered_grams = 0.0f;
    bool filter_valid = false;
};

Hx711State g_channels[] = {
    {HX711_1_DOUT_PIN, HX711_1_SCK_PIN},
    {HX711_2_DOUT_PIN, HX711_2_SCK_PIN},
};

float g_scale_counts_per_gram = HX711_SCALE_DEFAULT_COUNTS_PER_GRAM;
bool g_calibration_loaded = false;

uint32_t calibration_checksum(uint32_t magic, uint32_t version, float factor)
{
    uint32_t factor_bits = 0;
    static_assert(sizeof(factor_bits) == sizeof(factor),
                  "Unexpected float size");
    std::memcpy(&factor_bits, &factor, sizeof(factor_bits));

    return magic ^ version ^ factor_bits ^ CALIBRATION_CHECK_XOR;
}

bool calibration_factor_valid(float factor)
{
    return std::isfinite(factor) &&
           std::fabs(factor) >= MIN_VALID_CALIBRATION;
}

bool read_calibration_from_flash(float &factor)
{
    CalibrationRecord record{};

    const auto *flash_record =
        reinterpret_cast<const void *>(XIP_BASE + CALIBRATION_FLASH_OFFSET);
    std::memcpy(&record, flash_record, sizeof(record));

    if (record.magic != CALIBRATION_MAGIC ||
        record.version != CALIBRATION_VERSION) {
        return false;
    }

    const uint32_t expected_checksum =
        calibration_checksum(record.magic, record.version,
                             record.counts_per_gram);

    if (record.checksum != expected_checksum ||
        !calibration_factor_valid(record.counts_per_gram)) {
        return false;
    }

    factor = record.counts_per_gram;
    return true;
}

void erase_calibration_sector(void *param)
{
    const uint32_t offset =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
}

void program_calibration_page(void *param)
{
    const auto *program = static_cast<const FlashProgramParams *>(param);
    flash_range_program(program->offset, program->data, FLASH_PAGE_SIZE);
}

bool save_calibration_to_flash(float factor)
{
    if (!calibration_factor_valid(factor)) {
        return false;
    }

    CalibrationRecord record{};
    record.magic = CALIBRATION_MAGIC;
    record.version = CALIBRATION_VERSION;
    record.counts_per_gram = factor;
    record.checksum =
        calibration_checksum(record.magic, record.version,
                             record.counts_per_gram);

    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    std::memset(page, 0xFF, sizeof(page));
    std::memcpy(page, &record, sizeof(record));

    // flash_safe_execute handles the interrupt/XIP protection required while
    // erasing and programming flash.
    const int erase_result =
        flash_safe_execute(
            erase_calibration_sector,
            reinterpret_cast<void *>(
                static_cast<uintptr_t>(CALIBRATION_FLASH_OFFSET)),
            UINT32_MAX);

    if (erase_result != PICO_OK) {
        return false;
    }

    const FlashProgramParams params{
        CALIBRATION_FLASH_OFFSET,
        page,
    };

    const int program_result =
        flash_safe_execute(program_calibration_page,
                           const_cast<FlashProgramParams *>(&params),
                           UINT32_MAX);

    if (program_result != PICO_OK) {
        return false;
    }

    // Verify the record by reading it back through XIP.
    float stored_factor = 0.0f;
    return read_calibration_from_flash(stored_factor) &&
           std::fabs(stored_factor - factor) <=
               (std::fabs(factor) * 0.000001f + 0.000001f);
}

void load_saved_calibration()
{
    if (g_calibration_loaded) {
        return;
    }

    g_calibration_loaded = true;

    float stored_factor = 0.0f;
    if (read_calibration_from_flash(stored_factor)) {
        g_scale_counts_per_gram = stored_factor;
    } else {
        g_scale_counts_per_gram = HX711_SCALE_DEFAULT_COUNTS_PER_GRAM;
    }
}

Hx711State *get_channel(uint8_t channel)
{
    if (channel < 1 || channel > 2) {
        return nullptr;
    }
    return &g_channels[channel - 1];
}

void init_channel(Hx711State &state)
{
    if (state.initialized) {
        return;
    }

    gpio_init(state.dout_pin);
    gpio_set_dir(state.dout_pin, GPIO_IN);
    gpio_disable_pulls(state.dout_pin);

    gpio_init(state.sck_pin);
    gpio_set_dir(state.sck_pin, GPIO_OUT);
    gpio_put(state.sck_pin, 0);

    state.initialized = true;
}

bool is_ready(const Hx711State &state)
{
    return gpio_get(state.dout_pin) == 0;
}

bool wait_ready(const Hx711State &state, uint32_t timeout_ms)
{
    const absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (!is_ready(state)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            return false;
        }
        sleep_ms(1);
    }

    return true;
}

bool read_raw_ready(Hx711State &state, int32_t &raw)
{
    if (!is_ready(state)) {
        return false;
    }

    uint32_t value = 0;

    // Keep SCK HIGH pulses below the HX711 power-down threshold. Disabling
    // interrupts prevents an interrupt from stretching a HIGH pulse >60 us.
    const uint32_t irq_state = save_and_disable_interrupts();

    for (int bit = 0; bit < 24; ++bit) {
        gpio_put(state.sck_pin, 1);
        busy_wait_us_32(1);

        value = (value << 1) | (gpio_get(state.dout_pin) ? 1u : 0u);

        gpio_put(state.sck_pin, 0);
        busy_wait_us_32(1);
    }

    // 25th pulse selects Channel A, gain 128, for the next conversion.
    gpio_put(state.sck_pin, 1);
    busy_wait_us_32(1);
    gpio_put(state.sck_pin, 0);
    busy_wait_us_32(1);

    restore_interrupts(irq_state);

    if ((value & 0x00800000u) != 0) {
        value |= 0xFF000000u;
    }

    raw = static_cast<int32_t>(value);
    return true;
}

bool read_raw_blocking(Hx711State &state, int32_t &raw)
{
    if (!wait_ready(state, HX711_READY_TIMEOUT_MS)) {
        return false;
    }
    return read_raw_ready(state, raw);
}

bool average_raw(Hx711State &state, unsigned sample_count, int32_t &average)
{
    if (sample_count == 0) {
        return false;
    }

    int64_t sum = 0;

    for (unsigned i = 0; i < sample_count; ++i) {
        int32_t raw = 0;
        if (!read_raw_blocking(state, raw)) {
            return false;
        }
        sum += raw;
    }

    average = static_cast<int32_t>(sum / static_cast<int64_t>(sample_count));
    return true;
}

void reset_filter(Hx711State &state)
{
    state.filter_valid = false;
    state.last_grams_valid = false;
}

void reset_all_filters()
{
    for (Hx711State &state : g_channels) {
        reset_filter(state);
    }
}

} // namespace

void load_cells_init()
{
    load_saved_calibration();

    for (Hx711State &state : g_channels) {
        init_channel(state);
    }
}

bool load_cell_read_grams(uint8_t channel, float &grams)
{
    Hx711State *state = get_channel(channel);
    if (!state) {
        return false;
    }

    init_channel(*state);
    load_saved_calibration();

    // The scale needs a valid saved/runtime calibration and a fresh tare after
    // each power-up before this channel can truthfully report grams.
    if (!calibration_factor_valid(g_scale_counts_per_gram) ||
        !state->tare_valid) {
        return false;
    }

    if (!is_ready(*state)) {
        if (!state->last_grams_valid) {
            return false;
        }
        grams = state->last_grams;
        return true;
    }

    int32_t raw = 0;
    if (!read_raw_ready(*state, raw)) {
        return false;
    }

    const int32_t net_counts = raw - state->tare_offset;

    // This is this load cell's contribution to the total platform weight.
    // The GUI adds the two channel contributions together.
    const float new_grams =
        static_cast<float>(net_counts) / g_scale_counts_per_gram;

    if (!std::isfinite(new_grams)) {
        return false;
    }

    if (!state->filter_valid) {
        state->filtered_grams = new_grams;
        state->filter_valid = true;
    } else {
        state->filtered_grams +=
            HX711_FILTER_ALPHA * (new_grams - state->filtered_grams);
    }

    state->last_grams = state->filtered_grams;
    state->last_grams_valid = true;
    grams = state->last_grams;
    return true;
}

bool load_cell_tare(uint8_t channel)
{
    Hx711State *state = get_channel(channel);
    if (!state) {
        return false;
    }

    init_channel(*state);

    int32_t average = 0;
    if (!average_raw(*state, HX711_TARE_SAMPLES, average)) {
        return false;
    }

    state->tare_offset = average;
    state->tare_valid = true;
    reset_filter(*state);
    return true;
}

bool load_cells_calibrate_scale(float known_grams, float &counts_per_gram)
{
    if (!std::isfinite(known_grams) || known_grams <= 0.0f) {
        return false;
    }

    // Calibration must use the same zero reference for both load cells.
    for (Hx711State &state : g_channels) {
        init_channel(state);
        if (!state.tare_valid) {
            return false;
        }
    }

    int64_t combined_net_counts = 0;

    for (Hx711State &state : g_channels) {
        int32_t average = 0;
        if (!average_raw(state, HX711_CALIBRATION_SAMPLES, average)) {
            return false;
        }
        combined_net_counts +=
            static_cast<int64_t>(average) - state.tare_offset;
    }

    const float factor =
        static_cast<float>(combined_net_counts) / known_grams;

    if (!calibration_factor_valid(factor)) {
        return false;
    }

    // Only report calibration success if it was also persisted and verified.
    if (!save_calibration_to_flash(factor)) {
        return false;
    }

    g_scale_counts_per_gram = factor;
    counts_per_gram = factor;
    reset_all_filters();
    return true;
}
