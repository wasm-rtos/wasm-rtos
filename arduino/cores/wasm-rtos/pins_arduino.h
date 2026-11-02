/*
 * The board variant. Bus pins belong to the system's i2c and spi
 * interfaces, so their numbers lead nowhere: pinMode(SS, OUTPUT) and the
 * like do nothing. Other pin numbers are the device profile's "gpioN".
 */
#ifndef WASM_RTOS_ARDUINO_PINS_H
#define WASM_RTOS_ARDUINO_PINS_H

#include <stdint.h>

#define NUM_DIGITAL_PINS 240
#define NUM_ANALOG_INPUTS 8
#define digitalPinToPort(pin) (0)
#define digitalPinToBitMask(pin) (0)
#define portOutputRegister(port) ((volatile uint32_t*)0)
#define portInputRegister(port) ((volatile uint32_t*)0)
#define digitalPinIsValid(pin) ((pin) < NUM_DIGITAL_PINS)

static const uint8_t SS = 240;
static const uint8_t MOSI = 241;
static const uint8_t MISO = 242;
static const uint8_t SCK = 243;
static const uint8_t SDA = 244;
static const uint8_t SCL = 245;

/* Analog inputs: A0 is the device profile's "a0", and so on; analogRead(0) means A0 too. */
static const uint8_t A0 = 200;
static const uint8_t A1 = 201;
static const uint8_t A2 = 202;
static const uint8_t A3 = 203;
static const uint8_t A4 = 204;
static const uint8_t A5 = 205;
static const uint8_t A6 = 206;
static const uint8_t A7 = 207;

#endif
