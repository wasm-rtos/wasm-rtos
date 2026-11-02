#include "Arduino.h"

#include <ctype.h>
#include <stdio.h>

/* Digits of value in base (2-36), most significant first, into out (at least 66 bytes). */
static void format_unsigned(unsigned long long value, unsigned char base, char* out)
{
    char digits[65];
    int count = 0;
    if (base < 2 || base > 36)
        base = 10;
    do
    {
        int digit = (int)(value % base);
        digits[count++] = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
        value /= base;
    } while (value != 0);
    for (int index = 0; index < count; ++index)
        out[index] = digits[count - 1 - index];
    out[count] = '\0';
}

static void format_signed(long long value, unsigned char base, char* out)
{
    /* Arduino prints negative numbers with a sign only in base 10. */
    if (value < 0 && base == 10)
    {
        out[0] = '-';
        format_unsigned((unsigned long long)(-(value + 1)) + 1u, base, out + 1);
    }
    else
    {
        format_unsigned((unsigned long long)value, base, out);
    }
}

void String::init()
{
    buffer_ = nullptr;
    capacity_ = 0;
    length_ = 0;
}

bool String::reserve(unsigned int size)
{
    if (buffer_ != nullptr && capacity_ >= size)
        return true;
    char* grown = (char*)realloc(buffer_, size + 1u);
    if (grown == nullptr)
        return false;
    if (buffer_ == nullptr)
        grown[0] = '\0';
    buffer_ = grown;
    capacity_ = size;
    return true;
}

bool String::copy(const char* text, unsigned int length)
{
    if (!reserve(length))
    {
        free(buffer_);
        init();
        return false;
    }
    memmove(buffer_, text, length);
    buffer_[length] = '\0';
    length_ = length;
    return true;
}

String::String(const char* text) { copy(text != nullptr ? text : "", text != nullptr ? (unsigned int)strlen(text) : 0u); }
String::String(const char* text, unsigned int length) { copy(text, length); }
String::String(const String& other) { copy(other.c_str(), other.length_); }
String::String(String&& other) : buffer_(other.buffer_), capacity_(other.capacity_), length_(other.length_) { other.init(); }
String::String(const __FlashStringHelper* text) : String(reinterpret_cast<const char*>(text)) {}
String::String(char c) { copy(&c, 1u); }

#define STRING_FROM_NUMBER(type, formatter, cast)              \
    String::String(type value, unsigned char base)             \
    {                                                          \
        char text[68];                                         \
        formatter((cast)value, base, text);                    \
        copy(text, (unsigned int)strlen(text));                \
    }
STRING_FROM_NUMBER(unsigned char, format_unsigned, unsigned long long)
STRING_FROM_NUMBER(int, format_signed, long long)
STRING_FROM_NUMBER(unsigned int, format_unsigned, unsigned long long)
STRING_FROM_NUMBER(long, format_signed, long long)
STRING_FROM_NUMBER(unsigned long, format_unsigned, unsigned long long)
STRING_FROM_NUMBER(long long, format_signed, long long)
STRING_FROM_NUMBER(unsigned long long, format_unsigned, unsigned long long)

String::String(float value, unsigned char decimals) : String((double)value, decimals) {}
String::String(double value, unsigned char decimals)
{
    char text[64];
    snprintf(text, sizeof(text), "%.*f", (int)decimals, value);
    copy(text, (unsigned int)strlen(text));
}

String::~String() { free(buffer_); }

String& String::operator=(const String& other)
{
    if (this != &other)
        copy(other.c_str(), other.length_);
    return *this;
}

String& String::operator=(String&& other)
{
    if (this != &other)
    {
        free(buffer_);
        buffer_ = other.buffer_;
        capacity_ = other.capacity_;
        length_ = other.length_;
        other.init();
    }
    return *this;
}

String& String::operator=(const char* text)
{
    copy(text != nullptr ? text : "", text != nullptr ? (unsigned int)strlen(text) : 0u);
    return *this;
}

String& String::operator=(const __FlashStringHelper* text) { return *this = reinterpret_cast<const char*>(text); }

bool String::concat(const char* text, unsigned int length)
{
    if (text == nullptr || !reserve(length_ + length))
        return false;
    memmove(buffer_ + length_, text, length);
    length_ += length;
    buffer_[length_] = '\0';
    return true;
}

bool String::concat(const String& other) { return concat(other.c_str(), other.length_); }
bool String::concat(const char* text) { return text != nullptr && concat(text, (unsigned int)strlen(text)); }
bool String::concat(const __FlashStringHelper* text) { return concat(reinterpret_cast<const char*>(text)); }
bool String::concat(char c) { return concat(&c, 1u); }
bool String::concat(unsigned char value) { return concat(String(value)); }
bool String::concat(int value) { return concat(String(value)); }
bool String::concat(unsigned int value) { return concat(String(value)); }
bool String::concat(long value) { return concat(String(value)); }
bool String::concat(unsigned long value) { return concat(String(value)); }
bool String::concat(long long value) { return concat(String(value)); }
bool String::concat(unsigned long long value) { return concat(String(value)); }
bool String::concat(float value) { return concat(String(value)); }
bool String::concat(double value) { return concat(String(value)); }

int String::compareTo(const String& other) const { return strcmp(c_str(), other.c_str()); }
bool String::equals(const String& other) const { return length_ == other.length_ && compareTo(other) == 0; }
bool String::equals(const char* text) const { return strcmp(c_str(), text != nullptr ? text : "") == 0; }

bool String::equalsIgnoreCase(const String& other) const
{
    if (length_ != other.length_)
        return false;
    for (unsigned int index = 0; index < length_; ++index)
        if (tolower((unsigned char)buffer_[index]) != tolower((unsigned char)other.buffer_[index]))
            return false;
    return true;
}

