# Experiments

What was tried and measured, kept or not, with the numbers, so it is not
tried again blind.

## Compressing frames in RAM (decision 12), 2026-11-21

Frames gathered from real program memories (CoreMark at three points of
its run, the test programs under the kernel, a C program with a heap of
structs, a table of numbers and text), 4 KiB each.

| | result |
|---|---|
| frames all zero | 31% to 87% of a memory: the VM no longer stores them |
| dense frames, WKdm | 51% of their size |
| dense frames, LZ4 (table of 2 KB) | 57% |
| dense frames, zero words only (bitmap) | 81% |
| best of WKdm and LZ4 per frame | 43%, for two compressions each |
| ESP32 240 MHz, WKdm compress / decompress | 458 / 432 µs a frame |
| ESP32, LZ4 compress / decompress | 1297 / 228 µs |
| ESP32, memcpy of a frame | 30 µs |

Kept: WKdm. Not kept: LZ4 (larger here, slower to compress, 2 KB of
table); both per frame (twice the work for 8 points).

The store as first written reached frames through byte pointers with
4-byte memcpy and checked "one word repeated" with memcmp a word: on the
ESP32 a write took 1122 µs and a read 634 µs, as the compiler, not knowing
the alignment, loaded and stored byte by byte. By words (frames are
word-aligned): 421 µs and 417 µs.

The same on a Black Pill (STM32F411, Cortex-M4 at 100 MHz): WKdm
compresses a frame in 838 µs and decompresses it in 827 µs, LZ4 in
1272 / 545 µs; the store writes a frame in 812 µs and reads it in 960 µs
(its reads copy the chain of chunks first, and memcpy of a frame takes
264 µs here). Still about twice as fast as reading a frame from an SD card
over SPI, and no wear.
