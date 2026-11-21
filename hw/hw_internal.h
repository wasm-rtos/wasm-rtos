#ifndef WASM_RTOS_HW_INTERNAL_H
#define WASM_RTOS_HW_INTERNAL_H

/* Inside the device library: what one device file calls in another. */

#include "hw_host.h"

/* ---- GPIO for programs (gpio.c) -------------------------------------------- */

/*
 * Pins are named by id (1-based). Flags are the guest's: HW_GPIO_OUTPUT,
 * PULL_UP, PULL_DOWN, OPEN_DRAIN, HW_GPIO_GUEST_ACTIVE_LOW and INIT_HIGH as
 * a logical level. Levels and edges are logical (after inversion).
 * hw_gpio_open: HW_STATUS_FILE_NOT_FOUND for an unknown name, HW_STATUS_BUSY
 * when another program owns the pin.
 */
#define HW_GPIO_GUEST_ACTIVE_LOW 0x10U
#define HW_GPIO_GUEST_RISING     0x01U
#define HW_GPIO_GUEST_FALLING    0x02U

HwStatus hw_gpio_open(const char* name, uint32_t length, uint32_t flags, uint32_t* out_id);
void hw_gpio_close(uint32_t id);
HwStatus hw_gpio_set(uint32_t id, uint32_t level);
HwStatus hw_gpio_get(uint32_t id, uint32_t* out_level);
HwStatus hw_gpio_set_mode(uint32_t id, uint32_t flags);
HwStatus hw_gpio_watch(uint32_t id, uint32_t edges);
/* Edges since the last call; resets the count. */
uint32_t hw_gpio_take_events(uint32_t id);
uint8_t hw_gpio_ready(uint32_t id);
/*
 * A pin for a device that drives it itself (pulse trains): it must have
 * capability (else HW_STATUS_UNSUPPORTED) and opens with the guest flags.
 */
HwStatus hw_gpio_open_capable(const char* name, uint32_t length, uint32_t capability, uint32_t flags,
                              uint32_t* out_id);
/* An output whose levels are the pin's own, whatever the board inverts; it starts high (chip selects). */
HwStatus hw_gpio_open_physical(const char* name, uint32_t length, uint32_t* out_id);
/* The platform's pin number behind a GPIO id. */
uint32_t hw_gpio_pin_number(uint32_t id);
/*
 * A pin for a PWM output: it must have HW_GPIO_CAN_PWM (else
 * HW_STATUS_UNSUPPORTED); it is owned like any opened pin and starts as a
 * low output. *out_inverted says whether the program's levels are the
 * pin's inverted (board and program inversion together).
 */
HwStatus hw_gpio_open_pwm(const char* name, uint32_t length, uint8_t active_low, uint32_t* out_id,
                          uint8_t* out_inverted);
/* Deliver edges reported by hw_gpio_edge; wakes waiting programs. */
void hw_gpio_poll(void);
void hw_gpio_shutdown(void);

/* ---- Displays for programs (display.c) ------------------------------------ */

/* What display-get reports; unknown properties are HW_STATUS_UNSUPPORTED. */
#define HW_DISPLAY_PROP_WIDTH      1U
#define HW_DISPLAY_PROP_HEIGHT     2U
#define HW_DISPLAY_PROP_SHAPE      3U
#define HW_DISPLAY_PROP_FORMAT     4U
#define HW_DISPLAY_PROP_FORMATS    5U
#define HW_DISPLAY_PROP_DPI        6U
#define HW_DISPLAY_PROP_BRIGHTNESS 7U

/*
 * Displays are named by id (1-based). hw_display_open: HW_STATUS_FILE_NOT_FOUND
 * for an unknown name, HW_STATUS_BUSY when another program owns it.
 * Rectangles must lie inside the display and data must hold exactly the
 * rectangle (HW_STATUS_INVALID_ARGUMENT); a format the display does not
 * accept is HW_STATUS_UNSUPPORTED.
 */
HwStatus hw_display_open(const char* name, uint32_t length, uint32_t* out_id);
void hw_display_close(uint32_t id);
HwStatus hw_display_get(uint32_t id, uint32_t property, uint32_t* out_value);
HwStatus hw_display_write(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          uint32_t format, const void* pixels, uint32_t size);
HwStatus hw_display_fill(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                         uint32_t format, uint32_t color);
HwStatus hw_display_flush(uint32_t id);
HwStatus hw_display_set_brightness(uint32_t id, uint32_t level);
void hw_display_shutdown(void);

/* ---- Input devices for programs (input.c) --------------------------------- */

#define HW_INPUT_PROP_KIND           1U
#define HW_INPUT_PROP_WIDTH          2U
#define HW_INPUT_PROP_HEIGHT         3U
#define HW_INPUT_PROP_STEPS_PER_TURN 4U

