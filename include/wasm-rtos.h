#ifndef WASM_RTOS_H_GUEST
#define WASM_RTOS_H_GUEST

/*
 * wasm-rtos ABI 1.4 for programs written in C or C++ (wasm32).
 *
 * Every call returns an int32_t: zero or a value on success, a negative WR_E*
 * error otherwise. Handles belong to the calling program and close when it
 * ends. Strings are (pointer, length) in UTF-8; the *_cstr helpers take a
 * NUL-terminated string instead. Time, sleeping and yielding are WASI
 * (clock_gettime, nanosleep/usleep, sched_yield). See docs/abi.md.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_ABI_VERSION_MAJOR 1
#define WR_ABI_VERSION_MINOR 4

/* Errors; calls return their negatives. */
#define WR_EFAULT       1   /* a pointer or range lies outside the program's memory */
#define WR_EINVAL       2   /* an argument is invalid */
#define WR_EBADH        3   /* not an open handle of the right kind */
#define WR_ENOMEM       4   /* out of memory */
#define WR_ELIMIT       5   /* too many handles, processes or arguments */
#define WR_ENOTFOUND    6   /* no such file or name */
#define WR_ETIMEOUT     7   /* the timeout ran out */
#define WR_EABORTED     8   /* the wait was cancelled */
#define WR_EBUSY        9   /* the wait was cancelled because the program was suspended */
#define WR_ENOTOWNER    10  /* the mutex belongs to another program */
#define WR_EFULL        11  /* the queue or semaphore is full */
#define WR_EEMPTY       12  /* the queue is empty */
#define WR_EUNSUPPORTED 13  /* not available on this device or build */
#define WR_ETOOLONG     14  /* a string is too long */
#define WR_EBADPROGRAM  15  /* the module cannot be loaded */
#define WR_EISDIR       16  /* the path is a directory */
#define WR_ENOTDIR      17  /* a path component is not a directory */
#define WR_EDEAD        18  /* the process has already ended */
#define WR_EMISMATCH    19  /* a named object exists with other parameters */
#define WR_ETRAPPED     20  /* proc wait: the child trapped */
#define WR_EKILLED      21  /* proc wait: the child was killed */
#define WR_EINUSE       22  /* another program uses it (1.1) */
#define WR_ENACK        23  /* i2c: the address or a byte was not acknowledged (1.3) */
#define WR_EBUS         24  /* the bus failed: lost arbitration, stuck lines (1.3) */

#define WR_WAIT_FOREVER 0xFFFFFFFFu

#define WR_EVENT_ALL    1u  /* event-wait: every bit, not any */
#define WR_EVENT_CLEAR  2u  /* event-wait: clear the bits waited for */
#define WR_EVENT_BITS   0x00FFFFFFu

#define WR_TIMER_PERIODIC 1u

/* gpio-open and gpio-set-mode flags (1.1). */
#define WR_GPIO_OUTPUT     0x01u  /* an output; without it an input */
#define WR_GPIO_PULL_UP    0x02u
#define WR_GPIO_PULL_DOWN  0x04u
#define WR_GPIO_OPEN_DRAIN 0x08u  /* outputs only */
#define WR_GPIO_ACTIVE_LOW 0x10u  /* the program's 1 is the pin's low */
#define WR_GPIO_INIT_HIGH  0x20u  /* an output starts at 1 */
/* gpio-watch edges, as the program sees levels. */
#define WR_GPIO_RISING     0x01u
#define WR_GPIO_FALLING    0x02u

