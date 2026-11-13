# wasm-rtos ABI 1.8

What a WebAssembly program can rely on when it runs on wasm-rtos. Programs
built against ABI 1.x keep working on every kernel that supports 1.x; new
calls arrive in minor versions, and only a new major version may change or
remove anything here.

**1.x is frozen at 1.8.** For all of 1.x, every call of 1.8 keeps its
import name, arguments, result and meaning, and so do the error codes, the
layouts in memory, property numbers, flag bits, metadata keys and the WASI
calls the kernel answers. A minor version may only add: calls, properties,
flags, error codes, metadata keys, WASI calls answered. The list
[`abi-1.8.txt`](abi-1.8.txt) holds every call and constant of 1.8, and the
kernel's CI fails when one of its lines stops being true.

| Version | Adds |
|---|---|
| 1.0 | `handle`, `sync`, `timer`, `wait`, `proc`, WASI, metadata |
| 1.1 | `gpio`, error `WR_EINUSE` |
| 1.2 | `display`, `input` |
| 1.3 | `i2c`, `spi`, errors `WR_ENACK`, `WR_EBUS` |
| 1.4 | `adc`, `pwm`, `uart` |
| 1.5 | `pulse` |
| 1.6 | `onewire` |
| 1.7 | `counter` |
| 1.8 | `dac` |

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
  so one program runs everywhere and checks instead of failing to load. The
  same holds for calls of a newer minor version (see Interfaces) and for
  WASI calls the kernel does not answer (`ENOSYS`, see WASI).

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
- **Flags and properties.** A flag bit a call does not know is
  `-WR_EINVAL`; a property number it does not know is `-WR_EUNSUPPORTED`. So
  a program using a flag or property of a later minor version learns on an
  older kernel that it is not there, instead of having it ignored.
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
| 9 | `WR_EBUSY` | the wait ended because the program was suspended; `mutex-lock` on a mutex the program already holds (mutexes are not recursive) |
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
with the minor version the program was built for. A kernel accepts every
minor version, older and newer than its own:

- a program built for 1.0 imports `…@1.0.0` and runs on a 1.8 kernel; a
  call added in 1.1 exists only under 1.1 and later names;
- a program built for 1.9 imports `…@1.9.0` and still loads on a 1.8
  kernel. Calls 1.8 has work as always; a call or interface added after
  1.8 links too and returns `-WR_EUNSUPPORTED`, as an absent device does.
  A program that uses a new call checks its result, as it does for devices.

Another major version (`@2.0.0`) does not link. Functions not listed in an
interface may be added by later minor versions.

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
`any(handles, count, timeout)` returns the index of the first ready handle.
These handles can be waited for, and are ready when:

| Handle | Ready when |
|---|---|
| queue | it holds an item |
| semaphore | it can be taken |
| event group | a bit is set |
| timer | it has expired since the last `read` |
| child (`proc`) | it has ended |
| `gpio` pin (1.1) | a watched edge has not been read (`watch`, `events`) |
| `input` device (1.2) | events are queued |
| `uart` port (1.4) | received bytes wait to be read |
| `counter` (1.7) | the count is outside its `watch` window |

It does not consume anything; take the item, the count or the result
afterwards. Other handles (mutexes, the other devices) give `-WR_EBADH`.

### `proc`
- `spawn(module, length, args, argc, priority, stack_size)` and
  `spawn-path(path, length, args, argc, priority, stack_size)` start a child
  and return its handle. Priority 0 inherits the caller's base priority; a
  higher priority than the caller's is lowered to it, so a program never
  takes more of the processor than it was given. Stack size 0 lets the
  child's metadata or the kernel choose.
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

### `adc` (1.4)
Analog inputs, named as the device profile names them (`"a0"`,
`"battery"`).

- `open(name, flags)` returns a handle; flags are 0. Reading changes
  nothing, so programs share a channel (no `-WR_EINUSE`).
- `read(channel)` is the raw value, 0 to `max`; `read-mv(channel)` the
  voltage in millivolts, calibrated by the device where it can.
- `get(channel, property)`: 1 `resolution` (bits), 2 `max` (the largest raw
  value), 3 `full-scale-mv` (its voltage). Unknown properties are
  `-WR_EUNSUPPORTED`.

### `pwm` (1.4)
Pulse-width outputs on GPIO pins, by the pin's name (`"led"`, `"gpio5"`):
LED brightness, motor speed, servos, tones.

