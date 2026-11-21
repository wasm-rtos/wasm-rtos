#ifndef WASM_RTOS_HW_H
#define WASM_RTOS_HW_H

/*
 * wasm-rtos-hw: the devices programs reach (pins, buses, displays, input
 * devices, analog inputs and outputs, PWM, serial ports, pulse trains, 1-Wire
 * and counters). A port registers what its board has; programs reach it
 * through the ABI of devices. What the devices need from the operating
 * system is hw_host.h.
 */

#include <stdint.h>

/* Results of the calls below; the operating system uses the same values. */
typedef enum HwStatus
{
    HW_STATUS_OK = 0,
    HW_STATUS_ERROR,
    HW_STATUS_INVALID_ARGUMENT,
    HW_STATUS_OUT_OF_MEMORY,
    HW_STATUS_WASM_ERROR,
    HW_STATUS_TASK_DEAD,
    HW_STATUS_TASK_NOT_FOUND,
    HW_STATUS_NO_READY_TASKS,
    HW_STATUS_BUFFER_TOO_SMALL,
    HW_STATUS_UNSUPPORTED,
    HW_STATUS_OUT_OF_BOUNDS,
    HW_STATUS_QUEUE_FULL,
    HW_STATUS_QUEUE_EMPTY,
    HW_STATUS_QUEUE_NOT_FOUND,
    HW_STATUS_TIMEOUT,
    HW_STATUS_BUSY,
    HW_STATUS_NOT_OWNER,
    HW_STATUS_MUTEX_NOT_FOUND,
    HW_STATUS_SEMAPHORE_FULL,
    HW_STATUS_SEMAPHORE_NOT_FOUND,
    HW_STATUS_EVENT_GROUP_NOT_FOUND,
    HW_STATUS_TIMER_NOT_FOUND,
    HW_STATUS_ABORTED,
    HW_STATUS_NOT_WAITING,
    HW_STATUS_LIMIT_REACHED,
    HW_STATUS_FILE_NOT_FOUND,
    HW_STATUS_NOT_A_DIRECTORY,
    HW_STATUS_IS_A_DIRECTORY,
    HW_STATUS_READ_ONLY,
    HW_STATUS_END_OF_DIRECTORY,
    HW_STATUS_NACK,
    HW_STATUS_BUS_ERROR,
    HW_STATUS_PENDING
} HwStatus;

/* A timeout that never ends. */
#define HW_WAIT_FOREVER UINT32_MAX


/*
 * GPIO. The platform drives the pins through a port; the library lends them
 * to programs by name. Register every pin a program may use, under the names
 * of the device profile ("led", "button-a", "gpio5"); a pin may have several
 * names. A program owns a pin from open until it closes it or ends; then the
 * library configures the pin as an input without pulls.
 *
 * Port flags describe the physical pin: HW_GPIO_INIT_HIGH is the level an
 * output starts at, the edge flags enable edge reports through hw_gpio_edge.
 */
#define HW_GPIO_OUTPUT       0x01U
#define HW_GPIO_PULL_UP      0x02U
#define HW_GPIO_PULL_DOWN    0x04U
#define HW_GPIO_OPEN_DRAIN   0x08U
#define HW_GPIO_INIT_HIGH    0x20U
#define HW_GPIO_EDGE_RISING  0x40U
#define HW_GPIO_EDGE_FALLING 0x80U

/* What a pin can do (hw_gpio_add_pin capabilities). */
#define HW_GPIO_CAN_INPUT      0x01U
#define HW_GPIO_CAN_OUTPUT     0x02U
#define HW_GPIO_CAN_PULL_UP    0x04U
#define HW_GPIO_CAN_PULL_DOWN  0x08U
#define HW_GPIO_CAN_OPEN_DRAIN 0x10U
#define HW_GPIO_CAN_EDGES      0x20U
#define HW_GPIO_CAN_PWM        0x40U
#define HW_GPIO_CAN_PULSE      0x80U
#define HW_GPIO_CAN_ALL        0xFFU

/* hw_gpio_add_pin options: the board inverts the pin, so programs see 1 for "on". */
#define HW_GPIO_ACTIVE_LOW 0x01U

#define HW_GPIO_NAME_MAX 32U
#ifndef HW_GPIO_MAX_PINS
#define HW_GPIO_MAX_PINS 32U
#endif
#ifndef HW_GPIO_MAX_NAMES
#define HW_GPIO_MAX_NAMES 64U
#endif

