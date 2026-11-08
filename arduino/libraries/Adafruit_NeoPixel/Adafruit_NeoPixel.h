/*
 * Adafruit_NeoPixel: the common Arduino API for addressable LEDs (WS2812
 * and alike) over the wasm-rtos pulse interface. begin() opens the pulse
 * output of pin N ("gpioN", see wr_arduino_pin_name); show() sends the
 * pixel bytes with send-bits and the low time that latches them, so the
 * timing is the hardware's and does not depend on what else runs. Without
 * pulse hardware on the pin, show() sends nothing.
 */
#ifndef WASM_RTOS_ARDUINO_NEOPIXEL_H
#define WASM_RTOS_ARDUINO_NEOPIXEL_H

#include <Arduino.h>

typedef uint16_t neoPixelType;

/* Where white, red, green and blue go in a pixel's bytes; white at red's place means none. */
#define NEO_ORDER_(w, r, g, b) (((w) << 6) | ((r) << 4) | ((g) << 2) | (b))

#define NEO_RGB NEO_ORDER_(0, 0, 1, 2)
#define NEO_RBG NEO_ORDER_(0, 0, 2, 1)
#define NEO_GRB NEO_ORDER_(1, 1, 0, 2)
#define NEO_GBR NEO_ORDER_(2, 2, 0, 1)
#define NEO_BRG NEO_ORDER_(1, 1, 2, 0)
#define NEO_BGR NEO_ORDER_(2, 2, 1, 0)

#define NEO_WRGB NEO_ORDER_(0, 1, 2, 3)
#define NEO_WRBG NEO_ORDER_(0, 1, 3, 2)
#define NEO_WGRB NEO_ORDER_(0, 2, 1, 3)
#define NEO_WGBR NEO_ORDER_(0, 3, 1, 2)
#define NEO_WBRG NEO_ORDER_(0, 2, 3, 1)
#define NEO_WBGR NEO_ORDER_(0, 3, 2, 1)
#define NEO_RWGB NEO_ORDER_(1, 0, 2, 3)
#define NEO_RWBG NEO_ORDER_(1, 0, 3, 2)
#define NEO_RGWB NEO_ORDER_(2, 0, 1, 3)
#define NEO_RGBW NEO_ORDER_(3, 0, 1, 2)
#define NEO_RBWG NEO_ORDER_(2, 0, 3, 1)
#define NEO_RBGW NEO_ORDER_(3, 0, 2, 1)
#define NEO_GWRB NEO_ORDER_(1, 2, 0, 3)
#define NEO_GWBR NEO_ORDER_(1, 3, 0, 2)
#define NEO_GRWB NEO_ORDER_(2, 1, 0, 3)
#define NEO_GRBW NEO_ORDER_(3, 1, 0, 2)
#define NEO_GBWR NEO_ORDER_(2, 3, 0, 1)
#define NEO_GBRW NEO_ORDER_(3, 2, 0, 1)
#define NEO_BWRG NEO_ORDER_(1, 2, 3, 0)
#define NEO_BWGR NEO_ORDER_(1, 3, 2, 0)
#define NEO_BRWG NEO_ORDER_(2, 1, 3, 0)
#define NEO_BRGW NEO_ORDER_(3, 1, 2, 0)
#define NEO_BGWR NEO_ORDER_(2, 3, 1, 0)
#define NEO_BGRW NEO_ORDER_(3, 2, 1, 0)

/* Bit rate: 800 kHz for WS2812 and most others, 400 kHz for early WS2811 strips. */
#define NEO_KHZ800 0x0000
#define NEO_KHZ400 0x0100

class Adafruit_NeoPixel
{
public:
    Adafruit_NeoPixel(uint16_t n, int16_t pin = 6, neoPixelType type = NEO_GRB + NEO_KHZ800);
    Adafruit_NeoPixel();
    ~Adafruit_NeoPixel();
    Adafruit_NeoPixel(const Adafruit_NeoPixel&) = delete;
    Adafruit_NeoPixel& operator=(const Adafruit_NeoPixel&) = delete;

    void begin(void);
    void show(void);
    /* show() waits until the strip has latched, so another may follow at once. */
    bool canShow(void) { return true; }
    void setPin(int16_t pin);
    void setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b);
    void setPixelColor(uint16_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
    void setPixelColor(uint16_t n, uint32_t c);
    void fill(uint32_t c = 0, uint16_t first = 0, uint16_t count = 0);
    /* Scales the pixels as they are stored, so lowering it loses precision; set it once in setup(). */
    void setBrightness(uint8_t brightness);
    void clear(void);
    void updateLength(uint16_t n);
    void updateType(neoPixelType type);
    /* The bytes show() sends, brightness applied, in the strip's color order. */
    uint8_t* getPixels(void) const { return pixels_; }
    uint8_t getBrightness(void) const { return (uint8_t)(brightness_ - 1); }
    int16_t getPin(void) const { return pin_; }
    uint16_t numPixels(void) const { return count_; }
    uint32_t getPixelColor(uint16_t n) const;
    void rainbow(uint16_t first_hue = 0, int8_t reps = 1, uint8_t saturation = 255, uint8_t brightness = 255,
                 bool gammify = true);

    static uint8_t sine8(uint8_t x);
    static uint8_t gamma8(uint8_t x);
    static uint32_t Color(uint8_t r, uint8_t g, uint8_t b)
    {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    static uint32_t Color(uint8_t r, uint8_t g, uint8_t b, uint8_t w)
    {
        return ((uint32_t)w << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    /* hue 0-65535 around the color wheel from red; saturation and value 0-255. */
    static uint32_t ColorHSV(uint16_t hue, uint8_t sat = 255, uint8_t val = 255);
    static uint32_t gamma32(uint32_t x);
    /* An order from its letters ("GRB", "GRBW"). */
    static neoPixelType str2order(const char* v);

private:
    void open(void);
    void store(uint16_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t w);

    uint8_t* pixels_ = nullptr;
    uint16_t count_ = 0;
    int16_t pin_ = -1;
    neoPixelType type_ = NEO_GRB + NEO_KHZ800;
    /* The brightness plus one, wrapping: 0 leaves colors as they are. */
    uint8_t brightness_ = 0;
    bool begun_ = false;
    int32_t channel_ = 0;
};

#endif
