# wasm-rtos

**A real-time kernel that runs WebAssembly programs the way an operating
system runs executables**, on microcontrollers with no MMU. Programs are
ordinary `.wasm` files built from C, C++ or Arduino sketches; the same file
runs on an ESP32, an STM32 and in the browser simulator at
<https://wasm-rtos.github.io>.

```c
os_init();
os_clock_port_set(&clock);                              /* the board's millisecond clock */
os_task_create(&blink, blink_wasm, blink_size, NULL, "blink", 0, OS_TASK_PRIORITY_NORMAL);
os_task_spawn_path(&shell, "/sd/apps/shell.wasm", argv, 2, OS_TASK_PRIORITY_NORMAL, 0);
for (;;)
{
    os_clock_poll();
    os_schedule();                                      /* runs the programs, a slice at a time */
}
```

## What makes it different

- **Preemption without a timer interrupt.** Programs are interpreted by
  [wasm-rtos-vm](https://github.com/wasm-rtos/wasm-rtos-vm), which counts
  every WebAssembly instruction (fuel). The scheduler gives each program a
  slice of instructions, so a plain `int main` with an endless loop still
  shares the processor, and a program cannot block the others. Programs
  are not rewritten or instrumented first: the interpreter counts by
  itself.
- **The same work costs the same everywhere.** A program uses the same fuel
  on every device, so `os_task_get_fuel_used` compares programs where time
  would compare boards.
- **Isolation without memory protection hardware.** Each program has its
  own memory and reaches the kernel only through checked calls; a program
  that crashes ends alone, and the pins and devices it held come back.
- **Memory larger than RAM.** With a page store (an SD card, SPI RAM, FRAM,
  flash) a program's memory stays in one block while RAM has room and is
  paged in 4 KiB frames when it has not, and a waiting program can leave RAM
  altogether. A 640 KiB program runs on an ESP32 with 250 KiB
  free.
- **Programs stop at any instruction and move.** A program between two
  slices can be saved and continued in a new task, also on another device:
  its position is in the module's terms, not the host's.
- **Processes as in an OS.** Programs start other programs from memory or a
  file path with arguments, wait for their exit codes, and can share
  `dylink.0` libraries.
- **No platform code in the kernel.** A board gives it a clock and the
  devices it has, through small ports; a build carries only the drivers its
  board uses.

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
  wait for them and stop them. Processes of the same program share it: its
  bytes and parsed module are loaded once, found by their contents.
- Virtual file system behind the WASI file calls, with a read-only tar
  driver. Files from memory-mapped storage run without a copy.
- Optional `dylink.0` shared libraries: every process gets its own copy of a
  library's data, as with ELF shared objects.
- GPIO by pin name from the device profile, one owner per pin, edges from
  interrupt handlers delivered to waiting programs.
- Devices for programs besides pins: displays, input devices, I2C, SPI and
  1-Wire buses, analog inputs and outputs, PWM, serial ports, pulse trains
  and counters, each lent to one program at a time.
- Snapshots: a program between two slices can be saved and continued in a
  new task, also on another device.
- Deep sleep without losing work: the whole system (every program with what
  it waits for, the queues, mutexes, semaphores, events and timers it
  shares, its children and open files) is saved before the device loses its
  RAM and restored after, with the pins, buses, serial ports and other
  devices programs hold set up again; programs go on where they were, the
  clock with the time slept, and need nothing written for it. A pin a
  program waits on wakes the board.
- Paging: with a page store, programs' memory is a cache in RAM of frames
  (4 KiB by default), so programs may have more memory than the board has RAM, and a
  waiting program can leave RAM altogether and come back when it runs.

## Building

Compile the kernel sources together with the interpreter:

```sh
git clone --recurse-submodules https://github.com/wasm-rtos/wasm-rtos
cc -std=c11 -I. your_main.c *.c vm/source/*.c -lm
```

In a clone made without `--recurse-submodules`, run `git submodule update
--init` to get the interpreter.

Then give the kernel a clock and the board's devices:
[docs/porting.md](docs/porting.md).

### Kernel options

| Option | Default | Effect |
|---|---|---|
| `OS_HAS_PROCESSES` | 1 | `os_task_spawn*` and the guest `proc` interface (without it, `proc` calls return `-WR_EUNSUPPORTED`) |
| `OS_HAS_FS` | 1 | virtual file system (`fs.h`) and file-based spawning |
| `OS_FS_TAR` | `OS_HAS_FS` | read-only tar image driver |
| `OS_FS_MAX_MOUNTS` | 8 | mount points |
| `OS_FS_MAX_FILES` | 16 | open files per process |
| `OS_HAS_WASI` | 1 | the WASI preview 1 calls (arguments, exit, clocks, standard streams, files) |
| `OS_HAS_DYLINK` | 0 | `dylink.0` libraries (`os_wasm_library_register`) |
| `OS_HAS_MEMORY_ACCOUNTING` | 0 | full per-task interpreter memory in `os_task_get_memory_usage` |
| `OS_MAX_SPAWNED_TASKS` | 16 | processes alive at once |
| `OS_ABI_MAX_HANDLES` | 32 | handles one program may hold |
| `OS_MAX_DRIVERS` | 16 | drivers at once |
| `OS_TASK_SLICE_FUEL` | 10000 | interpreter work per scheduler slice |
| `OS_DEFAULT_STACK_SIZE` | 8 KiB | interpreter stack when neither creator nor program picks one |

The file system needs 32-bit pointers; build 8-bit targets with
`OS_HAS_FS=0`.

### Drivers

Pins, buses, displays and the rest are drivers (`driver.h`). The kernel
knows none of them by name: a driver joins when the platform registers its
first device, so a build carries exactly the drivers its platform uses.
A program that imports an interface no driver offers still loads, and those
calls return `-WR_EUNSUPPORTED`.

| Interface | Joins with | Limits (default) |
|---|---|---|
| `gpio` | `os_gpio_add_pin` | `OS_GPIO_MAX_PINS` / `OS_GPIO_MAX_NAMES` (32 / 64): pins and pin names |
| `display` | `os_display_add` | `OS_DISPLAY_MAX` (2) |
| `input` | `os_input_add` | `OS_INPUT_MAX` / `OS_INPUT_QUEUE` / `OS_INPUT_MAX_HELD` (8 / 32 / 4): devices, events queued per device (16 bytes each), keys tracked for long press and repeat |
| `i2c` | `os_i2c_add_bus` | `OS_I2C_MAX_BUSES` / `OS_I2C_MAX_DEVICES` (2 / 16): buses, and addresses programs hold open at once |
| `spi` | `os_spi_add_bus` | `OS_SPI_MAX_BUSES` / `OS_SPI_MAX_DEVICES` (2 / 8) |
| `onewire` | `os_onewire_add_bus` | `OS_ONEWIRE_MAX_BUSES` / `OS_ONEWIRE_MAX_DEVICES` (2 / 8) |
| | | `OS_BUS_MAX_OPS` (16): ops in one bus transaction |
| `adc` | `os_adc_add_channel` | `OS_ADC_MAX` (8) |
| `dac` | `os_dac_add_channel` | `OS_DAC_MAX` (4) |
| `pwm` | `os_pwm_port_set` | `OS_PWM_MAX` (8): outputs at once, on GPIO pins with `OS_GPIO_CAN_PWM` |
| `pulse` | `os_pulse_port_set` | `OS_PULSE_MAX` (4): channels at once, on GPIO pins with `OS_GPIO_CAN_PULSE` |
| `uart` | `os_uart_add` | `OS_UART_MAX` / `OS_UART_RX_BUFFER` (4 / 256): ports, and received bytes kept per port |
| `counter` | `os_counter_add` | `OS_COUNTER_MAX` / `OS_COUNTER_POLL_MS` (4 / 10): counters, and how often the kernel reads watched counters whose hardware cannot watch |

The `*_MAX` limits are slots: each costs a pointer. A pin, display, input
device, bus, serial port or analog channel takes memory when the platform
registers it, an input device's event queue while a program owns it, and a
serial port's receive buffer from the first time a program opens it (it
stays, as interrupt handlers write into it). A board with a few devices
spends a few hundred bytes on them.

