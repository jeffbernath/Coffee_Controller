#include "scale_display.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hardware/spi.h"
#include "pico/stdlib.h"

namespace {

// -----------------------------------------------------------------------------
// GC9A01 / SPI1 pin assignment
// -----------------------------------------------------------------------------
// Common RP2040 SPI1 mapping:
//   GP8  = display D/C
//   GP9  = SPI1 CSn
//   GP10 = SPI1 SCK
//   GP11 = SPI1 TX / MOSI
//   GP12 = display RESET
//   GP13 = display BL / backlight control
//
// The GC9A01 is write-only here, so MISO is not required.
spi_inst_t *const DISPLAY_SPI = spi1;
constexpr uint DISPLAY_DC_PIN = 8;
constexpr uint DISPLAY_CS_PIN = 9;
constexpr uint DISPLAY_SCK_PIN = 10;
constexpr uint DISPLAY_MOSI_PIN = 11;
constexpr uint DISPLAY_RESET_PIN = 12;
constexpr uint DISPLAY_BACKLIGHT_PIN = 13;
constexpr uint32_t DISPLAY_SPI_HZ = 40'000'000;
constexpr uint32_t DISPLAY_REFRESH_MS = 50;

// Local-only physical tare button. Wire a normally-open momentary switch from
// GP6 to GND; the Pico's internal pull-up is enabled.
constexpr uint TARE_BUTTON_PIN = 6;
constexpr uint32_t TARE_DEBOUNCE_MS = 35;

constexpr int DISPLAY_WIDTH = 240;
constexpr int DISPLAY_HEIGHT = 240;

// GC9A01 commands used by this driver.
constexpr uint8_t CMD_SWRESET = 0x01;
constexpr uint8_t CMD_SLPOUT = 0x11;
constexpr uint8_t CMD_INVON = 0x21;
constexpr uint8_t CMD_DISPON = 0x29;
constexpr uint8_t CMD_CASET = 0x2A;
constexpr uint8_t CMD_RASET = 0x2B;
constexpr uint8_t CMD_RAMWR = 0x2C;
constexpr uint8_t CMD_MADCTL = 0x36;
constexpr uint8_t CMD_COLMOD = 0x3A;

// Restrained color palette. RGB565 values.
constexpr uint16_t COLOR_BACKGROUND = 0x0063;   // near-black navy
constexpr uint16_t COLOR_SEGMENT_OFF = 0x1145;  // muted blue-green
constexpr uint16_t COLOR_WEIGHT = 0x4ED9;       // restrained teal
constexpr uint16_t COLOR_ACCENT = 0xED88;       // warm amber

bool g_initialized = false;
char g_last_text[16] = {};

bool g_button_raw_pressed = false;
bool g_button_stable_pressed = false;
bool g_button_event_pending = false;
uint32_t g_button_last_change_ms = 0;
uint32_t g_last_render_ms = 0;

void cs_select()
{
    gpio_put(DISPLAY_CS_PIN, 0);
}

void cs_deselect()
{
    gpio_put(DISPLAY_CS_PIN, 1);
}

void write_command(uint8_t command)
{
    gpio_put(DISPLAY_DC_PIN, 0);
    cs_select();
    spi_write_blocking(DISPLAY_SPI, &command, 1);
    cs_deselect();
}

void write_data(const uint8_t *data, size_t length)
{
    if (!data || length == 0) {
        return;
    }

    gpio_put(DISPLAY_DC_PIN, 1);
    cs_select();
    spi_write_blocking(DISPLAY_SPI, data, length);
    cs_deselect();
}

void write_command_data(uint8_t command, const uint8_t *data, size_t length)
{
    write_command(command);
    write_data(data, length);
}

void hardware_reset()
{
    gpio_put(DISPLAY_RESET_PIN, 1);
    sleep_ms(10);
    gpio_put(DISPLAY_RESET_PIN, 0);
    sleep_ms(20);
    gpio_put(DISPLAY_RESET_PIN, 1);
    sleep_ms(120);
}

void set_address_window(int x0, int y0, int x1, int y1)
{
    const uint8_t x_data[4] = {
        static_cast<uint8_t>((x0 >> 8) & 0xFF),
        static_cast<uint8_t>(x0 & 0xFF),
        static_cast<uint8_t>((x1 >> 8) & 0xFF),
        static_cast<uint8_t>(x1 & 0xFF),
    };
    const uint8_t y_data[4] = {
        static_cast<uint8_t>((y0 >> 8) & 0xFF),
        static_cast<uint8_t>(y0 & 0xFF),
        static_cast<uint8_t>((y1 >> 8) & 0xFF),
        static_cast<uint8_t>(y1 & 0xFF),
    };

    write_command_data(CMD_CASET, x_data, sizeof(x_data));
    write_command_data(CMD_RASET, y_data, sizeof(y_data));
    write_command(CMD_RAMWR);
}

void fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0 || x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
        return;
    }

    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > DISPLAY_WIDTH) {
        width = DISPLAY_WIDTH - x;
    }
    if (y + height > DISPLAY_HEIGHT) {
        height = DISPLAY_HEIGHT - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    set_address_window(x, y, x + width - 1, y + height - 1);

    constexpr size_t PIXELS_PER_CHUNK = 64;
    uint8_t buffer[PIXELS_PER_CHUNK * 2];
    const uint8_t high = static_cast<uint8_t>(color >> 8);
    const uint8_t low = static_cast<uint8_t>(color & 0xFF);

    for (size_t i = 0; i < PIXELS_PER_CHUNK; ++i) {
        buffer[i * 2] = high;
        buffer[i * 2 + 1] = low;
    }

    size_t remaining = static_cast<size_t>(width) * static_cast<size_t>(height);
    gpio_put(DISPLAY_DC_PIN, 1);
    cs_select();
    while (remaining > 0) {
        const size_t pixels = remaining > PIXELS_PER_CHUNK ? PIXELS_PER_CHUNK : remaining;
        spi_write_blocking(DISPLAY_SPI, buffer, pixels * 2);
        remaining -= pixels;
    }
    cs_deselect();
}

