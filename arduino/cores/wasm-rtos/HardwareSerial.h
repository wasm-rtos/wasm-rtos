/* Serial1 and Serial2: the device profile's "uart1" and "uart2". */
#ifndef WASM_RTOS_ARDUINO_HARDWARE_SERIAL_H
#define WASM_RTOS_ARDUINO_HARDWARE_SERIAL_H

#include "Stream.h"

/* Frame formats with the classic boards' values: data bits, parity, stop bits. */
#define SERIAL_7N1 0x04
#define SERIAL_7N2 0x0C
#define SERIAL_8N2 0x0E
#define SERIAL_7E1 0x24
#define SERIAL_8E1 0x26
#define SERIAL_7E2 0x2C
#define SERIAL_8E2 0x2E
#define SERIAL_7O1 0x34
#define SERIAL_8O1 0x36
#define SERIAL_7O2 0x3C
#define SERIAL_8O2 0x3E

class HardwareSerial : public Stream
{
public:
    explicit HardwareSerial(const char* name) : name_(name) {}

    void begin(unsigned long baud, uint32_t config = 0x06);
    void end();
    int available() override;
    int read() override;
    int peek() override;
    int availableForWrite() override;
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* buffer, size_t size) override;
    using Print::write;
    /* Integers are bytes, as on Arduino. */
    size_t write(unsigned long n) { return write((uint8_t)n); }
    size_t write(long n) { return write((uint8_t)n); }
    size_t write(unsigned int n) { return write((uint8_t)n); }
    size_t write(int n) { return write((uint8_t)n); }
    void flush() override;
    unsigned long baudRate() const { return baud_; }
    explicit operator bool() const { return port_ > 0; }

private:
    bool fill();

    const char* name_;
    int32_t port_ = 0;
    unsigned long baud_ = 0;
    uint8_t buffer_[64];
    size_t length_ = 0;
    size_t index_ = 0;
};

extern HardwareSerial Serial1;
extern HardwareSerial Serial2;

#endif
