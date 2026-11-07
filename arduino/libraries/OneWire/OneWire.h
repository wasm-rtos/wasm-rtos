/*
 * OneWire: Arduino's 1-Wire library over the wasm-rtos onewire interface.
 * OneWire(N) takes the whole 1-Wire bus of pin N ("gpioN", see
 * wr_arduino_pin_name) for the program: every call is one transaction, so
 * the sketch's resets and ROM commands keep their order. write(v, 1) leaves
 * the strong pull-up on until the next call or depower(). search() runs in
 * the kernel. Without such a bus, reset() finds no devices and reads return
 * ones, as on an empty line.
 */
#ifndef WASM_RTOS_ARDUINO_ONEWIRE_H
#define WASM_RTOS_ARDUINO_ONEWIRE_H

#include <Arduino.h>

#define ONEWIRE_SEARCH 1
#define ONEWIRE_CRC 1
#define ONEWIRE_CRC16 1

class OneWire
{
public:
    OneWire() {}
    OneWire(uint8_t pin) { begin(pin); }

    void begin(uint8_t pin);
    /* 1 when a device answered with a presence pulse. */
    uint8_t reset(void);
    void select(const uint8_t rom[8]);
    void skip(void);
    void write(uint8_t v, uint8_t power = 0);
    void write_bytes(const uint8_t* buf, uint16_t count, bool power = 0);
    uint8_t read(void);
    void read_bytes(uint8_t* buf, uint16_t count);
    void write_bit(uint8_t v);
    uint8_t read_bit(void);
    void depower(void);

    void reset_search();
    void target_search(uint8_t family_code);
    /* The next ROM into newAddr; search_mode false finds only devices in alarm. */
    bool search(uint8_t* newAddr, bool search_mode = true);

    static uint8_t crc8(const uint8_t* addr, uint8_t len);
    static bool check_crc16(const uint8_t* input, uint16_t len, const uint8_t* inverted_crc, uint16_t crc = 0);
    static uint16_t crc16(const uint8_t* input, uint16_t len, uint16_t crc = 0);

private:
    int32_t run(uint32_t kind, void* data, uint32_t length, bool power);

    int32_t bus_ = 0;
    char name_[24] = {};
    uint32_t name_length_ = 0;
    /* Where the search stands, as the kernel's onewire-search keeps it. */
    uint64_t search_rom_ = 0;
    uint32_t search_discrepancy_ = 0;
    uint32_t search_done_ = 0;
};

#endif
