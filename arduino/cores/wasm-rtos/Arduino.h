/*
 * Arduino API for wasm-rtos programs. It is a library that is built into a
 * sketch, not part of the operating system: a program that does not use it
 * carries none of it.
 *
 * Pins are named by the device profile: pin N is "gpioN", LED_BUILTIN is
 * "led", A0 is "a0". Interrupt handlers run between loop() calls and inside
 * delay(), never in the middle of other code. analogRead uses adc,
 * analogWrite and tone pwm, Serial1 and Serial2 uart.
 */
#ifndef WASM_RTOS_ARDUINO_H
#define WASM_RTOS_ARDUINO_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "avr/pgmspace.h"
#include "pins_arduino.h"

#define ARDUINO 10819
#define ARDUINO_ARCH_WASM_RTOS 1

typedef bool boolean;
typedef uint8_t byte;
typedef uint16_t word;

#define HIGH 0x1
#define LOW 0x0

#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define INPUT_PULLDOWN 0x3
#define OUTPUT_OPEN_DRAIN 0x4

/* An enum, as in the current Arduino API: libraries name the type. */
typedef enum
{
    LSBFIRST = 0,
    MSBFIRST = 1
} BitOrder;

#define CHANGE 1
#define FALLING 2
#define RISING 3

#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

/* The board's LED, whatever pin the device profile gives it. */
#define LED_BUILTIN 250
#define NOT_A_PIN 255
#define NOT_AN_INTERRUPT -1
#define digitalPinToInterrupt(pin) ((int)(pin))

#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)
#define sq(x) ((x) * (x))
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitToggle(value, bit) ((value) ^= (1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b) (1UL << (b))
#define clockCyclesPerMicrosecond() 1L
#define F_CPU 1000000UL

#ifdef __cplusplus
extern "C" {
#endif

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int digitalRead(uint8_t pin);
int analogRead(uint8_t pin);
void analogReadResolution(int bits);
void analogWrite(uint8_t pin, int value);
void analogWriteResolution(int bits);
void analogWriteFrequency(uint8_t pin, uint32_t frequency_hz);
/* An analog output's 8-bit value, as on the ESP32; dacDisable gives the pin back. */
void dacWrite(uint8_t pin, uint8_t value);
void dacDisable(uint8_t pin);
void analogReference(uint8_t mode);

unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield(void);

void attachInterrupt(int interrupt, void (*handler)(void), int mode);
void detachInterrupt(int interrupt);
void interrupts(void);
void noInterrupts(void);

void shiftOut(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order, uint8_t value);
uint8_t shiftIn(uint8_t data_pin, uint8_t clock_pin, uint8_t bit_order);
unsigned long pulseIn(uint8_t pin, uint8_t state, unsigned long timeout);

void setup(void);
void loop(void);

/* The device profile's name for an Arduino pin number; programs may override it. */
const char* wr_arduino_pin_name(uint8_t pin);
/* The device profile's name for an analog input (A0 -> "a0"); programs may override it. */
const char* wr_arduino_analog_name(uint8_t pin);

#ifdef __cplusplus
}

#include "WString.h"
#include "Print.h"
#include "Stream.h"
#include "Serial.h"
#include "HardwareSerial.h"

template <class T, class L>
auto min(const T& a, const L& b) -> decltype((b < a) ? b : a)
{
    return (b < a) ? b : a;
}

template <class T, class L>
auto max(const T& a, const L& b) -> decltype((b < a) ? b : a)
{
    return (a < b) ? b : a;
}

template <class T, class L, class H>
T constrain(T value, L low, H high)
{
    return value < low ? (T)low : value > high ? (T)high : value;
}

long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);
long map(long value, long from_low, long from_high, long to_low, long to_high);
uint16_t makeWord(uint16_t w);
uint16_t makeWord(uint8_t high, uint8_t low);
void tone(uint8_t pin, unsigned int frequency, unsigned long duration = 0);
void noTone(uint8_t pin);
#endif

#endif
