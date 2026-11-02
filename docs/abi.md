# wasm-rtos ABI 1.3

What a WebAssembly program can rely on when it runs on wasm-rtos. Programs
built against ABI 1.x keep working on every kernel that supports 1.x; new
calls arrive in minor versions, and only a new major version may change or
remove anything here.

| Version | Adds |
|---|---|
| 1.0 | `handle`, `sync`, `timer`, `wait`, `proc`, WASI, metadata |
| 1.1 | `gpio`, error `WR_EINUSE` |
| 1.2 | `display`, `input` |
| 1.3 | `i2c`, `spi`, errors `WR_ENACK`, `WR_EBUS` |

[`wit/kernel.wit`](../wit/kernel.wit) describes the interfaces;
[`include/wasm-rtos.h`](../include/wasm-rtos.h) declares them for C and C++.

## What a program is

- A core WebAssembly module for wasm32 with one memory. The kernel reads and
  writes memory 0.
- A WASI preview 1 command (`_start`), or a module exporting `app_main` or
  `main`.
- One program is one process with one task. Programs run concurrently with
  each other; inside a program, `wait.any` waits for several things at once.
- Programs call the kernel; the kernel never calls into a program.
- Every call of the ABI version is there to import, on every build. What a
  build or device leaves out (processes, files) returns `-WR_EUNSUPPORTED`,
  so one program runs everywhere and checks instead of failing to load.

## Rules every call follows

- **Return value.** Every call returns an `i32`: zero or a value on success,
  a negative error (`-WR_E*`) otherwise.
- **Types.** Only `i32` and `i64` cross the boundary; no floating point, so
  the ABI works where the interpreter is built without it. No C structs:
  where several values share memory the layout is spelled out, little-endian.
- **Pointers.** Arguments that point into the program's memory are checked.
  A range outside memory returns `-WR_EFAULT`; the kernel never traps on bad
  input. The kernel copies what it needs during the call and keeps no
  pointers into the program afterwards.
- **Strings** are (pointer, length) in UTF-8, without a terminating NUL. A
  list of strings is a pointer to (pointer, length) pairs plus a count. Names
  are at most 32 bytes and must be valid UTF-8.
- **Blocking.** A call that waits takes a timeout in milliseconds
  (`WR_WAIT_FOREVER` = 0xffffffff). Only the calling program waits; others
  keep running. A wait ends with `-WR_ETIMEOUT`, `-WR_EBUSY` when the program
  is suspended meanwhile, or `-WR_EABORTED` when it is cancelled.
- **Handles.** Objects are reached through small positive handles that belong
  to the program: another program's handle numbers mean nothing to it. A
  program holds up to 32 handles (a build may change this); one more gives
  `-WR_ELIMIT`. `handle.close` gives one back, and every handle closes when
  its program ends.
- **Ownership.** An object a program creates exists while some program holds
  a handle to it, and is deleted with the last one. Objects the embedder
  creates belong to the embedder.
- **Sharing.** Programs share nothing but kernel objects. A named object
  (`wr_queue_open("sensor", ...)`) is created by the first program that opens
  the name and found by the others; names are per object type. Opening a name
  with other parameters returns `-WR_EMISMATCH`.

## Errors