/* One event as programs read it: 16 bytes, little-endian. */
#define HW_INPUT_EVENT_SIZE 16U
#define HW_INPUT_EVENT_KEY     1U
#define HW_INPUT_EVENT_ROTATE  2U
#define HW_INPUT_EVENT_TOUCH   3U
#define HW_INPUT_EVENT_TEXT    4U
#define HW_INPUT_EVENT_DROPPED 5U
/* Key event values. */
#define HW_INPUT_KEY_RELEASED   0
#define HW_INPUT_KEY_PRESSED    1
#define HW_INPUT_KEY_REPEATED   2
#define HW_INPUT_KEY_LONG_PRESS 3

/* Devices are named by id (1-based), like displays. */
HwStatus hw_input_open(const char* name, uint32_t length, uint32_t* out_id);
void hw_input_close(uint32_t id);
HwStatus hw_input_get(uint32_t id, uint32_t property, uint32_t* out_value);
/* Move up to max_events events into out (HW_INPUT_EVENT_SIZE bytes each); returns how many. */
uint32_t hw_input_read(uint32_t id, uint8_t* out, uint32_t max_events);
uint8_t hw_input_ready(uint32_t id);
/* Long presses and repeats that are due; the next one, in ms from now (HW_WAIT_FOREVER: none). */
void hw_input_update_time(void);
uint32_t hw_input_next_deadline_ms(void);
void hw_input_shutdown(void);

/* ---- I2C for programs (bus.c) ---------------------------------------------- */

/*
 * Devices are named by id (1-based). hw_i2c_open: HW_STATUS_FILE_NOT_FOUND
 * for an unknown bus, HW_STATUS_BUSY when another program has the address
 * open, HW_STATUS_UNSUPPORTED without a port or buses.
 */
HwStatus hw_i2c_open(const char* bus_name, uint32_t length, uint32_t address, uint32_t* out_id);
void hw_i2c_close(uint32_t id);
HwStatus hw_i2c_transaction(uint32_t id, const HwI2cOp* ops, uint32_t count);
void hw_i2c_shutdown(void);

/* ---- SPI for programs (bus.c) ---------------------------------------------- */

/*
 * A cs of length 0 takes the whole bus: the program drives chip select
 * itself. Otherwise the kernel claims the cs pin and drives it around every
 * transaction, and the bus is shared. HW_STATUS_BUSY when that conflicts.
 */
HwStatus hw_spi_open(const char* bus_name, uint32_t bus_length, const char* cs, uint32_t cs_length,
                     uint32_t mode, uint32_t frequency_hz, uint32_t* out_id);
void hw_spi_close(uint32_t id);
HwStatus hw_spi_transaction(uint32_t id, const HwSpiOp* ops, uint32_t count);
void hw_spi_shutdown(void);

/* ---- 1-Wire for programs (bus.c) ------------------------------------------- */

/*
 * A device by its ROM code, or with rom 0 the whole bus: no other program
 * may then use it, and the program sends RESET and ROM commands itself.
 * HW_STATUS_BUSY when that conflicts or the ROM is taken,
 * HW_STATUS_INVALID_ARGUMENT for a ROM whose CRC is wrong.
 */
HwStatus hw_onewire_open(const char* bus_name, uint32_t length, uint64_t rom, uint32_t* out_id);
void hw_onewire_close(uint32_t id);
HwStatus hw_onewire_transaction(uint32_t id, const HwOneWireOp* ops, uint32_t count);

/* Where a ROM search stands, kept by the program between calls. */
typedef struct HwOneWireSearch
{
    uint64_t rom;          /* the last ROM found */
    uint32_t discrepancy;  /* bit (1-64) where the next pass takes the other branch; 0 at the start */
    uint32_t done;         /* the last ROM has been found */
} HwOneWireSearch;

/* The next ROM into state->rom (*out_found 1), or *out_found 0 when there are no more. */
#define HW_ONEWIRE_SEARCH_ALARM 0x01U
HwStatus hw_onewire_search(const char* bus_name, uint32_t length, HwOneWireSearch* state, uint32_t flags,
                           uint8_t* out_found);
void hw_onewire_shutdown(void);

/* ---- Counters for programs (counter.c) ------------------------------------- */
#define HW_COUNTER_PROP_KIND      1U
#define HW_COUNTER_PROP_MAX_HZ    2U
#define HW_COUNTER_PROP_FILTER_NS 3U
/* HW_STATUS_INVALID_ARGUMENT for a mode of the other kind. */
HwStatus hw_counter_open(const char* name, uint32_t length, uint32_t mode, uint32_t filter_ns, uint32_t* out_id);
void hw_counter_close(uint32_t id);
HwStatus hw_counter_read(uint32_t id, int32_t* out_count);
HwStatus hw_counter_write(uint32_t id, int32_t count);
/* Ready for wait.any while the count is below low or above high; INT32_MIN, INT32_MAX stops watching. */
HwStatus hw_counter_watch(uint32_t id, int32_t low, int32_t high);
HwStatus hw_counter_get(uint32_t id, uint32_t property, uint32_t* out_value);
uint8_t hw_counter_ready(uint32_t id);
/* Events from interrupt handlers, and watched counters read by the kernel. */
void hw_counter_poll(void);
/* When the kernel next reads a watched counter, in ms from now (HW_WAIT_FOREVER: none). */
uint32_t hw_counter_next_deadline_ms(void);
void hw_counter_shutdown(void);

