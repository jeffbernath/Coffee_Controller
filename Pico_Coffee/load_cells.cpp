#include "load_cells.hpp"

#include <algorithm>
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
constexpr float MIN_VALID_CALIBRATION = 0.0001f;

// Combined-scale stabilization.
//
// The two load cells are intentionally treated as one physical scale. Small
// load transfers between the cells therefore cancel before filtering.
//
// Stable changes use a slow filter. Larger changes use a faster filter so the
// scale still responds naturally while coffee is being added.
constexpr unsigned COMBINED_MEDIAN_WINDOW = 3;
constexpr float COMBINED_FILTER_ALPHA_STABLE = 0.10f;
constexpr float COMBINED_FILTER_ALPHA_MOVING = 0.60f;
constexpr float DISPLAY_RESOLUTION_G = 0.1f;
constexpr float DISPLAY_DEADBAND_G = 0.3f;
constexpr float ZERO_HOLD_RELEASE_G = 0.35f;
constexpr unsigned STABLE_CHANGE_REQUIRED_SAMPLES = 3;
constexpr float FAST_CHANGE_G = 1.0f;

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

    int32_t latest_net_counts = 0;
    bool sample_valid = false;
    uint32_t sample_counter = 0;
};

Hx711State g_channels[] = {
    {HX711_1_DOUT_PIN, HX711_1_SCK_PIN},
    {HX711_2_DOUT_PIN, HX711_2_SCK_PIN},
};

float g_scale_counts_per_gram = HX711_SCALE_DEFAULT_COUNTS_PER_GRAM;
bool g_calibration_loaded = false;

// State for the complete two-load-cell scale.
float g_combined_history[COMBINED_MEDIAN_WINDOW] = {};
unsigned g_combined_history_count = 0;
unsigned g_combined_history_index = 0;

float g_combined_filtered_grams = 0.0f;
bool g_combined_filter_valid = false;

float g_stabilized_total_grams = 0.0f;
bool g_stabilized_total_valid = false;
unsigned g_stable_change_sample_count = 0;

float g_reported_cell_grams[2] = {};
bool g_reported_pair_valid = false;

uint32_t g_consumed_sample_counter[2] = {};

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

void reset_combined_filter()
{
    g_combined_history_count = 0;
    g_combined_history_index = 0;
    g_combined_filtered_grams = 0.0f;
    g_combined_filter_valid = false;
    g_stabilized_total_grams = 0.0f;
    g_stabilized_total_valid = false;
    g_stable_change_sample_count = 0;
    g_reported_cell_grams[0] = 0.0f;
    g_reported_cell_grams[1] = 0.0f;
    g_reported_pair_valid = false;

    for (unsigned i = 0; i < 2; ++i) {
        g_channels[i].sample_valid = false;
        g_consumed_sample_counter[i] = g_channels[i].sample_counter;
    }
}

float median_combined_grams(float value)
{
    g_combined_history[g_combined_history_index] = value;
    g_combined_history_index =
        (g_combined_history_index + 1u) % COMBINED_MEDIAN_WINDOW;

    if (g_combined_history_count < COMBINED_MEDIAN_WINDOW) {
        ++g_combined_history_count;
    }

    float sorted[COMBINED_MEDIAN_WINDOW] = {};
    for (unsigned i = 0; i < g_combined_history_count; ++i) {
        sorted[i] = g_combined_history[i];
    }

    std::sort(sorted, sorted + g_combined_history_count);

    const unsigned middle = g_combined_history_count / 2u;
    if ((g_combined_history_count & 1u) != 0u) {
        return sorted[middle];
    }

    return 0.5f * (sorted[middle - 1u] + sorted[middle]);
}

float quantize_weight(float grams)
{
    return std::round(grams / DISPLAY_RESOLUTION_G) * DISPLAY_RESOLUTION_G;
}

bool capture_ready_sample(Hx711State &state)
{
    if (!state.tare_valid || !is_ready(state)) {
        return false;
    }

    int32_t raw = 0;
    if (!read_raw_ready(state, raw)) {
        return false;
    }

    state.latest_net_counts = raw - state.tare_offset;
    state.sample_valid = true;
    ++state.sample_counter;
    return true;
}

