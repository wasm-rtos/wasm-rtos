# Porting the devices

How to give programs the devices of a board: pins, displays, input devices,
buses, analog inputs and outputs, PWM, serial ports, pulse trains, 1-Wire and
counters. The [README](../README.md) has the overview and the limits; `hw.h`
and `hw_driver.h` document every call. The clock, paging, program output,
snapshots and deep sleep belong to the operating system (the wasm-rtos
kernel's `docs/porting.md`).

## GPIO

The library lends pins to programs by name and drives them through a GPIO
port: `configure`, `write` and `read` for one pin number of the chip.
Register the pins of the device profile once; a program then opens `"led"`
on every board that has one.

```c
static const HwGpioPort gpio = { NULL, board_configure, board_write, board_read };
hw_gpio_port_set(&gpio);
hw_gpio_add_pin("led", 2, HW_GPIO_CAN_OUTPUT, HW_GPIO_ACTIVE_LOW);
hw_gpio_add_pin("button-a", 0, HW_GPIO_CAN_INPUT | HW_GPIO_CAN_PULL_UP | HW_GPIO_CAN_EDGES, 0);

void IRAM_ATTR button_isr(void) { hw_gpio_edge(0, HW_GPIO_EDGE_FALLING); }
```

`configure` gets the physical mode and the edges to report; the interrupt
handler only calls `hw_gpio_edge`, which counts and is safe there, and the
next scheduling decision (`hw_drivers_poll`) wakes the program waiting for that pin. A program owns a
pin until it closes it or ends, even by a trap; the library then configures
it as an input without pulls.

## Display

Register each display of the device profile with its size, shape, pixel
formats and density, and draw through a display port. The library checks
every call, so `write` only ever gets a whole rectangle inside the display
in a format it accepts; the pixels are the program's buffer (or the library's
copy of it, when the program's memory is paged) and are valid during the
call. `fill` and `flush` are optional: without `fill`, or
when it returns `HW_STATUS_UNSUPPORTED` for a display or format, the library
writes rows, and `flush` is for displays that buffer (e-paper, a frame
sent whole).

```c
static const HwDisplayPort display = { NULL, lcd_write, lcd_fill, NULL, lcd_backlight };
static const HwDisplayInfo round_lcd = {
    .width = 240, .height = 240, .shape = HW_DISPLAY_ROUND, .format = HW_DISPLAY_RGB565, .dpi = 260,
    .formats = HW_DISPLAY_FORMAT_BIT(HW_DISPLAY_RGB565), .has_brightness = 1, .brightness = 200,
};
hw_display_port_set(&display);
hw_display_add("main", 0, &round_lcd);
```

A port converts to the panel's own order, such as big-endian RGB565 over SPI.

## Input

Register the input devices of the device profile and report what happens;
the library queues events for the program that owns each device and makes
long presses and key repeat from the timings given, so they feel the same in
every program. Keys use the Linux input event codes.

```c
static const HwInputInfo bezel = { .kind = HW_INPUT_ROTARY, .steps_per_turn = 24 };
static const HwInputInfo buttons = { .kind = HW_INPUT_BUTTONS, .long_press_ms = 600 };
hw_input_add("bezel", 0, &bezel);
hw_input_add("buttons", 1, &buttons);

hw_input_rotate(0, 7 /* REL_DIAL */, -1);      /* one detent counter-clockwise */
hw_input_key(1, 158 /* KEY_BACK */, 1);        /* pressed */
```

Report from the context that schedules (`hw_drivers_poll`); an interrupt handler
should only note what happened (as with `hw_gpio_edge`) and let the main
loop report it.

## Buses

Register the I2C and SPI buses of the device profile with their speed and
move bytes through a port. The library lends devices on them to programs,
checks every transaction and drives SPI chip selects through the GPIO port,
so the port gets whole transactions: a list of ops with the program's
buffers (or the library's copies of them, when the program's memory is
paged), valid until the transaction ends. A trace hook sees every finished
transaction, for logs and bus analyzers.

```c
static const HwI2cPort i2c = { NULL, board_i2c_transaction, NULL };
static const HwSpiPort spi = { NULL, board_spi_transaction, NULL };  /* synchronous: no finish */
hw_i2c_port_set(&i2c);
hw_spi_port_set(&spi);
hw_i2c_add_bus("i2c0", 0, 400000);
hw_spi_add_bus("spi0", 2, 40000000);
hw_gpio_add_pin("lcd-cs", 5, HW_GPIO_CAN_OUTPUT, 0);
hw_bus_trace_set(log_transaction, NULL);
```

An I2C port returns `HW_STATUS_NACK`, `HW_STATUS_BUS_ERROR` or
`HW_STATUS_TIMEOUT` as the bus reports them; programs get `-WR_ENACK`,
`-WR_EBUS` and `-WR_ETIMEOUT`.

A port with DMA starts the transfer and returns `HW_STATUS_PENDING`; only the
calling program waits, the others run. The interrupt at the end of the
transfer calls `hw_bus_complete(kind, bus, status)`, which only records it;
the next scheduling decision raises chip select, traces the transaction and wakes
the program. Until then the buffers belong to the port: the library does not
swap the program out, and before it pauses, cancels or ends the program it
calls the port's `finish`, which returns once the transfer is over and
reported.

```c
static HwStatus lcd_spi_transaction(void* context, uint32_t bus, uint32_t mode, uint32_t hz,
                                    const HwSpiOp* ops, uint32_t count)
{
    start_dma(bus, mode, hz, ops, count);    /* the ops and buffers stay valid until the end */
    return HW_STATUS_PENDING;
}

void IRAM_ATTR spi_dma_done_isr(void) { hw_bus_complete(HW_BUS_SPI, 2, HW_STATUS_OK); }
static void lcd_spi_finish(void* context, uint32_t bus) { wait_for_dma_done(bus); }
```

## Analog inputs, PWM and serial ports

```c
static const HwAdcPort adc = { NULL, board_adc_read, board_adc_read_mv };   /* read_mv may be NULL */
static const HwAdcInfo pot = { .resolution_bits = 12, .full_scale_mv = 3300 };
hw_adc_port_set(&adc);
hw_adc_add_channel("a0", 6, &pot);

static const HwPwmPort pwm = { NULL, ledc_start, ledc_set_duty, ledc_set_frequency, ledc_stop };
hw_pwm_port_set(&pwm);
hw_gpio_add_pin("led", 2, HW_GPIO_CAN_OUTPUT | HW_GPIO_CAN_PWM, HW_GPIO_ACTIVE_LOW);

static const HwUartPort uart = { NULL, uart_configure, uart_write, uart_close };
hw_uart_port_set(&uart);
hw_uart_add("uart1", 1, 921600);
void IRAM_ATTR uart1_rx_isr(void) { hw_uart_receive(1, fifo, count); }
void IRAM_ATTR uart1_tx_isr(void) { hw_uart_writable(1); }
```

A PWM port gets the duty on the pin, 0 to 65535, after the library has
applied the board's and the program's inversion; it scales to its own
resolution and may round the frequency, reporting what it set. A UART
port's `write` takes what fits in its send buffer and returns how many
bytes; when room comes back it calls `hw_uart_writable`. Received bytes go
to `hw_uart_receive`, errors to `hw_uart_error`; all three are safe in an
interrupt handler.

## Pulse trains

Pins with `HW_GPIO_CAN_PULSE` can send and capture timed pulse trains
through a pulse port backed by hardware made for it: the ESP32's RMT, the
RP2040's PIO, a timer with DMA. Items are a level bit and nanoseconds; the
port rounds to its tick (`resolution_ns`). `send_bits` encodes bytes bit by
bit on the fly, as an RMT bytes encoder does, so 300 addressable LEDs take
900 bytes, not 28 KB of items. Like background bus transfers, the port may
return `HW_STATUS_PENDING` and report the end with `hw_pulse_complete`
(interrupt-safe); `finish` must end the work at once, and `receive` times
its own timeout.

```c
static const HwPulsePort rmt = { NULL, rmt_configure, rmt_send, rmt_send_bits, rmt_receive,
                                 rmt_finish, rmt_release, 25 /* ns per tick at 40 MHz */ };
hw_pulse_port_set(&rmt);
hw_gpio_add_pin("strip", 18, HW_GPIO_CAN_OUTPUT | HW_GPIO_CAN_PULSE, 0);
hw_gpio_add_pin("ir-in", 19, HW_GPIO_CAN_INPUT | HW_GPIO_CAN_PULSE, 0);
```

## 1-Wire

A 1-Wire port gets the same whole transactions, with ops for resets, bytes,
single time slots and the strong pull-up of parasite power: what the
ESP32's RMT, a UART wired for 1-Wire or a DS2482 bridge can do. For a
program's device the library puts the reset and match ROM in front. Searches
run in the library with the port's `triplet` (one search step: two reads and
the write chosen, as a DS2482 does it in hardware), or with single time
slots when the port has none. A port that cannot drive the line high
returns `HW_STATUS_UNSUPPORTED` for power ops; no presence pulse is
`HW_STATUS_NACK`.