| Code | Name | Meaning |
|---|---|---|
| 1 | `WR_EFAULT` | a pointer or range lies outside the program's memory |
| 2 | `WR_EINVAL` | an argument is invalid |
| 3 | `WR_EBADH` | not an open handle of the right kind |
| 4 | `WR_ENOMEM` | out of memory |
| 5 | `WR_ELIMIT` | too many handles, processes or arguments |
| 6 | `WR_ENOTFOUND` | no such file or name |
| 7 | `WR_ETIMEOUT` | the timeout ran out |
| 8 | `WR_EABORTED` | the wait was cancelled |
| 9 | `WR_EBUSY` | the wait ended because the program was suspended |
| 10 | `WR_ENOTOWNER` | the mutex belongs to another program |
| 11 | `WR_EFULL` | the queue or semaphore is full |
| 12 | `WR_EEMPTY` | the queue is empty |
| 13 | `WR_EUNSUPPORTED` | not available on this device or build |
| 14 | `WR_ETOOLONG` | a string is too long |
| 15 | `WR_EBADPROGRAM` | the module cannot be loaded |
| 16 | `WR_EISDIR` | the path is a directory |
| 17 | `WR_ENOTDIR` | a path component is not a directory |
| 18 | `WR_EDEAD` | the process has already ended |
| 19 | `WR_EMISMATCH` | a named object exists with other parameters |
| 20 | `WR_ETRAPPED` | `proc.wait`: the child trapped |
| 21 | `WR_EKILLED` | `proc.wait`: the child was killed |
| 22 | `WR_EINUSE` | another program uses it (1.1) |
| 23 | `WR_ENACK` | `i2c`: the address or a byte was not acknowledged (1.3) |
| 24 | `WR_EBUS` | the bus failed: lost arbitration, stuck lines (1.3) |

## Interfaces

Each interface is the import module `wasm-rtos:kernel/<name>@1.<minor>.0`,
with the minor version the program was built for. A kernel of 1.M accepts
every 1.N with N <= M, so a program built for 1.0 imports `…@1.0.0` and
still runs on a 1.1 kernel; a call added in 1.1 exists only under 1.1 and
later names. Functions not listed in an interface may be added by later
minor versions.

### `handle`
`close(h)`.

### `sync`
- Mutex: `mutex-create(name)`, `mutex-lock(m, timeout)`, `mutex-unlock(m)`.
  The owner's priority is raised while a higher-priority program waits.
- Semaphore: `semaphore-create(name, max, initial)`, `semaphore-take(s,
  timeout)`, `semaphore-give(s)`.
- Event group (24 bits): `event-create(name)`, `event-set(e, bits)` → bits
  after, `event-clear(e, bits)` → bits before, `event-get(e)`,
  `event-wait(e, bits, flags, timeout)` → the bits when the wait was
  satisfied; flags `WR_EVENT_ALL` (every bit, not any) and `WR_EVENT_CLEAR`
  (clear the bits waited for).
- Queue: `queue-create(name, item_size, capacity)`, `queue-send(q, item,
  timeout)`, `queue-receive(q, item, timeout)`; `item` points to `item_size`
  bytes. With timeout 0 a full queue gives `-WR_EFULL` and an empty one
  `-WR_EEMPTY` instead of `-WR_ETIMEOUT`.

### `timer`
`create(period_ms, flags)` makes a stopped timer, periodic with
`WR_TIMER_PERIODIC`. `start`, `stop`, `set-period`. A timer counts its
expirations: wait for its handle, then `read(t)` returns and resets the
count.

### `wait`
`any(handles, count, timeout)` returns the index of the first ready handle:
a queue holding an item, a semaphore that can be taken, an event group with
a bit set, a timer that has expired, a child that has ended. It does not
consume anything; take the item, the count or the result afterwards.
Mutexes cannot be waited for here.

### `proc`
- `spawn(module, length, args, argc, priority, stack_size)` and
  `spawn-path(path, length, args, argc, priority, stack_size)` start a child
  and return its handle. Priority 0 inherits the caller's; stack size 0 lets
  the child's metadata or the kernel choose.
- `wait(child, timeout)` returns the child's exit code (0 to 2^31-1),
  `-WR_ETRAPPED` or `-WR_EKILLED`, and collects it: the handle then refers to
  nothing (`-WR_EBADH`). Close the handle afterwards.
- `kill(child)`; `self()` and `parent()` return process ids (0: no parent).

### `gpio` (1.1)
Pins, named as the device profile names them (`"led"`, `"button-a"`,
`"gpio5"`), so one program runs on every board that has the pin.