typedef struct HwGpioPort
{
    void* context;
    HwStatus (*configure)(void* context, uint32_t pin, uint32_t flags);
    HwStatus (*write)(void* context, uint32_t pin, uint8_t level);
    HwStatus (*read)(void* context, uint32_t pin, uint8_t* out_level);
} HwGpioPort;

/* NULL removes the port; programs then get -WR_EUNSUPPORTED. */
HwStatus hw_gpio_port_set(const HwGpioPort* port);

/*
 * Make a pin available to programs under name. Registering another name for
 * the same pin needs the same capabilities and options. HW_STATUS_BUSY when
 * the name is taken, HW_STATUS_LIMIT_REACHED past HW_GPIO_MAX_PINS or
 * HW_GPIO_MAX_NAMES.
 */
HwStatus hw_gpio_add_pin(const char* name, uint32_t pin, uint32_t capabilities, uint32_t options);

/*
 * Report edges on a pin (HW_GPIO_EDGE_RISING and/or HW_GPIO_EDGE_FALLING).
 * Safe to call from an interrupt handler: it only counts them; the next
 * scheduling decision (hw_drivers_poll) delivers them to the program
 * watching the pin.
 */
void hw_gpio_edge(uint32_t pin, uint32_t edges);

/* The id of the task that owns a pin, 0 when it is free or unknown. */
uint32_t hw_gpio_get_owner(uint32_t pin);

/*
 * The pins programs wait on, for a board about to sleep: each pin (the
 * port's number) with the edges watched on it as the port sees them
 * (HW_GPIO_EDGE_RISING, HW_GPIO_EDGE_FALLING). Fills up to max and returns
 * how many there are. The port arms them as wake sources; after the
 * restore, it reports the edge that woke the board with hw_gpio_edge.
 */
typedef struct HwGpioWake
{
    uint32_t pin;
    uint32_t edges;
} HwGpioWake;
uint32_t hw_gpio_wake_pins(HwGpioWake* out, uint32_t max);

/*
 * Displays. The platform registers each display a program may draw on under
 * the device profile's name ("main" for the primary one) and draws through a
 * port; the library lends a display to one program at a time and checks every
 * write, so the port only ever sees whole rectangles inside the display in a
 * format it accepts.
 *
 * Pixel formats, as programs write them:
 *   HW_DISPLAY_MONO1     1 bit per pixel, 1 = lit; rows of whole bytes, the
 *                        leftmost pixel in the most significant bit
 *   HW_DISPLAY_RGB565    16-bit little-endian, red in the top 5 bits
 *   HW_DISPLAY_XRGB8888  32-bit little-endian 0x00RRGGBB
 * A port converts to the panel's own order (such as big-endian RGB565 over
 * SPI) itself.
 */
#define HW_DISPLAY_MONO1    1U
#define HW_DISPLAY_RGB565   2U
#define HW_DISPLAY_XRGB8888 3U
#define HW_DISPLAY_FORMAT_BIT(format) (1U << (format))

#define HW_DISPLAY_RECTANGLE 0U
/* A circle inscribed in width x height; pixels outside it are not shown. */
#define HW_DISPLAY_ROUND     1U

#ifndef HW_DISPLAY_MAX
#define HW_DISPLAY_MAX 2U
#endif
#define HW_DISPLAY_NAME_MAX 32U

typedef struct HwDisplayInfo
{
    uint16_t width;
    uint16_t height;
    uint8_t shape;            /* HW_DISPLAY_RECTANGLE or HW_DISPLAY_ROUND */
    uint8_t format;           /* the panel's own format, the fastest to write */
    uint16_t dpi;             /* pixels per inch, 0 when unknown */
    uint32_t formats;         /* HW_DISPLAY_FORMAT_BIT of each format write accepts; includes format */
    uint8_t has_brightness;   /* set_brightness works */
    uint8_t brightness;       /* the level at registration, 0-255 */
    uint8_t keeps_image;      /* keeps its picture while the board sleeps: a saved system may hold it */
} HwDisplayInfo;

/*
 * write puts a rectangle of pixels on the display; pixels is valid only
 * during the call. fill paints a rectangle one color (a pixel value in
 * format); without it, or when it returns HW_STATUS_UNSUPPORTED for that
 * display or format, the library writes rows. flush makes everything written
 * visible: a port that buffers (e-paper, a frame it sends whole) shows it
 * now; one that writes the panel directly may leave it NULL.
 */
