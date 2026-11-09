#include "Adafruit_NeoPixel.h"

#include <stdlib.h>
#include <string.h>
#include <wasm-rtos.h>

/* The low time after the bytes that latches the colors; newer LEDs need 280 us. */
static const uint32_t LATCH_NS = 300000u;

static uint8_t red_offset(neoPixelType type) { return (type >> 4) & 3u; }
static uint8_t green_offset(neoPixelType type) { return (type >> 2) & 3u; }
static uint8_t blue_offset(neoPixelType type) { return type & 3u; }
static uint8_t white_offset(neoPixelType type) { return (type >> 6) & 3u; }
static uint8_t bytes_per_pixel(neoPixelType type) { return white_offset(type) == red_offset(type) ? 3u : 4u; }

Adafruit_NeoPixel::Adafruit_NeoPixel(uint16_t n, int16_t pin, neoPixelType type)
{
    updateType(type);
    updateLength(n);
    setPin(pin);
}

Adafruit_NeoPixel::Adafruit_NeoPixel() {}

Adafruit_NeoPixel::~Adafruit_NeoPixel()
{
    if (channel_ > 0)
        wr_close(channel_);
    free(pixels_);
}

void Adafruit_NeoPixel::open(void)
{
    if (channel_ > 0)
        wr_close(channel_);
    channel_ = pin_ >= 0 ? wr_pulse_open_cstr(wr_arduino_pin_name((uint8_t)pin_), 0u) : 0;
}

void Adafruit_NeoPixel::begin(void)
{
    begun_ = true;
    open();
}

void Adafruit_NeoPixel::setPin(int16_t pin)
{
    pin_ = pin;
    if (begun_)
        open();
}

void Adafruit_NeoPixel::show(void)
{
    uint32_t length = (uint32_t)count_ * bytes_per_pixel(type_);
    bool slow = (type_ & NEO_KHZ400) != 0;
    if (channel_ <= 0 || pixels_ == nullptr)
        return;
    /* A bit is a high then a low time: 0.4 + 0.85 us for 0 and 0.8 + 0.45 us for 1, twice as long at 400 kHz. */
    if ((slow ? wr_pulse_send_bits(channel_, pixels_, length, 500u, 2000u, 1200u, 1300u)
              : wr_pulse_send_bits(channel_, pixels_, length, 400u, 850u, 800u, 450u)) < 0)
        return;
    wr_pulse_send(channel_, &LATCH_NS, 1u, 0u);
}

void Adafruit_NeoPixel::updateLength(uint16_t n)
{
    free(pixels_);
    pixels_ = (uint8_t*)calloc(n, bytes_per_pixel(type_));
    count_ = pixels_ != nullptr ? n : 0;
}

void Adafruit_NeoPixel::updateType(neoPixelType type)
{
    bool resize = bytes_per_pixel(type) != bytes_per_pixel(type_);
    type_ = type;
    if (resize && pixels_ != nullptr)
        updateLength(count_);
}

void Adafruit_NeoPixel::store(uint16_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    uint8_t* p;
    if (n >= count_)
        return;
    if (brightness_ != 0)
    {
        r = (uint8_t)((r * brightness_) >> 8);
        g = (uint8_t)((g * brightness_) >> 8);
        b = (uint8_t)((b * brightness_) >> 8);
        w = (uint8_t)((w * brightness_) >> 8);
    }
    p = pixels_ + (uint32_t)n * bytes_per_pixel(type_);
    p[red_offset(type_)] = r;
    p[green_offset(type_)] = g;
    p[blue_offset(type_)] = b;
    if (bytes_per_pixel(type_) == 4u)
        p[white_offset(type_)] = w;
}

void Adafruit_NeoPixel::setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b) { store(n, r, g, b, 0); }

void Adafruit_NeoPixel::setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    store(n, r, g, b, w);
}

void Adafruit_NeoPixel::setPixelColor(uint16_t n, uint32_t c)
{
    store(n, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24));
}