bool update_stabilized_pair()
{
    load_saved_calibration();

    if (!calibration_factor_valid(g_scale_counts_per_gram)) {
        return false;
    }

    for (Hx711State &state : g_channels) {
        init_channel(state);
        if (!state.tare_valid) {
            return false;
        }
        capture_ready_sample(state);
    }

    if (!g_channels[0].sample_valid || !g_channels[1].sample_valid) {
        return g_reported_pair_valid;
    }

    // Only build a new combined sample when both HX711 channels have supplied
    // fresh data. This keeps the two reported contributions synchronized.
    const bool both_fresh =
        g_channels[0].sample_counter != g_consumed_sample_counter[0] &&
        g_channels[1].sample_counter != g_consumed_sample_counter[1];

    if (!both_fresh) {
        return g_reported_pair_valid;
    }

    g_consumed_sample_counter[0] = g_channels[0].sample_counter;
    g_consumed_sample_counter[1] = g_channels[1].sample_counter;

    const float cell_1_grams =
        static_cast<float>(g_channels[0].latest_net_counts) /
        g_scale_counts_per_gram;
    const float cell_2_grams =
        static_cast<float>(g_channels[1].latest_net_counts) /
        g_scale_counts_per_gram;

    const float raw_total_grams = cell_1_grams + cell_2_grams;
    if (!std::isfinite(raw_total_grams)) {
        return false;
    }

    const float median_total = median_combined_grams(raw_total_grams);

    if (!g_combined_filter_valid) {
        g_combined_filtered_grams = median_total;
        g_combined_filter_valid = true;
    } else {
        const float movement =
            std::fabs(median_total - g_combined_filtered_grams);
        const float alpha =
            movement >= FAST_CHANGE_G
                ? COMBINED_FILTER_ALPHA_MOVING
                : COMBINED_FILTER_ALPHA_STABLE;

        g_combined_filtered_grams +=
            alpha * (median_total - g_combined_filtered_grams);
    }

    bool stabilized_changed = false;

    if (!g_stabilized_total_valid) {
        float initial = quantize_weight(g_combined_filtered_grams);
        if (std::fabs(initial) < ZERO_HOLD_RELEASE_G) {
            initial = 0.0f;
        }
        g_stabilized_total_grams = initial;
        g_stabilized_total_valid = true;
        stabilized_changed = true;
    } else {
        const float filtered_delta =
            std::fabs(g_combined_filtered_grams -
                      g_stabilized_total_grams);
        const float immediate_delta =
            std::fabs(median_total - g_stabilized_total_grams);

        float next_stabilized = g_stabilized_total_grams;

        // Hold zero more aggressively. The display stays at 0.0 g until the
        // filtered weight has clearly moved outside the zero-release window.
        if (g_stabilized_total_grams == 0.0f &&
            std::fabs(g_combined_filtered_grams) < ZERO_HOLD_RELEASE_G) {
            g_stable_change_sample_count = 0;
        } else if (immediate_delta >= FAST_CHANGE_G) {
            // Large real changes should still feel responsive while pouring.
            next_stabilized = quantize_weight(g_combined_filtered_grams);
            g_stable_change_sample_count = 0;
        } else if (filtered_delta >= DISPLAY_DEADBAND_G) {
            // A small change must remain outside the deadband for several
            // consecutive synchronized samples before the displayed value is
            // allowed to move. This rejects one- or two-sample wander.
            ++g_stable_change_sample_count;
            if (g_stable_change_sample_count >=
                STABLE_CHANGE_REQUIRED_SAMPLES) {
                next_stabilized = quantize_weight(g_combined_filtered_grams);
                g_stable_change_sample_count = 0;
            }
        } else {
            g_stable_change_sample_count = 0;
        }

        if (std::fabs(next_stabilized) < ZERO_HOLD_RELEASE_G) {
            next_stabilized = 0.0f;
        }

        if (std::fabs(next_stabilized - g_stabilized_total_grams) >=
            (DISPLAY_RESOLUTION_G * 0.5f)) {
            g_stabilized_total_grams = next_stabilized;
            stabilized_changed = true;
        }
    }

    // Freeze the reported pair while the stabilized total is unchanged. This
    // is important because Cell 1 and Cell 2 are sent as separate serial
    // messages. If their individual values drift equal-and-opposite while the
    // total is stable, updating them independently makes the GUI briefly show
    // a false 0.1 g step between the two messages.
    if (!g_reported_pair_valid || stabilized_changed) {
        const float correction =
            g_stabilized_total_grams - raw_total_grams;

        g_reported_cell_grams[0] =
            cell_1_grams + (0.5f * correction);
        g_reported_cell_grams[1] =
            cell_2_grams + (0.5f * correction);
        g_reported_pair_valid = true;
    }

    return true;
}

} // namespace

void load_cells_init()
{
    load_saved_calibration();

    for (Hx711State &state : g_channels) {
        init_channel(state);
    }
}

bool load_cells_read_stabilized_pair(float &cell_1_grams, float &cell_2_grams)
{
    if (!update_stabilized_pair() || !g_reported_pair_valid) {
        return false;
    }

    cell_1_grams = g_reported_cell_grams[0];
    cell_2_grams = g_reported_cell_grams[1];
    return true;
}

bool load_cell_read_grams(uint8_t channel, float &grams)
{
    if (channel < 1 || channel > 2) {
        return false;
    }

    float cell_1_grams = 0.0f;
    float cell_2_grams = 0.0f;
    if (!load_cells_read_stabilized_pair(cell_1_grams, cell_2_grams)) {
        return false;
    }

    grams = channel == 1 ? cell_1_grams : cell_2_grams;
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
    reset_combined_filter();
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
    reset_combined_filter();
    return true;
}
