# Porting wasm-rtos

How to give the kernel a clock and the devices of a board. The
[README](../README.md) has the overview and the build options; `os.h` and
`driver.h` document every call.

## Clock

Time is the kernel's only platform dependency, and the kernel contains no
platform code. Give it a millisecond clock through a clock port, or drive time
yourself with `os_tick()` / `os_update_time_ms()`:

```c
#include "os.h"

static uint32_t board_now_ms(void* context)
{
    (void)context;
    return board_millis(); /* your monotonic millisecond counter */
}

int run_program(uint8_t* wasm, uint32_t size)
{
    OsClockPort clock = { board_now_ms, NULL, NULL, NULL };
    OsTaskHandle task = NULL;

    os_init();
    os_clock_port_set(&clock);
    if (os_task_create(&task, wasm, size, NULL, "app", 0, OS_TASK_PRIORITY_NORMAL) != OS_STATUS_OK)
        return -1;

    while (os_task_get_state(task) != OS_TASK_DEAD)
    {
        os_clock_poll();
        os_schedule();
    }
    os_shutdown();
    return 0;
}
```

`arm_wakeup` and `cancel_wakeup` in the clock port are optional: supply both
to let the kernel ask for a wakeup at the next deadline instead of being
polled (tickless operation).

## GPIO

The kernel lends pins to programs by name and drives them through a GPIO
port: `configure`, `write` and `read` for one pin number of the chip.
Register the pins of the device profile once; a program then opens `"led"`
on every board that has one.

```c
static const OsGpioPort gpio = { NULL, board_configure, board_write, board_read };
os_gpio_port_set(&gpio);
os_gpio_add_pin("led", 2, OS_GPIO_CAN_OUTPUT, OS_GPIO_ACTIVE_LOW);
os_gpio_add_pin("button-a", 0, OS_GPIO_CAN_INPUT | OS_GPIO_CAN_PULL_UP | OS_GPIO_CAN_EDGES, 0);

void IRAM_ATTR button_isr(void) { os_gpio_edge(0, OS_GPIO_EDGE_FALLING); }
```

`configure` gets the physical mode and the edges to report; the interrupt
handler only calls `os_gpio_edge`, which counts and is safe there, and the
next `os_schedule` wakes the program waiting for that pin. A program owns a
pin until it closes it or ends, even by a trap; the kernel then configures
it as an input without pulls.

## Display

Register each display of the device profile with its size, shape, pixel
formats and density, and draw through a display port. The kernel checks
every call, so `write` only ever gets a whole rectangle inside the display
in a format it accepts; the pixels are the program's buffer (or the kernel's
copy of it, when the program's memory is paged) and are valid during the
call. `fill` and `flush` are optional: without `fill`, or
when it returns `OS_STATUS_UNSUPPORTED` for a display or format, the kernel
writes rows, and `flush` is for displays that buffer (e-paper, a frame
sent whole).

```c
static const OsDisplayPort display = { NULL, lcd_write, lcd_fill, NULL, lcd_backlight };
static const OsDisplayInfo round_lcd = {
    .width = 240, .height = 240, .shape = OS_DISPLAY_ROUND, .format = OS_DISPLAY_RGB565, .dpi = 260,
    .formats = OS_DISPLAY_FORMAT_BIT(OS_DISPLAY_RGB565), .has_brightness = 1, .brightness = 200,
};
os_display_port_set(&display);
os_display_add("main", 0, &round_lcd);
```

A port converts to the panel's own order, such as big-endian RGB565 over SPI.

## Input

Register the input devices of the device profile and report what happens;
the kernel queues events for the program that owns each device and makes
long presses and key repeat from the timings given, so they feel the same in
every program. Keys use the Linux input event codes.

```c
static const OsInputInfo bezel = { .kind = OS_INPUT_ROTARY, .steps_per_turn = 24 };
static const OsInputInfo buttons = { .kind = OS_INPUT_BUTTONS, .long_press_ms = 600 };
os_input_add("bezel", 0, &bezel);
os_input_add("buttons", 1, &buttons);

os_input_rotate(0, 7 /* REL_DIAL */, -1);      /* one detent counter-clockwise */
os_input_key(1, 158 /* KEY_BACK */, 1);        /* pressed */
```

Report from the context that calls `os_schedule`; an interrupt handler
should only note what happened (as with `os_gpio_edge`) and let the main
loop report it.

## Buses

