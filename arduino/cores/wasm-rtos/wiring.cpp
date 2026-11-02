/* Pins, time and interrupts over the wasm-rtos ABI. */
#include "Arduino.h"

#include <sched.h>
#include <stdio.h>
#include <time.h>
#include <wasm-rtos.h>

#define MAX_PINS 256
#define MAX_INTERRUPTS 8
/* Handler calls one delay or loop pass makes for one pin, so a noisy pin cannot stall the sketch. */
#define MAX_CALLS_PER_PASS 16

static int32_t g_pins[MAX_PINS];
static uint8_t g_pin_modes[MAX_PINS];
/* PWM outputs from analogWrite and tone, by pin; a pin is either these or g_pins. */
static int32_t g_pwm[MAX_PINS];
static int g_read_bits = 10;
static int g_write_bits = 8;
static uint32_t g_write_hz = 1000;
static uint8_t g_tone_pin = NOT_A_PIN;
static uint64_t g_tone_end_ms;

typedef struct
{
    uint8_t pin;
    void (*handler)(void);
} Interrupt;

static Interrupt g_interrupts[MAX_INTERRUPTS];
static bool g_interrupts_off;

extern "C" __attribute__((weak)) const char* wr_arduino_analog_name(uint8_t pin)
{
    static char name[12];
    snprintf(name, sizeof(name), "a%u", (unsigned)(pin >= A0 ? pin - A0 : pin));
    return name;
}

static void release_pwm(uint8_t pin)
{
    if (g_pwm[pin] > 0)
        wr_close(g_pwm[pin]);
    g_pwm[pin] = 0;
}

extern "C" __attribute__((weak)) const char* wr_arduino_pin_name(uint8_t pin)
{
    static char name[12];
    if (pin == LED_BUILTIN)
        return "led";
    snprintf(name, sizeof(name), "gpio%u", (unsigned)pin);
    return name;
}

static uint32_t gpio_flags(uint8_t mode)
{
    switch (mode)
    {
        case OUTPUT: return WR_GPIO_OUTPUT;
        case INPUT_PULLUP: return WR_GPIO_PULL_UP;
        case INPUT_PULLDOWN: return WR_GPIO_PULL_DOWN;
        case OUTPUT_OPEN_DRAIN: return WR_GPIO_OUTPUT | WR_GPIO_OPEN_DRAIN | WR_GPIO_INIT_HIGH;
        default: return 0;
    }
}

/* The pin's handle, opened in mode on first use; 0 when the device has no such pin. */
static int32_t pin_handle(uint8_t pin, uint8_t mode)
{
    /* Digital use of a pin ends its PWM, as on Arduino. */
    release_pwm(pin);
    if (g_pins[pin] <= 0)
    {
        g_pins[pin] = wr_gpio_open_cstr(wr_arduino_pin_name(pin), gpio_flags(mode));
        g_pin_modes[pin] = mode;
    }
    return g_pins[pin] > 0 ? g_pins[pin] : 0;
}

void pinMode(uint8_t pin, uint8_t mode)
{
    release_pwm(pin);
    if (g_pins[pin] > 0)
    {
        if (wr_gpio_set_mode(g_pins[pin], gpio_flags(mode)) == 0)
            g_pin_modes[pin] = mode;
        return;
    }
    pin_handle(pin, mode);
}

void digitalWrite(uint8_t pin, uint8_t value)
{
    int32_t handle = pin_handle(pin, OUTPUT);
    if (handle == 0)
        return;
    /* On an input, HIGH turns the pull-up on and LOW off, as on the classic boards. */
    if (g_pin_modes[pin] == INPUT || g_pin_modes[pin] == INPUT_PULLUP)
    {
        pinMode(pin, value ? INPUT_PULLUP : INPUT);
        return;
    }
    wr_gpio_set(handle, value ? 1u : 0u);
}

int digitalRead(uint8_t pin)
{
    int32_t handle = pin_handle(pin, INPUT);
    return handle != 0 && wr_gpio_get(handle) == 1 ? HIGH : LOW;
}

/* Scale a value of from_bits to to_bits, keeping full scale full. */
static uint32_t rescale(uint32_t value, int from_bits, int to_bits)
{
    uint64_t from_max = (1ull << from_bits) - 1u;
    uint64_t to_max = (1ull << to_bits) - 1u;
    return from_max == 0 ? 0 : (uint32_t)(((uint64_t)value * to_max + from_max / 2u) / from_max);
}