/* display-get properties (1.2); a property the kernel does not know is -WR_EUNSUPPORTED. */
#define WR_DISPLAY_WIDTH      1u
#define WR_DISPLAY_HEIGHT     2u
#define WR_DISPLAY_SHAPE      3u  /* WR_DISPLAY_RECTANGLE or WR_DISPLAY_ROUND */
#define WR_DISPLAY_FORMAT     4u  /* the panel's own format, the fastest to write */
#define WR_DISPLAY_FORMATS    5u  /* WR_DISPLAY_FORMAT_BIT of every format display-write accepts */
#define WR_DISPLAY_DPI        6u  /* pixels per inch, 0 when unknown */
#define WR_DISPLAY_BRIGHTNESS 7u  /* 0-255; -WR_EUNSUPPORTED without brightness control */
#define WR_DISPLAY_RECTANGLE  0u
#define WR_DISPLAY_ROUND      1u  /* a circle inscribed in width x height */
/* Pixel formats. MONO1: 1 bit per pixel, 1 = lit, rows of whole bytes,
 * leftmost pixel in the top bit. RGB565: uint16_t, red in the top 5 bits.
 * XRGB8888: uint32_t 0x00RRGGBB. Both little-endian, as wasm32 stores them. */
#define WR_PIXEL_MONO1    1u
#define WR_PIXEL_RGB565   2u
#define WR_PIXEL_XRGB8888 3u
#define WR_DISPLAY_FORMAT_BIT(format) (1u << (format))

/* input-get properties (1.2). */
#define WR_INPUT_KIND           1u  /* WR_INPUT_BUTTONS ... WR_INPUT_KEYBOARD */
#define WR_INPUT_WIDTH          2u  /* touch: x runs 0..width-1 */
#define WR_INPUT_HEIGHT         3u
#define WR_INPUT_STEPS_PER_TURN 4u  /* rotary: detents in a full turn, 0 when unknown */
#define WR_INPUT_BUTTONS  1u
#define WR_INPUT_ROTARY   2u
#define WR_INPUT_TOUCH    3u
#define WR_INPUT_KEYBOARD 4u

/* One input event as input-read stores it (16 bytes, little-endian). */
typedef struct wr_input_event
{
    uint8_t type;      /* WR_EVENT_KEY ... WR_EVENT_DROPPED */
    uint8_t reserved;
    uint16_t code;     /* key: Linux key code; rotate: REL_* axis; touch: contact */
    int32_t value;     /* see the event types */
    uint16_t x;        /* touch position */
    uint16_t y;
    uint32_t time_ms;  /* kernel time, low 32 bits, when it happened */
} wr_input_event;

#define WR_EVENT_KEY     1u  /* value: WR_KEY_RELEASED, _PRESSED, _REPEATED or _LONG_PRESS */
#define WR_EVENT_ROTATE  2u  /* value: detents, positive clockwise */
#define WR_EVENT_TOUCH   3u  /* value: WR_TOUCH_UP, _DOWN or _MOVE; x, y */
#define WR_EVENT_TEXT    4u  /* value: a Unicode code point the keyboard typed */
#define WR_EVENT_DROPPED 5u  /* value: events lost before the ones that follow */
#define WR_KEY_RELEASED   0
#define WR_KEY_PRESSED    1
#define WR_KEY_REPEATED   2  /* still held, at the device's repeat rate */
#define WR_KEY_LONG_PRESS 3  /* held for the device's long-press time; once per press */
#define WR_TOUCH_UP   0
#define WR_TOUCH_DOWN 1
#define WR_TOUCH_MOVE 2

/* Codes are the Linux input event codes (linux/input-event-codes.h); some common ones. */
#define WR_KEY_ESC        1
#define WR_KEY_BACKSPACE  14
#define WR_KEY_ENTER      28
#define WR_KEY_SPACE      57
#define WR_KEY_HOME       102
#define WR_KEY_UP         103
#define WR_KEY_LEFT       105
#define WR_KEY_RIGHT      106
#define WR_KEY_DOWN       108
#define WR_KEY_POWER      116
#define WR_KEY_MENU       139
#define WR_KEY_BACK       158
#define WR_KEY_SELECT     0x161
#define WR_BTN_0          0x100  /* BTN_0 ... BTN_9: buttons without a meaning of their own */
#define WR_REL_HWHEEL     6
#define WR_REL_DIAL       7      /* a bezel or a dial */
#define WR_REL_WHEEL      8

