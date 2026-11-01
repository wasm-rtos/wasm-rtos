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
- Queues, mutexes (with priority inheritance), semaphores, event groups and
  task notifications, for the host and for guest programs.
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

## Building

Compile the kernel sources together with wasm3 and WASI support:

```sh
cc -std=c11 -Dd_m3HasWASI -I. your_main.c os.c fs.c wasm3/source/*.c -lm
```

Clone with `--recurse-submodules`, or run `git submodule update --init`, to get
wasm3. Every subsystem can be left out to save space:

| Option | Default | Effect |
|---|---|---|
| `OS_HAS_PROCESSES` | 1 | `os_task_spawn*` and the `wasm-rtos:proc` imports |
| `OS_HAS_FS` | 1 | virtual file system (`fs.h`) and file-based spawning |
| `OS_FS_TAR` | `OS_HAS_FS` | read-only tar image driver |
| `OS_FS_MAX_MOUNTS` | 8 | mount points |
| `OS_FS_MAX_FILES` | 16 | open files per process |
| `OS_MAX_SPAWNED_TASKS` | 16 | processes alive at once |
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

## Guest programs

Any language that compiles to WebAssembly with WASI preview 1 works. Kernel
services are imported from two modules:

- `env`: `os_yield`, `os_delay_ms`, `os_task_delay_until`, `os_get_time_ms`,
  queues (`os_queue_*`), mutexes, semaphores, event groups, task
  notifications and timers, with the same names as the host API.
- `wasm-rtos:proc`: `spawn`, `spawn_path`, `wait`, `kill`, `self`, `parent`.

A program can state the interpreter stack it needs in a `wasm-rtos.app`
custom section (records of a key byte and a little-endian u32; key 1 is the
stack size). The creator's `stack_size` argument wins over it, and without
either the task gets `OS_DEFAULT_STACK_SIZE`. In C:

```c
__asm__(".section \".custom_section.wasm-rtos.app\",\"\",@\n"
        ".int8 1\n.int32 4096\n.text\n");
```

Overflowing the stack traps; `os_task_get_memory_usage` reports the most a
task has used, so the size can be set from a measurement.

### Small programs

How much linear memory a program gets is decided when it is linked. By
default `wasm-ld` uses 64 KiB pages and puts a 64 KiB C stack in that
memory, so even "hello world" takes 128 KiB. Link with 1-byte pages
(custom page sizes) and a small C stack instead:

```sh
clang --target=wasm32-wasip1 -O2 -Wl,--page-size=1 -Wl,-z,stack-size=4096 app.c -o app.wasm
```

Measured linear memory: printf hello world 128 KiB → 5.3 KiB, CoreMark
128 KiB → 10.4 KiB, with the same speed. A program that grows a large heap
with `malloc` does about 17% more work in its allocator while the heap grows
(wasi-libc asks for memory in small steps); `-Wl,--initial-heap=<bytes>`
sized for its heap removes that.

1-byte pages need a libc whose `malloc` asks the linker for the page size.
wasi-libc does that from the release whose `sbrk` uses
`__wasm_first_page_end` (Homebrew's wasi-libc 34 does; the Ubuntu 26.04
package, a 2025-07-26 snapshot, still assumes 64 KiB and its `malloc`
traps once it grows memory). Check a sysroot with
`llvm-objdump -r libc.a | grep __wasm_first_page_end`.

`os.h` documents every call and its status codes.

## Tests

Isolated tests, one directory per feature, live in
[wasm-rtos-tests](https://github.com/wasm-rtos/wasm-rtos-tests) and run on
every change, also under AddressSanitizer and UndefinedBehaviorSanitizer.

## License

The kernel is released into the public domain (see `LICENSE`). wasm3 is
MIT-licensed.
