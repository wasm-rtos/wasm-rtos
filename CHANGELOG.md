# Changelog

## 1.0.0 — 2026-11-13

The first release. **Guest ABI 1.x is frozen at 1.8**: a program built for
any 1.x runs on every 1.x kernel ([`docs/abi.md`](docs/abi.md),
[`docs/abi-1.8.txt`](docs/abi-1.8.txt)).

- **Programs as executables.** `.wasm` files start like executables: the
  entry point is found, every import is checked at load, processes start
  others from memory or by path, wait for them and stop them. Processes of
  one program share it, loaded once. `dylink.0` libraries are shared, with
  data per process.
- **A scheduler with preemption by fuel**: priorities, delays, suspend and
  resume, software timers, a tickless clock; queues, mutexes with priority
  inheritance, semaphores, event groups and notifications, for the host
  and for programs.
- **The guest ABI 1.8**: handles, synchronization, timers, waiting on
  several objects, processes, and devices by name: GPIO, displays, input
  (keys, rotary, touch), I2C, SPI and 1-Wire buses, analog inputs and
  outputs, PWM, serial ports, pulse trains, pulse counters and encoders.
  Newer minor versions load; calls a kernel lacks return
  `-WR_EUNSUPPORTED`; every WASI call links. `tools/check-abi.py` holds the
  kernel to the frozen list in CI.
- **WASI preview 1**, with a virtual file system (a read-only tar driver;
  files run in place where they can be mapped).
- **Drivers as modules**: a board registers the devices it has; buses can
  work in the background (DMA), only the calling program waiting.
- **Memory larger than RAM.** Programs' memory is paged to up to four page
  stores at once, by priority, which may change while programs run; a
  store leaves with its frames moved, or is lost and takes only the
  programs on it. Memory stays one block while it fits; frames come from a
  pool set aside; whole tasks swap out. Memory use is reported per task.
- **Deep sleep.** The whole system (tasks, their waits, objects, timers,
  files, the devices they hold) is saved before the RAM goes and restored
  after, with the clock going on; programs wake by a watched pin.
- **Portable snapshots** of a single task.
- Built in every configuration and run under sanitizers in CI.

Decisions: [`docs/decisions.md`](docs/decisions.md).