A driver of your own is an `OsDriver` added with `os_driver_add`: see
[docs/porting.md](docs/porting.md#drivers-of-your-own).

## API

`os.h` documents every call and its status codes. The core of it:

### Start and run

```c
OsStatus os_init(void);
void os_shutdown(void);
OsStatus os_output_port_set(const OsOutputPort* port);
OsStatus os_clock_port_set(const OsClockPort* port);
void os_clock_port_clear(void);
OsStatus os_clock_poll(void);
OsStatus os_clock_wakeup(void);
uint8_t os_clock_is_tickless(void);
void os_tick(uint32_t milliseconds);
OsStatus os_update_time_ms(uint32_t now_ms);
uint32_t os_get_tick_ms(void);
uint32_t os_get_next_wakeup_ms(void);
OsStatus os_schedule(void);
uint32_t os_get_task_count(void);
uint32_t os_get_ready_task_count(void);
uint32_t os_get_waiting_task_count(void);
uint32_t os_get_timer_count(void);
```

### Tasks and processes

```c
OsStatus os_task_create(OsTaskHandle* out_task, uint8_t* wasm_bytes, uint32_t wasm_size, const char* entry_function_name, const char* task_name, uint32_t stack_size, uint32_t priority);
OsStatus os_task_create_with_args(OsTaskHandle* out_task, uint8_t* wasm_bytes, uint32_t wasm_size, const char* entry_function_name, OsValue* entry_args, uint32_t entry_arg_count, const char* task_name, uint32_t stack_size, uint32_t priority);
OsStatus os_task_delete(OsTaskHandle task);
OsStatus os_task_spawn(OsTaskHandle* out_task, const uint8_t* wasm_bytes, uint32_t wasm_size, const char* const* argv, uint32_t argc, uint32_t priority, uint32_t stack_size);
uint32_t os_task_get_parent_id(OsTaskHandle task);
OsStatus os_task_spawn_path(OsTaskHandle* out_task, const char* path, const char* const* argv, uint32_t argc, uint32_t priority, uint32_t stack_size);
OsStatus os_task_delay_ms(uint32_t delay_ms);
OsStatus os_task_delay_until(uint32_t* previous_wake_time_ms, uint32_t period_ms);
OsStatus os_task_abort_wait(OsTaskHandle task);
OsStatus os_task_yield(void);
void os_request_preempt(void);
OsStatus os_task_suspend(OsTaskHandle task);
OsStatus os_task_resume(OsTaskHandle task);
OsStatus os_task_set_priority(OsTaskHandle task, uint32_t priority);
uint32_t os_task_get_priority(OsTaskHandle task);
uint32_t os_task_get_base_priority(OsTaskHandle task);
uint32_t os_task_get_id(OsTaskHandle task);
OsTaskHandle os_task_find_by_id(uint32_t task_id);
uint32_t os_task_get_id_list(uint32_t* out_task_ids, uint32_t max_task_ids);
const char* os_task_get_app_name(OsTaskHandle task);
OsTaskState os_task_get_state(OsTaskHandle task);
OsTaskExitReason os_task_get_exit_reason(OsTaskHandle task);
uint32_t os_task_get_exit_code(OsTaskHandle task);
OsStatus os_task_get_return_value(OsTaskHandle task, OsValue* out_value);
uint32_t os_task_get_return_value_count(OsTaskHandle task);
OsStatus os_task_get_return_values(OsTaskHandle task, OsValue* out_values, uint32_t max_values, uint32_t* out_value_count);
OsStatus os_task_read_memory(OsTaskHandle task, uint32_t wasm_address, uint8_t* out_buffer, uint32_t byte_count);
OsStatus os_task_write_memory(OsTaskHandle task, uint32_t wasm_address, const uint8_t* buffer, uint32_t byte_count);
const char* os_task_get_name(OsTaskHandle task);
OsTaskHandle os_task_get_current(void);
OsStatus os_task_get_last_wait_status(OsTaskHandle task);
uint32_t os_task_get_last_wait_value(OsTaskHandle task);
uint8_t os_task_is_notification_pending(OsTaskHandle task);
```

### Queues, mutexes, semaphores, event groups

```c
OsStatus os_queue_create(OsQueueHandle* out_queue, uint32_t item_size, uint32_t item_count);
void os_queue_delete(OsQueueHandle queue);
uint32_t os_queue_get_id(OsQueueHandle queue);
OsQueueHandle os_queue_find_by_id(uint32_t queue_id);
OsStatus os_queue_send(OsQueueHandle queue, const void* item);
OsStatus os_queue_receive(OsQueueHandle queue, void* out_item);
OsStatus os_queue_send_wait(OsQueueHandle queue, const void* item, uint32_t timeout_ms);
OsStatus os_queue_receive_wait(OsQueueHandle queue, void* out_item, uint32_t timeout_ms);
uint32_t os_queue_get_count(OsQueueHandle queue);
uint32_t os_queue_get_space(OsQueueHandle queue);
uint32_t os_queue_get_item_size(OsQueueHandle queue);
OsStatus os_queue_set_name(OsQueueHandle queue, const char* name);
OsStatus os_mutex_set_name(OsMutexHandle mutex, const char* name);
OsStatus os_semaphore_set_name(OsSemaphoreHandle semaphore, const char* name);
OsStatus os_event_group_set_name(OsEventGroupHandle event_group, const char* name);
OsStatus os_mutex_create(OsMutexHandle* out_mutex);
void os_mutex_delete(OsMutexHandle mutex);
uint32_t os_mutex_get_id(OsMutexHandle mutex);
OsMutexHandle os_mutex_find_by_id(uint32_t mutex_id);
OsStatus os_mutex_lock(OsMutexHandle mutex, uint32_t timeout_ms);
OsStatus os_mutex_unlock(OsMutexHandle mutex);
OsTaskHandle os_mutex_get_owner(OsMutexHandle mutex);
OsStatus os_semaphore_create(OsSemaphoreHandle* out_semaphore, uint32_t max_count, uint32_t initial_count);
void os_semaphore_delete(OsSemaphoreHandle semaphore);
uint32_t os_semaphore_get_id(OsSemaphoreHandle semaphore);
OsSemaphoreHandle os_semaphore_find_by_id(uint32_t semaphore_id);
OsStatus os_semaphore_take(OsSemaphoreHandle semaphore, uint32_t timeout_ms);
OsStatus os_semaphore_give(OsSemaphoreHandle semaphore);
uint32_t os_semaphore_get_count(OsSemaphoreHandle semaphore);
uint32_t os_semaphore_get_max_count(OsSemaphoreHandle semaphore);
OsStatus os_event_group_create(OsEventGroupHandle* out_event_group);
void os_event_group_delete(OsEventGroupHandle event_group);
uint32_t os_event_group_get_id(OsEventGroupHandle event_group);
OsEventGroupHandle os_event_group_find_by_id(uint32_t event_group_id);
uint32_t os_event_group_get_bits(OsEventGroupHandle event_group);
uint32_t os_event_group_set_bits(OsEventGroupHandle event_group, uint32_t bits);
uint32_t os_event_group_clear_bits(OsEventGroupHandle event_group, uint32_t bits);
OsStatus os_event_group_wait_bits(OsEventGroupHandle event_group, uint32_t bits_to_wait_for, uint8_t clear_on_exit, uint8_t wait_for_all, uint32_t timeout_ms);
```

### Notifications and timers

```c
OsStatus os_task_notify(OsTaskHandle task, uint32_t value, OsNotifyAction action);
OsStatus os_task_notify_wait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t timeout_ms);
OsStatus os_task_notify_take(uint8_t clear_count_on_exit, uint32_t timeout_ms);
uint32_t os_task_get_notification_value(OsTaskHandle task);
OsStatus os_timer_create(OsTimerHandle* out_timer, uint32_t period_ms, uint8_t auto_reload, OsTaskHandle target_task, uint32_t notify_value, OsNotifyAction notify_action);
void os_timer_delete(OsTimerHandle timer);
uint32_t os_timer_get_id(OsTimerHandle timer);
OsTimerHandle os_timer_find_by_id(uint32_t timer_id);
OsStatus os_timer_start(OsTimerHandle timer);
OsStatus os_timer_stop(OsTimerHandle timer);
OsStatus os_timer_reset(OsTimerHandle timer);
OsStatus os_timer_change_period(OsTimerHandle timer, uint32_t period_ms);
uint8_t os_timer_is_active(OsTimerHandle timer);
uint32_t os_timer_get_period_ms(OsTimerHandle timer);
```

### Snapshots, swapping and paging

```c
OsStatus os_task_write_snapshot(OsTaskHandle task, OsSnapshotWrite write, void* context);
OsStatus os_task_read_snapshot(OsTaskHandle task, OsSnapshotRead read, void* context);
OsStatus os_task_get_snapshot_size(OsTaskHandle task, uint32_t* out_size);
OsStatus os_task_save_snapshot(OsTaskHandle task, uint8_t* buffer, uint32_t buffer_size, uint32_t* out_size);
OsStatus os_task_load_snapshot(OsTaskHandle task, const uint8_t* buffer, uint32_t buffer_size);
void os_page_read_done(uint32_t slot, OsStatus status);
void os_page_write_done(uint32_t slot, OsStatus status);
OsStatus os_page_store_set(const OsPageStore* store);
OsStatus os_page_get_stats(OsPageStats* out_stats);
OsStatus os_task_swap_out(OsTaskHandle task);
OsStatus os_task_swap_in(OsTaskHandle task);
uint8_t os_task_is_swapped(OsTaskHandle task);
OsStatus os_system_save(OsSnapshotWrite write, void* context);
OsStatus os_system_restore(OsSnapshotRead read, void* context, OsModuleFind find, void* find_context,
                           uint32_t slept_ms);
uint32_t os_gpio_wake_pins(OsGpioWake* out, uint32_t max);
```

### Measuring and errors

```c
OsStatus os_task_get_memory_usage(OsTaskHandle task, OsTaskMemoryUsage* out_usage);
uint32_t os_task_get_run_count(OsTaskHandle task);
uint64_t os_task_get_fuel_used(OsTaskHandle task);
const char* os_get_last_error_phase(void);
const char* os_get_last_error_result(void);
const char* os_get_last_error_task_name(void);
const char* os_task_get_error(OsTaskHandle task);
OsStatus os_get_last_error_status(void);
```

### Host functions and libraries

```c
OsStatus os_host_import_register(const char* module_name, const char* import_name, const char* signature, OsHostImportFunction function);
void os_host_import_clear_all(void);
OsStatus os_host_wait_begin(uint32_t* result_slot);
OsStatus os_wasm_library_register(OsWasmLibraryHandle* out_library, const char* module_name, uint8_t* wasm_bytes, uint32_t wasm_size);
OsWasmLibraryHandle os_wasm_library_find(const char* module_name);
uint32_t os_wasm_library_get_id(OsWasmLibraryHandle library);
const char* os_wasm_library_get_name(OsWasmLibraryHandle library);
uint32_t os_wasm_library_get_count(void);
```

Devices are registered by the board through ports (`os_gpio_*`,
`os_display_*`, `os_input_*`, `os_i2c_*`, `os_spi_*`, `os_onewire_*`,
`os_adc_*`, `os_dac_*`, `os_pwm_*`, `os_uart_*`, `os_pulse_*`,
`os_counter_*`): see [docs/porting.md](docs/porting.md). The file system is
in `fs.h`, drivers of your own in `driver.h`.

## Guest programs

Any language that compiles to WebAssembly with WASI preview 1 works. Beyond
WASI, programs import the wasm-rtos ABI: kernel objects behind per-program
handles (mutexes, semaphores, event groups, queues, named to share them
between programs), counting timers, waiting for several objects at once,
child processes, GPIO pins, displays, input devices, I2C, SPI and 1-Wire buses, analog inputs and outputs, PWM outputs, serial ports, pulse trains and counters. [`docs/abi.md`](docs/abi.md) is the specification,
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

ABI 1.x is frozen at 1.8: a program built for any 1.x runs on every 1.x
kernel, older or newer. A call the kernel does not have yet returns
`-WR_EUNSUPPORTED` and a WASI call it does not answer returns `ENOSYS`, so
programs load everywhere and check. A child never runs above the priority
of the program that started it. CI holds the kernel to the frozen list
[`docs/abi-1.8.txt`](docs/abi-1.8.txt) (`tools/check-abi.py`).

`os.h` documents every call and its status codes.

## Tests

Isolated tests, one directory per feature, live in
[wasm-rtos-tests](https://github.com/wasm-rtos/wasm-rtos-tests) and run on
every change, also under AddressSanitizer and UndefinedBehaviorSanitizer.

## Arduino

[wasm-rtos-libraries](https://github.com/wasm-rtos/wasm-rtos-libraries) (`arduino/`) builds Arduino sketches and libraries into
programs: `Arduino.h`, `Serial`, `Wire` and `SPI` on top of the guest ABI.
It is a library inside the program, not part of the kernel.

## License

The kernel and the interpreter are released into the public domain (see
`LICENSE`).