```c
static const HwOneWirePort onewire = { NULL, board_onewire_transaction, board_onewire_triplet, NULL };
hw_onewire_port_set(&onewire);
hw_onewire_add_bus("gpio4", 4);  /* Arduino's OneWire(4) finds it by the pin's name */
```

## Analog outputs

Register each converter output with its resolution and full-scale voltage;
the port switches it on, writes raw values the library has checked and
switches it off. Register an output under its pin's GPIO name and the
library takes the pin while a program uses the output.

```c
static const HwDacPort dac = { NULL, board_dac_start, board_dac_write, board_dac_stop };
static const HwDacInfo esp32_dac = { 8, 3300 };
hw_dac_port_set(&dac);
hw_gpio_add_pin("gpio25", 25, HW_GPIO_CAN_ALL, 0);
hw_dac_add_channel("gpio25", 1, &esp32_dac);
```

## Counters

Pulse counters and quadrature encoders are hardware units the platform
registers by name and kind: the ESP32's PCNT, an STM32 timer in encoder
mode, an RP2040 PIO program. The port starts a unit in a mode with a glitch
filter, reads and sets the count (extending a 16-bit counter to 32 bits on
its overflow interrupt) and stops it. With `watch` the port arms the
hardware's compare or limit events and calls `hw_counter_event` from the
interrupt when the count leaves a program's window; without it the library
reads watched counters every `HW_COUNTER_POLL_MS`.

