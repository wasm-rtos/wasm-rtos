# Porting wasm-rtos

How to give the kernel a clock, storage for paging and snapshots, and a
place for programs' output. The [README](../README.md) has the overview and
the build options; `os.h` documents every call. The devices of a board
(pins, buses, displays and the rest) are registered through the device
library: `hw/docs/porting.md`.

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
restores it when it wakes (decisions 6 and 7 in `docs/decisions.md`). When
the kernel has nothing to do for longer than sleeping and waking take
(`os_get_next_wakeup_ms`), the port writes the system to storage that keeps
it, flash or an SD card, arms the pins programs wait on as wake sources, and
sleeps:

```c
if (os_schedule() == OS_STATUS_NO_READY_TASKS && os_get_next_wakeup_ms() > 5000U)
{
    uint32_t sleep_ms = os_get_next_wakeup_ms();   /* OS_WAIT_FOREVER: until a pin wakes it */
    HwGpioWake wake[8];
    uint32_t pins = hw_gpio_wake_pins(wake, 8U);   /* pins (your numbers) and the edges awaited */
    if (os_system_save(flash_write, &cursor) == OS_STATUS_OK)
    {
        board_arm_wake_pins(wake, pins < 8U ? pins : 8U);
        board_deep_sleep(sleep_ms);                 /* does not return: the board starts again */
    }
}
```

A chip that wakes only by level, or only from some pins, arms what it can.
One that wakes by level must not sleep while a wake pin is at that level
(a button still held): it would wake at once, for a press already seen.

At start, the port sets everything up as at a first start (`os_init`, the
clock, the page store, file systems, its ports and devices under the same
names, host imports), but creates no task or object; it restores instead:

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
    if (woken_by_pin)
        hw_gpio_edge(wake_pin, HW_GPIO_EDGE_FALLING);  /* the press that woke it reaches the program */
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
  `OS_STATUS_UNSUPPORTED` while a program runs a dylink.0 program or holds
  a display that loses its picture in the sleep; the last error says which.
- **Devices** are saved by their drivers and found again by name, so the
  firmware may register them in another order. Each is set up again as the
  program left it: a pin's mode and level, a PWM output's frequency and
  duty, an analog output's value, a counter's count and window, a serial
  port's speed and format and the bytes not yet read, input events not yet
  read, a display's brightness. What happened during the sleep is lost:
  edges and key presses (except the one that woke the board), bytes that
  arrived, counts, a 1-Wire strong pull-up (and the conversion it powered).
- **A display** comes back as the panel kept it. Set
  `HwDisplayInfo.keeps_image` when the panel keeps its picture (it stays
  powered, or holds it without power); without it, saving waits until no
  program holds the display.
- **Modules** are not in the stream. A process started by path is opened
  from the same path, one the kernel copied (`os_task_spawn`) is in the
  stream, and for a task the host created the kernel calls `find_module` by
  the task's name.
- **A stream that does not fit** (cut, another format, another kernel
  build) restores nothing, and the kernel is as before: start afresh.
- **A program whose world changed** in the sleep (its module replaced or
  gone, a file it had open gone or changed, a device it held no longer
  registered under its name or refusing to be set up) ends as a program
  that traps,
  its error `restore: its module changed` and the like
  (`os_task_get_error`); its mutexes go to the next waiter, its parent sees
  it end, and every other program goes on.