Register the I2C and SPI buses of the device profile with their speed and
move bytes through a port. The kernel lends devices on them to programs,
checks every transaction and drives SPI chip selects through the GPIO port,
so the port gets whole transactions: a list of ops with the program's
buffers (or the kernel's copies of them, when the program's memory is
paged), valid until the transaction ends. A trace hook sees every finished
transaction, for logs and bus analyzers.

```c
static const OsI2cPort i2c = { NULL, board_i2c_transaction, NULL };
static const OsSpiPort spi = { NULL, board_spi_transaction, NULL };  /* synchronous: no finish */
os_i2c_port_set(&i2c);
os_spi_port_set(&spi);
os_i2c_add_bus("i2c0", 0, 400000);
os_spi_add_bus("spi0", 2, 40000000);
os_gpio_add_pin("lcd-cs", 5, OS_GPIO_CAN_OUTPUT, 0);
os_bus_trace_set(log_transaction, NULL);
```

An I2C port returns `OS_STATUS_NACK`, `OS_STATUS_BUS_ERROR` or
`OS_STATUS_TIMEOUT` as the bus reports them; programs get `-WR_ENACK`,
`-WR_EBUS` and `-WR_ETIMEOUT`.

A port with DMA starts the transfer and returns `OS_STATUS_PENDING`; only the
calling program waits, the others run. The interrupt at the end of the
transfer calls `os_bus_complete(kind, bus, status)`, which only records it;
the next `os_schedule` raises chip select, traces the transaction and wakes
the program. Until then the buffers belong to the port: the kernel does not
swap the program out, and before it pauses, cancels or ends the program it
calls the port's `finish`, which returns once the transfer is over and
reported.

```c
static OsStatus lcd_spi_transaction(void* context, uint32_t bus, uint32_t mode, uint32_t hz,
                                    const OsSpiOp* ops, uint32_t count)
{
    start_dma(bus, mode, hz, ops, count);    /* the ops and buffers stay valid until the end */
    return OS_STATUS_PENDING;
}

void IRAM_ATTR spi_dma_done_isr(void) { os_bus_complete(OS_BUS_SPI, 2, OS_STATUS_OK); }
static void lcd_spi_finish(void* context, uint32_t bus) { wait_for_dma_done(bus); }
```

## Analog inputs, PWM and serial ports

```c
static const OsAdcPort adc = { NULL, board_adc_read, board_adc_read_mv };   /* read_mv may be NULL */
static const OsAdcInfo pot = { .resolution_bits = 12, .full_scale_mv = 3300 };
os_adc_port_set(&adc);
os_adc_add_channel("a0", 6, &pot);

static const OsPwmPort pwm = { NULL, ledc_start, ledc_set_duty, ledc_set_frequency, ledc_stop };
os_pwm_port_set(&pwm);
os_gpio_add_pin("led", 2, OS_GPIO_CAN_OUTPUT | OS_GPIO_CAN_PWM, OS_GPIO_ACTIVE_LOW);

static const OsUartPort uart = { NULL, uart_configure, uart_write, uart_close };
os_uart_port_set(&uart);
os_uart_add("uart1", 1, 921600);
void IRAM_ATTR uart1_rx_isr(void) { os_uart_receive(1, fifo, count); }
void IRAM_ATTR uart1_tx_isr(void) { os_uart_writable(1); }
```

A PWM port gets the duty on the pin, 0 to 65535, after the kernel has
applied the board's and the program's inversion; it scales to its own
resolution and may round the frequency, reporting what it set. A UART
port's `write` takes what fits in its send buffer and returns how many
bytes; when room comes back it calls `os_uart_writable`. Received bytes go
to `os_uart_receive`, errors to `os_uart_error`; all three are safe in an
interrupt handler.

## Pulse trains

Pins with `OS_GPIO_CAN_PULSE` can send and capture timed pulse trains
through a pulse port backed by hardware made for it: the ESP32's RMT, the
RP2040's PIO, a timer with DMA. Items are a level bit and nanoseconds; the
port rounds to its tick (`resolution_ns`). `send_bits` encodes bytes bit by
bit on the fly, as an RMT bytes encoder does, so 300 addressable LEDs take
900 bytes, not 28 KB of items. Like background bus transfers, the port may
return `OS_STATUS_PENDING` and report the end with `os_pulse_complete`
(interrupt-safe); `finish` must end the work at once, and `receive` times
its own timeout.