/* i2c (1.3): ops of a transaction. Between a write and a read comes a
 * repeated start, at the end a stop. 12 bytes on wasm32. */
#define WR_I2C_WRITE 1u
#define WR_I2C_READ  2u
typedef struct wr_i2c_op
{
    uint32_t kind;
    void* data;
    uint32_t length;
} wr_i2c_op;

/* spi (1.3): ops of a transaction, chip select held throughout. A read
 * sends zeros; a transfer sends write and receives into read at once (they
 * may be the same buffer); a delay waits length microseconds. 16 bytes. */
#define WR_SPI_WRITE    1u
#define WR_SPI_READ     2u
#define WR_SPI_TRANSFER 3u
#define WR_SPI_DELAY_US 4u
typedef struct wr_spi_op
{
    uint32_t kind;
    const void* write;
    void* read;
    uint32_t length;
} wr_spi_op;
/* spi-open mode: SPI mode 0-3 is CPOL << 1 | CPHA. */
#define WR_SPI_CPHA      0x01u
#define WR_SPI_CPOL      0x02u
#define WR_SPI_LSB_FIRST 0x04u
#define WR_SPI_MODE_0 0u
#define WR_SPI_MODE_1 WR_SPI_CPHA
#define WR_SPI_MODE_2 WR_SPI_CPOL
#define WR_SPI_MODE_3 (WR_SPI_CPOL | WR_SPI_CPHA)

/* adc-get properties (1.4). */
#define WR_ADC_RESOLUTION    1u  /* bits of a raw value */
#define WR_ADC_MAX           2u  /* the largest raw value */
#define WR_ADC_FULL_SCALE_MV 3u  /* the voltage of the largest raw value */

/* pwm (1.4): duty runs from 0 (off) to WR_PWM_DUTY_MAX (fully on) on every chip. */
#define WR_PWM_DUTY_MAX   65535u
#define WR_PWM_ACTIVE_LOW 0x01u  /* pwm-open: "on" is the pin's low */
#define WR_PWM_FREQUENCY  1u     /* pwm-get: the frequency the hardware runs at */
#define WR_PWM_DUTY       2u     /* pwm-get: the duty last set */

/* uart-open frame format (1.4); 0 is 8 data bits, no parity, 1 stop bit. */
#define WR_UART_PARITY_EVEN  0x01u
#define WR_UART_PARITY_ODD   0x02u
#define WR_UART_STOP_2       0x04u
#define WR_UART_DATA_7       0x08u
#define WR_UART_FLOW_RTS_CTS 0x10u
/* uart-get properties. */
#define WR_UART_BAUD      1u  /* the speed the port runs at */
#define WR_UART_AVAILABLE 2u  /* received bytes waiting to be read */
#define WR_UART_ERRORS    3u  /* frame, parity and overrun errors and lost bytes since the last ask */

/* One string argument for spawn: matches the ABI's (pointer, length) pair. */
typedef struct wr_str
{
    const char* text;
    uint32_t length;
} wr_str;

#define WR_IMPORT(interface, name) \
    __attribute__((import_module("wasm-rtos:kernel/" interface "@1.4.0"), import_name(name)))

/* handle */
WR_IMPORT("handle", "close") int32_t wr_close(int32_t handle);

/* sync: a name of length 0 makes a private object; a name opens the object
 * of that name if one exists, or creates it. */
WR_IMPORT("sync", "mutex-create") int32_t wr_mutex_create(const char* name, uint32_t name_length);
WR_IMPORT("sync", "mutex-lock") int32_t wr_mutex_lock(int32_t mutex, uint32_t timeout_ms);
WR_IMPORT("sync", "mutex-unlock") int32_t wr_mutex_unlock(int32_t mutex);
WR_IMPORT("sync", "semaphore-create") int32_t wr_semaphore_create(const char* name, uint32_t name_length,
                                                                  uint32_t max_count, uint32_t initial_count);
