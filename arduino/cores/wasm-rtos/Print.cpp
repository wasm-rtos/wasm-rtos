#include "Arduino.h"

#include <stdio.h>

size_t Print::write(const uint8_t* buffer, size_t size)
{
    size_t count = 0;
    while (size-- > 0)
    {
        if (write(*buffer++) == 0)
            break;
        ++count;
    }
    return count;
}

size_t Print::print(const __FlashStringHelper* text) { return print(reinterpret_cast<const char*>(text)); }
size_t Print::print(const String& text) { return write((const uint8_t*)text.c_str(), text.length()); }
size_t Print::print(const char* text) { return write(text); }
size_t Print::print(char c) { return write((uint8_t)c); }
size_t Print::print(unsigned char value, int base) { return print((unsigned long long)value, base); }
size_t Print::print(int value, int base) { return print((long long)value, base); }
size_t Print::print(unsigned int value, int base) { return print((unsigned long long)value, base); }
size_t Print::print(long value, int base) { return print((long long)value, base); }
size_t Print::print(unsigned long value, int base) { return print((unsigned long long)value, base); }

/* Base 0 writes the byte itself, as Arduino does; base 10 prints a sign. */
size_t Print::print(long long value, int base)
{
    if (base == 0)
        return write((uint8_t)value);
    if (base == 10 && value < 0)
        return print('-') + printNumber((unsigned long long)(-(value + 1)) + 1u, 10);
    return printNumber((unsigned long long)value, base);
}

size_t Print::print(unsigned long long value, int base)
{
    return base == 0 ? write((uint8_t)value) : printNumber(value, base);
}

size_t Print::printNumber(unsigned long long value, int base)
{
    char text[65];
    char* cursor = &text[sizeof(text) - 1];
    if (base < 2 || base > 36)
        base = 10;
    *cursor = '\0';
    do
    {
        int digit = (int)(value % (unsigned)base);
        *--cursor = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
        value /= (unsigned)base;
    } while (value != 0);
    return write(cursor);
}

size_t Print::print(double value, int digits)
{
    char text[64];
    if (isnan(value))
        return print("nan");
    if (isinf(value))
        return print("inf");
    if (digits < 0)
        digits = 0;
    snprintf(text, sizeof(text), "%.*f", digits, value);
    return print(text);
}

size_t Print::print(const Printable& object) { return object.printTo(*this); }

size_t Print::println() { return write("\r\n"); }

size_t Print::printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    size_t count = vprintf(format, args);
    va_end(args);
    return count;
}

size_t Print::vprintf(const char* format, va_list args)
{
    char small[128];
    va_list copy;
    int length;
    va_copy(copy, args);
    length = vsnprintf(small, sizeof(small), format, copy);
    va_end(copy);
    if (length < 0)
        return 0;
    if ((size_t)length < sizeof(small))
        return write((const uint8_t*)small, (size_t)length);
    char* large = (char*)malloc((size_t)length + 1u);
    if (large == nullptr)
        return 0;
    vsnprintf(large, (size_t)length + 1u, format, args);
    size_t count = write((const uint8_t*)large, (size_t)length);
    free(large);
    return count;
}
