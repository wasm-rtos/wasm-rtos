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

**Decided and built (2026-11-13); checked on an ESP32 and a Black Pill.**

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
- ESP32 (no external RAM), two programs from one module: one sleeps six
  times for 4 s and checks its 16 KiB of memory each time, the other
  waits for presses of GPIO33 (a wire to GND). Whenever the kernel had
  nothing to do for over a second the port saved the system to the SD
  card, armed the wake pins `os_gpio_wake_pins` gave and the timer, and
  went into deep sleep; at start it restored. In one run of 8 starts:
  6 ticks with the memory intact 6 times, the board woken once by the
  wire and the press delivered, the ticks at 4207, 8416, 12625, 16830,
  21038 and 25244 ms on the programs' clock, also the one after the
  press (the board then slept 3229 ms, the time left). Stream 17523 to
  17894 bytes; saved to the card in 106 to 240 ms, restored in 61 to
  73 ms, about 200 ms of the chip's own start before that.
- On that board, two programs with pages of 64 KiB did not fit: the
  second needed another 64 KiB in one piece, and the heap's largest was
  110580 bytes with the first's in it. Built with pages of one byte
  (`-Wl,--page-size=1`) both fit.
- The ESP32 wakes by level, not by edge: a wire still touching GND would
  wake it at once for a press already seen. The port does not sleep while
  a wake pin is at the level that wakes it (`docs/porting.md`).
- Black Pill, the same two programs, its KEY button (PA0) as the pin:
  when the kernel had nothing to do for over a second the port erased the
  last flash sector (128 KiB), saved the system into it, waited as if
  asleep until the time was up or KEY was pressed, and reset the chip,
  losing the RAM; at start it restored. 9 starts: 6 ticks with the memory
  intact 6 times, at 4004, 8009, 12014, 16019, 20025 and 24029 ms, two
  presses that woke the board delivered (it then slept 2796 and 3692 ms,
  the time left). Stream 17524 to 17896 bytes, written in 79 to 84 ms
  after about 1 s of erasing the sector (the chip's clock stands still
  while flash erases; measured on the host), restored in 40 ms.

### Work, all of it

| # | What | Where | Status |
|---|---|---|---|
| 1 | `save`/`restore` in `OsDriver`; handles and waits for devices saved; a lost device ends its task | `driver.h`, `abi.c`, `system.c` | done |
| 2 | The twelve drivers above | `gpio.c`, `bus.c` (i2c, spi, 1-Wire), `uart.c`, `adc.c`, `dac.c`, `pwm.c`, `counter.c`, `pulse.c`, `input.c`, `display.c` | done |
| 3 | `os_gpio_wake_pins`; the display flag | `gpio.c`, `os.h`, `display.c` | done |
| 4 | Test: programs holding every kind of device, saved and restored, the fake ports set up the same way, results equal; a device gone ends its task; a watched pin listed and its edge delivered | `wasm-rtos-tests` | done |
| 5 | Each rule broken on purpose: the test fails | | done, 17 rules |
| 6 | Boards: an ESP32 in deep sleep, woken by its timer and by a button, the stream on the SD card; a Black Pill reset, the stream in flash; size and time measured | `esp32-test` (local) | done |
| 7 | `docs/porting.md` (the sleep loop with wake pins), this decision with the results, the roadmap | | done |

Not in this work: a framebuffer for displays that lose power; dylink.0
programs; moving one program to another device.

## 8. ABI 1.x is frozen at 1.8

**Decided and built (2026-11-13).**

### Problem

Version 1.0 promises that a program built for ABI 1.x keeps working on
every 1.x kernel (`docs/abi.md`). The promise is only as good as what holds
it: today nothing stops a change from renaming a call, reordering its
arguments or changing what it returns. And a review of the 76 calls
before freezing them found four behaviours that would be hard to live with
for a whole major version.

The review: every call is the same in `wit/kernel.wit`,
`include/wasm-rtos.h` and the kernel's tables (name, arguments, the minor
version that added it); unknown flags give `-WR_EINVAL` and unknown
properties `-WR_EUNSUPPORTED` everywhere; names are checked as UTF-8. What
is not right:

1. **A program built with a newer header does not load on an older
   kernel.** The header names every import `@1.8.0`; a 1.9 header will name
   them all `@1.9.0`, and a 1.8 kernel refuses those names, so a program
   that uses only 1.0 calls stops loading on every device not yet updated,
   just because it was rebuilt.
2. **Unknown WASI calls fail the load.** A program importing a WASI call
   the kernel does not answer (a socket call, or a file call on a build
   without files) does not load at all, against the rule that what a build
   leaves out returns an error instead.
3. **A program can start a child above its own priority** (`proc.spawn`
   passes the priority through), so any program can take the processor
   from everything else.
