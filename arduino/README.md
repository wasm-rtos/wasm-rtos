# Arduino core for wasm-rtos

Arduino sketches and libraries, built into wasm-rtos programs. The core is a
library that becomes part of the sketch's `.wasm`: the operating system has
no Arduino code in it, and a program that does not use the core carries
none of it.

```sh
arduino/build.sh sketch.ino path/to/SomeLibrary path/to/OtherLibrary -o sketch.wasm
```

A library directory contributes its `.cpp` and `.c` files and headers (also
under `src/`). The script needs `clang++` with a WASI target, libc++ for it
(wasi-sdk has both) and `wasm-ld`; `CXX`, `WASI_SYSROOT` and `WASI_TARGET`
override the defaults. The `.ino` is compiled as C++ with `Arduino.h`
included; declare functions before using them, as the Arduino IDE's
generated prototypes are not made.

Unmodified libraries work when they use the Arduino API rather than a
chip's registers: checked with Adafruit GFX, Adafruit BusIO, Adafruit
GC9A01A (a round TFT over SPI) and an LM75 driver over Wire.

## What maps to what

| Arduino | wasm-rtos |
|---|---|
| pin `N` | GPIO pin `"gpioN"` of the device profile; `LED_BUILTIN` is `"led"` (override `wr_arduino_pin_name` for other names) |
| `pinMode`, `digitalWrite`, `digitalRead` | `gpio` open, set, get; a pin is opened on first use and kept |
| `attachInterrupt` | `gpio` watch; handlers run between `loop()` calls and inside `delay()`, never in the middle of other code |
| `millis`, `micros`, `delay` | WASI clocks and sleep; only the sketch waits, other programs run |
| `Serial` | standard output: the system keeps it per program or sends it to a UART; input reads nothing |
| `Wire` (`"i2c0"`), `Wire1` (`"i2c1"`) | `i2c`; `endTransmission(false)` and `requestFrom` make one transaction with a repeated start |
| `SPI` (`"spi0"`) | `spi`, taking the whole bus: libraries drive chip select with `digitalWrite` |
| `String`, `Print`, `Stream`, `F()`, `PROGMEM` | plain memory: wasm has no separate flash |

## Limits

- No analog hardware yet: `analogRead` returns 0, `analogWrite` switches the
  pin on from half scale, `tone` is silent.
- The kernel's clock counts milliseconds. `delayMicroseconds` waits at least
  as long by giving other programs a turn; timing a pulse (`pulseIn`) or
  bit-banging a protocol in microseconds is not possible from a program.
- Code that writes a chip's registers (`PORTB`, ESP32 peripherals) does not
  work: the program runs on any chip.
- Libraries that send each byte with `SPI.transfer()` on boards they do not
  know (Adafruit's TFT drivers outside ESP32 and RP2040, for example) make
  one bus transaction per byte: correct, but slower than a bulk write.
- Every `Wire` address a sketch uses stays open for the program until it
  ends, so another program cannot use the same address meanwhile.