- `open(name, flags)` returns a handle; the pin has one owner at a time
  (`-WR_EINUSE`), and closing the handle or ending the program gives it back
  as an input without pulls. Flags: `WR_GPIO_OUTPUT` (else an input),
  `PULL_UP`, `PULL_DOWN`, `OPEN_DRAIN` (outputs), `ACTIVE_LOW`, `INIT_HIGH`
  (the level an output starts at, so it never glitches). An unknown name is
  `-WR_ENOTFOUND`; a mode the pin cannot do, `-WR_EUNSUPPORTED`; a
  contradictory one, `-WR_EINVAL`.
- Levels are 0 or 1 as the program sees them: a pin the board wires
  active-low and a pin opened with `WR_GPIO_ACTIVE_LOW` are inverted for the
  program, so writing 1 means "on".
- `set(pin, level)` drives an output; `get(pin)` reads the pin, outputs
  included. `set-mode(pin, flags)` changes the mode, for pins that turn
  around (one-wire sensors).
- `watch(pin, edges)` with `WR_GPIO_RISING` and/or `WR_GPIO_FALLING` (as the
  program sees levels; 0 stops) makes the handle ready for `wait.any` once
  such an edge happened. `events(pin)` returns how many happened since the
  last call and resets the count. Edges before the watch do not count.

Timing finer than the scheduler, such as bit-banged protocols, is not what
`gpio` is for; buses and pulse interfaces get their own interfaces.

### `display` (1.2)
Screens, named as the device profile names them (`"main"` for the primary
one). A program draws by rectangles, the way graphics libraries hand over
what they rendered, so it never needs a whole frame in memory.

- `open(name, flags)` returns a handle; flags are 0. A display has one owner
  at a time (`-WR_EINUSE`); when the handle closes the picture stays until
  the next owner draws. An unknown name is `-WR_ENOTFOUND`; a device without
  displays gives `-WR_EUNSUPPORTED`.
- `get(display, property)` describes it, so a program adapts instead of
  assuming:

  | Property | Value |
  |---|---|
  | 1 `width`, 2 `height` | pixels |
  | 3 `shape` | 0 rectangle, 1 round: a circle inscribed in width x height; pixels outside it are not shown |
  | 4 `format` | the panel's own pixel format, the fastest to write |
  | 5 `formats` | bit `1 << format` for every format `write` accepts |
  | 6 `dpi` | pixels per inch, 0 when unknown |
  | 7 `brightness` | 0-255; `-WR_EUNSUPPORTED` without brightness control |

  A property this kernel does not know is `-WR_EUNSUPPORTED`, so programs
  may ask for properties of later versions.
- Pixel formats, as the program stores them:

  | Format | Layout |
  |---|---|
  | 1 `mono1` | 1 bit per pixel, 1 = lit; each row starts on a byte; the leftmost pixel is the top bit |
  | 2 `rgb565` | 16-bit little-endian, red in bits 15-11, green 10-5, blue 4-0 |
  | 3 `xrgb8888` | 32-bit little-endian `0x00RRGGBB` |

  The kernel does not convert: a program writes a format the display
  accepts, at best its own.
- `write(display, x, y, width, height, format, pixels, length)` puts a
  rectangle on the display. It must lie inside the display and `length` must
  be exactly the rectangle's bytes (`-WR_EINVAL`); another format is
  `-WR_EUNSUPPORTED`. The kernel reads the pixels during the call, so the
  buffer can be reused when it returns.
- `fill(display, x, y, width, height, format, color)` paints a rectangle one
  pixel value, for clearing without a buffer.
- `flush(display)`: what was written shows no later than this. Displays that
  draw at once make it free; e-paper and displays that send whole frames
  show the picture here. Call it when a frame is complete.
- `set-brightness(display, level)`, 0-255.

### `input` (1.2)
Buttons, rotary controls (a bezel, a knob), touch surfaces and keyboards,
named as the device profile names them (`"buttons"`, `"bezel"`, `"touch"`,
`"keyboard"`).

- `open(name, flags)` returns a handle; flags are 0. A device has one owner
  at a time (`-WR_EINUSE`). An unknown name is `-WR_ENOTFOUND`; a device
  without any input gives `-WR_EUNSUPPORTED`. Events are kept only while a
  program owns the device: what happened before `open` does not arrive.
