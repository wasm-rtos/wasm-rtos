# Lazy WebAssembly libraries

wasm-rtos can satisfy normal Core WebAssembly function imports from separately
stored WebAssembly modules. An application and a library therefore use the
standard import/export mechanism; the lazy loading and cache policy belong to
the wasm-rtos embedder.

Application:

```c
__attribute__((import_module("math"), import_name("add"))) extern int
math_add(int left, int right);
```

Library:

```c
__attribute__((export_name("add")))
int add(int left, int right)
{
    return left + right;
}
```

Build the application and library as independent `.wasm` files. Register the
library before creating tasks which import it:

```c
static OsStatus acquire(
    void* context,
    const uint8_t** out_bytes,
    uint32_t* out_size
);

static void release(
    void* context,
    const uint8_t* bytes,
    uint32_t size
);

OsWasmLibrarySource source =
{
    .acquire = acquire,
    .release = release,
    .context = storage_context,
    .stack_size = 16U * 1024U,
    .resident_size_bytes = 0U
};
OsWasmLibraryHandle math = NULL;

os_wasm_library_register(
    &math,
    "math",
    &source,
    OS_WASM_LIBRARY_EVICTABLE
);
```

`acquire` may read from the platform's normal storage layer: flash or an SD
card on a microcontroller, a file on a native host, or storage prepared by the
browser port. The returned bytes only need to remain valid until the matching
`release` call. wasm-rtos briefly acquires them during registration to validate
the module and copy its exported signatures, then releases them. It acquires
them again only when a task first calls one of that library's imports.

Each resident library instance belongs to one task. Its globals and linear
memory persist between calls while it remains resident. Eviction destroys its
wasm3 runtime, compiled code, stack, globals, and linear memory, then calls
`release` for the source bytes. A later call creates a fresh instance.

Library calls run with the same fuel slices as the owning task. A long-running
library function or start function therefore cannot monopolize the scheduler.
Every library call introduces at least one scheduling boundary because the
application and library use separate wasm3 runtimes.

## Cache

The cache is unlimited after `os_init()` unless a port sets a byte limit:

```c
os_wasm_library_cache_set_limit(256U * 1024U);
```

Evictable instances use global least-recently-used replacement. Active calls
and `OS_WASM_LIBRARY_PINNED` instances are never selected. A load fails with
`OS_STATUS_WASM_CACHE_FULL` when enough space cannot be made.

If `resident_size_bytes` is nonzero, wasm-rtos uses it as the accounting cost
of each task-local instance. If it is zero, wasm-rtos estimates the cost from
the source bytes, stack, linear memory, runtime structures, and wasm3 code
pages, refreshing the estimate as execution allocates memory or code.

Use the cache inspection functions in `os.h` to display resident count and
size. `os_wasm_library_evict()` explicitly unloads an inactive, evictable
instance. Task exit/deletion and `os_shutdown()` unload all affected instances,
including pinned ones.

## Version 1 ABI

Version 1 supports function parameters and results made only from `i32`, `i64`,
`f32`, and `f64`, including multiple results supported by wasm3. The registered
metadata and the module loaded later must have identical signatures.

Library imports, imported memory, and imported globals are rejected. A library
may define its own memory, but an application pointer is only an `i32` number
inside the library and does not refer to the application's memory. Shared
memory, strings, buffers, inter-library dependencies, and swapping persistent
library state require a later explicit ABI.

Because a snapshot currently contains only the application's wasm3 runtime,
snapshot operations return `OS_STATUS_BUSY` while the task has a resident or
in-flight library.
