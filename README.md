# wasm-rtos

A small real-time kernel that runs WebAssembly programs as tasks and
processes, the way an operating system runs executables. The same `.wasm`
file runs on a microcontroller and in the browser simulator at
<https://wasm-rtos.github.io>.

Programs are interpreted by a fork of [wasm3](https://github.com/wasm-rtos/wasm3)
(the `wasm3` submodule). The scheduler preempts programs by counting the
interpreter work they do ("fuel"), so a plain `int main` with an endless loop
still shares the processor with other programs.

## Features

- Priority scheduler with fuel preemption, delays, suspend/resume and
  software timers.
- Queues, mutexes (with priority inheritance), semaphores and event groups,
  for the host and for guest programs; task notifications for the host.
- Loader: detects the entry point (`_start`, `app_main`, `main`) and checks
  every function import when the program is loaded, not when it is first
  called.
- WASI preview 1 for programs, including arguments.
- Processes: programs start other programs from memory or from a file path,
  wait for them and stop them.
- Virtual file system behind the WASI file calls, with a read-only tar
  driver. Files from memory-mapped storage run without a copy.
- Optional `dylink.0` shared libraries: every process gets its own copy of a
  library's data, as with ELF shared objects.
- GPIO by pin name from the device profile, one owner per pin, edges from
  interrupt handlers delivered to waiting programs.
- Snapshots and swapping: a program between two slices can be saved and
  continued in a new task, also on another device, or moved out of RAM while
  it waits and brought back when it runs.

## Building

Compile the kernel sources together with wasm3 and WASI support:

```sh
cc -std=c11 -Dd_m3HasWASI -I. your_main.c os.c abi.c fs.c wasm3/source/*.c -lm
```

Clone with `--recurse-submodules`, or run `git submodule update --init`, to get
wasm3. Every subsystem can be left out to save space:

| Option | Default | Effect |
|---|---|---|
| `OS_HAS_PROCESSES` | 1 | `os_task_spawn*` and the guest `proc` interface (without it, `proc` calls return `-WR_EUNSUPPORTED`) |
| `OS_HAS_FS` | 1 | virtual file system (`fs.h`) and file-based spawning |
| `OS_FS_TAR` | `OS_HAS_FS` | read-only tar image driver |
| `OS_FS_MAX_MOUNTS` | 8 | mount points |
| `OS_FS_MAX_FILES` | 16 | open files per process |
| `OS_MAX_SPAWNED_TASKS` | 16 | processes alive at once |
| `OS_ABI_MAX_HANDLES` | 32 | handles one program may hold |
| `OS_HAS_GPIO` | 1 | pins for programs through a GPIO port (without it, `gpio` calls return `-WR_EUNSUPPORTED`) |
| `OS_GPIO_MAX_PINS` / `OS_GPIO_MAX_NAMES` | 32 / 64 | pins and pin names a platform can register |
| `OS_TASK_SLICE_FUEL` | 10000 | interpreter work per scheduler slice |
| `OS_DEFAULT_STACK_SIZE` | 8 KiB | interpreter stack when neither creator nor program picks one |
| `OS_NATIVE_STACK_BUDGET` | 16 KiB | C stack nested Wasm calls may use in one slice before it ends early (0 = unlimited) |
| `d_m3HasDylink` | 0 | `dylink.0` libraries (`os_wasm_library_register`) |
| `d_m3HasHeapAccounting` | 0 | full per-task interpreter memory in `os_task_get_memory_usage` |

The file system needs 32-bit pointers; build 8-bit targets with
`OS_HAS_FS=0`.

## Porting

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

### GPIO

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

### Swapping

To move idle programs out of RAM, give the kernel somewhere to put them: a
swap port with `write`, `read` and `discard` for one image per task, backed
by a file on an SD card, a flash partition, PSRAM or IndexedDB. Then
`os_task_swap_out(task)` frees the task's linear memory, interpreter stack and
compiled code, leaving the task control block. The task keeps waiting, its
timers and handles keep working, and what its wait delivers meanwhile (a
queue item, a result) is applied when it returns. The scheduler brings it
back before its next slice. When and what to swap out is the platform's
choice; a program playing music should stay, a menu waiting for a button can
go.

```c
static const OsSwapPort swap = { sd_card, swap_write, swap_read, swap_discard };
os_swap_port_set(&swap);
os_task_swap_out(menu_task);   /* RAM back, until the menu has work to do */
```

### Snapshots

`os_task_write_snapshot` saves a program that is between two slices and
neither waiting nor timed; `os_task_read_snapshot` continues it in a new task
created from the same module. A snapshot names positions by wasm bytecode
offsets and holds no host addresses, so it moves between devices running the
same kernel version, whatever their pointer size. Kernel state (waits,
handles, open files) stays behind.

## Guest programs

Any language that compiles to WebAssembly with WASI preview 1 works. Beyond
WASI, programs import the wasm-rtos ABI: kernel objects behind per-program
handles (mutexes, semaphores, event groups, queues, named to share them
between programs), counting timers, waiting for several objects at once,
child processes and GPIO pins. [`docs/abi.md`](docs/abi.md) is the specification,
[`wit/kernel.wit`](wit/kernel.wit) the interface description, and
[`include/wasm-rtos.h`](include/wasm-rtos.h) declares everything for C and C++:

```c
#include "wasm-rtos.h"
#include <unistd.h>

WR_APP_NAME("blink");
WR_APP_STACK_SIZE(4096);

int main(void)
{
    int32_t ticks = wr_timer_create(500, WR_TIMER_PERIODIC);
    wr_timer_start(ticks);
    for (;;)
    {
        wr_wait_any(&ticks, 1, WR_WAIT_FOREVER);
        wr_timer_read(ticks);
        /* toggle something */
    }
}
```

Time, sleeping and yielding are plain WASI (`clock_gettime`, `usleep`,
`sched_yield`); the kernel answers them so that only the calling program
waits. A program states its interpreter stack and name in its
`wasm-rtos.app` section; the creator's `stack_size` argument wins over it,
and without either the task gets `OS_DEFAULT_STACK_SIZE`. Overflowing the
stack traps; `os_task_get_memory_usage` reports the most a task has used, so
the size can be set from a measurement.

`os.h` documents every call and its status codes.

## Tests

Isolated tests, one directory per feature, live in
[wasm-rtos-tests](https://github.com/wasm-rtos/wasm-rtos-tests) and run on
every change, also under AddressSanitizer and UndefinedBehaviorSanitizer.

## License

The kernel is released into the public domain (see `LICENSE`). wasm3 is
MIT-licensed.