typedef struct HwDisplayPort
{
    void* context;
    HwStatus (*write)(void* context, uint32_t display, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t format, const void* pixels, uint32_t size);
    HwStatus (*fill)(void* context, uint32_t display, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                     uint32_t format, uint32_t color);
    HwStatus (*flush)(void* context, uint32_t display);
    HwStatus (*set_brightness)(void* context, uint32_t display, uint8_t level);
} HwDisplayPort;

/* NULL removes the port; programs then get -WR_EUNSUPPORTED. */
HwStatus hw_display_port_set(const HwDisplayPort* port);
/*
 * Make display number display available to programs under name.
 * HW_STATUS_BUSY when the name or number is taken, HW_STATUS_LIMIT_REACHED
 * past HW_DISPLAY_MAX.
 */
HwStatus hw_display_add(const char* name, uint32_t display, const HwDisplayInfo* info);
/* The id of the task that owns a display, 0 when it is free or unknown. */
uint32_t hw_display_get_owner(uint32_t display);

/*
 * Input devices: a group of buttons, a rotary control such as a bezel or a
 * knob, a touch surface, a keyboard. The platform registers each under the
 * device profile's name ("buttons", "bezel", "touch", "keyboard") and reports
 * what happens; the library queues events for the program that owns the
 * device and makes long presses and key repeat from the timings given here,
 * so they feel the same in every program.
 *
 * Keys and buttons use Linux input event codes (KEY_*, BTN_*); rotation is
 * reported for a REL_* code, such as REL_DIAL for a bezel or REL_WHEEL.
 * Report from the context that schedules (hw_drivers_poll), not from an interrupt
 * handler.
 */
#define HW_INPUT_BUTTONS  1U
#define HW_INPUT_ROTARY   2U
#define HW_INPUT_TOUCH    3U
#define HW_INPUT_KEYBOARD 4U

#ifndef HW_INPUT_MAX
#define HW_INPUT_MAX 8U
#endif
/* Events a device holds for its program before old ones are dropped. */
#ifndef HW_INPUT_QUEUE
#define HW_INPUT_QUEUE 32U
#endif
/* Keys a device tracks as held, for long presses and repeat. */
#ifndef HW_INPUT_MAX_HELD
#define HW_INPUT_MAX_HELD 4U
#endif
#define HW_INPUT_NAME_MAX 32U

typedef struct HwInputInfo
{
    uint8_t kind;                /* HW_INPUT_* */
    uint16_t width;              /* touch: coordinates run 0..width-1, 0..height-1 */
    uint16_t height;
    uint16_t steps_per_turn;     /* rotary: detents in a full turn, 0 when unknown */
    uint16_t long_press_ms;      /* keys held this long report a long press; 0: never */
    uint16_t repeat_delay_ms;    /* keys held this long start repeating; 0: never */
    uint16_t repeat_interval_ms; /* then repeat this often */
} HwInputInfo;

/* HW_STATUS_BUSY when the name or number is taken, HW_STATUS_LIMIT_REACHED past HW_INPUT_MAX. */
HwStatus hw_input_add(const char* name, uint32_t device, const HwInputInfo* info);
/* A key or button went down (1) or up (0). */
HwStatus hw_input_key(uint32_t device, uint32_t code, uint8_t down);
/* steps detents on axis code (REL_*), positive clockwise. */
HwStatus hw_input_rotate(uint32_t device, uint32_t code, int32_t steps);
/* Contact contact touched (HW_INPUT_TOUCH_DOWN), moved, or lifted at x, y. */
#define HW_INPUT_TOUCH_UP   0U
#define HW_INPUT_TOUCH_DOWN 1U
#define HW_INPUT_TOUCH_MOVE 2U
HwStatus hw_input_touch(uint32_t device, uint32_t contact, uint32_t phase, uint32_t x, uint32_t y);
/* Text a keyboard produced, one Unicode code point, besides its key events. */
HwStatus hw_input_text(uint32_t device, uint32_t code_point);
/* The id of the task that owns a device, 0 when it is free or unknown. */
uint32_t hw_input_get_owner(uint32_t device);

/*
 * Buses. The platform registers each bus under the device profile's name
 * ("i2c0", "spi0", "ow0") and moves the bytes through a port; the library lends
 * devices on a bus to programs and checks every transaction, so a port only
 * ever sees whole, valid transactions. Data pointers point into the
 * program's memory and are valid during the call.
 *
 * A transaction runs to the end before the next one starts: one program's
 * bytes never mix with another's.
 *
 * A port may work in the background (DMA): transaction starts the transfer
 * and returns HW_STATUS_PENDING, and the port reports the end with
 * hw_bus_complete. Only the calling program waits meanwhile; the others
 * run. Until then the ops array and every buffer stay valid and untouched by
 * the library: the program is not swapped out, and before it is paused,
 * cancelled or ended the library calls finish, which returns only once the
 * transfer is over and reported. A port that never returns
 * HW_STATUS_PENDING may leave finish NULL.
 */