void clear_screen(uint16_t color)
{
    fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void gc9a01_init_controller()
{
    write_command(CMD_SWRESET);
    sleep_ms(150);

    // Common GC9A01A 240x240 panel initialization. Each record is:
    // command, data-length, data bytes. The 0x00,0xFF pair terminates the list.
    static const uint8_t init_sequence[] = {
        0xEF, 0,
        0xEB, 1, 0x14,
        0xFE, 0,
        0xEF, 0,
        0xEB, 1, 0x14,
        0x84, 1, 0x40,
        0x85, 1, 0xFF,
        0x86, 1, 0xFF,
        0x87, 1, 0xFF,
        0x88, 1, 0x0A,
        0x89, 1, 0x21,
        0x8A, 1, 0x00,
        0x8B, 1, 0x80,
        0x8C, 1, 0x01,
        0x8D, 1, 0x01,
        0x8E, 1, 0xFF,
        0x8F, 1, 0xFF,
        0xB6, 2, 0x00, 0x20,
        CMD_MADCTL, 1, 0x08,
        CMD_COLMOD, 1, 0x05,
        0x90, 4, 0x08, 0x08, 0x08, 0x08,
        0xBD, 1, 0x06,
        0xBC, 1, 0x00,
        0xFF, 3, 0x60, 0x01, 0x04,
        0xC3, 1, 0x13,
        0xC4, 1, 0x13,
        0xC9, 1, 0x22,
        0xBE, 1, 0x11,
        0xE1, 2, 0x10, 0x0E,
        0xDF, 3, 0x21, 0x0C, 0x02,
        0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
        0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
        0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
        0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
        0xED, 2, 0x1B, 0x0B,
        0xAE, 1, 0x77,
        0xCD, 1, 0x63,
        0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03,
        0xE8, 1, 0x34,
        0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70,
                  0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
        0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70,
                  0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
        0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
        0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
        0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
        0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00,
        0x98, 2, 0x3E, 0x07,
        0x35, 0,
        CMD_INVON, 0,
        0x00, 0xFF,
    };

    size_t index = 0;
    while (index + 1 < sizeof(init_sequence)) {
        const uint8_t command = init_sequence[index++];
        const uint8_t length = init_sequence[index++];

        if (command == 0x00 && length == 0xFF) {
            break;
        }

        write_command(command);
        if (length > 0) {
            write_data(&init_sequence[index], length);
            index += length;
        }
    }

    write_command(CMD_SLPOUT);
    sleep_ms(120);
    write_command(CMD_DISPON);
    sleep_ms(20);
}

// Seven-segment bit assignments.
constexpr uint8_t SEG_A = 1u << 0;
constexpr uint8_t SEG_B = 1u << 1;
constexpr uint8_t SEG_C = 1u << 2;
constexpr uint8_t SEG_D = 1u << 3;
constexpr uint8_t SEG_E = 1u << 4;
constexpr uint8_t SEG_F = 1u << 5;
constexpr uint8_t SEG_G = 1u << 6;

uint8_t digit_segments(char c)
{
    switch (c) {
        case '0': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
        case '1': return SEG_B | SEG_C;
        case '2': return SEG_A | SEG_B | SEG_D | SEG_E | SEG_G;
        case '3': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_G;
        case '4': return SEG_B | SEG_C | SEG_F | SEG_G;
        case '5': return SEG_A | SEG_C | SEG_D | SEG_F | SEG_G;
        case '6': return SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        case '7': return SEG_A | SEG_B | SEG_C;
        case '8': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        case '9': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
        case '-': return SEG_G;
        default:  return 0;
    }
}

void draw_digit(int x, int y, int width, int height, int thickness, char c)
{
    const uint8_t active = digit_segments(c);
    const int half = height / 2;

    const struct SegmentRect {
        uint8_t bit;
        int x;
        int y;
        int w;
        int h;
    } segments[] = {
        {SEG_A, thickness, 0, width - 2 * thickness, thickness},
        {SEG_B, width - thickness, thickness, thickness, half - thickness},
        {SEG_C, width - thickness, half, thickness, half - thickness},
        {SEG_D, thickness, height - thickness, width - 2 * thickness, thickness},
        {SEG_E, 0, half, thickness, half - thickness},
        {SEG_F, 0, thickness, thickness, half - thickness},
        {SEG_G, thickness, half - thickness / 2, width - 2 * thickness, thickness},
    };

    for (const SegmentRect &segment : segments) {
        const uint16_t color = (active & segment.bit) ? COLOR_WEIGHT : COLOR_SEGMENT_OFF;
        fill_rect(x + segment.x, y + segment.y, segment.w, segment.h, color);
    }
}

void draw_small_g(int x, int y, int scale)
{
    // 5x7 lowercase 'g'. Only the unit needed by this display is implemented.
    constexpr uint8_t rows[7] = {
        0b00000,
        0b01110,
        0b10001,
        0b01111,
        0b00001,
        0b10001,
        0b01110,
    };

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if ((rows[row] & (1u << (4 - col))) != 0) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, COLOR_ACCENT);
            }
        }
    }
}