```c
static const OsPulsePort rmt = { NULL, rmt_configure, rmt_send, rmt_send_bits, rmt_receive,
                                 rmt_finish, rmt_release, 25 /* ns per tick at 40 MHz */ };
os_pulse_port_set(&rmt);
os_gpio_add_pin("strip", 18, OS_GPIO_CAN_OUTPUT | OS_GPIO_CAN_PULSE, 0);
os_gpio_add_pin("ir-in", 19, OS_GPIO_CAN_INPUT | OS_GPIO_CAN_PULSE, 0);
```

## 1-Wire

A 1-Wire port gets the same whole transactions, with ops for resets, bytes,
single time slots and the strong pull-up of parasite power: what the
ESP32's RMT, a UART wired for 1-Wire or a DS2482 bridge can do. For a
program's device the kernel puts the reset and match ROM in front. Searches
run in the kernel with the port's `triplet` (one search step: two reads and
the write chosen, as a DS2482 does it in hardware), or with single time
slots when the port has none. A port that cannot drive the line high
returns `OS_STATUS_UNSUPPORTED` for power ops; no presence pulse is
`OS_STATUS_NACK`.

```c
static const OsOneWirePort onewire = { NULL, board_onewire_transaction, board_onewire_triplet, NULL };
os_onewire_port_set(&onewire);
os_onewire_add_bus("gpio4", 4);  /* Arduino's OneWire(4) finds it by the pin's name */
```

## Analog outputs

Register each converter output with its resolution and full-scale voltage;
the port switches it on, writes raw values the kernel has checked and
switches it off. Register an output under its pin's GPIO name and the
kernel takes the pin while a program uses the output.

```c
static const OsDacPort dac = { NULL, board_dac_start, board_dac_write, board_dac_stop };
static const OsDacInfo esp32_dac = { 8, 3300 };
os_dac_port_set(&dac);
os_gpio_add_pin("gpio25", 25, OS_GPIO_CAN_ALL, 0);
os_dac_add_channel("gpio25", 1, &esp32_dac);
```

## Counters

Pulse counters and quadrature encoders are hardware units the platform
registers by name and kind: the ESP32's PCNT, an STM32 timer in encoder
mode, an RP2040 PIO program. The port starts a unit in a mode with a glitch
filter, reads and sets the count (extending a 16-bit counter to 32 bits on
its overflow interrupt) and stops it. With `watch` the port arms the
hardware's compare or limit events and calls `os_counter_event` from the
interrupt when the count leaves a program's window; without it the kernel
reads watched counters every `OS_COUNTER_POLL_MS`.

```c
static const OsCounterPort pcnt = { NULL, pcnt_start, pcnt_read, pcnt_write, pcnt_watch, pcnt_stop };
static const OsCounterInfo encoder = { OS_COUNTER_QUADRATURE, 40000000 };
os_counter_port_set(&pcnt);
os_counter_add("enc0", 0, &encoder);
```

## Paging

Paging moves single frames of programs' memory out of RAM and back, so a
program may have more memory than the board has RAM. Give the kernel a page
store before creating tasks: slots of a frame's size (`wm_frame_size()`:
4096 bytes, or what the interpreter is built with as `WM_FRAME_SIZE`, from
256 for small external RAM to 64 KiB on a desktop) with `write` and `read`, backed by a file on an SD card, external SPI RAM or FRAM, or a
flash partition, and how many frames to keep in RAM. Those frames are one
region set aside when the store is set (`os_page_ram_bytes(store)` bytes:
taken from the heap, or the region in `store.ram`, external RAM for one),
so programs' frames never cut the heap into pieces. A program whose
memory fits in what is left of that budget keeps it in one block, reached at
full speed. The others are paged: a frame comes into RAM when it is touched,
and when RAM is full the kernel stores a frame not touched lately and brings
it back when it is needed. When RAM runs short the coldest memory goes
first: a frame of a program that is not running, then the whole block of a
program that waits (it goes on paged when it wakes), then the running
program's own frames. When RAM has room again, a paged memory goes back
into one block before its program's next slice, if no more than a quarter
of it is left in the store to read back; `block_memory_bytes` in
`os_task_get_memory_usage` tells whether it is. A program sees nothing of
this, only time.

A whole task can also leave RAM at once, through the same store:
`os_task_swap_out(task)` writes its state (stack, calls, globals, tables)
to a few slots, sends its memory's frames to the store as it would one by
one, and frees everything else, leaving the task control block. The task
keeps waiting, its timers and handles keep working, and a queue item its
wait delivers goes straight into its memory. The scheduler brings it back
before its next slice, from that state and with the same memory, whose
frames return as they are touched. When and what to swap out is the
platform's choice; a program playing music should stay, a menu waiting for
a button can go.

