#include "Arduino.h"

#include <stdio.h>

StdioSerial Serial;

/* Standard input has no "how much is waiting" on every device: report none rather than block. */
int StdioSerial::available() { return 0; }
int StdioSerial::read() { return -1; }
int StdioSerial::peek() { return -1; }

size_t StdioSerial::write(uint8_t byte) { return write(&byte, 1); }

/* Whole lines reach the system at once; flush() sends what is left. */
size_t StdioSerial::write(const uint8_t* buffer, size_t size)
{
    size_t written = fwrite(buffer, 1, size, stdout);
    if (memchr(buffer, '\n', size) != nullptr)
        fflush(stdout);
    return written;
}

void StdioSerial::flush() { fflush(stdout); }