void render_weight(const char *text)
{
    if (!text) {
        return;
    }

    // Repaint only the center content area, leaving the round-screen edge as a
    // clean uninterrupted field of navy.
    fill_rect(8, 54, 224, 142, COLOR_BACKGROUND);

    constexpr int DIGIT_W = 36;
    constexpr int DIGIT_H = 82;
    constexpr int DIGIT_T = 8;
    constexpr int DIGIT_GAP = 8;
    constexpr int DIGIT_Y = 74;

    int glyph_count = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p != '.') {
            ++glyph_count;
        }
    }

    if (glyph_count <= 0) {
        return;
    }

    const int total_width = glyph_count * DIGIT_W + (glyph_count - 1) * DIGIT_GAP;
    int x = (DISPLAY_WIDTH - total_width) / 2;
    int previous_digit_x = x;

    for (const char *p = text; *p != '\0'; ++p) {
        if (*p == '.') {
            // Decimal point belongs to the previous digit and gets the warm
            // accent color so the full-color panel has a subtle focal detail.
            fill_rect(previous_digit_x + DIGIT_W + 2,
                      DIGIT_Y + DIGIT_H - 10,
                      6,
                      6,
                      COLOR_ACCENT);
            continue;
        }

        previous_digit_x = x;
        draw_digit(x, DIGIT_Y, DIGIT_W, DIGIT_H, DIGIT_T, *p);
        x += DIGIT_W + DIGIT_GAP;
    }

    // Unit is deliberately small so the screen remains visually just the weight.
    draw_small_g(111, 171, 3);
}