#ifndef HW_BUS_MAX_OPS
#define HW_BUS_MAX_OPS 16U
#endif
#define HW_BUS_NAME_MAX 32U

/* The ops of a transaction; see I2C, SPI and 1-Wire below. */
#define HW_I2C_WRITE 1U
#define HW_I2C_READ  2U

typedef struct HwI2cOp
{
    uint8_t kind;      /* HW_I2C_WRITE or HW_I2C_READ */
    uint8_t* data;     /* written from, or read into */
    uint32_t length;
} HwI2cOp;

#define HW_SPI_WRITE    1U
#define HW_SPI_READ     2U
#define HW_SPI_TRANSFER 3U
#define HW_SPI_DELAY_US 4U

typedef struct HwSpiOp
{
    uint8_t kind;          /* HW_SPI_* */
    const uint8_t* write;  /* WRITE and TRANSFER */
    uint8_t* read;         /* READ and TRANSFER */
    uint32_t length;       /* bytes, or microseconds for DELAY_US */
} HwSpiOp;

#define HW_ONEWIRE_WRITE      1U
#define HW_ONEWIRE_READ       2U
#define HW_ONEWIRE_WRITE_BITS 3U
#define HW_ONEWIRE_READ_BITS  4U
#define HW_ONEWIRE_POWER      5U
#define HW_ONEWIRE_RESET      6U

typedef struct HwOneWireOp
{
    uint8_t kind;      /* HW_ONEWIRE_* */
    uint8_t* data;     /* written from, or read into; bits least significant first */
    uint32_t length;   /* bytes, bits for the _BITS kinds, milliseconds for POWER */
} HwOneWireOp;

/*
 * I2C: ops in order, a repeated start where a read follows a write or a
 * write follows a read, a stop at the end. Adjacent ops of the same kind
 * continue without a start. No ops: just the address, to see whether a
 * device answers. Return HW_STATUS_NACK when the address or a byte is not
 * acknowledged, HW_STATUS_BUS_ERROR for a lost arbitration or a stuck bus,
 * HW_STATUS_TIMEOUT when a device stretches the clock too long.
 */
#ifndef HW_I2C_MAX_BUSES
#define HW_I2C_MAX_BUSES 2U
#endif
/* Devices programs may hold open at once, over all buses. */
#ifndef HW_I2C_MAX_DEVICES
#define HW_I2C_MAX_DEVICES 16U
#endif

typedef struct HwI2cPort
{
    void* context;
    HwStatus (*transaction)(void* context, uint32_t bus, uint32_t address, uint32_t frequency_hz,
                            const HwI2cOp* ops, uint32_t count);
    void (*finish)(void* context, uint32_t bus);
} HwI2cPort;

/* NULL removes the port; programs then get -WR_EUNSUPPORTED. */
HwStatus hw_i2c_port_set(const HwI2cPort* port);
/* HW_STATUS_BUSY when the name or number is taken, HW_STATUS_LIMIT_REACHED past HW_I2C_MAX_BUSES. */
HwStatus hw_i2c_add_bus(const char* name, uint32_t bus, uint32_t frequency_hz);
/* The id of the task that has address open on bus, 0 when none. */
uint32_t hw_i2c_get_owner(uint32_t bus, uint32_t address);

/*
 * SPI: chip select (when the library drives it) goes low before the first op
 * and high after the last, also after an error. A read sends zeros; a
 * transfer sends and receives at once, and may read into the bytes it
 * sends. A delay waits length microseconds with chip select held.
 * mode: HW_SPI_CPHA | HW_SPI_CPOL (SPI modes 0-3), HW_SPI_LSB_FIRST.
 */
#define HW_SPI_CPHA      0x01U
#define HW_SPI_CPOL      0x02U
#define HW_SPI_LSB_FIRST 0x04U

#ifndef HW_SPI_MAX_BUSES
#define HW_SPI_MAX_BUSES 2U
#endif
#ifndef HW_SPI_MAX_DEVICES
#define HW_SPI_MAX_DEVICES 8U
#endif

