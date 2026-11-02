#include "Arduino.h"

#include <wasm-rtos.h>

HardwareSerial Serial1("uart1");
HardwareSerial Serial2("uart2");

/* A classic config byte as a uart format: bits 1-2 data bits - 5, bit 3 two stop bits, bits 4-5 parity. */
static uint32_t uart_format(uint32_t config)
{
    uint32_t format = 0;
    if (((config >> 1) & 0x03u) == 2u)
        format |= WR_UART_DATA_7;
    if (config & 0x08u)
        format |= WR_UART_STOP_2;
    if (((config >> 4) & 0x03u) == 2u)
        format |= WR_UART_PARITY_EVEN;
    if (((config >> 4) & 0x03u) == 3u)
        format |= WR_UART_PARITY_ODD;
    return format;
}

void HardwareSerial::begin(unsigned long baud, uint32_t config)
{
    end();
    port_ = wr_uart_open_cstr(name_, (uint32_t)baud, uart_format(config));
    baud_ = port_ > 0 ? (unsigned long)wr_uart_get(port_, WR_UART_BAUD) : 0;
}

void HardwareSerial::end()
{
    if (port_ > 0)
        wr_close(port_);
    port_ = 0;
    length_ = index_ = 0;
}

/* Take what has arrived into the local buffer when it is empty. */
bool HardwareSerial::fill()
{
    if (index_ < length_)
        return true;
    if (port_ <= 0)
        return false;
    int32_t got = wr_uart_read(port_, buffer_, sizeof(buffer_));
    index_ = 0;
    length_ = got > 0 ? (size_t)got : 0;
    return length_ > 0;
}

int HardwareSerial::available()
{
    if (port_ <= 0)
        return 0;
    int32_t waiting = wr_uart_get(port_, WR_UART_AVAILABLE);
    return (int)(length_ - index_) + (waiting > 0 ? waiting : 0);
}

int HardwareSerial::read() { return fill() ? buffer_[index_++] : -1; }

int HardwareSerial::peek() { return fill() ? buffer_[index_] : -1; }

int HardwareSerial::availableForWrite() { return port_ > 0 && wr_uart_wait_writable(port_, 0) == 0 ? 64 : 0; }

/* Waits for room, as Arduino's write blocks while the send buffer is full. */
size_t HardwareSerial::write(const uint8_t* buffer, size_t size)
{
    if (port_ <= 0)
        return 0;
    int32_t sent = wr_uart_write_all(port_, buffer, (uint32_t)size, WR_WAIT_FOREVER);
    return sent > 0 ? (size_t)sent : 0;
}

void HardwareSerial::flush()
{
    if (port_ > 0)
        wr_uart_wait_writable(port_, WR_WAIT_FOREVER);
}