/* Channels stay open: reading is shared, and opening again each time would cost a call. */
int analogRead(uint8_t pin)
{
    static int32_t channels[MAX_PINS];
    int32_t raw;
    int32_t bits;
    if (channels[pin] <= 0)
        channels[pin] = wr_adc_open_cstr(wr_arduino_analog_name(pin));
    if (channels[pin] <= 0)
        return 0;
    raw = wr_adc_read(channels[pin]);
    bits = wr_adc_get(channels[pin], WR_ADC_RESOLUTION);
    return raw < 0 || bits <= 0 ? 0 : (int)rescale((uint32_t)raw, bits, g_read_bits);
}

void analogReadResolution(int bits)
{
    if (bits >= 1 && bits <= 16)
        g_read_bits = bits;
}

void analogReference(uint8_t mode) { (void)mode; }

/* The pin's PWM output, opened at frequency_hz on first use; 0 when the pin cannot do PWM. */
static int32_t pwm_handle(uint8_t pin, uint32_t frequency_hz)
{
    if (g_pwm[pin] > 0)
        return g_pwm[pin];
    if (g_pins[pin] > 0)
    {
        wr_close(g_pins[pin]);
        g_pins[pin] = 0;
    }
    g_pwm[pin] = wr_pwm_open_cstr(wr_arduino_pin_name(pin), frequency_hz, 0u);
    return g_pwm[pin] > 0 ? g_pwm[pin] : 0;
}

void analogWrite(uint8_t pin, int value)
{
    uint32_t max = (1u << g_write_bits) - 1u;
    uint32_t level = value < 0 ? 0u : (uint32_t)value > max ? max : (uint32_t)value;
    int32_t output = pwm_handle(pin, g_write_hz);
    if (output == 0)
    {
        /* No PWM on this pin: on from half scale, as some cores do. */
        digitalWrite(pin, level >= (max + 1u) / 2u ? HIGH : LOW);
        return;
    }
    wr_pwm_set_duty(output, rescale(level, g_write_bits, 16));
}

void analogWriteResolution(int bits)
{
    if (bits >= 1 && bits <= 16)
        g_write_bits = bits;
}

void analogWriteFrequency(uint8_t pin, uint32_t frequency_hz)
{
    g_write_hz = frequency_hz;
    if (g_pwm[pin] > 0)
        wr_pwm_set_frequency(g_pwm[pin], frequency_hz);
}

/* A square wave: half duty at the frequency; with a duration it stops by itself. */
void tone(uint8_t pin, unsigned int frequency, unsigned long duration)
{
    int32_t output;
    if (frequency == 0)
    {
        noTone(pin);
        return;
    }
    output = pwm_handle(pin, frequency);
    if (output == 0)
        return;
    wr_pwm_set_frequency(output, frequency);
    wr_pwm_set_duty(output, WR_PWM_DUTY_MAX / 2u + 1u);
    g_tone_pin = pin;
    g_tone_end_ms = duration > 0 ? wr_time_ms() + duration : 0;
}

void noTone(uint8_t pin)
{
    if (g_pwm[pin] > 0)
        wr_pwm_set_duty(g_pwm[pin], 0u);
    if (g_tone_pin == pin)
        g_tone_pin = NOT_A_PIN;
}

/* A tone with a duration ends here, between loop() calls and in delay(). */
static void end_tone(void)
{
    if (g_tone_pin != NOT_A_PIN && g_tone_end_ms != 0 && wr_time_ms() >= g_tone_end_ms)
        noTone(g_tone_pin);
}

unsigned long millis(void) { return (unsigned long)wr_time_ms(); }

unsigned long micros(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long)((unsigned long long)now.tv_sec * 1000000u + (unsigned long long)now.tv_nsec / 1000u);
}

/* Run the handlers of pins whose edges arrived; between loop() calls and in delay(), never in between. */
static void run_interrupts(void)
{
    end_tone();
    if (g_interrupts_off)
        return;
    for (int index = 0; index < MAX_INTERRUPTS; ++index)
    {
        Interrupt* interrupt = &g_interrupts[index];
        int32_t events;
        if (interrupt->handler == nullptr)
            continue;
        events = wr_gpio_events(g_pins[interrupt->pin]);
        for (int32_t call = 0; call < events && call < MAX_CALLS_PER_PASS; ++call)
            interrupt->handler();
    }
}

