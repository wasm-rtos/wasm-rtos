#ifndef WASM_RTOS_HAL_H
#define WASM_RTOS_HAL_H

#include <stdint.h>

#if defined(WASM_RTOS_BROWSER)
#define WASM_IMPORT(module_name, imported_name) \
    extern __attribute__((import_module(module_name), import_name(imported_name)))
#else
#define WASM_IMPORT(module_name, import_name)
#endif

WASM_IMPORT("env", "hal_init") void hal_init(void);
WASM_IMPORT("env", "hal_shutdown") void hal_shutdown(void);
WASM_IMPORT("env", "hal_get_time_ms") uint32_t hal_get_time_ms(void);
WASM_IMPORT("env", "hal_panic") void hal_panic(const char* message);

#endif
