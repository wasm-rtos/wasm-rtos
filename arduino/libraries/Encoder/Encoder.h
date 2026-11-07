/*
 * Encoder: the classic Arduino Encoder API over the wasm-rtos counter
 * interface. Encoder(A, B) opens the quadrature encoder registered under
 * pin A's name ("gpioA", see wr_arduino_pin_name) and counts every edge of
 * both signals (x4), as the classic library does. The hardware counts, so
 * no step is lost while the sketch is busy. Without such an encoder, read()
 * stays 0.
 */
#ifndef WASM_RTOS_ARDUINO_ENCODER_H
#define WASM_RTOS_ARDUINO_ENCODER_H

#include <Arduino.h>

class Encoder
{
public:
    Encoder(uint8_t pin1, uint8_t pin2);

    int32_t read();
    void write(int32_t position);
    int32_t readAndReset();

private:
    int32_t raw();

    int32_t counter_ = 0;
    /* The position is the hardware's count minus this, so resetting loses no step. */
    int32_t offset_ = 0;
};

#endif