WR_IMPORT("sync", "semaphore-take") int32_t wr_semaphore_take(int32_t semaphore, uint32_t timeout_ms);
WR_IMPORT("sync", "semaphore-give") int32_t wr_semaphore_give(int32_t semaphore);
WR_IMPORT("sync", "event-create") int32_t wr_event_create(const char* name, uint32_t name_length);
WR_IMPORT("sync", "event-set") int32_t wr_event_set(int32_t event, uint32_t bits);
WR_IMPORT("sync", "event-clear") int32_t wr_event_clear(int32_t event, uint32_t bits);
WR_IMPORT("sync", "event-get") int32_t wr_event_get(int32_t event);
WR_IMPORT("sync", "event-wait") int32_t wr_event_wait(int32_t event, uint32_t bits, uint32_t flags,
                                                      uint32_t timeout_ms);
WR_IMPORT("sync", "queue-create") int32_t wr_queue_create(const char* name, uint32_t name_length,
                                                          uint32_t item_size, uint32_t capacity);
WR_IMPORT("sync", "queue-send") int32_t wr_queue_send(int32_t queue, const void* item, uint32_t timeout_ms);
WR_IMPORT("sync", "queue-receive") int32_t wr_queue_receive(int32_t queue, void* item, uint32_t timeout_ms);

/* timer: expirations are counted; wait for the handle, then read the count. */
WR_IMPORT("timer", "create") int32_t wr_timer_create(uint32_t period_ms, uint32_t flags);
WR_IMPORT("timer", "start") int32_t wr_timer_start(int32_t timer);
WR_IMPORT("timer", "stop") int32_t wr_timer_stop(int32_t timer);
WR_IMPORT("timer", "set-period") int32_t wr_timer_set_period(int32_t timer, uint32_t period_ms);
WR_IMPORT("timer", "read") int32_t wr_timer_read(int32_t timer);

/* wait: the index of the first ready handle (queue with an item, semaphore
 * that can be taken, event with a bit set, expired timer, ended child). */
WR_IMPORT("wait", "any") int32_t wr_wait_any(const int32_t* handles, uint32_t count, uint32_t timeout_ms);

/* proc: children are handles; stack_size 0 lets the child choose. */
WR_IMPORT("proc", "spawn") int32_t wr_spawn(const void* module, uint32_t module_length, const wr_str* args,
                                            uint32_t argc, uint32_t priority, uint32_t stack_size);
WR_IMPORT("proc", "spawn-path") int32_t wr_spawn_path(const char* path, uint32_t path_length, const wr_str* args,
                                                      uint32_t argc, uint32_t priority, uint32_t stack_size);
WR_IMPORT("proc", "wait") int32_t wr_wait(int32_t child, uint32_t timeout_ms);
WR_IMPORT("proc", "kill") int32_t wr_kill(int32_t child);
WR_IMPORT("proc", "self") int32_t wr_self(void);
WR_IMPORT("proc", "parent") int32_t wr_parent(void);

/* gpio (1.1): pins by the device profile's names; a pin has one owner at a
 * time (-WR_EINUSE). Levels are 0 or 1 as the program sees them. A watched
 * pin is ready for wait.any once an edge happened; gpio-events returns and
 * resets their count. */
WR_IMPORT("gpio", "open") int32_t wr_gpio_open(const char* name, uint32_t name_length, uint32_t flags);
WR_IMPORT("gpio", "set") int32_t wr_gpio_set(int32_t pin, uint32_t level);
WR_IMPORT("gpio", "get") int32_t wr_gpio_get(int32_t pin);
WR_IMPORT("gpio", "set-mode") int32_t wr_gpio_set_mode(int32_t pin, uint32_t flags);
WR_IMPORT("gpio", "watch") int32_t wr_gpio_watch(int32_t pin, uint32_t edges);
WR_IMPORT("gpio", "events") int32_t wr_gpio_events(int32_t pin);