4. **Gaps in the text:** `wait.any` lists only kernel objects, not the
   devices that can be waited for; `-WR_EBUSY` also means "you already hold
   this mutex"; the rule on unknown flags is followed but not written; the
   WASI calls answered are not listed; nothing says what a program sees
   when the device sleeps (decisions 6 and 7).

### Decision

**What is frozen.** For all of 1.x: every call of 1.8 with its import
name, arguments, result and meaning; the error codes; the layouts in
memory (ops, events, search state, items); property numbers and flag bits;
the metadata keys; the WASI calls the kernel answers. A minor version may
only add: calls, properties, flags, error codes, metadata keys, WASI calls
answered. Anything else is 2.0.

**The four fixes, before freezing:**

1. **Newer minor versions load.** The kernel accepts
   `wasm-rtos:kernel/<interface>@1.N.0` for any N. A call it has answers
   whatever minor the program names; a call it does not have (added after
   its version) returns `-WR_EUNSUPPORTED`, as an absent device does. A
   program checks, as it already does for devices. Metadata stating a newer
   minor still loads; only another major fails.
2. **Every WASI preview 1 call links.** The ones the kernel answers are
   listed in `docs/abi.md`; every other one, and the file calls on a build
   without files, returns `ENOSYS` (52).
3. **A child's priority is at most its parent's**: a higher one is lowered
   to the caller's base priority. The host still creates tasks at any
   priority.
4. **The text**: `wait.any` lists every handle that can be waited for;
   `-WR_EBUSY` gets its second meaning (`mutex-lock` on a mutex the program
   holds); the rules say unknown flags are `-WR_EINVAL` and unknown
   properties `-WR_EUNSUPPORTED`, so a later flag or property is detected
   on an older kernel; the WASI list; a section "When the device sleeps":
   the clock goes on, what each device keeps and what it loses.

**What holds the promise.** `tools/check-abi.py` in the kernel, run by its
CI: the three descriptions agree with each other, and every call of the
frozen list `docs/abi-1.8.txt` (interface, name, arguments, minor) is still
there unchanged. A later minor version appends to its own list; removing or
changing a line fails CI.

### Why

- A promise that CI checks survives the next refactoring; one in a
  document does not.
- Fix 1 decides how the ecosystem upgrades: the header can move to 1.9
  without making every rebuilt program fail on devices that have 1.8.
  Programs already ask before using a device; asking before using a new
  call is the same.
- Fix 2 is the existing rule (what a build leaves out returns an error)
  applied to WASI, where it was missing.
- Fix 3: a program must not be able to take more than its creator gave
  it; after the freeze a change here would break programs that rely on it.

### Instead

- **Newer minor fails to load, with a clear message**: simple, but every
  header update would split devices into "has the newest kernel" and "runs
  nothing rebuilt".
- **Freeze without a check**: the next change to a driver's table could
  break programs silently.
- **Recursive mutexes** instead of documenting `-WR_EBUSY`: more state in
  every mutex, and C libraries that need recursion build it themselves.

### Work, all of it

| # | What | Where | Status |
|---|---|---|---|
| 1 | Newer minor versions load; calls the kernel does not have return `-WR_EUNSUPPORTED` | `abi.c` | done |
| 2 | Every WASI preview 1 call links; the rest return `ENOSYS` | `abi.c`, `fs.c` | done |
| 3 | A child's priority at most its parent's | `abi.c` / `task.c` | done |
| 4 | `docs/abi.md`: what is frozen, the rules on flags and properties, `wait.any`, `-WR_EBUSY`, the WASI list, when the device sleeps; a dead `#if` in `abi.c` | `docs/abi.md`, `abi.c` | done |
| 5 | `tools/check-abi.py`, `docs/abi-1.8.txt`, a CI step | kernel | done |
| 6 | Test `abi_freeze`: a program importing all 76 calls loads on the default build and on the one without files and processes; imports named `@1.0.0` and `@1.9.0` load, a call 1.8 does not have returns `-WR_EUNSUPPORTED`; an unanswered WASI call returns `ENOSYS`; a child asked above its parent runs at the parent's priority | `wasm-rtos-tests` | done |
| 7 | Each rule broken on purpose: the test or the check fails | | done |
| 8 | Roadmap, README | `.github`, kernel | with the merge |

Not in this work: boards (nothing here depends on one); new calls (they
come in later minor versions).

## 9. Processes of one program share it

**Decided and built (2026-11-13).**

### Problem

Every process carries its own copy of its program: the module's bytes
(read from the file when the file system cannot map it, as on an SD card)
and the parsed module with its decoded code. A program that starts itself
as a child, or a shell that starts the same tool twice, pays for the
program twice. On an ESP32 without PSRAM a 40 KB program written with
`printf` takes about 70 KB of heap to start; a second process of it does
not fit next to the first (measured: the parent starts, its child gets
`-WR_ENOMEM`), although nothing in the second copy differs from the first.

