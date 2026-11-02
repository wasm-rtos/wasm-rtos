/*
 * SPI: Arduino's SPI over the wasm-rtos spi interface, on bus "spi0". The
 * sketch drives chip select with digitalWrite, as Arduino libraries do, so
 * the program takes the whole bus while it uses it.
 */
#ifndef WASM_RTOS_ARDUINO_SPI_H
#define WASM_RTOS_ARDUINO_SPI_H

#include <Arduino.h>

#define SPI_MODE0 0x00
#define SPI_MODE1 0x01
#define SPI_MODE2 0x02
#define SPI_MODE3 0x03
#define SPI_HAS_TRANSACTION 1

/* Old-style clock dividers, from a 16 MHz clock. */
#define SPI_CLOCK_DIV2 2
#define SPI_CLOCK_DIV4 4
#define SPI_CLOCK_DIV8 8
#define SPI_CLOCK_DIV16 16
#define SPI_CLOCK_DIV32 32
#define SPI_CLOCK_DIV64 64
#define SPI_CLOCK_DIV128 128

class SPISettings
{
public:
    SPISettings() : clock_(4000000), bit_order_(MSBFIRST), data_mode_(SPI_MODE0) {}
    SPISettings(uint32_t clock, uint8_t bit_order, uint8_t data_mode)
        : clock_(clock), bit_order_(bit_order), data_mode_(data_mode)
    {
    }
    bool operator==(const SPISettings& other) const
    {
        return clock_ == other.clock_ && bit_order_ == other.bit_order_ && data_mode_ == other.data_mode_;
    }
    bool operator!=(const SPISettings& other) const { return !(*this == other); }

private:
    friend class SPIClass;
    uint32_t clock_;
    uint8_t bit_order_;
    uint8_t data_mode_;
};

class SPIClass
{
public:
    explicit SPIClass(const char* bus) : bus_(bus) {}

    void begin() {}
    void end();
    void beginTransaction(SPISettings settings);
    void endTransaction() {}
    void usingInterrupt(int interrupt) { (void)interrupt; }
    void notUsingInterrupt(int interrupt) { (void)interrupt; }

    uint8_t transfer(uint8_t data);
    uint16_t transfer16(uint16_t data);
    void transfer(void* buffer, size_t count);
    /* Sends without keeping what comes back. */
    void writeBytes(const uint8_t* data, size_t count);
    void transferBytes(const uint8_t* out, uint8_t* in, size_t count);

    void setBitOrder(uint8_t bit_order);
    void setDataMode(uint8_t data_mode);
    void setClockDivider(uint8_t divider);
    void setFrequency(uint32_t frequency_hz);

private:
    int32_t device();

    const char* bus_;
    int32_t handle_ = 0;
    SPISettings settings_;
    SPISettings opened_;
};

extern SPIClass SPI;

#endif