/* display (1.2): screens by the device profile's names ("main"); one owner at
 * a time (-WR_EINUSE). display-write takes exactly the rectangle's pixels,
 * which must lie inside the display; what is written shows no later than
 * display-flush. flags are 0. */
WR_IMPORT("display", "open") int32_t wr_display_open(const char* name, uint32_t name_length, uint32_t flags);
WR_IMPORT("display", "get") int32_t wr_display_get(int32_t display, uint32_t property);
WR_IMPORT("display", "write") int32_t wr_display_write(int32_t display, uint32_t x, uint32_t y, uint32_t width,
                                                       uint32_t height, uint32_t format, const void* pixels,
                                                       uint32_t length);
WR_IMPORT("display", "fill") int32_t wr_display_fill(int32_t display, uint32_t x, uint32_t y, uint32_t width,
                                                     uint32_t height, uint32_t format, uint32_t color);
WR_IMPORT("display", "flush") int32_t wr_display_flush(int32_t display);
WR_IMPORT("display", "set-brightness") int32_t wr_display_set_brightness(int32_t display, uint32_t level);

/* input (1.2): devices by the device profile's names ("buttons", "bezel",
 * "touch", "keyboard"); one owner at a time. The handle is ready for
 * wait.any while events are queued; input-read moves up to max_events of
 * them and returns how many (0: none). flags are 0. */
WR_IMPORT("input", "open") int32_t wr_input_open(const char* name, uint32_t name_length, uint32_t flags);
WR_IMPORT("input", "get") int32_t wr_input_get(int32_t device, uint32_t property);
WR_IMPORT("input", "read") int32_t wr_input_read(int32_t device, wr_input_event* events, uint32_t max_events);

/* i2c (1.3): a device is an address on a bus named by the device profile
 * ("i2c0"); one program per address (-WR_EINUSE), the bus is shared. No ops:
 * only the address, to see whether a device answers (-WR_ENACK if not).
 * flags are 0. */
WR_IMPORT("i2c", "open") int32_t wr_i2c_open(const char* bus, uint32_t bus_length, uint32_t address, uint32_t flags);
WR_IMPORT("i2c", "transaction") int32_t wr_i2c_transaction(int32_t device, const wr_i2c_op* ops, uint32_t count);

/* spi (1.3): with a chip select pin name the kernel drives it around each
 * transaction and the bus is shared; with an empty one the program takes
 * the whole bus and drives chip select itself through gpio. frequency_hz 0
 * or above the bus's maximum gets the maximum. */
WR_IMPORT("spi", "open") int32_t wr_spi_open(const char* bus, uint32_t bus_length, const char* cs, uint32_t cs_length,
                                             uint32_t mode, uint32_t frequency_hz);
WR_IMPORT("spi", "transaction") int32_t wr_spi_transaction(int32_t device, const wr_spi_op* ops, uint32_t count);

/* adc (1.4): analog inputs by the device profile's names ("a0", "battery").
 * Reading changes nothing, so programs share a channel. adc-read gives the
 * raw value, adc-read-mv millivolts. flags are 0. */
WR_IMPORT("adc", "open") int32_t wr_adc_open(const char* name, uint32_t name_length, uint32_t flags);
WR_IMPORT("adc", "read") int32_t wr_adc_read(int32_t channel);
WR_IMPORT("adc", "read-mv") int32_t wr_adc_read_mv(int32_t channel);
WR_IMPORT("adc", "get") int32_t wr_adc_get(int32_t channel, uint32_t property);

/* pwm (1.4): a pulse-width output on a GPIO pin, by its name; the pin is
 * owned as with gpio-open and starts off. */
