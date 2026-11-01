# wasm-rtos ABI 1.0

What a WebAssembly program can rely on when it runs on wasm-rtos. Programs
built against ABI 1.x keep working on every kernel that supports 1.x; new
calls arrive in minor versions, and only a new major version may change or
remove anything here.

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

## Interfaces

Each interface is the import module `wasm-rtos:kernel/<name>@1.0.0`.
Functions not listed in an interface may be added by later minor versions.

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

In C: `WR_APP_ABI_VERSION();`, `WR_APP_STACK_SIZE(4096);`,
`WR_APP_NAME("Clock");`. In Rust: `#[link_section = "wasm-rtos.app"]` on a
static byte array.

## Reserved for later minor versions

These names are taken; their shape will follow the rules above.

- `wasm-rtos:kernel/pipe`: byte streams between programs, seen as WASI file
  descriptors.
- `wasm-rtos:kernel/device`: what the device offers (display size and shape,
  inputs, sensors), so programs ask instead of assuming.
- `wasm-rtos:kernel/gpio`, `i2c`, `spi`, `uart`, `adc`, `pwm`: buses with
  whole transactions and explicit errors, the same on every port.
- `wasm-rtos:kernel/input`: rotary, button and touch events.
- `wasm-rtos:kernel/display`, `power`, `system`, `ui`: drawing, sleep and
  wake sources, battery and indicators, windows and dialogs.

## Not in 1.0

Callbacks into programs, threads inside a program, sharing memory between
programs, and calls that hand the program memory allocated by the kernel.
