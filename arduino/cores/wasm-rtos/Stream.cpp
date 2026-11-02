#include "Arduino.h"

int Stream::timedRead()
{
    unsigned long start = millis();
    do
    {
        int c = read();
        if (c >= 0)
            return c;
        yield();
    } while (millis() - start < timeout_);
    return -1;
}

int Stream::timedPeek()
{
    unsigned long start = millis();
    do
    {
        int c = peek();
        if (c >= 0)
            return c;
        yield();
    } while (millis() - start < timeout_);
    return -1;
}

int Stream::peekNextDigit(LookaheadMode lookahead, bool detect_decimal)
{
    for (;;)
    {
        int c = timedPeek();
        if (c < 0 || c == '-' || (c >= '0' && c <= '9') || (detect_decimal && c == '.'))
            return c;
        if (lookahead == SKIP_NONE || (lookahead == SKIP_WHITESPACE && c != ' ' && c != '\t' && c != '\r' && c != '\n'))
            return c;
        read();
    }
}

bool Stream::find(const char* target) { return find(target, strlen(target)); }

bool Stream::find(const char* target, size_t length)
{
    size_t matched = 0;
    if (length == 0)
        return true;
    for (int c; (c = timedRead()) >= 0;)
    {
        matched = c == target[matched] ? matched + 1 : c == target[0] ? 1 : 0;
        if (matched == length)
            return true;
    }
    return false;
}

bool Stream::findUntil(const char* target, const char* terminator)
{
    size_t target_length = strlen(target);
    size_t terminator_length = terminator != nullptr ? strlen(terminator) : 0;
    size_t matched = 0;
    size_t terminated = 0;
    for (int c; (c = timedRead()) >= 0;)
    {
        matched = c == target[matched] ? matched + 1 : c == target[0] ? 1 : 0;
        if (matched == target_length)
            return true;
        if (terminator_length > 0)
        {
            terminated = c == terminator[terminated] ? terminated + 1 : c == terminator[0] ? 1 : 0;
            if (terminated == terminator_length)
                return false;
        }
    }
    return false;
}

long Stream::parseInt(LookaheadMode lookahead, char ignore)
{
    bool negative = false;
    long value = 0;
    int c = peekNextDigit(lookahead, false);
    if (c < 0)
        return 0;
    do
    {
        if (c == ignore)
            ;
        else if (c == '-')
            negative = true;
        else if (c >= '0' && c <= '9')
            value = value * 10 + c - '0';
        read();
        c = timedPeek();
    } while ((c >= '0' && c <= '9') || c == ignore);
    return negative ? -value : value;
}

float Stream::parseFloat(LookaheadMode lookahead, char ignore)
{
    bool negative = false;
    bool fraction = false;
    double value = 0.0;
    double scale = 1.0;
    int c = peekNextDigit(lookahead, true);
    if (c < 0)
        return 0.0f;
    do
    {
        if (c == ignore)
            ;
        else if (c == '-')
            negative = true;
        else if (c == '.')
            fraction = true;
        else if (c >= '0' && c <= '9')
        {
            value = value * 10.0 + (c - '0');
            if (fraction)
                scale *= 10.0;
        }
        read();
        c = timedPeek();
    } while ((c >= '0' && c <= '9') || (c == '.' && !fraction) || c == ignore);
    value /= scale;
    return (float)(negative ? -value : value);
}

size_t Stream::readBytes(char* buffer, size_t length)
{
    size_t count = 0;
    while (count < length)
    {
        int c = timedRead();
        if (c < 0)
            break;
        buffer[count++] = (char)c;
    }
    return count;
}

size_t Stream::readBytesUntil(char terminator, char* buffer, size_t length)
{
    size_t count = 0;
    while (count < length)
    {
        int c = timedRead();
        if (c < 0 || c == terminator)
            break;
        buffer[count++] = (char)c;
    }
    return count;
}

String Stream::readString()
{
    String text;
    for (int c; (c = timedRead()) >= 0;)
        text += (char)c;
    return text;
}

String Stream::readStringUntil(char terminator)
{
    String text;
    for (int c; (c = timedRead()) >= 0 && c != terminator;)
        text += (char)c;
    return text;
}