WR_IMPORT("pwm", "open") int32_t wr_pwm_open(const char* name, uint32_t name_length, uint32_t frequency_hz,
                                             uint32_t flags);
WR_IMPORT("pwm", "set-duty") int32_t wr_pwm_set_duty(int32_t output, uint32_t duty);
WR_IMPORT("pwm", "set-frequency") int32_t wr_pwm_set_frequency(int32_t output, uint32_t frequency_hz);
WR_IMPORT("pwm", "get") int32_t wr_pwm_get(int32_t output, uint32_t property);

/* uart (1.4): serial ports by the device profile's names ("uart1"), one
 * owner each. The handle is ready for wait.any while received bytes wait;
 * uart-read takes what is there (0: nothing yet). uart-write hands the port
 * what fits and returns how many; uart-wait-writable waits until it has
 * room again. */
WR_IMPORT("uart", "open") int32_t wr_uart_open(const char* name, uint32_t name_length, uint32_t baud, uint32_t format);
WR_IMPORT("uart", "read") int32_t wr_uart_read(int32_t port, void* buffer, uint32_t max);
WR_IMPORT("uart", "write") int32_t wr_uart_write(int32_t port, const void* data, uint32_t length);
WR_IMPORT("uart", "wait-writable") int32_t wr_uart_wait_writable(int32_t port, uint32_t timeout_ms);
WR_IMPORT("uart", "get") int32_t wr_uart_get(int32_t port, uint32_t property);

/* Time, sleep and yield through WASI, for programs without a C library. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")))
int32_t wr_wasi_clock_time_get_(uint32_t clock, uint64_t precision, uint64_t* out_time);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("poll_oneoff")))
int32_t wr_wasi_poll_oneoff_(const void* subscriptions, void* events, uint32_t count, uint32_t* out_count);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("sched_yield")))
int32_t wr_wasi_sched_yield_(void);

/* Monotonic kernel time in milliseconds. */
static inline uint64_t wr_time_ms(void)
{
    uint64_t now = 0;
    wr_wasi_clock_time_get_(1, 1000000, &now);
    return now / 1000000u;
}

/* Sleep; only the calling program waits. */
static inline void wr_sleep_ms(uint32_t milliseconds)
{
    /* WASI subscription_t (48 bytes, relative clock) and event_t (32 bytes). */
    uint64_t subscription[6] = { 0, 0, 1, (uint64_t)milliseconds * 1000000u, 0, 0 };
    uint64_t event[4];
    uint32_t count = 0;
    wr_wasi_poll_oneoff_(subscription, event, 1, &count);
}

static inline void wr_yield(void)
{
    wr_wasi_sched_yield_();
}

/* Helpers for NUL-terminated strings. */
static inline uint32_t wr_strlen(const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
        ++length;
    return length;
}

static inline wr_str wr_cstr(const char* text)
{
    wr_str value = { text, wr_strlen(text) };
    return value;
}

#define wr_mutex_open(name) wr_mutex_create((name), wr_strlen(name))
#define wr_semaphore_open(name, max, initial) wr_semaphore_create((name), wr_strlen(name), (max), (initial))
#define wr_event_open(name) wr_event_create((name), wr_strlen(name))
#define wr_queue_open(name, item_size, capacity) wr_queue_create((name), wr_strlen(name), (item_size), (capacity))
#define wr_gpio_open_cstr(name, flags) wr_gpio_open((name), wr_strlen(name), (flags))
#define wr_display_open_cstr(name) wr_display_open((name), wr_strlen(name), 0)
#define wr_adc_open_cstr(name) wr_adc_open((name), wr_strlen(name), 0)
#define wr_pwm_open_cstr(name, frequency_hz, flags) wr_pwm_open((name), wr_strlen(name), (frequency_hz), (flags))
#define wr_uart_open_cstr(name, baud, format) wr_uart_open((name), wr_strlen(name), (baud), (format))