- `get(device, property)`: 1 `kind` (1 buttons, 2 rotary, 3 touch,
  4 keyboard), 2 `width` and 3 `height` (touch coordinates run from 0 to
  width-1 and height-1, the pixels of the display it covers), 4
  `steps-per-turn` (rotary detents in a full turn, 0 when unknown). Unknown
  properties are `-WR_EUNSUPPORTED`.
- The handle is ready for `wait.any` while events are queued.
  `read(device, events, max_events)` moves up to `max_events` of them into
  `events` and returns how many; 0 means none, it never blocks.
- An event is 16 bytes, little-endian:

  | Offset | Field | |
  |---|---|---|
  | 0 | u8 `type` | see below |
  | 1 | u8 | reserved, 0 |
  | 2 | u16 `code` | key code, axis or contact |
  | 4 | s32 `value` | |
  | 8 | u16 `x`, 10 u16 `y` | touch position, else 0 |
  | 12 | u32 `time` | kernel time in ms (low 32 bits) when it happened |

  | Type | `code` | `value` |
  |---|---|---|
  | 1 key | Linux key code (`KEY_*`, `BTN_*`) | 0 released, 1 pressed, 2 repeated, 3 long press |
  | 2 rotate | Linux axis: `REL_DIAL` (7) for a bezel or dial, `REL_WHEEL` (8) | detents, positive clockwise |
  | 3 touch | contact, 0 for the first finger | 0 up, 1 down, 2 move |
  | 4 text | 0 | a Unicode code point a keyboard typed |
  | 5 dropped | 0 | events lost before the ones that follow |

- Keys use the Linux input event codes (`linux/input-event-codes.h`): one
  table for keyboards and for device buttons such as `KEY_BACK` or
  `KEY_SELECT`.
- Long presses and repeats come from the kernel, with the timings of the
  device profile, so they feel the same in every program: a key held for
  the long-press time reports `3` once; a key that repeats reports `2` at
  the repeat rate until it is released. Gestures (double clicks, swipes) are
  for libraries, from the event times and positions.
- Turns the program has not read yet add up into one event, and so do
  unread moves of one contact (only the last position stays), so a fast
  spin or drag never floods the queue. When the queue is full anyway, the
  oldest events go and the next `read` starts with a `dropped` event.

### `i2c` (1.3)
Devices on I2C buses, the buses named as the device profile names them
(`"i2c0"`). Drivers built once for wasm work on every board that has the
chip on a bus.

- `open(bus, address, flags)` returns a handle for the device at a 7-bit
  address; flags are 0. Programs share the bus, one program per address
  (`-WR_EINUSE`). An unknown bus is `-WR_ENOTFOUND`; a device without I2C,
  `-WR_EUNSUPPORTED`.
- `transaction(device, ops, count)` runs up to 16 ops as one transaction.
  Each op is 12 bytes, little-endian: u32 `kind` (1 write, 2 read), u32
  `data` (pointer), u32 `length`. Between a write and a read comes a
  repeated start, at the end a stop; adjacent ops of one kind continue
  without a start, so a register address and its data can sit in two
  buffers. With no ops only the address goes out: a scan for devices.
- Results: 0, `-WR_ENACK` (no device at the address, or it refused a byte),
  `-WR_EBUS` (lost arbitration, stuck lines), `-WR_ETIMEOUT` (a device held
  the clock too long), `-WR_EFAULT`, `-WR_EINVAL`, `-WR_ELIMIT` (more than
  16 ops).
- The bus speed is the profile's. C helpers: `wr_i2c_write`, `wr_i2c_read`,
  `wr_i2c_write_read` (register reads).

### `spi` (1.3)
SPI buses named as the device profile names them (`"spi0"`).