- `open(name, frequency_hz, flags)` returns a handle and owns the pin as
  `gpio.open` does (`-WR_EINUSE`); it starts off. A pin that cannot do PWM is
  `-WR_EUNSUPPORTED`; no free PWM channel, `-WR_ELIMIT`. Flag
  `WR_PWM_ACTIVE_LOW` (1) makes "on" the pin's low; a pin the board wires
  active-low is inverted already, as with `gpio`.
- `set-duty(output, duty)`: 0 (off) to 65535 (fully on), whatever the
  hardware's resolution, so the same value means the same on every chip.
- `set-frequency(output, hz)`; `get(output, property)`: 1 `frequency` (what
  the hardware runs at, which may be rounded), 2 `duty` (the last one set).
- Closing the handle, or ending the program, stops the output and gives the
  pin back as an input.

### `uart` (1.4)
Serial ports, named as the device profile names them (`"uart1"`, `"gps"`).

- `open(name, baud, format)` returns a handle; one program per port
  (`-WR_EINUSE`). `format` 0 is 8 data bits, no parity, one stop bit;
  `WR_UART_PARITY_EVEN` (1), `_ODD` (2), `_STOP_2` (4), `_DATA_7` (8),
  `_FLOW_RTS_CTS` (16). A speed above the port's maximum is
  `-WR_EUNSUPPORTED`; the speed the port runs at is `get(port, 1)`.
- Received bytes wait in the kernel, 256 per port; more are dropped and
  counted. The handle is ready for `wait.any` while bytes wait;
  `read(port, buffer, max)` takes up to `max` and returns how many, 0 when
  nothing has arrived.
- `write(port, data, length)` hands the port what fits in its send buffer
  and returns how many bytes that was. After a short write,
  `wait-writable(port, timeout)` waits until there is room again (0 at once
  when there is; with timeout 0, `-WR_EFULL` when there is not). C:
  `wr_uart_write_all` loops over both.
- `get(port, property)`: 1 `baud`, 2 `available` (bytes waiting), 3
  `errors` (frame, parity and overrun errors and dropped bytes since the
  last time the program asked).

### `pulse` (1.5)
Pulse trains timed finer than a program can keep: addressable LEDs,
infrared remotes, echo sensors. Hardware made for it does the timing (a
remote-control peripheral, programmable I/O, a timer with DMA); only the
calling program waits while it works.

- `open(name, flags)` on a GPIO pin by its name, owned as `gpio.open` does;
  a pin without pulse hardware is `-WR_EUNSUPPORTED`. Flags:
  `WR_PULSE_RECEIVE` (1) captures, otherwise the pin sends;
  `WR_PULSE_IDLE_HIGH` (2) is the level between trains (sending only).
  Levels are the pin's own.
- An item is 32 bits: bit 31 the level, bits 0-30 the time in nanoseconds
  (up to 2.1 s). The device rounds to its resolution, `get(channel, 1)`.
  Item arrays must be 4-byte aligned (`-WR_EINVAL`).
- `send(channel, items, count, carrier_hz)`: with a carrier, high items are
  that carrier (infrared), low items silence.
- `send-bits(channel, data, length, zero_high, zero_low, one_high,
  one_low)` sends bytes, most significant bit first, each bit as a high and
  a low time in nanoseconds. Addressable LEDs need 3 bytes per LED instead
  of 96 bytes of items. `-WR_EUNSUPPORTED` on devices that cannot.
- `receive(channel, items, max, idle_ns, timeout_ms)` captures from the
  first edge until `idle_ns` pass without one or `max` items are full, and
  returns how many it wrote; `-WR_ETIMEOUT` when no edge comes within
  `timeout_ms`.
- C helpers: `wr_ws2812_show` (bytes plus the latch time),
  `wr_nec_encode` and `wr_nec_decode` for NEC infrared remotes.
- Like a background bus transfer, the wait for a train is not cut short:
  pausing or aborting the program's wait lets the train end first.

### `onewire` (1.6)
1-Wire buses, named as the device profile names them (`"ow0"`; a bus on a
single pin is best named after the pin, `"gpio4"`). Devices are told apart
by their 64-bit ROM code: the family code in the low byte, then the serial
number, the CRC-8 in the high byte (the order they go over the wire).