typedef struct HwSpiPort
{
    void* context;
    HwStatus (*transaction)(void* context, uint32_t bus, uint32_t mode, uint32_t frequency_hz,
                            const HwSpiOp* ops, uint32_t count);
    void (*finish)(void* context, uint32_t bus);
} HwSpiPort;

HwStatus hw_spi_port_set(const HwSpiPort* port);
/* max_hz caps what programs ask for. */
HwStatus hw_spi_add_bus(const char* name, uint32_t bus, uint32_t max_hz);
/* The id of the task that holds bus exclusively or has a device on it with chip select pin cs_pin; 0 when none. */
uint32_t hw_spi_get_owner(uint32_t bus, uint32_t cs_pin);

/*
 * 1-Wire: one data line with a pull-up, devices told apart by a 64-bit ROM
 * code (family code in the low byte, CRC in the high one). Ops in order:
 *   RESET       a reset pulse; HW_STATUS_NACK when no device answers with a
 *               presence pulse
 *   WRITE/READ  bytes, least significant bit first; a read sends 1 slots
 *   WRITE_BITS/READ_BITS  single time slots: length bits, packed least
 *               significant first
 *   POWER       right after a write, hold the line high through a strong
 *               pull-up (parasite-powered devices converting or copying)
 *               for length milliseconds; 0 keeps it high until the next
 *               transaction on the bus (POWER 0 comes last). Ports that
 *               cannot drive the line return HW_STATUS_UNSUPPORTED.
 * A transaction without ops only ends a strong pull-up left on.
 *
 * For a program's device the library sends RESET and the ROM command (match
 * ROM with the code) itself before the program's ops. triplet, when the
 * port has it, runs one step of a ROM search: it reads a bit and its
 * complement, writes the bit chosen (that bit when they differ, direction
 * when both are 0) and returns bit 0 = the bit, bit 1 = the complement,
 * bit 2 = the direction written. Without it the library searches with bit
 * ops. A search runs at once: a port returning HW_STATUS_PENDING is
 * finished on the spot.
 */
#ifndef HW_ONEWIRE_MAX_BUSES
#define HW_ONEWIRE_MAX_BUSES 2U
#endif
/* Devices programs may hold open at once, over all buses. */
#ifndef HW_ONEWIRE_MAX_DEVICES
#define HW_ONEWIRE_MAX_DEVICES 8U
#endif
#define HW_ONEWIRE_MAX_POWER_MS 65535U
/* hw_onewire_get_owner: the program that holds the whole bus. */
#define HW_ONEWIRE_WHOLE_BUS 0U

typedef struct HwOneWirePort
{
    void* context;
    HwStatus (*transaction)(void* context, uint32_t bus, const HwOneWireOp* ops, uint32_t count);
    HwStatus (*triplet)(void* context, uint32_t bus, uint8_t direction, uint8_t* out_bits);
    void (*finish)(void* context, uint32_t bus);
} HwOneWirePort;

HwStatus hw_onewire_port_set(const HwOneWirePort* port);
/*
 * A bus on a pin is best registered under the pin's name ("gpio4"), which
 * is what Arduino's OneWire(4) opens.
 */
HwStatus hw_onewire_add_bus(const char* name, uint32_t bus);
/* The id of the task that has rom open on bus (HW_ONEWIRE_WHOLE_BUS: holds the bus), 0 when none. */
uint32_t hw_onewire_get_owner(uint32_t bus, uint64_t rom);
/* The 1-Wire CRC-8 (polynomial x^8 + x^5 + x^4 + 1) of length bytes. */
uint8_t hw_onewire_crc8(const uint8_t* data, uint32_t length);

/*
 * Every finished transaction, for bus analyzers and logs: which program,
 * which bus and device, the ops with the bytes written and read (valid
 * during the call) and the result. device is the I2C address, or the chip
 * select pin (HW_BUS_NO_CS when the program drives it), or for 1-Wire 0:
 * the ROM goes out in the ops (the library's RESET and match ROM included).
 */
#define HW_BUS_I2C 1U
#define HW_BUS_SPI 2U
#define HW_BUS_ONEWIRE 3U
#define HW_BUS_NO_CS 0xFFFFFFFFU

typedef struct HwBusTrace
{
    uint8_t kind;           /* HW_BUS_I2C, HW_BUS_SPI or HW_BUS_ONEWIRE */
    uint32_t bus;
    uint32_t device;
    uint32_t task_id;
    uint32_t frequency_hz;
    const void* ops;        /* const HwI2cOp*, const HwSpiOp* or const HwOneWireOp* */
    uint32_t count;
    HwStatus status;
} HwBusTrace;