bool String::startsWith(const String& prefix, unsigned int offset) const
{
    return offset <= length_ && prefix.length_ <= length_ - offset &&
           strncmp(c_str() + offset, prefix.c_str(), prefix.length_) == 0;
}

bool String::startsWith(const String& prefix) const { return startsWith(prefix, 0); }

bool String::endsWith(const String& suffix) const
{
    return suffix.length_ <= length_ && strcmp(c_str() + length_ - suffix.length_, suffix.c_str()) == 0;
}

char String::charAt(unsigned int index) const { return index < length_ ? buffer_[index] : '\0'; }

void String::setCharAt(unsigned int index, char c)
{
    if (index < length_)
        buffer_[index] = c;
}

char String::operator[](unsigned int index) const { return charAt(index); }

char& String::operator[](unsigned int index)
{
    static char outside;
    outside = '\0';
    return index < length_ ? buffer_[index] : outside;
}

void String::getBytes(unsigned char* out, unsigned int size, unsigned int index) const
{
    toCharArray((char*)out, size, index);
}

void String::toCharArray(char* out, unsigned int size, unsigned int index) const
{
    unsigned int count;
    if (out == nullptr || size == 0)
        return;
    if (index >= length_)
    {
        out[0] = '\0';
        return;
    }
    count = length_ - index < size - 1 ? length_ - index : size - 1;
    memcpy(out, buffer_ + index, count);
    out[count] = '\0';
}

int String::indexOf(char c, unsigned int from) const
{
    const char* found = from < length_ ? strchr(buffer_ + from, c) : nullptr;
    return found != nullptr ? (int)(found - buffer_) : -1;
}

int String::indexOf(const String& text, unsigned int from) const
{
    const char* found = from <= length_ ? strstr(c_str() + from, text.c_str()) : nullptr;
    return found != nullptr ? (int)(found - c_str()) : -1;
}

int String::lastIndexOf(char c) const { return length_ == 0 ? -1 : lastIndexOf(c, length_ - 1); }

int String::lastIndexOf(char c, unsigned int from) const
{
    if (length_ == 0)
        return -1;
    for (int index = (int)(from < length_ ? from : length_ - 1); index >= 0; --index)
        if (buffer_[index] == c)
            return index;
    return -1;
}

int String::lastIndexOf(const String& text) const
{
    return text.length_ > length_ ? -1 : lastIndexOf(text, length_ - text.length_);
}

int String::lastIndexOf(const String& text, unsigned int from) const
{
    if (text.length_ > length_)
        return -1;
    for (int index = (int)(from < length_ - text.length_ ? from : length_ - text.length_); index >= 0; --index)
        if (strncmp(buffer_ + index, text.c_str(), text.length_) == 0)
            return index;
    return -1;
}

String String::substring(unsigned int from, unsigned int to) const
{
    if (from > to)
    {
        unsigned int swap = from;
        from = to;
        to = swap;
    }
    if (from >= length_)
        return String();
    if (to > length_)
        to = length_;
    return String(buffer_ + from, to - from);
}

void String::replace(char find, char replacement)
{
    for (unsigned int index = 0; index < length_; ++index)
        if (buffer_[index] == find)
            buffer_[index] = replacement;
}

void String::replace(const String& find, const String& replacement)
{
    String result;
    unsigned int index = 0;
    if (find.length_ == 0 || length_ == 0)
        return;
    while (index < length_)
    {
        int found = indexOf(find, index);
        if (found < 0)
        {
            result.concat(buffer_ + index, length_ - index);
            break;
        }
        result.concat(buffer_ + index, (unsigned int)found - index);
        result.concat(replacement);
        index = (unsigned int)found + find.length_;
    }
    *this = static_cast<String&&>(result);
}

void String::remove(unsigned int index) { remove(index, (unsigned int)-1); }

void String::remove(unsigned int index, unsigned int count)
{
    if (index >= length_)
        return;
    if (count > length_ - index)
        count = length_ - index;
    memmove(buffer_ + index, buffer_ + index + count, length_ - index - count + 1u);
    length_ -= count;
}

void String::toLowerCase()
{
    for (unsigned int index = 0; index < length_; ++index)
        buffer_[index] = (char)tolower((unsigned char)buffer_[index]);
}

void String::toUpperCase()
{
    for (unsigned int index = 0; index < length_; ++index)
        buffer_[index] = (char)toupper((unsigned char)buffer_[index]);
}

void String::trim()
{
    unsigned int begin = 0;
    unsigned int end = length_;
    if (buffer_ == nullptr)
        return;
    while (begin < end && isspace((unsigned char)buffer_[begin]))
        ++begin;
    while (end > begin && isspace((unsigned char)buffer_[end - 1]))
        --end;
    memmove(buffer_, buffer_ + begin, end - begin);
    length_ = end - begin;
    buffer_[length_] = '\0';
}

long String::toInt() const { return atol(c_str()); }
float String::toFloat() const { return (float)atof(c_str()); }
double String::toDouble() const { return atof(c_str()); }

String operator+(const String& left, const String& right)
{
    String result(left);
    result.concat(right);
    return result;
}

#define STRING_PLUS(type)                                     \
    String operator+(const String& left, type right)          \
    {                                                         \
        String result(left);                                  \
        result.concat(right);                                 \
        return result;                                        \
    }
STRING_PLUS(const char*)
STRING_PLUS(char)
STRING_PLUS(int)
STRING_PLUS(unsigned int)
STRING_PLUS(long)
STRING_PLUS(unsigned long)
STRING_PLUS(float)
STRING_PLUS(double)

String operator+(const char* left, const String& right)
{
    String result(left);
    result.concat(right);
    return result;
}