- `open(bus, rom, flags)` returns a handle; flags are 0.
  - With a ROM code, a device: programs share the bus, one program per ROM
    (`-WR_EINUSE`). A code whose CRC is wrong is `-WR_EINVAL`. Every
    transaction starts with a reset and match ROM, which the kernel sends.
  - With rom 0 the program takes the whole bus (`-WR_EINUSE` while others
    use it, and they get it while it does) and sends resets and ROM
    commands itself, over as many transactions as it likes. Libraries that
    work byte by byte (Arduino's OneWire) need this.
- `transaction(device, ops, count)` runs up to 16 ops in one go. Each op is
  12 bytes, little-endian: u32 `kind`, u32 `data`, u32 `length`.

  | Kind | |
  |---|---|
  | 1 write | sends `length` bytes, least significant bit first |
  | 2 read | reads `length` bytes |
  | 3 write-bits | sends `length` single time slots from `data`, packed least significant bit first |
  | 4 read-bits | reads `length` single time slots into `data` |
  | 5 power | right after a write: holds the line high through a strong pull-up for `length` ms (at most 65535), for parasite-powered devices converting or copying; 0 keeps it until the next transaction on the bus, and then comes last |
  | 6 reset | a reset pulse; the whole bus only |

  With no ops a device's transaction is only the reset and match ROM, and
  answers whether anything is present; on the whole bus an empty
  transaction ends a pull-up left on.
- `search(bus, state, flags)` finds the next ROM code on the bus, as many
  as there are, and returns 1, or 0 when there are no more (then the state
  starts over). `state` points to 16 bytes the program keeps between calls
  and zeroes to start: u64 `rom` (the code found), u32 `discrepancy`, u32
  `done`. Starting with `rom` set to a family code and `discrepancy` 64
  finds that family first. Flag 1 finds only devices in alarm. A search
  needs no handle and runs while other programs hold devices, but not
  while another program holds the whole bus (`-WR_EINUSE`).
- Results: 0, `-WR_ENACK` (no presence pulse after a reset), `-WR_EBUS`
  (a shorted line, a ROM code that arrived broken), `-WR_EUNSUPPORTED` (a
  power op on a bus without a strong pull-up), `-WR_EFAULT`, `-WR_EINVAL`,
  `-WR_ELIMIT`.
- C helpers: `wr_onewire_crc8`, `wr_onewire_write_read`, and for DS18B20
  temperature sensors `wr_ds18b20_convert` (powers the conversion where it
  can, waits otherwise) and `wr_ds18b20_read` (thousandths of a degree).
- Transactions run whole and wait like those of `i2c` and `spi` above.

### `counter` (1.7)
Pulse counters and quadrature encoders, named as the device profile names
them (`"enc0"`, `"flow"`): motor encoders, flow meters, anemometers. The
hardware counts, so no edge is lost while the program is busy or waits.

- `open(name, mode, filter_ns)` returns a handle; one owner per counter
  (`-WR_EINUSE`). The mode depends on the kind (`get(counter, 1)`: 1 a pulse
  counter on one pin, 2 a quadrature encoder on two):

  | Mode | |
  |---|---|
  | 1 rising, 2 falling, 3 both | the edges a pulse counter counts |
  | 4 x1, 5 x2, 6 x4 | an encoder's steps per cycle of A and B: one, both edges of A, every edge |

  A mode of the other kind is `-WR_EINVAL`. `filter_ns` is the shortest
  pulse that counts (0: none); the device rounds it (`get(counter, 3)`).
- `read(counter, count)` writes the count, a signed 32-bit value that
  wraps, to `count` (4 bytes, little-endian) and returns 0. A distance is
  the difference of two reads. Counting starts at 0 when opened;
  `set(counter, value)` sets it (homing).
- `watch(counter, low, high)` makes the handle ready for `wait.any` while
  the count is below `low` or above `high`: `watch(c, n, n)` wakes when the
  count moves from `n`. Watch again after it fired; `INT32_MIN, INT32_MAX`
  stops watching. Devices without watch points in hardware read the count
  every 10 ms, so a wait may end up to that late.
- `get(counter, 2)`: the fastest edges it counts in Hz (0: unknown).

### `dac` (1.8)
Analog outputs, named as the device profile names them: a pin's own
converter (`"gpio25"`) or an output of its own (`"dac0"`, an external
chip).

- `open(name, flags)` returns a handle; flags are 0. One program per output
  (`-WR_EINUSE`). An output named like a GPIO pin takes that pin as
  `gpio.open` would, so no program drives it digitally meanwhile. The
  output is 0 when opened and switches off when closed (the pin goes back
  to an input).
- `write(output, raw)` sets a raw value from 0 to `get(output, 2)`;
  `write-mv(output, mv)` the nearest step to a voltage up to
  `get(output, 3)`. More than that is `-WR_EINVAL`.
- `get(output, property)`: 1 resolution in bits, 2 the largest raw value,
  3 its voltage in mV, 4 the raw value last written.

## WASI

Everything WASI preview 1 covers comes from WASI: arguments and environment,
files (the kernel's file system, see the README), output, exit, random
numbers. Both `wasi_snapshot_preview1` and the older `wasi_unstable` link.

**Every WASI call links.** One the kernel (or the embedder) does not
answer, sockets for example, returns `ENOSYS` (52), so a C library that
imports it only in case does not keep the program from loading.

On every build the kernel answers:

- `clock_time_get`, `clock_res_get`: kernel time, 1 ms resolution, for every
  clock. Until a platform supplies wall time, the realtime clock counts from
  boot too.
- `poll_oneoff`: sleeps only the calling program until the earliest clock
  subscription (`nanosleep`, `usleep`). File subscriptions are ready at once.
- `sched_yield`: ends the program's time slice.
- `args_sizes_get`, `args_get`, `environ_sizes_get`, `environ_get` (no
  variables), `proc_exit`, `random_get`.
- `fd_write` to 1 and 2, `fd_read`, `fd_seek`, `fd_close`, `fd_fdstat_get`
  on the standard streams; `fd_prestat_get`, `fd_prestat_dir_name` (no
  directory without files: `EBADF`).

With files, also: `path_open`, `fd_read`, `fd_pread`, `fd_seek`,
`fd_tell`, `fd_readdir`, `fd_filestat_get`, `path_filestat_get` (preview 1
only), `fd_prestat_get`, `fd_prestat_dir_name`, `fd_fdstat_get`,
`fd_close`; `fd_advise`, `fd_datasync`, `fd_sync` succeed and do nothing.
The file system is read-only: `fd_pwrite`, `fd_allocate`,
`fd_filestat_set_size`, `fd_filestat_set_times`, `fd_fdstat_set_flags`,
`path_create_directory`, `path_filestat_set_times`, `path_link`,
`path_remove_directory`, `path_rename`, `path_symlink`,
`path_unlink_file` return `EROFS`. Every other WASI call returns `ENOSYS`.

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

A program for 1.8 states 65544 (1.8); kernels refuse only another major,
and a newer minor loads like its imports do.
In C: `WR_APP_ABI_VERSION();`, `WR_APP_STACK_SIZE(4096);`,
`WR_APP_NAME("Clock");`. In Rust: `#[link_section = "wasm-rtos.app"]` on a
static byte array.

## When the device sleeps

A device may save the whole system, sleep deeply (losing all RAM) and go on
where it stopped. To a program this looks like a long pause between two
instructions:

- **The clock goes on.** After waking, `clock_time_get` and the kernel's
  time include the sleep; they do not start again from boot. A timeout or
  delay that fell due during the sleep ends at once, with the result it
  would have had; one still running keeps its time left.
- **Kernel objects are kept** as they were: handles, queues and their
  items, mutexes and their owners, semaphores, event bits, timers, children,
  open files and their positions.
- **Devices are set up again** as the program left them, by name: pin
  modes and output levels, bus devices, serial speed and format, PWM duty,
  analog outputs, counters and their counts, pulse channels, display
  brightness. Data waiting to be read (edges, input events, received bytes)
  is kept.
- **What happened during the sleep is lost**: edges, key presses, serial
  bytes and counts that arrived while the device slept, except the edge or
  press that woke it, which the program sees. Keys held are released. A
  display that loses power loses its picture; a device that would lose a
  picture this way does not sleep while a program has it open.
- **When something is gone after waking** (the program's file changed, a
  device is no longer there), only the programs that relied on it end, with
  an error; the others go on.

## Reserved for later minor versions

These names are taken; their shape will follow the rules above.

- `wasm-rtos:kernel/pipe`: byte streams between programs, seen as WASI file
  descriptors.
- `wasm-rtos:kernel/device`: what the device offers (display size and shape,
  inputs, sensors), so programs ask instead of assuming.
- `wasm-rtos:kernel/power`, `system`, `ui`: sleep and wake sources, battery
  and indicators, windows and dialogs. `ui` will hand programs windows that
  behave like `display` and `input` devices.

## Not in 1.0

Callbacks into programs, threads inside a program, sharing memory between
programs, and calls that hand the program memory allocated by the kernel.