typedef void (*HwBusTraceFunction)(void* context, const HwBusTrace* trace);
/* NULL stops tracing. */
void hw_bus_trace_set(HwBusTraceFunction function, void* context);

/*
 * The end of a transfer a port started with HW_STATUS_PENDING, with its
 * result (kind is HW_BUS_I2C, HW_BUS_SPI or HW_BUS_ONEWIRE). Safe to call
 * from an interrupt handler: it only records the result; the next
 * The next scheduling decision (or finish) delivers it to the waiting program. With a tickless clock, wake the
 * scheduler afterwards as for hw_gpio_edge.
 */
void hw_bus_complete(uint32_t kind, uint32_t bus, HwStatus status);
/* Whether a background transfer is in progress on a bus. */
uint8_t hw_bus_is_busy(uint32_t kind, uint32_t bus);

/*
 * Analog inputs, registered under the device profile's names ("a0",
 * "battery"). Reading changes nothing, so programs share a channel. read
 * returns the converter's raw value; read_mv, when the port has one, a
 * calibrated voltage. Without it, or when it returns HW_STATUS_UNSUPPORTED
 * for a channel, the library scales the raw value.
 */
#ifndef HW_ADC_MAX
#define HW_ADC_MAX 8U
#endif
#define HW_ADC_NAME_MAX 32U

typedef struct HwAdcInfo
{
    uint8_t resolution_bits;   /* raw values run 0 .. 2^bits - 1 */
    uint32_t full_scale_mv;    /* the voltage of the largest raw value */
} HwAdcInfo;

typedef struct HwAdcPort
{
    void* context;
    HwStatus (*read)(void* context, uint32_t channel, uint32_t* out_raw);
    HwStatus (*read_mv)(void* context, uint32_t channel, uint32_t* out_mv);
} HwAdcPort;

HwStatus hw_adc_port_set(const HwAdcPort* port);
/* HW_STATUS_BUSY when the name or number is taken, HW_STATUS_LIMIT_REACHED past HW_ADC_MAX. */
HwStatus hw_adc_add_channel(const char* name, uint32_t channel, const HwAdcInfo* info);

/*
 * Analog outputs, registered under the device profile's names. One program
 * owns an output at a time. A channel named like a GPIO pin ("gpio25") is
 * that pin: opening it takes the pin as an opened GPIO would, so nothing
 * else drives it, and closing gives it back as an input. Other names are
 * outputs of their own (an external DAC chip).
 *
 * start (may be NULL) switches the channel on; write sets a raw value,
 * 0 .. 2^resolution_bits - 1, already checked; stop switches it off. The
 * output is 0 when a program opens it. Voltages are scaled by the library
 * from full_scale_mv, the voltage of the largest raw value.
 */
#ifndef HW_DAC_MAX
#define HW_DAC_MAX 4U
#endif
#define HW_DAC_NAME_MAX 32U

typedef struct HwDacInfo
{
    uint8_t resolution_bits;
    uint32_t full_scale_mv;
} HwDacInfo;

typedef struct HwDacPort
{
    void* context;
    HwStatus (*start)(void* context, uint32_t channel);
    HwStatus (*write)(void* context, uint32_t channel, uint32_t raw);
    void (*stop)(void* context, uint32_t channel);
} HwDacPort;

HwStatus hw_dac_port_set(const HwDacPort* port);
/* HW_STATUS_BUSY when the name or number is taken, HW_STATUS_LIMIT_REACHED past HW_DAC_MAX. */
HwStatus hw_dac_add_channel(const char* name, uint32_t channel, const HwDacInfo* info);
/* The id of the task that owns a channel, 0 when it is free. */
uint32_t hw_dac_get_owner(uint32_t channel);

/*
 * Pulse-width outputs on GPIO pins that have HW_GPIO_CAN_PWM. A program owns
 * the pin as it would through gpio; closing gives it back as an input.
 * Duty is 0 (always low) to 65535 (always high) on the pin; the library has
 * already applied the board's and the program's inversion. A port without a
 * free channel or timer for the pin returns HW_STATUS_LIMIT_REACHED; it may
 * round the frequency and reports the one it set.
 */
#define HW_PWM_DUTY_MAX 65535U

