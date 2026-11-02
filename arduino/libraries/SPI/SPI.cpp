#include "SPI.h"

#include <wasm-rtos.h>

SPIClass SPI("spi0");

void SPIClass::end()
{
    if (handle_ > 0)
        wr_close(handle_);
    handle_ = 0;
}

/* The bus with the current settings; reopened when they change. */
int32_t SPIClass::device()
{
    if (handle_ > 0 && opened_ == settings_)
        return handle_;
    end();
    uint32_t mode = settings_.data_mode_ & 0x03u;
    /* SPI_MODE1 is CPHA, SPI_MODE2 is CPOL: the same bits as the ABI. */
    if (settings_.bit_order_ == LSBFIRST)
        mode |= WR_SPI_LSB_FIRST;
    handle_ = wr_spi_open_cstr(bus_, "", mode, settings_.clock_);
    opened_ = settings_;
    return handle_;
}

void SPIClass::beginTransaction(SPISettings settings) { settings_ = settings; }

uint8_t SPIClass::transfer(uint8_t data)
{
    uint8_t in = 0;
    transferBytes(&data, &in, 1);
    return in;
}

uint16_t SPIClass::transfer16(uint16_t data)
{
    uint8_t buffer[2];
    if (settings_.bit_order_ == LSBFIRST)
    {
        buffer[0] = (uint8_t)data;
        buffer[1] = (uint8_t)(data >> 8);
    }
    else
    {
        buffer[0] = (uint8_t)(data >> 8);
        buffer[1] = (uint8_t)data;
    }
    transfer(buffer, 2);
    return settings_.bit_order_ == LSBFIRST ? (uint16_t)(buffer[0] | (buffer[1] << 8))
                                            : (uint16_t)((buffer[0] << 8) | buffer[1]);
}

void SPIClass::transfer(void* buffer, size_t count) { transferBytes((uint8_t*)buffer, (uint8_t*)buffer, count); }

void SPIClass::writeBytes(const uint8_t* data, size_t count)
{
    int32_t handle = device();
    if (handle > 0 && count > 0)
        wr_spi_write(handle, data, (uint32_t)count);
}

void SPIClass::transferBytes(const uint8_t* out, uint8_t* in, size_t count)
{
    int32_t handle = device();
    if (handle > 0 && count > 0)
        wr_spi_transfer(handle, out, in, (uint32_t)count);
}

void SPIClass::setBitOrder(uint8_t bit_order) { settings_.bit_order_ = bit_order; }
void SPIClass::setDataMode(uint8_t data_mode) { settings_.data_mode_ = data_mode; }
void SPIClass::setClockDivider(uint8_t divider) { settings_.clock_ = divider != 0 ? 16000000u / divider : 4000000u; }
void SPIClass::setFrequency(uint32_t frequency_hz) { settings_.clock_ = frequency_hz; }
