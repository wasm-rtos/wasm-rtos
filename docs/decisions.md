# Decisions

Why the kernel is the way it is: each entry says what was decided, why, and
what was considered instead. New entries start as **Proposed** and are
discussed before any code; once built they say what was measured. The
engine's own decisions are in `vm/docs/decisions.md`.

## 1. Only WebAssembly is loaded at run time

**Decided.** Programs, libraries and anything a device loads after it is
built are `.wasm`. Native code is only what the port builds into its
firmware: the kernel, drivers, host functions.

**Why.** One format runs on every chip, is checked before it runs, is
stopped by fuel, and is saved and moved as plain data. A native module would
need a loader and relocations per processor, could not be stopped or saved,
and could break the kernel.

**Instead.** Loading native modules (ELF or the like) for speed: per chip,
unsafe, and every guarantee above stops at them.

## 2. Tasks are preempted by fuel

**Decided.** A task runs for a slice of fuel (`OS_TASK_SLICE_FUEL`
instructions, engine decision 1); then the scheduler picks again. No timer
interrupt stops a program.

**Why.** A program stops at an instruction boundary, where its whole state
is data: it can be saved, swapped out or moved there. Slices are the same on
every device and in the simulator.

**Instead.** A timer interrupt and a stack per task: needs per-processor
context switching, and a program stopped anywhere cannot be saved.

## 3. Drivers join by what the platform registers

**Decided.** The kernel knows no driver by name; a platform registers the
drivers it has (`os_driver_add`), and a program importing an interface no
driver offers still loads and gets `-WR_EUNSUPPORTED`.

**Why.** A build carries only the drivers its board uses, and one program
runs on boards with and without a device.

## 4. Under pressure, a cold waiting block leaves RAM first

**Decided (2026-11-12).** When frames are needed, the kernel sends out the
block of a task that waits with no deadline or one at least
`OS_PAGE_COLD_MS` away; then single frames by CLOCK; then any block.

**Why.** The first rule tried, "frames of tasks other than the running one
first", made programs take their frames back from each other: on an ESP32
with an SD card store, 126 to 223 reads against 111 when every memory was
paged. With cold blocks first, 111 to 112, and the runs no slower than all
paged.

## 5. Frames come from a pool the page store sets aside

**Decided and built (2026-11-12).** The kernel makes the engine's frame
pool (engine decision 6) from the page store's RAM budget plus the frames a
store writing in the background may have on their way out
(`os_page_ram_bytes`), or from the region the platform names
(`OsPageStore.ram`). A transfer in the background pins a block as it pins a
paged memory.

## 6. The whole system is saved before deep sleep and restored after

**Decided and built in the kernel (2026-11-13); devices and boards next.**

### Problem

A task snapshot (`os_task_write_snapshot`) holds only the program inside
the engine: position, stack, globals, tables, memory. Everything the kernel
knows about the task is lost: what it waits for and until when, its
handles, the queues, mutexes, semaphores, event groups and timers it shares
with others, its notifications, its children, its open files, the devices
it owns. So only a task that waits for nothing and has no timer can be
saved (`os_task_snapshot_is_busy`), that is, one that only computes.

A device that sleeps deeply (an ESP32 in deep sleep, a board switched off)
loses all RAM, the kernel's included. To go on after it wakes as if it had
not slept, everything has to be saved, not one task.

Swapping a task out (`os_task_swap_out`) does not have this problem: the
task stays in the kernel and only its memory leaves RAM.

### Decision

The kernel saves the whole system to a stream and restores it into a
freshly started kernel. It is the host's choice when: typically when the
kernel has nothing to do for longer than sleeping and waking take.

```c
OsStatus os_system_save(OsSnapshotWrite write, void* context);

/* The module of a task the host created; the snapshot checks it is the same one. */
typedef OsStatus (*OsModuleFind)(void* context, const char* task_name,
                                 const uint8_t** out_bytes, uint32_t* out_size);

OsStatus os_system_restore(OsSnapshotRead read, void* context,
                           OsModuleFind find, void* find_context,
                           uint32_t slept_ms);
```

- **When.** Between slices only (no task running), from the host. The
  stream goes wherever the host writes it: flash, an SD card, external RAM
  that keeps power.
- **What is saved.** Plain numbers and names, no addresses, in a versioned
  format of its own, like engine snapshots:
  - every task, dead ones too (a parent may not have collected its exit
    code): id, name, state, priorities, what it waits for (the object by
    type and id, bits, the place of the result in its memory, the time
    left), notifications, process fields (parent, child it waits for,
    argv), fuel used, and its engine snapshot. A swapped-out task is
    brought back first (its state only: its memory stays paged);
  - every queue, mutex, semaphore, event group and timer: id, name,
    references, owner, contents (items, owner task, count, bits, period
    and time left);
  - the next ids, so objects made after the restore do not collide;
  - open files: path and position;
  - each driver's objects, through two new optional calls in `OsDriver`:
    `save` writes which object a task holds and how it is set up, and
    `restore` sets the device up again (the next step: until then a task
    holding a device cannot be saved).
