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

typedef struct
{
    uint8_t pin;
    void (*handler)(void);
} Interrupt;

static Interrupt g_interrupts[MAX_INTERRUPTS];
static bool g_interrupts_off;

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
    if (g_pins[pin] <= 0)
    {
        g_pins[pin] = wr_gpio_open_cstr(wr_arduino_pin_name(pin), gpio_flags(mode));
        g_pin_modes[pin] = mode;
    }
    return g_pins[pin] > 0 ? g_pins[pin] : 0;
}

void pinMode(uint8_t pin, uint8_t mode)
{
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

/* No analog hardware in the ABI yet: inputs read 0, outputs are on from half scale. */
int analogRead(uint8_t pin)
{
    (void)pin;
    return 0;
}

void analogReadResolution(int bits) { (void)bits; }
void analogReference(uint8_t mode) { (void)mode; }

void analogWrite(uint8_t pin, int value) { digitalWrite(pin, value >= 128 ? HIGH : LOW); }

void tone(uint8_t pin, unsigned int frequency, unsigned long duration)
{
    (void)pin;
    (void)frequency;
    (void)duration;
}

void noTone(uint8_t pin) { (void)pin; }

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
        for (int index = 0; index < MAX_INTERRUPTS; ++index)
            if (g_interrupts[index].handler != nullptr && !g_interrupts_off)
                watched[count++] = g_pins[g_interrupts[index].pin];
        /* Sleep until the time is up or a watched pin has an edge, whichever comes first. */
        if (count == 0)
            wr_sleep_ms((uint32_t)(deadline - now));
        else
            wr_wait_any(watched, count, (uint32_t)(deadline - now));
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
