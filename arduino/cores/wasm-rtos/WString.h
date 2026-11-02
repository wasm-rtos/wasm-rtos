/* Arduino's String: a growable text that keeps a terminating NUL. */
#ifndef WASM_RTOS_ARDUINO_WSTRING_H
#define WASM_RTOS_ARDUINO_WSTRING_H

#include <stddef.h>
#include <stdint.h>

/* F("text"): flash strings are ordinary strings here; the type keeps overloads apart. */
class __FlashStringHelper;
#define F(literal) (reinterpret_cast<const __FlashStringHelper*>(literal))
#define FPSTR(pointer) (reinterpret_cast<const __FlashStringHelper*>(pointer))

class String
{
public:
    String(const char* text = "");
    String(const char* text, unsigned int length);
    String(const String& other);
    String(String&& other);
    String(const __FlashStringHelper* text);
    explicit String(char c);
    explicit String(unsigned char value, unsigned char base = 10);
    explicit String(int value, unsigned char base = 10);
    explicit String(unsigned int value, unsigned char base = 10);
    explicit String(long value, unsigned char base = 10);
    explicit String(unsigned long value, unsigned char base = 10);
    explicit String(long long value, unsigned char base = 10);
    explicit String(unsigned long long value, unsigned char base = 10);
    explicit String(float value, unsigned char decimals = 2);
    explicit String(double value, unsigned char decimals = 2);
    ~String();

    String& operator=(const String& other);
    String& operator=(String&& other);
    String& operator=(const char* text);
    String& operator=(const __FlashStringHelper* text);

    bool reserve(unsigned int size);
    unsigned int length() const { return length_; }
    bool isEmpty() const { return length_ == 0; }
    const char* c_str() const { return buffer_ != nullptr ? buffer_ : ""; }
    explicit operator bool() const { return buffer_ != nullptr; }

    bool concat(const String& other);
    bool concat(const char* text);
    bool concat(const char* text, unsigned int length);
    bool concat(const __FlashStringHelper* text);
    bool concat(char c);
    bool concat(unsigned char value);
    bool concat(int value);
    bool concat(unsigned int value);
    bool concat(long value);
    bool concat(unsigned long value);
    bool concat(long long value);
    bool concat(unsigned long long value);
    bool concat(float value);
    bool concat(double value);
    template <typename T>
    String& operator+=(const T& value)
    {
        concat(value);
        return *this;
    }

    int compareTo(const String& other) const;
    bool equals(const String& other) const;
    bool equals(const char* text) const;
    bool equalsIgnoreCase(const String& other) const;
    bool startsWith(const String& prefix) const;
    bool startsWith(const String& prefix, unsigned int offset) const;
    bool endsWith(const String& suffix) const;
    bool operator==(const String& other) const { return equals(other); }
    bool operator==(const char* text) const { return equals(text); }
    bool operator!=(const String& other) const { return !equals(other); }
    bool operator!=(const char* text) const { return !equals(text); }
    bool operator<(const String& other) const { return compareTo(other) < 0; }
    bool operator>(const String& other) const { return compareTo(other) > 0; }
    bool operator<=(const String& other) const { return compareTo(other) <= 0; }
    bool operator>=(const String& other) const { return compareTo(other) >= 0; }

    char charAt(unsigned int index) const;
    void setCharAt(unsigned int index, char c);
    char operator[](unsigned int index) const;
    char& operator[](unsigned int index);
    void getBytes(unsigned char* out, unsigned int size, unsigned int index = 0) const;
    void toCharArray(char* out, unsigned int size, unsigned int index = 0) const;

    int indexOf(char c, unsigned int from = 0) const;
    int indexOf(const String& text, unsigned int from = 0) const;
    int lastIndexOf(char c) const;
    int lastIndexOf(char c, unsigned int from) const;
    int lastIndexOf(const String& text) const;
    int lastIndexOf(const String& text, unsigned int from) const;
    String substring(unsigned int from) const { return substring(from, length_); }
    String substring(unsigned int from, unsigned int to) const;

    void replace(char find, char replacement);
    void replace(const String& find, const String& replacement);
    void remove(unsigned int index);
    void remove(unsigned int index, unsigned int count);
    void toLowerCase();
    void toUpperCase();
    void trim();

    long toInt() const;
    float toFloat() const;
    double toDouble() const;

private:
    void init();
    bool copy(const char* text, unsigned int length);

    char* buffer_ = nullptr;
    unsigned int capacity_ = 0;
    unsigned int length_ = 0;
};

String operator+(const String& left, const String& right);
String operator+(const String& left, const char* right);
String operator+(const char* left, const String& right);
String operator+(const String& left, char right);
String operator+(const String& left, int right);
String operator+(const String& left, unsigned int right);
String operator+(const String& left, long right);
String operator+(const String& left, unsigned long right);
String operator+(const String& left, float right);
String operator+(const String& left, double right);

#endif
