#include "Wire.h"

#include <wasm-rtos.h>

TwoWire Wire("i2c0");
TwoWire Wire1("i2c1");

TwoWire::TwoWire(const char* bus) : bus_(bus) {}

void TwoWire::end()
{
    for (int32_t& handle : handles_)
    {
        if (handle > 0)
            wr_close(handle);
        handle = 0;
    }
}

int32_t TwoWire::device(uint8_t address)
{
    if (address > 0x7F)
        return -WR_EINVAL;
    if (handles_[address] <= 0)
        handles_[address] = wr_i2c_open_cstr(bus_, address);
    return handles_[address];
}

void TwoWire::beginTransmission(uint8_t address)
{
    tx_address_ = address;
    tx_length_ = 0;
    tx_overflow_ = false;
    transmitting_ = true;
    pending_ = false;
}

size_t TwoWire::write(uint8_t byte)
{
    if (!transmitting_ || tx_length_ >= WIRE_BUFFER_SIZE)
    {
        tx_overflow_ = transmitting_;
        return 0;
    }
    tx_buffer_[tx_length_++] = byte;
    return 1;
}

size_t TwoWire::write(const uint8_t* data, size_t length)
{
    size_t count = 0;
    while (count < length && write(data[count]) == 1)
        ++count;
    return count;
}

static uint8_t wire_result(int32_t result)
{
    if (result == 0)
        return 0;
    if (result == -WR_ENACK)
        return 2;
    if (result == -WR_ETIMEOUT)
        return 5;
    return 4;
}

uint8_t TwoWire::endTransmission(uint8_t send_stop)
{
    int32_t handle;
    transmitting_ = false;
    if (tx_overflow_)
        return 1;
    if (!send_stop)
    {
        /* Sent with the next requestFrom, behind a repeated start. */
        pending_ = true;
        return 0;
    }
    handle = device(tx_address_);
    if (handle <= 0)
        return 4;
    wr_i2c_op op = { WR_I2C_WRITE, tx_buffer_, (uint32_t)tx_length_ };
    return wire_result(wr_i2c_transaction(handle, &op, tx_length_ > 0 ? 1u : 0u));
}

size_t TwoWire::requestFrom(uint8_t address, size_t quantity, bool send_stop)
{
    wr_i2c_op ops[2];
    uint32_t count = 0;
    int32_t handle = device(address);
    (void)send_stop;

    rx_length_ = 0;
    rx_index_ = 0;
    if (quantity > WIRE_BUFFER_SIZE)
        quantity = WIRE_BUFFER_SIZE;
    if (handle <= 0 || quantity == 0)
    {
        pending_ = false;
        return 0;
    }
    if (pending_ && tx_address_ == address && tx_length_ > 0)
        ops[count++] = { WR_I2C_WRITE, tx_buffer_, (uint32_t)tx_length_ };
    ops[count++] = { WR_I2C_READ, rx_buffer_, (uint32_t)quantity };
    pending_ = false;
    if (wr_i2c_transaction(handle, ops, count) != 0)
        return 0;
    rx_length_ = quantity;
    return quantity;
}

uint8_t TwoWire::requestFrom(uint8_t address, uint8_t quantity, uint32_t internal, uint8_t size, uint8_t send_stop)
{
    if (size > 0)
    {
        beginTransmission(address);
        if (size > 4)
            size = 4;
        while (size-- > 0)
            write((uint8_t)(internal >> (size * 8)));
        endTransmission((uint8_t)0);
    }
    return (uint8_t)requestFrom(address, (size_t)quantity, send_stop != 0);
}
