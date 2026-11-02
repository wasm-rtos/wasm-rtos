/* Serial is the program's standard output (and input): the system shows it, keeps it per program, or sends it to a UART. */
#ifndef WASM_RTOS_ARDUINO_SERIAL_H
#define WASM_RTOS_ARDUINO_SERIAL_H

#include "Stream.h"

#define SERIAL_8N1 0x06

class StdioSerial : public Stream
{
public:
    void begin(unsigned long baud, uint8_t config = SERIAL_8N1)
    {
        (void)baud;
        (void)config;
    }
    void end() {}
    int available() override;
    int read() override;
    int peek() override;
    int availableForWrite() override { return 4096; }
    size_t write(uint8_t byte) override;
    size_t write(const uint8_t* buffer, size_t size) override;
    using Print::write;
    /* Integers are bytes, as on Arduino: write(0x00) is not a null pointer. */
    size_t write(unsigned long n) { return write((uint8_t)n); }
    size_t write(long n) { return write((uint8_t)n); }
    size_t write(unsigned int n) { return write((uint8_t)n); }
    size_t write(int n) { return write((uint8_t)n); }
    void flush() override;
    explicit operator bool() const { return true; }
};

extern StdioSerial Serial;

#endif