typedef struct HwPwmPort
{
    void* context;
    HwStatus (*start)(void* context, uint32_t pin, uint32_t frequency_hz, uint32_t duty, uint32_t* out_frequency_hz);
    HwStatus (*set_duty)(void* context, uint32_t pin, uint32_t duty);
    HwStatus (*set_frequency)(void* context, uint32_t pin, uint32_t frequency_hz, uint32_t* out_frequency_hz);
    void (*stop)(void* context, uint32_t pin);
} HwPwmPort;

HwStatus hw_pwm_port_set(const HwPwmPort* port);

/*
 * Serial ports, registered under the device profile's names ("uart1",
 * "gps"), one owner each. Received bytes go into a buffer of the library the port
 * fills with hw_uart_receive; sending hands bytes to the port's write,
 * which takes what fits in its own buffer and returns how many. When it
 * has room again it calls hw_uart_writable. Both are safe in interrupt
 * handlers; the next scheduling decision (hw_drivers_poll) wakes the programs waiting for them.
 */
#ifndef HW_UART_MAX
#define HW_UART_MAX 4U
#endif
/* Bytes the library keeps for a program between reads; a power of two. */
#ifndef HW_UART_RX_BUFFER
#define HW_UART_RX_BUFFER 256U
#endif
#define HW_UART_NAME_MAX 32U

/* Frame format for configure; 8 data bits, no parity, 1 stop bit when 0. */
#define HW_UART_PARITY_EVEN 0x01U
#define HW_UART_PARITY_ODD  0x02U
#define HW_UART_STOP_2      0x04U
#define HW_UART_DATA_7      0x08U
#define HW_UART_FLOW_RTS_CTS 0x10U

typedef struct HwUartPort
{
    void* context;
    HwStatus (*configure)(void* context, uint32_t uart, uint32_t baud, uint32_t format, uint32_t* out_baud);
    /* Takes up to size bytes into the port's own buffer; how many in *out_written. */
    HwStatus (*write)(void* context, uint32_t uart, const uint8_t* data, uint32_t size, uint32_t* out_written);
    /* The port stops receiving: the program closed the port. May be NULL. */
    void (*close)(void* context, uint32_t uart);
} HwUartPort;

HwStatus hw_uart_port_set(const HwUartPort* port);
/* max_baud caps what programs ask for (0: no cap). HW_STATUS_BUSY or HW_STATUS_LIMIT_REACHED as above. */
HwStatus hw_uart_add(const char* name, uint32_t uart, uint32_t max_baud);
/* Received bytes; those that do not fit are dropped and counted. */
void hw_uart_receive(uint32_t uart, const uint8_t* data, uint32_t size);
/* A frame error, parity error or overrun in the port: counted. */
void hw_uart_error(uint32_t uart);
/* The port's send buffer has room again. */
void hw_uart_writable(uint32_t uart);
/* The id of the task that owns a port, 0 when it is free. */
uint32_t hw_uart_get_owner(uint32_t uart);

/*
 * Counters, registered under the device profile's names ("enc0", "flow"):
 * a pulse counter on one pin, or a quadrature encoder on two. Hardware
 * counts (a pulse counter unit, a timer in encoder mode), so no edge is
 * lost to a busy program; one owner each.
 *
 * start begins counting from 0 in a mode (the edges counted, or the
 * encoder's resolution) with a glitch filter the port rounds to what it has
 * (0: none) and reports. read returns the count as 32 bits that wrap: a
 * port with a 16-bit counter extends it on its overflow interrupt. write
 * sets the count.
 *
 * watch, when the port has it, arms the hardware to call
 * hw_counter_event once the count goes below low or above high (extra
 * calls are harmless: the library reads the count again). Without it the
 * library reads watched counters every HW_COUNTER_POLL_MS.
 */
#ifndef HW_COUNTER_MAX
#define HW_COUNTER_MAX 4U
#endif
#ifndef HW_COUNTER_POLL_MS
#define HW_COUNTER_POLL_MS 10U
#endif
#define HW_COUNTER_NAME_MAX 32U

#define HW_COUNTER_PULSE      1U  /* one pin */
#define HW_COUNTER_QUADRATURE 2U  /* two pins, A and B */

/* Modes: the edges a pulse counter counts, or an encoder's steps per cycle. */
#define HW_COUNTER_RISING  1U
#define HW_COUNTER_FALLING 2U
#define HW_COUNTER_BOTH    3U
#define HW_COUNTER_X1      4U
#define HW_COUNTER_X2      5U
#define HW_COUNTER_X4      6U

