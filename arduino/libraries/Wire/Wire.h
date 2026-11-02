/*
 * Wire: Arduino's I2C over the wasm-rtos i2c interface. Wire is bus "i2c0",
 * Wire1 is "i2c1". endTransmission(false) keeps the write for the next
 * requestFrom, so a register read is one transaction with a repeated start.
 * The bus speed is the device profile's; this device is always the
 * controller.
 */
#ifndef WASM_RTOS_ARDUINO_WIRE_H
#define WASM_RTOS_ARDUINO_WIRE_H

#include <Arduino.h>

#define WIRE_BUFFER_SIZE 128
#define BUFFER_LENGTH WIRE_BUFFER_SIZE
#define WIRE_HAS_END 1

class TwoWire : public Stream
{
public:
    explicit TwoWire(const char* bus);

    void begin() {}
    void end();
    void setClock(uint32_t frequency_hz) { (void)frequency_hz; }
    void setWireTimeout(uint32_t timeout_us = 25000, bool reset = false)
    {
        (void)timeout_us;
        (void)reset;
    }

    void beginTransmission(uint8_t address);
    void beginTransmission(int address) { beginTransmission((uint8_t)address); }
    /* 0 sent, 1 too much data, 2 not acknowledged, 4 other error, 5 timeout. */
    uint8_t endTransmission() { return endTransmission((uint8_t)1); }
    uint8_t endTransmission(uint8_t send_stop);

    /* The overloads of the classic boards, which libraries call with these exact types. */
    size_t requestFrom(uint8_t address, size_t quantity, bool send_stop);
    size_t requestFrom(uint8_t address, size_t quantity) { return requestFrom(address, quantity, true); }
    uint8_t requestFrom(uint8_t address, uint8_t quantity) { return (uint8_t)requestFrom(address, (size_t)quantity, true); }
    uint8_t requestFrom(uint8_t address, uint8_t quantity, uint8_t send_stop)
    {
        return (uint8_t)requestFrom(address, (size_t)quantity, send_stop != 0);
    }
    uint8_t requestFrom(int address, int quantity) { return (uint8_t)requestFrom((uint8_t)address, (size_t)quantity, true); }
    uint8_t requestFrom(int address, int quantity, int send_stop)
    {
        return (uint8_t)requestFrom((uint8_t)address, (size_t)quantity, send_stop != 0);
    }
    /* Writes an internal address of size bytes (most significant first), then reads. */
    uint8_t requestFrom(uint8_t address, uint8_t quantity, uint32_t internal, uint8_t size, uint8_t send_stop);

    size_t write(uint8_t byte) override;
    size_t write(const uint8_t* data, size_t length) override;
    using Print::write;
    /* Integers are bytes, as on Arduino: write(0x00) is not a null pointer. */
    size_t write(unsigned long n) { return write((uint8_t)n); }
    size_t write(long n) { return write((uint8_t)n); }
    size_t write(unsigned int n) { return write((uint8_t)n); }
    size_t write(int n) { return write((uint8_t)n); }
    int available() override { return (int)(rx_length_ - rx_index_); }
    int read() override { return rx_index_ < rx_length_ ? rx_buffer_[rx_index_++] : -1; }
    int peek() override { return rx_index_ < rx_length_ ? rx_buffer_[rx_index_] : -1; }
    void flush() override {}

private:
    int32_t device(uint8_t address);

    const char* bus_;
    /* Handles of the addresses this program talks to, kept open. */
    int32_t handles_[128] = {};
    uint8_t tx_address_ = 0;
    uint8_t tx_buffer_[WIRE_BUFFER_SIZE];
    size_t tx_length_ = 0;
    bool tx_overflow_ = false;
    bool transmitting_ = false;
    /* A write ended without a stop, sent together with the next read from the same address. */
    bool pending_ = false;
    uint8_t rx_buffer_[WIRE_BUFFER_SIZE];
    size_t rx_length_ = 0;
    size_t rx_index_ = 0;
};

extern TwoWire Wire;
extern TwoWire Wire1;

#endif