### Decision

**A program is loaded once and shared.** The kernel keeps the programs
its processes run: the bytes (a mapping of the file, or the kernel's
copy) and the parsed module. A process holds a reference to its program; the program goes when
the last process of it is deleted.

- **The same bytes, not the same name.** A program is found by its size
  and its contents, compared byte by byte with each loaded program of
  that size. `spawn-path` compares the file as it reads it, in pieces,
  before anything is copied: a file of the same contents shares the loaded
  program, a file that changed since is loaded as a new one (the
  processes of the old one keep running it). `spawn` from bytes compares
  them where they are, and copies them only when no loaded program has
  them. A process restored after a sleep finds its program the same way.
- **The module is parsed once**, and its decoded code is the code cache's
  as before, shared by every process of the program. It is freed while no
  process of the program has an instance (all of them swapped out or
  ended), and parsed again when one comes back, as now.
- **What stays per process**: its instance, memory, stack, handles,
  arguments, everything a process owns today.
- **Not shared**: programs the host creates from its own bytes
  (`os_task_create`: the host owns those bytes), and `dylink.0` programs
  (each process links its libraries).
- `os_task_get_memory_usage` still reports the program's image and code
  for each of its processes; new `os_get_program_count` says how many
  programs are loaded.

### Why

- A program's bytes and parsed module never change while it runs; one
  copy serves every process of it, as an operating system shares a
  program's text between processes.
- Comparing the contents rather than trusting the path keeps the rule
  that a process runs the program it was started with: replacing the file
  while it runs changes nothing for it. Comparing bytes rather than a hash
  cannot mistake two programs, and costs the same: the file is read once
  either way.
- On the boards this is the difference between one and two processes of a
  `printf` program; on every board it makes a second start cheap.

### Instead

- **Share by path**: cheaper to look up, but a file replaced on the card
  would be mistaken for the running program.
- **Keep copies, make programs smaller**: every C program that prints
  would pay twice, and the board would run one of them.
- **Share only the bytes**: the parsed module and its code are most of
  the cost.

### Work, all of it

| # | What | Where | Status |
|---|---|---|---|
| 1 | Loaded programs: find by size and hash, references, the module parsed while a process has an instance | new `program.c`, `os_kernel.h` | done |
| 2 | `spawn-path` compares the file in pieces before copying; `spawn` compares the bytes before copying | `task.c`, `fs.c` | done |
| 3 | Processes use their program's bytes and module; swapping out and snapshots release and take the module; deleting releases the program | `os.c`, `task.c`, `snapshot.c` | done |
| 4 | A restored process finds its program by size and contents | `system.c` | done |
| 5 | `os_get_program_count`; docs (`os.h`, README) | `os.h`, README | done |
| 6 | Test `program_share`: two processes of one file share one program; a changed file is a second program and the first runs on; spawn from bytes shares; all swapped out frees the module and it comes back; restore shares; the count goes to 0; each rule broken on purpose fails the test | `wasm-rtos-tests` | done |
| 7 | ESP32: the tour with its child from the SD card; heap before and after, with and without sharing | board tests (local) | done |

Measured on the ESP32 (no PSRAM, the tour example from the SD card, 40 KB
with `printf`): without sharing its child got `-WR_ENOMEM` with 12 or 16
frames of RAM for programs; with it the tour and its child run with 16.

## 10. Several page stores, and one going away

**Decided and built (2026-11-13).**

### Problem

The kernel pages programs' memory to one store (`os_page_store_set`). A
board often has more than one place for frames: an ESP32 with a card and a
computer on its cable, a chip with PSRAM and a card. With one store the
port must pick, and a board whose card is full cannot use the computer.
And nothing says what happens when a store goes: today a frame that cannot
be read makes its program trap with "exhaustion: ... could not move a
frame", which reads like running out of memory.

### Decision

**Stores are added with a priority**, as swap areas are on Linux
(`swapon -p`):

```c
OsStatus os_page_store_add(const OsPageStore* store, uint32_t priority);
OsStatus os_page_store_priority(const OsPageStore* store, uint32_t priority);
OsStatus os_page_store_remove(const OsPageStore* store);   /* gracefully */
OsStatus os_page_store_lost(const OsPageStore* store);     /* it is gone */
```

`os_page_store_set` stays: it sets the RAM budget (the store's
`ram_frames` and `ram`) and makes that store the only one; stores added
after it give slots only (their RAM fields must be 0). Up to
`OS_PAGE_MAX_STORES` (4).

- **One space of slots.** Each store's slots follow the previous one's:
  the interpreter still sees a frame's slot as one number, and the kernel
  knows which store it is on. The promise that every frame has a slot
  counts the slots of all stores.
