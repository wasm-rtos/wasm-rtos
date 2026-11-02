/* Arduino's Stream: Print plus reading, with a timeout for the parsing helpers. */
#ifndef WASM_RTOS_ARDUINO_STREAM_H
#define WASM_RTOS_ARDUINO_STREAM_H

#include "Print.h"

enum LookaheadMode
{
    SKIP_ALL,
    SKIP_NONE,
    SKIP_WHITESPACE
};

class Stream : public Print
{
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;

    void setTimeout(unsigned long timeout_ms) { timeout_ = timeout_ms; }
    unsigned long getTimeout() const { return timeout_; }

    bool find(const char* target);
    bool find(const char* target, size_t length);
    bool find(char target) { return find(&target, 1); }
    bool findUntil(const char* target, const char* terminator);

    long parseInt(LookaheadMode lookahead = SKIP_ALL, char ignore = '\x01');
    float parseFloat(LookaheadMode lookahead = SKIP_ALL, char ignore = '\x01');

    size_t readBytes(char* buffer, size_t length);
    size_t readBytes(uint8_t* buffer, size_t length) { return readBytes((char*)buffer, length); }
    size_t readBytesUntil(char terminator, char* buffer, size_t length);
    size_t readBytesUntil(char terminator, uint8_t* buffer, size_t length)
    {
        return readBytesUntil(terminator, (char*)buffer, length);
    }
    String readString();
    String readStringUntil(char terminator);

protected:
    int timedRead();
    int timedPeek();
    int peekNextDigit(LookaheadMode lookahead, bool detect_decimal);

    unsigned long timeout_ = 1000;
};

#endif