typedef struct HwCounterInfo
{
    uint8_t kind;      /* HW_COUNTER_PULSE or HW_COUNTER_QUADRATURE */
    uint32_t max_hz;   /* the fastest edges it counts, 0 when unknown */
} HwCounterInfo;

typedef struct HwCounterPort
{
    void* context;
    HwStatus (*start)(void* context, uint32_t unit, uint32_t mode, uint32_t filter_ns, uint32_t* out_filter_ns);
    HwStatus (*read)(void* context, uint32_t unit, int32_t* out_count);
    HwStatus (*write)(void* context, uint32_t unit, int32_t count);
    /* May be NULL. */
    HwStatus (*watch)(void* context, uint32_t unit, int32_t low, int32_t high);
    void (*stop)(void* context, uint32_t unit);
} HwCounterPort;

HwStatus hw_counter_port_set(const HwCounterPort* port);
/* HW_STATUS_BUSY when the name or number is taken, HW_STATUS_LIMIT_REACHED past HW_COUNTER_MAX. */
HwStatus hw_counter_add(const char* name, uint32_t unit, const HwCounterInfo* info);
/* The count left the window given to watch. Safe in an interrupt handler. */
void hw_counter_event(uint32_t unit);
/* The id of the task that owns a counter, 0 when it is free. */
uint32_t hw_counter_get_owner(uint32_t unit);

/*
 * Timed pulse trains on GPIO pins that have HW_GPIO_CAN_PULSE, for what needs
 * timing finer than a program can keep: addressable LEDs, infrared remotes,
 * echo and one-wire-like sensors. Hardware made for it (a remote-control
 * peripheral, programmable I/O, a timer with DMA) does the work.
 *
 * An item is 32 bits: HW_PULSE_HIGH for the level, the low 31 bits the time
 * in nanoseconds. The port rounds to its resolution.
 *
 * send, send_bits and receive may return HW_STATUS_PENDING and report the
 * end with hw_pulse_complete; meanwhile the items and data stay valid and
 * belong to the port, exactly as for background bus transfers, and finish
 * must end the work at once. receive writes captured items into the
 * program's buffer: it starts at the first edge, ends after idle_ns without
 * one or when the buffer is full, and fails with HW_STATUS_TIMEOUT when no
 * edge comes within timeout_ms (the port times it, so the buffer is never
 * left to it after the library has moved on).
 */
#define HW_PULSE_HIGH       0x80000000U
#define HW_PULSE_TIME_MASK  0x7FFFFFFFU
#define HW_PULSE_RECEIVE    0x01U  /* an input that captures; otherwise an output */
#define HW_PULSE_IDLE_HIGH  0x02U  /* outputs: the level between trains */
#ifndef HW_PULSE_MAX
#define HW_PULSE_MAX 4U
#endif

typedef struct HwPulsePort
{
    void* context;
    /* The pin's direction and idle level; HW_STATUS_LIMIT_REACHED when no channel is free. */
    HwStatus (*configure)(void* context, uint32_t pin, uint32_t flags);
    /* carrier_hz 0: plain levels; otherwise high items are that carrier (infrared). */
    HwStatus (*send)(void* context, uint32_t pin, const uint32_t* items, uint32_t count, uint32_t carrier_hz);
    /*
     * Bytes, most significant bit first: a 0 bit is timing[0] ns high then
     * timing[1] ns low, a 1 bit timing[2] high then timing[3] low. May be
     * NULL: programs then get -WR_EUNSUPPORTED.
     */
    HwStatus (*send_bits)(void* context, uint32_t pin, const uint8_t* data, uint32_t length, const uint32_t timing[4]);
    /* May be NULL when no pin can capture. Done at once: the items written in *out_count. */
    HwStatus (*receive)(void* context, uint32_t pin, uint32_t* items, uint32_t max, uint32_t idle_ns,
                        uint32_t timeout_ms, uint32_t* out_count);
    void (*finish)(void* context, uint32_t pin);
    /* The pin goes back to the library: stop and free its channel. */
    void (*release)(void* context, uint32_t pin);
    /* Nanoseconds of one tick of the hardware, for programs that want to know. */
    uint32_t resolution_ns;
} HwPulsePort;

HwStatus hw_pulse_port_set(const HwPulsePort* port);
/*
 * The end of work a port started with HW_STATUS_PENDING: its result and,
 * for receive, how many items it wrote. Safe in an interrupt handler.
 */
void hw_pulse_complete(uint32_t pin, HwStatus status, uint32_t count);


#endif