/* Send all of data, waiting for room as needed: the count sent, or an error. */
static inline int32_t wr_uart_write_all(int32_t port, const void* data, uint32_t length, uint32_t timeout_ms)
{
    uint32_t sent = 0;
    while (sent < length)
    {
        int32_t written = wr_uart_write(port, (const uint8_t*)data + sent, length - sent);
        if (written < 0)
            return written;
        sent += (uint32_t)written;
        if (sent < length)
        {
            int32_t ready = wr_uart_wait_writable(port, timeout_ms);
            if (ready < 0)
                return ready == -WR_ETIMEOUT || ready == -WR_EFULL ? (int32_t)sent : ready;
        }
    }
    return (int32_t)sent;
}
#define wr_i2c_open_cstr(bus, address) wr_i2c_open((bus), wr_strlen(bus), (address), 0)
#define wr_spi_open_cstr(bus, cs, mode, frequency_hz) \
    wr_spi_open((bus), wr_strlen(bus), (cs), wr_strlen(cs), (mode), (frequency_hz))

/* The usual I2C and SPI transactions. */
static inline int32_t wr_i2c_write(int32_t device, const void* data, uint32_t length)
{
    wr_i2c_op op = { WR_I2C_WRITE, (void*)data, length };
    return wr_i2c_transaction(device, &op, 1);
}

static inline int32_t wr_i2c_read(int32_t device, void* data, uint32_t length)
{
    wr_i2c_op op = { WR_I2C_READ, data, length };
    return wr_i2c_transaction(device, &op, 1);
}

/* Write (a register address, usually), then read with a repeated start. */
static inline int32_t wr_i2c_write_read(int32_t device, const void* out, uint32_t out_length, void* in,
                                        uint32_t in_length)
{
    wr_i2c_op ops[2] = { { WR_I2C_WRITE, (void*)out, out_length }, { WR_I2C_READ, in, in_length } };
    return wr_i2c_transaction(device, ops, 2);
}

static inline int32_t wr_spi_write(int32_t device, const void* data, uint32_t length)
{
    wr_spi_op op = { WR_SPI_WRITE, data, 0, length };
    return wr_spi_transaction(device, &op, 1);
}

static inline int32_t wr_spi_transfer(int32_t device, const void* out, void* in, uint32_t length)
{
    wr_spi_op op = { WR_SPI_TRANSFER, out, in, length };
    return wr_spi_transaction(device, &op, 1);
}
#define wr_input_open_cstr(name) wr_input_open((name), wr_strlen(name), 0)
#define wr_spawn_path_cstr(path, args, argc, priority, stack) \
    wr_spawn_path((path), wr_strlen(path), (args), (argc), (priority), (stack))

/*
 * Program metadata, the "wasm-rtos.app" custom section: records of a key
 * byte, a LEB128 length and the value. Use each macro once, at file scope.
 */
#define WR_APP_SECTION_ ".section \".custom_section.wasm-rtos.app\",\"\",@\n"
#define WR_APP_U32_(key, value) __asm__(WR_APP_SECTION_ ".int8 " #key "\n.uleb128 4\n.int32 " #value "\n.text\n")
#define WR_APP_TEXT_(key, text) \
    __asm__(WR_APP_SECTION_ ".int8 " #key "\n.uleb128 2f-1f\n1:\n.ascii \"" text "\"\n2:\n.text\n")

/* The ABI version the program was written for (major << 16 | minor). */
#define WR_APP_ABI_VERSION() WR_APP_U32_(1, 65540)
/* Interpreter stack in bytes, unless whoever starts the program picks one. */
#define WR_APP_STACK_SIZE(bytes) WR_APP_U32_(2, bytes)
/* Name shown for the program. */
#define WR_APP_NAME(text) WR_APP_TEXT_(3, text)

#ifdef __cplusplus
}
#endif

#endif