- **Modules are not saved.** A task started from a path is opened from the
  same path again; one the kernel copied (`os_task_spawn`) is the kernel's
  only copy, so it goes into the stream. For a task the host created, the
  kernel asks `find` by task name. The stream keeps each module's size and
  hash (FNV-1a, as engine snapshots), and a module of other bytes is
  refused.
- **Time.** The kernel's clock and every deadline are saved as they are,
  so the time left is kept. On restore the clock goes on from where it was
  plus `slept_ms` (the host knows how long it slept: an ESP32's sleep timer
  does): what fell due during the sleep is delivered at once, and programs
  see the clock go on, not start again.
- **Two kinds of failure.** After a restore the system has to be in a
  state it could have reached without sleeping.
  - *The stream itself* is cut, unreadable, of another format or version,
    or does not agree with itself (a wait names an object it does not
    hold): nothing in it can be trusted. The restore fails, the kernel is
    left as it was before the restore, and the host starts afresh.
  - *The world outside changed* for one task: its module was replaced or
    removed, a file it had open is gone, a device it holds cannot be set
    up. Only that task is not restored. It ends as a task that traps ends:
    dead, with `OS_TASK_EXIT_WASM_ERROR`, the error phase `"restore"` and
    the reason; its mutexes are released, its handles closed, a parent
    waiting for it gets its exit; the other tasks go on. Its memory, stack
    and code are never made, so it keeps only its record (344 bytes on an
    ESP32, and its error text) until the host deletes it or its parent
    collects it. The kernel does not start the program again by itself:
    the host sees why and decides.
- **Refused to save**, with the reason in the last error:
  - a transfer in the background or a page being read or written: the
    host tries again after the next slice;
  - a device whose driver has no `save` and an object a task holds open:
    it could not be set up again;
  - a task of a dylink.0 program: its engine snapshot is not supported yet.
- **The host's own part.** Before `os_system_restore` the host does what it
  does at start: `os_init`, the clock, the page store, its drivers, host
  imports, dylink libraries. Objects the host made come back with their ids
  and names; it finds them by id or by name instead of creating them.
  Waits of the host itself do not exist (only tasks wait), so nothing of
  the host's is lost.

### Why

- Saving everything needs no line between one task and the others: a queue
  shared by two tasks is saved once, with both of them.
- Every piece is already data: the engine's state is portable, objects have
  ids, waits name objects, results go to offsets in a program's memory.
  What is missing is writing it down.
- It is the base for moving one task to another device later (the next
  decision): the same writing of objects and waits, then a line drawn at
  names.

### Instead

- **All or nothing** for every failure: after a program on the card is
  replaced, every task would be lost, not the one whose program changed.
  Systems that save a whole image refuse a broken image and start afresh,
  as here; for a resource gone outside, they lose only what depended on it
  (a device, an object) or the group of processes sharing it. A task here
  can end alone, as it does when it traps, so only it is lost.
- **Light sleep only**, RAM kept: no saving needed, but an ESP32 then draws
  around a milliampere instead of microamperes, and a board switched off
  loses everything anyway.
- **Each task saves itself** (a program writes its own state to a file and
  reads it at start): every program has to be written for it, and waits,
  handles and shared objects still cannot be saved by a program.