```c
static const HwCounterPort pcnt = { NULL, pcnt_start, pcnt_read, pcnt_write, pcnt_watch, pcnt_stop };
static const HwCounterInfo encoder = { HW_COUNTER_QUADRATURE, 40000000 };
hw_counter_port_set(&pcnt);
hw_counter_add("enc0", 0, &encoder);
```

## Drivers of your own

A driver of your own is a `HwDriver`: the calls of its interface
(`wasm-rtos:kernel/<name>@1.<minor>.0`), and what the library asks of it when
a program's handle closes, for `wait.any`, on every scheduling decision, when
the clock moves, when a task must be done with the device, and at shutdown.
Add it with `hw_driver_add`. For deep sleep it gives `save` and `restore`:
it writes, by name, what each task holds and how it is set up, and after the
sleep sets it up again and says what became of each with
`hw_host_system_device` (the operating system then maps the programs' handles); without
them, a program holding one of its devices cannot be saved.

## An operating system of your own

The devices need from the operating system only what `hw_host.h` declares:
the calling program, its memory, waiting, time, handles and saving the
whole system. The wasm-rtos kernel implements it; a scheduler of your own
that implements the same functions and calls `hw_drivers_poll` before each
scheduling decision (and the other `hw_drivers_*` when its clock moves, a
task ends and at shutdown) gets every device and port here as they are.