void Adafruit_NeoPixel::fill(uint32_t c, uint16_t first, uint16_t count)
{
    uint32_t end;
    if (first >= count_)
        return;
    end = count == 0 || (uint32_t)first + count > count_ ? count_ : (uint32_t)first + count;
    for (uint32_t i = first; i < end; ++i)
        setPixelColor((uint16_t)i, c);
}

void Adafruit_NeoPixel::setBrightness(uint8_t brightness)
{
    uint8_t next = (uint8_t)(brightness + 1u);
    /* What the stored bytes were scaled by, less one (255 at full), and the factor in 1/256 to the new one. */
    uint8_t old = (uint8_t)(brightness_ - 1u);
    uint32_t scale = old == 0 ? 0u : brightness == 255u ? 65535u / old : (((uint32_t)next << 8) - 1u) / old;
    uint32_t length = (uint32_t)count_ * bytes_per_pixel(type_);
    if (next == brightness_)
        return;
    /* The same rounding as the classic library, so fades by brightness end on the same colors. */
    for (uint32_t i = 0; i < length; ++i)
        pixels_[i] = (uint8_t)((pixels_[i] * scale) >> 8);
    brightness_ = next;
}

void Adafruit_NeoPixel::clear(void)
{
    if (pixels_ != nullptr)
        memset(pixels_, 0, (size_t)count_ * bytes_per_pixel(type_));
}

uint32_t Adafruit_NeoPixel::getPixelColor(uint16_t n) const
{
    const uint8_t* p;
    uint32_t color;
    if (n >= count_)
        return 0;
    p = pixels_ + (uint32_t)n * bytes_per_pixel(type_);
    /* Undo the brightness as far as the stored bytes allow. */
    auto raw = [this](uint8_t value) {
        return brightness_ == 0 ? (uint32_t)value : ((uint32_t)value << 8) / brightness_;
    };
    color = (raw(p[red_offset(type_)]) << 16) | (raw(p[green_offset(type_)]) << 8) | raw(p[blue_offset(type_)]);
    if (bytes_per_pixel(type_) == 4u)
        color |= raw(p[white_offset(type_)]) << 24;
    return color;
}

void Adafruit_NeoPixel::rainbow(uint16_t first_hue, int8_t reps, uint8_t saturation, uint8_t brightness, bool gammify)
{
    for (uint16_t i = 0; i < count_; ++i)
    {
        uint16_t hue = (uint16_t)(first_hue + (int64_t)i * reps * 65536 / count_);
        uint32_t color = ColorHSV(hue, saturation, brightness);
        setPixelColor(i, gammify ? gamma32(color) : color);
    }
}

/* One period of a sine from 0 to 255, starting at the middle. */
static const uint8_t SINE[256] = {
    128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 162, 165, 167, 170, 173,
    176, 179, 182, 185, 188, 190, 193, 196, 198, 201, 203, 206, 208, 211, 213, 215,
    218, 220, 222, 224, 226, 228, 230, 232, 234, 235, 237, 238, 240, 241, 243, 244,
    245, 246, 248, 249, 250, 250, 251, 252, 253, 253, 254, 254, 254, 255, 255, 255,
    255, 255, 255, 255, 254, 254, 254, 253, 253, 252, 251, 250, 250, 249, 248, 246,
    245, 244, 243, 241, 240, 238, 237, 235, 234, 232, 230, 228, 226, 224, 222, 220,
    218, 215, 213, 211, 208, 206, 203, 201, 198, 196, 193, 190, 188, 185, 182, 179,
    176, 173, 170, 167, 165, 162, 158, 155, 152, 149, 146, 143, 140, 137, 134, 131,
    128, 124, 121, 118, 115, 112, 109, 106, 103, 100, 97, 93, 90, 88, 85, 82,
    79, 76, 73, 70, 67, 65, 62, 59, 57, 54, 52, 49, 47, 44, 42, 40,
    37, 35, 33, 31, 29, 27, 25, 23, 21, 20, 18, 17, 15, 14, 12, 11,
    10, 9, 7, 6, 5, 5, 4, 3, 2, 2, 1, 1, 1, 0, 0, 0,
    0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4, 5, 5, 6, 7, 9,
    10, 11, 12, 14, 15, 17, 18, 20, 21, 23, 25, 27, 29, 31, 33, 35,
    37, 40, 42, 44, 47, 49, 52, 54, 57, 59, 62, 65, 67, 70, 73, 76,
    79, 82, 85, 88, 90, 93, 97, 100, 103, 106, 109, 112, 115, 118, 121, 124,
};

