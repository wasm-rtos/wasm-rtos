#!/usr/bin/env bash
# Build an Arduino sketch into a wasm-rtos program.
#
#   arduino/build.sh sketch.ino [library directory or source file ...] -o program.wasm
#
# A library directory contributes its .cpp and .c files (also under src/)
# and its headers. Needs clang++ with a WASI target, libc++ for it, and
# wasm-ld. CXX (default clang++), WASI_SYSROOT (default: the compiler's own)
# and WASI_TARGET (default wasm32-wasip1) override the defaults.
set -euo pipefail

ARDUINO_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="$(dirname "$ARDUINO_DIR")"
CXX="${CXX:-clang++}"
WASI_TARGET="${WASI_TARGET:-wasm32-wasip1}"
target=(--target="$WASI_TARGET")
if [ -n "${WASI_SYSROOT:-}" ]; then
    target+=(--sysroot="$WASI_SYSROOT")
fi
output=""
sources=()
includes=(-I"$ARDUINO_DIR/cores/wasm-rtos" -I"$ARDUINO_DIR/libraries/Wire" -I"$ARDUINO_DIR/libraries/SPI" -I"$KERNEL_DIR/include")

while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) output="$2"; shift 2 ;;
        *)
            if [ -d "$1" ]; then
                for root in "$1" "$1/src"; do
                    [ -d "$root" ] || continue
                    includes+=(-I"$root")
                    while IFS= read -r file; do sources+=("$file"); done \
                        < <(find "$root" -maxdepth 1 \( -name '*.cpp' -o -name '*.c' \) | sort)
                done
            else
                sources+=("$1")
            fi
            shift
            ;;
    esac
done
[ -n "$output" ] && [ "${#sources[@]}" -gt 0 ] || { echo "usage: $0 sketch.ino [libraries...] -o program.wasm" >&2; exit 2; }

objects=()
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
index=0
for source in "${sources[@]}" "$ARDUINO_DIR"/cores/wasm-rtos/*.cpp "$ARDUINO_DIR"/libraries/*/*.cpp; do
    object="$work/$index.o"
    index=$((index + 1))
    case "$source" in
        *.c) language=(-x c -std=gnu11) ;;
        *.ino) language=(-x c++ -std=gnu++17 -include Arduino.h) ;;
        *) language=(-x c++ -std=gnu++17) ;;
    esac
    "$CXX" "${target[@]}" -O2 -fno-exceptions -fno-rtti \
        -DARDUINO=10819 "${includes[@]}" "${language[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
"$CXX" "${target[@]}" -fno-exceptions -fno-rtti "${objects[@]}" -o "$output"