void delay(unsigned long ms)
{
    uint64_t deadline = wr_time_ms() + ms;
    run_interrupts();
    for (;;)
    {
        int32_t watched[MAX_INTERRUPTS];
        uint32_t count = 0;
        uint64_t now = wr_time_ms();
        if (now >= deadline)
            break;
        uint64_t until = deadline;
        for (int index = 0; index < MAX_INTERRUPTS; ++index)
            if (g_interrupts[index].handler != nullptr && !g_interrupts_off)
                watched[count++] = g_pins[g_interrupts[index].pin];
        /* A tone that ends during the delay wakes it on time. */
        if (g_tone_pin != NOT_A_PIN && g_tone_end_ms != 0 && g_tone_end_ms < until)
            until = g_tone_end_ms > now ? g_tone_end_ms : now;
        /* Sleep until the time is up or a watched pin has an edge, whichever comes first. */
        if (count == 0)
            wr_sleep_ms((uint32_t)(until - now));
        else
            wr_wait_any(watched, count, (uint32_t)(until - now));
        run_interrupts();
    }
}

/* The kernel's clock counts milliseconds; shorter waits give other programs a turn until it moves on. */
void delayMicroseconds(unsigned int us)
{
    unsigned long start = micros();
    if (us >= 1000u)
    {
        delay(us / 1000u);
        us %= 1000u;
        start = micros();
    }
    while (us > 0 && micros() - start < us)
        sched_yield();
}

void yield(void)
{
    run_interrupts();
    sched_yield();
}

void attachInterrupt(int interrupt, void (*handler)(void), int mode)
{
    uint8_t pin = (uint8_t)interrupt;
    uint32_t edges = mode == RISING ? WR_GPIO_RISING : mode == FALLING ? WR_GPIO_FALLING : WR_GPIO_RISING | WR_GPIO_FALLING;
    Interrupt* slot = nullptr;
    int32_t handle;

    if (interrupt < 0 || interrupt >= MAX_PINS || handler == nullptr)
        return;
    handle = pin_handle(pin, INPUT);
    if (handle == 0 || wr_gpio_watch(handle, edges) != 0)
        return;
    for (int index = 0; index < MAX_INTERRUPTS && slot == nullptr; ++index)
        if (g_interrupts[index].handler != nullptr && g_interrupts[index].pin == pin)
            slot = &g_interrupts[index];
    for (int index = 0; index < MAX_INTERRUPTS && slot == nullptr; ++index)
        if (g_interrupts[index].handler == nullptr)
            slot = &g_interrupts[index];
    if (slot == nullptr)
        return;
    slot->pin = pin;
    slot->handler = handler;
}

void detachInterrupt(int interrupt)
{
    for (int index = 0; index < MAX_INTERRUPTS; ++index)
    {
        if (g_interrupts[index].handler != nullptr && g_interrupts[index].pin == (uint8_t)interrupt)
        {
            wr_gpio_watch(g_pins[g_interrupts[index].pin], 0u);
            g_interrupts[index].handler = nullptr;
        }
    }
}

void interrupts(void) { g_interrupts_off = false; }
void noInterrupts(void) { g_interrupts_off = true; }

void shiftOut(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order, uint8_t value)
{
    for (int index = 0; index < 8; ++index)
    {
        int bit = bit_order == LSBFIRST ? index : 7 - index;
        digitalWrite(data_pin, (value >> bit) & 1);
        digitalWrite(clock_pin, HIGH);
        digitalWrite(clock_pin, LOW);
    }
}

uint8_t shiftIn(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order)
{
    uint8_t value = 0;
    for (int index = 0; index < 8; ++index)
    {
        digitalWrite(clock_pin, HIGH);
        int bit = bit_order == LSBFIRST ? index : 7 - index;
        value |= (uint8_t)(digitalRead(data_pin) << bit);
        digitalWrite(clock_pin, LOW);
    }
    return value;
}

/* Pulses shorter than the scheduler cannot be timed from a program: report a timeout. */
unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout)
{
    (void)pin;
    (void)state;
    (void)timeout;
    return 0;
}

/* For main.cpp: interrupts that came in during loop(). */
void wr_arduino_after_loop(void) { run_interrupts(); }