/* ---- Analog outputs for programs (dac.c) ----------------------------------- */
#define HW_DAC_PROP_RESOLUTION 1U
#define HW_DAC_PROP_MAX        2U
#define HW_DAC_PROP_FULL_SCALE 3U
#define HW_DAC_PROP_VALUE      4U
/* HW_STATUS_BUSY when another program owns the channel or its pin. */
HwStatus hw_dac_open(const char* name, uint32_t length, uint32_t* out_id);
void hw_dac_close(uint32_t id);
/* HW_STATUS_INVALID_ARGUMENT past the largest value. */
HwStatus hw_dac_write(uint32_t id, uint32_t raw);
HwStatus hw_dac_write_mv(uint32_t id, uint32_t mv);
HwStatus hw_dac_get(uint32_t id, uint32_t property, uint32_t* out_value);
void hw_dac_shutdown(void);

/* ---- Analog inputs for programs (adc.c) ------------------------------------ */
#define HW_ADC_PROP_RESOLUTION 1U
#define HW_ADC_PROP_MAX        2U
#define HW_ADC_PROP_FULL_SCALE 3U
HwStatus hw_adc_open(const char* name, uint32_t length, uint32_t* out_id);
HwStatus hw_adc_read(uint32_t id, uint32_t* out_raw);
HwStatus hw_adc_read_mv(uint32_t id, uint32_t* out_mv);
HwStatus hw_adc_get(uint32_t id, uint32_t property, uint32_t* out_value);
void hw_adc_shutdown(void);

/* ---- PWM outputs for programs (pwm.c) -------------------------------------- */
#define HW_PWM_PROP_FREQUENCY 1U
#define HW_PWM_PROP_DUTY      2U
#define HW_PWM_ACTIVE_LOW     0x01U
/* Duty as the program sees it: 0 off .. 65535 fully on. */
HwStatus hw_pwm_open(const char* name, uint32_t length, uint32_t frequency_hz, uint32_t flags, uint32_t* out_id);
HwStatus hw_pwm_set_duty(uint32_t id, uint32_t duty);
HwStatus hw_pwm_set_frequency(uint32_t id, uint32_t frequency_hz);
HwStatus hw_pwm_get(uint32_t id, uint32_t property, uint32_t* out_value);
void hw_pwm_close(uint32_t id);
void hw_pwm_shutdown(void);

/* ---- Serial ports for programs (uart.c) ------------------------------------ */
#define HW_UART_PROP_BAUD      1U
#define HW_UART_PROP_AVAILABLE 2U
#define HW_UART_PROP_ERRORS    3U
HwStatus hw_uart_open(const char* name, uint32_t length, uint32_t baud, uint32_t format, uint32_t* out_id);
void hw_uart_close(uint32_t id);
uint32_t hw_uart_read(uint32_t id, uint8_t* out, uint32_t max);
HwStatus hw_uart_write(uint32_t id, const uint8_t* data, uint32_t size, uint32_t* out_written);
/* 0 when the port can take bytes again; starts a wait otherwise (HW_STATUS_PENDING). */
HwStatus hw_uart_wait_writable(uint32_t id, uint32_t timeout_ms);
HwStatus hw_uart_get(uint32_t id, uint32_t property, uint32_t* out_value);
uint8_t hw_uart_ready(uint32_t id);
/* Deliver what ports reported from interrupt handlers. */
void hw_uart_poll(void);
void hw_uart_shutdown(void);

/* ---- Pulse trains for programs (pulse.c) ----------------------------------- */
#define HW_PULSE_PROP_RESOLUTION 1U
HwStatus hw_pulse_open(const char* name, uint32_t length, uint32_t flags, uint32_t* out_id);
void hw_pulse_close(uint32_t id);
HwStatus hw_pulse_send(uint32_t id, const uint32_t* items, uint32_t count, uint32_t carrier_hz);
HwStatus hw_pulse_send_bits(uint32_t id, const uint8_t* data, uint32_t length, const uint32_t timing[4]);
/* Items captured in *out_count when done at once; HW_STATUS_PENDING delivers the count on wakeup. */
HwStatus hw_pulse_receive(uint32_t id, uint32_t* items, uint32_t max, uint32_t idle_ns, uint32_t timeout_ms,
                          uint32_t* out_count);
HwStatus hw_pulse_get(uint32_t id, uint32_t property, uint32_t* out_value);
void hw_pulse_poll(void);
void hw_pulse_finish_task(uint32_t task_id);
void hw_pulse_shutdown(void);

#endif