/* Gamma 2.6: how bright a level looks rather than how much current it takes. */
static const uint8_t GAMMA[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3,
    3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 5, 6, 6, 6, 6, 7,
    7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12,
    13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20,
    20, 21, 21, 22, 22, 23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29,
    30, 31, 31, 32, 33, 34, 34, 35, 36, 37, 38, 38, 39, 40, 41, 42,
    42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    58, 59, 60, 61, 62, 63, 64, 65, 66, 68, 69, 70, 71, 72, 73, 75,
    76, 77, 78, 80, 81, 82, 84, 85, 86, 88, 89, 90, 92, 93, 94, 96,
    97, 99, 100, 102, 103, 105, 106, 108, 109, 111, 112, 114, 115, 117, 119, 120,
    122, 124, 125, 127, 129, 130, 132, 134, 136, 137, 139, 141, 143, 145, 146, 148,
    150, 152, 154, 156, 158, 160, 162, 164, 166, 168, 170, 172, 174, 176, 178, 180,
    182, 184, 186, 188, 191, 193, 195, 197, 199, 202, 204, 206, 209, 211, 213, 215,
    218, 220, 223, 225, 227, 230, 232, 235, 237, 240, 242, 245, 247, 250, 252, 255,
};

uint8_t Adafruit_NeoPixel::sine8(uint8_t x) { return SINE[x]; }

uint8_t Adafruit_NeoPixel::gamma8(uint8_t x) { return GAMMA[x]; }

uint32_t Adafruit_NeoPixel::gamma32(uint32_t x)
{
    return ((uint32_t)GAMMA[(x >> 24) & 0xFFu] << 24) | ((uint32_t)GAMMA[(x >> 16) & 0xFFu] << 16) |
           ((uint32_t)GAMMA[(x >> 8) & 0xFFu] << 8) | GAMMA[x & 0xFFu];
}

uint32_t Adafruit_NeoPixel::ColorHSV(uint16_t hue, uint8_t sat, uint8_t val)
{
    /* Six ramps of 255 steps around the wheel: red, yellow, green, cyan, blue, magenta, back to red. */
    uint32_t h = ((uint32_t)hue * 1530u + 32768u) / 65536u;
    uint32_t rgb[3];
    uint32_t color = 0;
    if (h < 510u)
    {
        rgb[0] = h < 255u ? 255u : 510u - h;
        rgb[1] = h < 255u ? h : 255u;
        rgb[2] = 0;
    }
    else if (h < 1020u)
    {
        rgb[0] = 0;
        rgb[1] = h < 765u ? 255u : 1020u - h;
        rgb[2] = h < 765u ? h - 510u : 255u;
    }
    else if (h < 1530u)
    {
        rgb[0] = h < 1275u ? h - 1020u : 255u;
        rgb[1] = 0;
        rgb[2] = h < 1275u ? 255u : 1530u - h;
    }
    else
    {
        rgb[0] = 255u;
        rgb[1] = rgb[2] = 0;
    }
    /* Desaturate toward white, then scale by value. */
    for (int i = 0; i < 3; ++i)
        color = (color << 8) | ((((rgb[i] * (sat + 1u)) >> 8) + (255u - sat)) * (val + 1u) >> 8);
    return color;
}

neoPixelType Adafruit_NeoPixel::str2order(const char* v)
{
    int r = 0, g = 0, b = 0, w = -1;
    if (v == nullptr)
        return NEO_GRB;
    for (int i = 0; v[i] != '\0' && i < 4; ++i)
    {
        switch (v[i] | 0x20)
        {
            case 'r': r = i; break;
            case 'g': g = i; break;
            case 'b': b = i; break;
            case 'w': w = i; break;
        }
    }
    if (w < 0)
        w = r;
    return (neoPixelType)NEO_ORDER_(w, r, g, b);
}
