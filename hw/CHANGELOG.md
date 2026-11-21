# Changelog

## Unreleased

- The devices of wasm-rtos as a library of their own (wasm-rtos decision
  14): moved out of the kernel with the prefix `hw_`, nothing they do
  changed. What they need from an operating system is `hw_host.h`.