void format_weight(float grams, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return;
    }

    if (!std::isfinite(grams)) {
        grams = 0.0f;
    }

    // Avoid rendering a negative zero after filtering/tare.
    if (std::fabs(grams) < 0.05f) {
        grams = 0.0f;
    }

    // Keep one decimal through the normal coffee-scale range. Above 999.9 g,
    // switch to whole grams so the value still fits comfortably on 240 pixels.
    if (std::fabs(grams) < 1000.0f) {
        std::snprintf(buffer, buffer_size, "%.1f", static_cast<double>(grams));
    } else {
        const float clamped = grams > 9999.0f ? 9999.0f : (grams < -9999.0f ? -9999.0f : grams);
        std::snprintf(buffer, buffer_size, "%.0f", static_cast<double>(clamped));
    }
}

void poll_tare_button()
{
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const bool raw_pressed = gpio_get(TARE_BUTTON_PIN) == 0;

    if (raw_pressed != g_button_raw_pressed) {
        g_button_raw_pressed = raw_pressed;
        g_button_last_change_ms = now_ms;
    }

    if (raw_pressed != g_button_stable_pressed &&
        now_ms - g_button_last_change_ms >= TARE_DEBOUNCE_MS) {
        g_button_stable_pressed = raw_pressed;
        if (g_button_stable_pressed) {
            g_button_event_pending = true;
        }
    }
}

} // namespace

void scale_display_init()
{
    // SPI1 display bus. No MISO pin is needed for this write-only display.
    spi_init(DISPLAY_SPI, DISPLAY_SPI_HZ);
    spi_set_format(DISPLAY_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(DISPLAY_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(DISPLAY_MOSI_PIN, GPIO_FUNC_SPI);

    gpio_init(DISPLAY_CS_PIN);
    gpio_set_dir(DISPLAY_CS_PIN, GPIO_OUT);
    gpio_put(DISPLAY_CS_PIN, 1);

    gpio_init(DISPLAY_DC_PIN);
    gpio_set_dir(DISPLAY_DC_PIN, GPIO_OUT);
    gpio_put(DISPLAY_DC_PIN, 1);

    gpio_init(DISPLAY_RESET_PIN);
    gpio_set_dir(DISPLAY_RESET_PIN, GPIO_OUT);
    gpio_put(DISPLAY_RESET_PIN, 1);

    gpio_init(DISPLAY_BACKLIGHT_PIN);
    gpio_set_dir(DISPLAY_BACKLIGHT_PIN, GPIO_OUT);
    gpio_put(DISPLAY_BACKLIGHT_PIN, 0);

    gpio_init(TARE_BUTTON_PIN);
    gpio_set_dir(TARE_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(TARE_BUTTON_PIN);

    hardware_reset();
    gc9a01_init_controller();
    clear_screen(COLOR_BACKGROUND);

    gpio_put(DISPLAY_BACKLIGHT_PIN, 1);

    g_initialized = true;
    g_last_text[0] = '\0';
    scale_display_update(false, 0.0f);
}

void scale_display_update(bool scale_available, float total_grams)
{
    if (!g_initialized) {
        return;
    }

    poll_tare_button();

    char text[16] = {};
    format_weight(scale_available ? total_grams : 0.0f, text, sizeof(text));

    if (std::strcmp(text, g_last_text) == 0) {
        return;
    }

    // Limit display traffic while the scale is moving. The HX711 and serial
    // communication stay responsive, while 20 FPS is still very smooth for a
    // numeric scale display.
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (g_last_text[0] != '\0' &&
        now_ms - g_last_render_ms < DISPLAY_REFRESH_MS) {
        return;
    }

    render_weight(text);
    g_last_render_ms = now_ms;
    std::snprintf(g_last_text, sizeof(g_last_text), "%s", text);
}

bool scale_tare_button_pressed()
{
    if (!g_initialized) {
        return false;
    }

    poll_tare_button();

    if (!g_button_event_pending) {
        return false;
    }

    g_button_event_pending = false;
    return true;
}
