/* Arduino's Print: text and numbers out of anything that can write bytes. */
#ifndef WASM_RTOS_ARDUINO_PRINT_H
#define WASM_RTOS_ARDUINO_PRINT_H

#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "Printable.h"
#include "WString.h"

class Print
{
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t byte) = 0;
    virtual size_t write(const uint8_t* buffer, size_t size);
    size_t write(const char* text) { return text != nullptr ? write((const uint8_t*)text, strlen(text)) : 0; }
    size_t write(const char* buffer, size_t size) { return write((const uint8_t*)buffer, size); }
    virtual int availableForWrite() { return 0; }
    virtual void flush() {}

    int getWriteError() const { return write_error_; }
    void clearWriteError() { write_error_ = 0; }

    size_t print(const __FlashStringHelper* text);
    size_t print(const String& text);
    size_t print(const char* text);
    size_t print(char c);
    size_t print(unsigned char value, int base = 10);
    size_t print(int value, int base = 10);
    size_t print(unsigned int value, int base = 10);
    size_t print(long value, int base = 10);
    size_t print(unsigned long value, int base = 10);
    size_t print(long long value, int base = 10);
    size_t print(unsigned long long value, int base = 10);
    size_t print(double value, int digits = 2);
    size_t print(const Printable& object);

    size_t println();
    template <typename T>
    size_t println(const T& value)
    {
        size_t count = print(value);
        return count + println();
    }
    template <typename T>
    size_t println(const T& value, int format)
    {
        size_t count = print(value, format);
        return count + println();
    }

    size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
    size_t vprintf(const char* format, va_list args);

protected:
    void setWriteError(int error = 1) { write_error_ = error; }

private:
    size_t printNumber(unsigned long long value, int base);
    int write_error_ = 0;
};

#endif