- **Where a dirty frame goes:** the store with the highest priority that
  has a free slot; between stores of equal priority, one after the other
  (their writes then overlap when they write in the background). Each
  store keeps writing like a log from its own head.
- **Priorities change while it runs** (`os_page_store_priority`): frames
  written from then on follow the new order; frames already stored stay
  where they are. The port gives the defaults; the person changes them
  from the page (below) or a script.
- **Removing a store** (`os_page_store_remove`): every frame on it comes
  back into RAM and is written to another store when it leaves again, and
  every swapped-out task's state on it is copied to another; then the store
  is forgotten. Refused (`OS_STATUS_BUSY`) when the other stores lack the
  slots, and nothing is moved.
- **A store that is gone** (`os_page_store_lost`, when the port knows: a
  card pulled out, a cable unplugged): its frames are lost. Every task with
  a frame or its state on it ends now, as a trap, with the error
  `page store gone`; every other task goes on; its slots are forgotten.
- **A read that fails** ends the task that needed the frame with
  `page store: a frame could not be read`, not an "exhaustion" (which made
  the kernel drop the code cache and try again, for nothing).
- `os_page_get_stats` counts all stores; `os_page_get_store_stats` gives
  one store's slots, used slots, writes and reads.

**Where the person sets them.** The board port lists its stores to the
page over the cable (decision 3 of the site) and takes a new priority for
one; the page's Board section shows each store (name, priority, slots
used, writes, reads) with its priority to edit. The script does the same
(`stores`, `priority <store> <n>`). On the page, a chip of one's own may
have several stores with priorities, so a board with, say, PSRAM and a
card can be tried before it exists.

The interpreter gains two calls the kernel needs to move frames:
`wm_memory_frame_slot` (a frame's slot) and `wm_memory_frame_unstore`
(forget a frame's slot and count it dirty, so it is written to a new one
when it leaves RAM).

### Why

- A board's places for frames differ in speed and lifetime; the fastest
  that has room should take the next frame, the others only when it is
  full, and a store that comes and goes (a computer on a cable) must not
  take the programs that never used it.
- One space of slots keeps the interpreter unchanged in how it pages;
  only the kernel knows there are several stores.
- Linux's rules (priorities, equal ones taking turns, `swapoff` bringing
  pages back) are known and have held for decades.

### Instead

- **One store made of several by the port**: every port would write the
  same code, and the kernel could not tell which programs a lost store
  takes with it.
- **Copy every frame to two stores** (a mirror): a lost store then costs
  nothing, but every write is twice, on chips where writes are what is
  slow.
- **A lost store ends every program**: simpler, and wrong for a computer
  on a cable that only some programs used.

### Work, all of it

| # | What | Where | Status |
|---|---|---|---|
| 1 | `wm_memory_frame_slot`, `wm_memory_frame_unstore`; a test | `wasm-rtos-vm` | done |
| 2 | Stores in a list with priorities and slot ranges; where a frame goes; per-store logs, background work and statistics | `page.c` | done |
| 3 | `os_page_store_add`, `os_page_store_priority`, `os_page_store_remove` (frames and swapped states moved, or BUSY), `os_page_store_lost` (owners end), `os_page_get_store_stats`; the read-failure error | `page.c`, `snapshot.c`, `os.h` | done |
| 4 | Test `page_stores`: a full store spills to the next; equal priorities take turns; removing moves frames and states with the bytes intact, and is refused without room; a lost store ends its owners and only them; a save and restore with two stores; each rule broken on purpose | `wasm-rtos-tests` | done |
| 5 | ESP32 port: the card first, the computer on the cable after it; the cable gone is a lost store; STORES and PRIORITY commands; `board.py stores`, `priority`; measured: a program larger than the card's share | board tests (local) | done |
| 6 | The page: the board's stores in the Board section with priorities to edit; a chip of one's own with several stores and priorities | `wasm-rtos.github.io` | done |
| 7 | README, `os.h`; the roadmap | kernel, `.github` | done |

Checked (2026-11-13). Test `page_stores`: 32 checks; ten ways of breaking
the rules (priority ignored, no turns, no room check, swapped states or
frames left behind, a leaving store still written to, a lost store ending
nobody or everybody, a failed read reported as exhaustion, an image not
moved) each fail it. On the ESP32 with its card (priority 2) and the
computer on the cable (priority 1), a program walking 256 KB:

| | Result |
|---|---|
| card above the computer | 448 frames out, 396 in, all on the card; 21 s |
| the computer raised to 3 | the same frames on the computer; 68 s over the cable |
| one program on each, then the cable silent | the computer's ends with `page store gone`, the card's runs to its end |
| the cable back | the computer's store returns, empty, at the priority last set |