- `open(bus, chip_select, mode, frequency_hz)` returns a handle.
  - With a chip select pin name (a `gpio` name such as `"lcd-cs"`), the
    kernel owns that pin and drives it low around every transaction; other
    programs may use other chip selects on the same bus.
  - With an empty name the program takes the whole bus (`-WR_EINUSE` while
    others use it, and they get it while it does) and drives chip select
    itself through `gpio`, as Arduino libraries do.
  - `mode`: bit 0 CPHA, bit 1 CPOL (SPI modes 0-3), bit 2 least significant
    bit first. `frequency_hz` is the most the device takes; 0, or more than
    the bus's maximum, gets the bus's maximum.
- `transaction(device, ops, count)` runs 1 to 16 ops with chip select held
  throughout. Each op is 16 bytes, little-endian: u32 `kind`, u32 `write`,
  u32 `read`, u32 `length`.

  | Kind | |
  |---|---|
  | 1 write | sends `length` bytes from `write` |
  | 2 read | receives `length` bytes into `read`, sending zeros |
  | 3 transfer | sends from `write` and receives into `read` at once; they may be the same buffer |
  | 4 delay | waits `length` microseconds (at most 65535), chip select still low |

- Results: 0, `-WR_EBUS`, `-WR_EFAULT`, `-WR_EINVAL`, `-WR_ELIMIT`.
- Data/command lines, resets and interrupts of a chip are `gpio` pins: set
  them between transactions.

A transaction runs to its end before another starts, so one program's bytes
never mix with another's. The kernel reads and writes the program's buffers
in place. Only the calling program waits for a transaction: on devices that
move the bytes in the background (DMA) the other programs run meanwhile,
and the call returns when the transfer has ended. A wait for a transfer is
not cut short: when the program is paused or its wait is aborted, the
transfer still ends and the program gets its result.

## WASI

Everything WASI preview 1 covers comes from WASI: arguments and environment,
files (the kernel's file system, see the README), output, exit, random
numbers. The kernel answers these calls itself:

- `clock_time_get`, `clock_res_get`: kernel time, 1 ms resolution, for every
  clock. Until a platform supplies wall time, the realtime clock counts from
  boot too.
- `poll_oneoff`: sleeps only the calling program until the earliest clock
  subscription (`nanosleep`, `usleep`). File subscriptions are ready at once.
- `sched_yield`: ends the program's time slice.

## Metadata: the `wasm-rtos.app` section

A program may describe itself in a custom section named `wasm-rtos.app`:
records of a key byte, a LEB128 length and that many bytes. Records may
appear in any order and come from several object files. Unknown keys are
skipped; a section that is not whole records fails the load.

| Key | Value | |
|---|---|---|
| 1 | u32 LE, `major << 16 \| minor` | ABI version; another major fails the load |
| 2 | u32 LE | interpreter stack in bytes, unless the creator picks one |
| 3 | UTF-8 | name shown for the program |
| 4 | bytes | reserved: icon |
| 5 | UTF-8 | reserved: file extensions the program opens, `;`-separated |
| 6 | bytes | reserved: device requirements |

A program for 1.3 states 65539 (1.3); kernels refuse only another major.
In C: `WR_APP_ABI_VERSION();`, `WR_APP_STACK_SIZE(4096);`,
`WR_APP_NAME("Clock");`. In Rust: `#[link_section = "wasm-rtos.app"]` on a
static byte array.

## Reserved for later minor versions

These names are taken; their shape will follow the rules above.

- `wasm-rtos:kernel/pipe`: byte streams between programs, seen as WASI file
  descriptors.
- `wasm-rtos:kernel/device`: what the device offers (display size and shape,
  inputs, sensors), so programs ask instead of assuming.
- `wasm-rtos:kernel/uart`, `adc`, `pwm`: the same way as `i2c` and `spi`.
- `wasm-rtos:kernel/power`, `system`, `ui`: sleep and wake sources, battery
  and indicators, windows and dialogs. `ui` will hand programs windows that
  behave like `display` and `input` devices.

## Not in 1.0

Callbacks into programs, threads inside a program, sharing memory between
programs, and calls that hand the program memory allocated by the kernel.