```c
os_task_swap_out(menu_task);   /* RAM back, until the menu has work to do */
```

Memory is promised only as far as it can be kept: creating a task or growing
its memory fails unless every program's frame could have a slot, so a
program never runs out of memory it was given. Only dirty frames are
written: a frame keeps its slot once stored, so one that came back and was
only read leaves RAM again for free, as one never written leaves as zeros.
A port's buffer stays in RAM while the port works on it.

```c
static const OsPageStore store = { sd_card, 256, 24, frame_write, frame_read };
os_page_store_set(&store);     /* 1 MiB on the card, 96 KiB of RAM for frames */
```

On an ESP32 with about 250 KiB of free heap, a program with 640 KiB of
memory runs this way from a file on an SD card.

## Program output

What programs write to stdout and stderr goes to the C library's `stdout`
and `stderr` unless the platform installs an output port. Its `write` gets
the id of the task that wrote, so the platform can keep a log per program,
send everything to a UART, or both. Output is a byte stream: a line may
arrive in several pieces.

```c
static void uart_output(void* context, uint32_t task_id, uint32_t fd, const void* data, uint32_t size);
static const OsOutputPort output = { NULL, uart_output };
os_output_port_set(&output);
```

`os_task_get_fuel_used` tells how much interpreter work a task has done. It
counts the same on every device, so it compares programs where time would
compare devices.

## Snapshots

`os_task_write_snapshot` saves a program that is between two slices and
neither waiting nor timed; `os_task_read_snapshot` continues it in a new task
created from the same module. A snapshot names positions by wasm bytecode
offsets and holds no host addresses, so it moves between devices running the
same kernel version, whatever their pointer size. Kernel state (waits,
handles, open files) stays behind.

## Deep sleep

A board that loses its RAM while it sleeps saves the whole system first and
restores it when it wakes (decision 6 in `docs/decisions.md`). When the
kernel has nothing to do for longer than sleeping and waking take
(`os_get_next_wakeup_ms`), the port writes the system to storage that keeps
it, flash or an SD card, and sleeps:

```c
if (os_schedule() == OS_STATUS_NO_READY_TASKS && os_get_next_wakeup_ms() > 5000U)
{
    uint32_t sleep_ms = os_get_next_wakeup_ms();   /* OS_WAIT_FOREVER: until a pin wakes it */
    if (os_system_save(flash_write, &cursor) == OS_STATUS_OK)
        board_deep_sleep(sleep_ms);                 /* does not return: the board starts again */
}
```

At start, the port sets everything up as at a first start (`os_init`, the
clock, the page store, file systems, drivers, host imports), but creates no
task or object; it restores instead:

```c
static OsStatus find_module(void* context, const char* task_name, const uint8_t** bytes, uint32_t* size)
{
    *bytes = app_wasm;          /* the module the host gave that task at first */
    *size = app_wasm_size;
    return OS_STATUS_OK;
}

if (woke_from_sleep && os_system_restore(flash_read, &cursor, find_module, NULL, slept_ms) == OS_STATUS_OK)
{
    /* Tasks and objects have the ids they had: os_task_find_by_id, os_queue_find_by_id... */
}
else
{
    start_tasks_afresh();
}
```

`slept_ms` is how long the board slept, from a timer that runs through the
sleep (an ESP32's RTC timer). Waits and timers go on from there: what fell
due during the sleep is delivered at once.

- **Saving** happens between slices. It returns `OS_STATUS_BUSY` while a
  transfer or page is on its way (save after the next slice) and
  `OS_STATUS_UNSUPPORTED` while a program holds a device or runs a dylink.0
  program; the last error names the task.
- **Modules** are not in the stream. A process started by path is opened
  from the same path, one the kernel copied (`os_task_spawn`) is in the
  stream, and for a task the host created the kernel calls `find_module` by
  the task's name.
- **A stream that does not fit** (cut, another format, another kernel
  build) restores nothing, and the kernel is as before: start afresh.
- **A program whose world changed** in the sleep (its module replaced or
  gone, a file it had open gone or changed) ends as a program that traps,
  its error `restore: its module changed` and the like
  (`os_task_get_error`); its mutexes go to the next waiter, its parent sees
  it end, and every other program goes on.

## Drivers of your own

A driver of your own is an `OsDriver`: the calls of its interface
(`wasm-rtos:kernel/<name>@1.<minor>.0`), and what the kernel asks of it when
a program's handle closes, for `wait.any`, on every scheduling decision, when
the clock moves, when a task must be done with the device, and at shutdown.
Add it with `os_driver_add`.
