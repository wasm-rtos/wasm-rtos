#include "Encoder.h"

#include <wasm-rtos.h>

Encoder::Encoder(uint8_t pin1, uint8_t pin2)
{
    (void)pin2;
    counter_ = wr_counter_open_cstr(wr_arduino_pin_name(pin1), WR_COUNTER_X4, 0);
}

int32_t Encoder::raw()
{
    int32_t count = 0;
    if (counter_ <= 0 || wr_counter_read(counter_, &count) != 0)
        return 0;
    return count;
}

int32_t Encoder::read()
{
    return (int32_t)((uint32_t)raw() - (uint32_t)offset_);
}

void Encoder::write(int32_t position)
{
    offset_ = (int32_t)((uint32_t)raw() - (uint32_t)position);
}

int32_t Encoder::readAndReset()
{
    int32_t count = raw();
    int32_t position = (int32_t)((uint32_t)count - (uint32_t)offset_);
    offset_ = count;
    return position;
}