- **Saving the RAM as it is** (an image of the kernel's memory): ties the
  image to one firmware build and one address layout, saves heap free
  space too, and devices still have to be set up again.

### Costs

- The stream holds every task's memory that is not zero. With a page store
  on an SD card most of it is already there; writing it again is the first
  version, referring to the store's slots is a later one.
- Drivers get two optional calls. Until a driver has them, a task holding
  one of its objects blocks saving.
- Code: `system.c`, about 1500 lines, half of them reading and checking
  the stream; the handles and open files write and read themselves
  (`abi.c`, `fs.c`). An open file now keeps its path, as an open directory
  did.

### Checks

- `wasm-rtos-tests/tests/system_snapshot`: eight programs from one module
  share a queue the host made, a mutex, a semaphore and an event, wait for
  a periodic timer, a child started from a file and a file read 5 bytes at
  a time; the host's timer notifies one of them, and one is a process the
  kernel keeps a copy of. Each returns a hash of what it saw and when.
  - Saved once after 10 ms, with the locker asleep holding the mutex, the
    contender, the parent and the reader waiting, and the consumer swapped
    out; `os_shutdown`, started again, restored: every program ends with
    the result of the run never stopped, the ticker was notified as often,
    the host finds its queue by its id, and a queue made afterwards gets an
    id of its own.
  - Saved, shut down and restored after every millisecond (57 times): the
    same results.
  - A sleeper saved 30 ms into a sleep of 100 ms and restored with
    `slept_ms` 50 wakes 21 ms later (the 1 ms step of the test) and sees
    100 ms go by.
  - A stream cut in half, and one of another version: nothing restored,
    and the kernel starts tasks with id 1 and runs them.
  - Restored with the locker's module changed, the file shorter and the
    child's program gone: the locker and the reader end with
    `restore: its module changed` and `restore: a file it had open
    changed`; the contender gets the mutex and ends; the parent gets its
    child's end and ends; the four others end with the results of the run
    never stopped.
  - Each rule broken on purpose, 13 of them (the clock, the time slept, a
    wait's object, the next ids, a queue's items, a mutex's owner, a
    timer's task, a notification, a lost task left alive, the module's
    hash, a file's position, the handles, a cut stream taken): the test
    fails every time.
- Under AddressSanitizer this found that `os_shutdown` freed tasks still
  in the list: releasing the last one's handles could delete a shared
  object, which looked at the freed ones. Fixed, as in `system.c`.
- Mac, the test's eight programs mid-way (9 tasks with the child): a stream
  of 29582 bytes, saved in 0.63 ms and restored in 0.39 ms.
- Boards (next): an ESP32 in real deep sleep with the stream on the SD
  card or in flash, and a Black Pill reset with the stream in flash;
  measured, size and time to save and restore.

### Then

- **Moving one task** to another device or a browser: its own unnamed
  objects go with it, shared ones are opened again by name, devices do not
  move.
- The stream refers to frames already in the page store instead of copying
  them.

## 7. Drivers save their devices; a watched pin wakes the board

**Decided and built in the kernel (2026-11-13); boards next.**

### Problem

Decision 6 saves the whole system, but refuses while a program holds a
device (a pin, a bus device, a serial port, a display...): the kernel does
not know what the device's driver keeps or how to set the hardware up
again. On a real board almost every program holds one, so deep sleep is of
little use without it. And a program that waits for a button sleeps
forever: the board only wakes by its timer.

### Decision

**Drivers write and read their own state.** `OsDriver` gets two optional
calls:

```c
/* Write every object a task holds, by name, and how it is set up. */
void (*save)(OsSystemStream* stream);
/* Read them back and set the hardware up again through the port. */
void (*restore)(OsSystemStream* stream);
```

- Objects are saved **by name** (`"led"`, `"i2c0"`, `"uart1"`), not by
  slot: after the sleep the platform registers its devices again, and the
  name finds the same one.
- A handle to a device saves as its driver's interface and the object's
  id; a wait for a device (`wait.any`, a serial port's signal wait) the
  same. Each driver's part of the stream is in chunks, so the part of a
  driver the firmware no longer has is skipped and its devices are gone.
  After the drivers restore, the kernel maps each handle and wait to the
  id its device has now.
- Restoring a device that **is not there** (the firmware no longer
  registers that name) or that **the port refuses to set up** is the
  world changed for the task holding it (decision 6): only that task ends,
  `restore: a device it held is gone` or `... cannot be set up`.
- `gpio` restores first: PWM, pulse, DAC and SPI chip select take pins.
- A driver with no `save` still blocks saving while a task holds one of its
  objects; the drivers below all get one.

What each driver keeps, and what the sleep loses:

| Driver | Saved and set up again | Lost in the sleep |
|---|---|---|
| gpio | owner, mode (output, pull, open drain, active low), output level, edges watched, edges not yet read | edges during the sleep (except the one that woke it) |
| i2c | owner and address of each device; the bus frequency | — |
| spi | owner, mode, frequency, chip select pin | — |
| 1-Wire | owner of the bus or device | a strong pull-up (and the conversion it powered) |
| uart | owner, baud, format, bytes received and not yet read | bytes that arrived during the sleep |
| adc | the handles (nothing is set up) | — |
| dac | owner, the value written | — |
| pwm | owner, frequency, duty | — |
| counter | owner, what it counts, filter, watch window, when it was last polled, the count (set back into the port) | counts during the sleep, unless the hardware keeps counting |
| pulse | owner, direction and idle level | — |
| input | owner, events not yet read; keys held are released | presses during the sleep (except the one that woke it) |
| display | owner, brightness | the pixels, if the display loses power: see below |

**A display's pixels** are in the display, not in the kernel. A display
that keeps power in the sleep keeps its image. One that loses it comes back
blank, and the kernel cannot redraw it without a copy of every pixel; the
port says which (`OsDisplayInfo`, a flag *keeps its image*), and for one
that does not, saving is refused while a program holds it, as now. Keeping
a framebuffer for that is a later choice, not this one.

**Waking by a pin.** Before it sleeps, the port asks which pins programs
are waiting on:

```c
/* Pins (the port's numbers) some program watches, and the edges. */
uint32_t os_gpio_wake_pins(OsGpioWake* out, uint32_t max);
```

and arms them as wake sources along with the timer
(`os_get_next_wakeup_ms`). After the restore it tells the kernel which pin
woke it, `os_gpio_edge(pin, edges)` as an interrupt handler does, and the
waiting program sees the edge. A chip that can wake only by level, or only
from some pins, arms what it can; the port documents which.

### Why

- Each driver already knows what it keeps and how its port sets it up;
  the kernel cannot know it for every device. Two calls per driver keep
  `system.c` free of device code.
- By name, because the platform's registration is the one thing that is
  the same after the sleep; slots could differ with another firmware.
- Waking by a pin makes the common case (a button, a sensor's alert line)
  work without the program knowing the board slept.

### Instead

- **Release every device before sleeping** and let programs reopen them:
  every program would have to be written for it.
- **Save the port's registers**: per chip, and some state (a display's
  memory, a bus device's own registers) is not in the chip at all.

### Checks

- `wasm-rtos-tests/tests/system_devices`: six programs hold a pin they
  set and a button they wait on, a PWM output, an analog output and input,
  a counter they watch, a serial port, a keypad, an I2C and an SPI device,
  a 1-Wire bus, a display and a pulse channel, with fake ports standing in
  for the hardware. A press and a key arrive just before the save.
  - Saved after 9 ms; the button is the one wake pin, falling edge; the
    hardware cleared as at power-on; the firmware after the sleep
    registers its pins in another order. Restored: the hardware is in the
    same state as before (pin modes and levels, PWM frequency and duty,
    analog output, counter mode and count, pulse channel, serial speed and
    format, brightness), the SPI device and its chip select are the
    program's again; with the press that woke the board reported after
    the restore, every program ends with the result of the run never
    stopped, and the serial line carried the same bytes.
  - After the sleep the firmware has no counter and the PWM port refuses
    to start: those two programs end, `restore: a device it held is gone`
    and `... cannot be set up`; the pin and analog output the second held
    are free again; the other four end with the results of the run never
    stopped.
  - A display that does not keep its picture: saving is refused and the
    error says why.
  - Each rule broken on purpose, 17 of them (a pin's level, its unread
    edges, pins found by slot instead of name, the wake pin's edge, PWM
    duty, analog output, the count, the counter's poll phase, unread serial
    bytes, the serial format, unread key events, brightness, handles left
    at old ids, the chip select, a device gone ignored, the display check,
    a broken device keeping its pin): the test fails every time.
- `system_snapshot` (decision 6) and the whole suite pass unchanged, also
  under the sanitizers; every CI configuration builds with `-Werror`.
- Mac, the test's six programs with every device held: a stream of 4144
  bytes, saved in 0.25 to 0.31 ms and restored in 0.28 to 0.32 ms.
- Boards (next): an ESP32 in deep sleep woken by its timer and by a
  button, the stream on the SD card; a Black Pill reset, the stream in
  flash.

### Work, all of it

| # | What | Where | Status |
|---|---|---|---|
| 1 | `save`/`restore` in `OsDriver`; handles and waits for devices saved; a lost device ends its task | `driver.h`, `abi.c`, `system.c` | done |
| 2 | The twelve drivers above | `gpio.c`, `bus.c` (i2c, spi, 1-Wire), `uart.c`, `adc.c`, `dac.c`, `pwm.c`, `counter.c`, `pulse.c`, `input.c`, `display.c` | done |
| 3 | `os_gpio_wake_pins`; the display flag | `gpio.c`, `os.h`, `display.c` | done |
| 4 | Test: programs holding every kind of device, saved and restored, the fake ports set up the same way, results equal; a device gone ends its task; a watched pin listed and its edge delivered | `wasm-rtos-tests` | done |
| 5 | Each rule broken on purpose: the test fails | | done, 17 rules |
| 6 | Boards: an ESP32 in deep sleep, woken by its timer and by a button, the stream on the SD card; a Black Pill reset, the stream in flash; size and time measured | `esp32-test` (local) | next |
| 7 | `docs/porting.md` (the sleep loop with wake pins), this decision with the results, the roadmap | | done but the boards |

Not in this work: a framebuffer for displays that lose power; dylink.0
programs; moving one program to another device.
