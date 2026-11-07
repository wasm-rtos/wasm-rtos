#include "OneWire.h"

#include <string.h>
#include <wasm-rtos.h>

void OneWire::begin(uint8_t pin)
{
    const char* name = wr_arduino_pin_name(pin);

    if (bus_ > 0)
        wr_close(bus_);
    name_length_ = wr_strlen(name);
    if (name_length_ >= sizeof(name_))
        name_length_ = sizeof(name_) - 1;
    memcpy(name_, name, name_length_);
    name_[name_length_] = '\0';
    bus_ = wr_onewire_open(name_, name_length_, WR_ONEWIRE_WHOLE_BUS, 0);
    reset_search();
}

int32_t OneWire::run(uint32_t kind, void* data, uint32_t length, bool power)
{
    wr_onewire_op ops[2] = { { kind, data, length }, { WR_ONEWIRE_POWER, 0, 0 } };

    if (bus_ <= 0)
        return -WR_EUNSUPPORTED;
    return wr_onewire_transaction(bus_, ops, power ? 2u : 1u);
}

uint8_t OneWire::reset(void)
{
    return run(WR_ONEWIRE_RESET, 0, 0, false) == 0 ? 1 : 0;
}

void OneWire::select(const uint8_t rom[8])
{
    uint8_t match[9] = { 0x55 };

    memcpy(match + 1, rom, 8);
    run(WR_ONEWIRE_WRITE, match, 9, false);
}

void OneWire::skip(void)
{
    uint8_t command = 0xCC;
    run(WR_ONEWIRE_WRITE, &command, 1, false);
}

void OneWire::write(uint8_t v, uint8_t power)
{
    run(WR_ONEWIRE_WRITE, &v, 1, power != 0);
}

void OneWire::write_bytes(const uint8_t* buf, uint16_t count, bool power)
{
    if (count > 0)
        run(WR_ONEWIRE_WRITE, (void*)buf, count, power);
}

uint8_t OneWire::read(void)
{
    uint8_t value = 0xFF;
    if (run(WR_ONEWIRE_READ, &value, 1, false) != 0)
        value = 0xFF;
    return value;
}

void OneWire::read_bytes(uint8_t* buf, uint16_t count)
{
    if (count > 0 && run(WR_ONEWIRE_READ, buf, count, false) != 0)
        memset(buf, 0xFF, count);
}

void OneWire::write_bit(uint8_t v)
{
    uint8_t bit = v & 1;
    run(WR_ONEWIRE_WRITE_BITS, &bit, 1, false);
}

uint8_t OneWire::read_bit(void)
{
    uint8_t bit = 1;
    if (run(WR_ONEWIRE_READ_BITS, &bit, 1, false) != 0)
        bit = 1;
    return bit & 1;
}

void OneWire::depower(void)
{
    if (bus_ > 0)
        wr_onewire_transaction(bus_, 0, 0);
}

void OneWire::reset_search()
{
    search_rom_ = 0;
    search_discrepancy_ = 0;
    search_done_ = 0;
}

void OneWire::target_search(uint8_t family_code)
{
    reset_search();
    search_rom_ = family_code;
    search_discrepancy_ = 64;
}

bool OneWire::search(uint8_t* newAddr, bool search_mode)
{
    wr_onewire_search_state state = { search_rom_, search_discrepancy_, search_done_ };
    int32_t found = wr_onewire_search(name_, name_length_, &state, search_mode ? 0u : WR_ONEWIRE_ALARM);

    search_rom_ = state.rom;
    search_discrepancy_ = state.discrepancy;
    search_done_ = state.done;
    if (found != 1)
        return false;
    for (int i = 0; i < 8; ++i)
        newAddr[i] = (uint8_t)(state.rom >> (i * 8));
    return true;
}

uint8_t OneWire::crc8(const uint8_t* addr, uint8_t len)
{
    return wr_onewire_crc8(addr, len);
}

/* CRC-16 of 1-Wire devices (x^16 + x^15 + x^2 + 1, least significant bit first). */
uint16_t OneWire::crc16(const uint8_t* input, uint16_t len, uint16_t crc)
{
    for (uint16_t i = 0; i < len; ++i)
    {
        crc ^= input[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* Devices send the CRC inverted, low byte first. */
bool OneWire::check_crc16(const uint8_t* input, uint16_t len, const uint8_t* inverted_crc, uint16_t crc)
{
    crc = (uint16_t)~crc16(input, len, crc);
    return (crc & 0xFF) == inverted_crc[0] && (crc >> 8) == inverted_crc[1];
}
