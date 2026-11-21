#ifndef WASM_RTOS_HW_HOST_H
#define WASM_RTOS_HW_HOST_H

/*
 * What the devices need from the operating system, and nothing else. The
 * wasm-rtos kernel implements it (its hw_host.c); another scheduler that
 * implements these functions gets every device and port as they are.
 *
 * Six groups: the calling program, its memory, waiting, time, handles
 * (hw_driver.h) and saving the whole system.
 */

#include <stdint.h>
#include <string.h>

#include "hw.h"
#include "wm.h"

/* ---- Calls from a program ------------------------------------------------ */

/*
 * A device's call is an import of the interpreter (wm.h). HW_HOST_FUNCTION
 * declares one: arguments in 32-bit slots, i64 and f64 taking two, the
 * result in the first. Inside it, HW_HOST_RETURNS(type) comes first and
 * names the result slot raw_return; HW_HOST_ARG reads the next argument (a
 * pointer into the program's memory arrives as a uint32_t address).
 * HW_HOST_RETURN sets the result and returns.
 */

#define HW_HOST_FUNCTION(NAME) \
    WmResult NAME(WmRuntime* runtime, void* hw_host_user, uint32_t* hw_host_slot, WmMemory* hw_host_memory)
#define HW_HOST_RETURNS(TYPE) TYPE* const raw_return = (TYPE*)(void*)hw_host_slot
#define HW_HOST_ARG(TYPE, NAME) \
    TYPE NAME; \
    memcpy(&NAME, hw_host_slot, sizeof(TYPE)); \
    hw_host_slot += (sizeof(TYPE) + 3U) / 4U
#define HW_HOST_RETURN(VALUE) \
    do \
    { \
        *raw_return = (VALUE); \
        return NULL; \
    } while (0)
#define HW_HOST_CHECK_ADDR(ADDRESS, LENGTH) \
    do \
    { \
        if (!hw_host_memory_in_bounds(hw_host_memory, (uint32_t)(ADDRESS), (uint64_t)(LENGTH))) \
            return hw_host_out_of_bounds; \
    } while (0)
#define HW_HOST_LOAD32(ADDRESS) hw_host_load32(runtime, hw_host_memory, (uint32_t)(ADDRESS))
#define HW_HOST_LOAD64(ADDRESS) hw_host_load64(runtime, hw_host_memory, (uint32_t)(ADDRESS))
#define HW_HOST_STORE8(ADDRESS, VALUE) hw_host_store8(runtime, hw_host_memory, (uint32_t)(ADDRESS), (uint8_t)(VALUE))
#define HW_HOST_STORE32(ADDRESS, VALUE) hw_host_store32(runtime, hw_host_memory, (uint32_t)(ADDRESS), (uint32_t)(VALUE))
#define HW_HOST_STORE64(ADDRESS, VALUE) hw_host_store64(runtime, hw_host_memory, (uint32_t)(ADDRESS), (uint64_t)(VALUE))
#define HW_HOST_COPY_FROM(ADDRESS, OUT, LENGTH) \
    hw_host_fault(runtime, wm_memory_read(hw_host_memory, (uint32_t)(ADDRESS), (OUT), (uint32_t)(LENGTH)))
#define HW_HOST_COPY_TO(ADDRESS, DATA, LENGTH) \
    hw_host_fault(runtime, wm_memory_write(hw_host_memory, (uint32_t)(ADDRESS), (DATA), (uint32_t)(LENGTH)))
/* Little-endian values in kernel memory (a record copied out of the program). */
#define HW_HOST_READ32(POINTER) hw_host_read32(POINTER)
#define HW_HOST_READ64(POINTER) hw_host_read64(POINTER)
#define HW_HOST_WRITE8(POINTER, VALUE) (*(uint8_t*)(POINTER) = (uint8_t)(VALUE))
#define HW_HOST_WRITE32(POINTER, VALUE) hw_host_write32((POINTER), (uint32_t)(VALUE))
#define HW_HOST_WRITE64(POINTER, VALUE) hw_host_write64((POINTER), (uint64_t)(VALUE))

/* Whether length bytes from address lie in the memory. */
static inline int hw_host_memory_in_bounds(const WmMemory* memory, uint32_t address, uint64_t length)
{
    uint32_t size = 0U;

    if (memory == NULL)
    {
        return 0;
    }
    (void)wm_memory_data(memory, &size);
    return (uint64_t)address + length <= size;
}
/* An access in bounds that failed (paged memory): the call traps when the host function returns. */
static inline void hw_host_fault(WmRuntime* runtime, WmResult result)
{
    if (result != NULL)
    {
        (void)wm_trap(runtime, result);
    }
}
static inline uint32_t hw_host_load32(WmRuntime* runtime, WmMemory* memory, uint32_t address)
{
    uint32_t value = 0U;
    hw_host_fault(runtime, wm_memory_read(memory, address, &value, sizeof(value)));
    return value;
}
static inline uint64_t hw_host_load64(WmRuntime* runtime, WmMemory* memory, uint32_t address)
{
    uint64_t value = 0U;
    hw_host_fault(runtime, wm_memory_read(memory, address, &value, sizeof(value)));
    return value;
}
static inline void hw_host_store8(WmRuntime* runtime, WmMemory* memory, uint32_t address, uint8_t value)
{
    hw_host_fault(runtime, wm_memory_write(memory, address, &value, sizeof(value)));
}
static inline void hw_host_store32(WmRuntime* runtime, WmMemory* memory, uint32_t address, uint32_t value)
{
    hw_host_fault(runtime, wm_memory_write(memory, address, &value, sizeof(value)));
}
static inline void hw_host_store64(WmRuntime* runtime, WmMemory* memory, uint32_t address, uint64_t value)
{
    hw_host_fault(runtime, wm_memory_write(memory, address, &value, sizeof(value)));
}
static inline uint32_t hw_host_read32(const void* pointer)
{
    uint32_t value;
    memcpy(&value, pointer, sizeof(value));
    return value;
}
static inline uint64_t hw_host_read64(const void* pointer)
{
    uint64_t value;
    memcpy(&value, pointer, sizeof(value));
    return value;
}
static inline void hw_host_write32(void* pointer, uint32_t value) { memcpy(pointer, &value, sizeof(value)); }
static inline void hw_host_write64(void* pointer, uint64_t value) { memcpy(pointer, &value, sizeof(value)); }
/* The trap HW_HOST_CHECK_ADDR returns. */
extern const char hw_host_out_of_bounds[];

/* The id of the program (task) whose call runs now, never 0 inside a call. */
uint32_t hw_host_task_id(void);

/*
 * Bracket a call that may block. hw_host_guest_begin records where the
 * import's i32 result lives; hw_host_guest_end yields when the call started
 * a wait and makes the wakeup deliver the result in ABI form (the value on
 * success, a negative ABI error otherwise).
 */
HwStatus hw_host_guest_begin(uint32_t* raw_return);
WmResult hw_host_guest_end(void);

/* ---- The program's memory ------------------------------------------------ */

/*
 * The calling program's memory is reached by address, as it may be paged:
 * ranges are checked (HW_STATUS_OUT_OF_BOUNDS when outside it, with
 * addresses near 4 GiB) and bytes copied in or out.
 */
HwStatus hw_host_guest_check(uint32_t address, uint32_t length);
HwStatus hw_host_guest_read(uint32_t address, void* out, uint32_t length);
HwStatus hw_host_guest_write(uint32_t address, const void* data, uint32_t length);

/*
 * A range of the program's memory a port works on in place. data points
 * into the memory when the bytes lie in one piece; otherwise at a copy of
 * them, which hw_host_buffer_close writes back when access has
 * HW_GUEST_OUT (a copy starts as the program's bytes, so what the port
 * left alone stays). A buffer is open while the port works and is closed
 * or dropped once. data is NULL for length 0.
 *
 * A pointer into a paged memory keeps its frame in RAM (pinned) until the
 * buffer is closed, so a page store never takes it away under the port.
 *
 * A port working in the background (the program waits meanwhile and keeps
 * its memory) needs the buffers past the call: hw_host_buffer_keep moves a
 * copy, or a pinned frame, into a list, and hw_host_kept_close writes the
 * copies back and lets the frames go when the transfer ends; after the
 * program has ended it only lets the copies go.
 */
#define HW_GUEST_IN 1U  /* the port reads it */
#define HW_GUEST_OUT 2U /* the port writes it */
typedef struct HwGuestBuffer
{
    uint8_t* data;
    uint32_t address;
    uint32_t length;
    uint32_t task_id;
    uint8_t access;
    uint8_t copied;
    uint8_t pinned;
} HwGuestBuffer;
typedef struct HwGuestKept HwGuestKept;
HwStatus hw_host_buffer_open(HwGuestBuffer* buffer, uint32_t address, uint32_t length, uint8_t access);
HwStatus hw_host_buffer_close(HwGuestBuffer* buffer);
/* Let a buffer go without writing a copy back. */
void hw_host_buffer_drop(HwGuestBuffer* buffer);
void hw_host_buffer_keep(HwGuestBuffer* buffer, HwGuestKept** list);
HwStatus hw_host_kept_close(HwGuestKept** list);

/* ---- Waiting ------------------------------------------------------------- */

/*
 * Waits for something a device delivers, named by a key: the calling
 * program blocks until hw_host_signal_wake(key) or the timeout. It starts
 * in a call between hw_host_guest_begin and hw_host_guest_end.
 */
HwStatus hw_host_signal_wait(uint32_t key, uint32_t timeout_ms);
void hw_host_signal_wake(uint32_t key);
/* Programs in wait.any whose devices became ready: look again. */
void hw_host_wait_any_recheck(void);

/* The calling program waits for its background transfer. */
HwStatus hw_host_transfer_wait_begin(void);
/* The transfer of task_id ended with status and value; wakes it. */
void hw_host_transfer_wait_end(uint32_t task_id, HwStatus status, uint32_t value);

/* ---- Time ---------------------------------------------------------------- */

/* Milliseconds, without the 32-bit wrap, and with it. */
uint64_t hw_host_time_ms64(void);
uint32_t hw_host_tick_ms(void);

/* ---- Saving the whole system ---------------------------------------------- */

/*
 * The stream a driver's save writes and its restore reads: numbers
 * little-endian. The first failure sticks: HW_STATUS_INVALID_ARGUMENT marks
 * a stream that does not agree with itself.
 */
typedef struct HwSystemStream HwSystemStream;
void hw_host_system_put32(HwSystemStream* stream, uint32_t value);
void hw_host_system_put64(HwSystemStream* stream, uint64_t value);
/* Bytes as they are; their count is the reader's to know. */
void hw_host_system_put(HwSystemStream* stream, const void* data, uint32_t size);
void hw_host_system_get(HwSystemStream* stream, void* data, uint32_t size);
void hw_host_system_skip(HwSystemStream* stream, uint32_t size);
/* A length, then the bytes. */
void hw_host_system_put_bytes(HwSystemStream* stream, const void* data, uint32_t size);
uint32_t hw_host_system_get32(HwSystemStream* stream);
uint64_t hw_host_system_get64(HwSystemStream* stream);
/* A number at most max; a larger one breaks the stream. */
uint32_t hw_host_system_get_max(HwSystemStream* stream, uint32_t max);
/* Bytes put with hw_host_system_put_bytes into out (at most max; more breaks the stream); their count. */
uint32_t hw_host_system_get_bytes(HwSystemStream* stream, void* out, uint32_t max);
void hw_host_system_fail(HwSystemStream* stream, HwStatus status);
/* A driver cannot save now: why, for the last error. */
void hw_host_system_refuse(HwSystemStream* stream, HwStatus status, const char* why);
uint8_t hw_host_system_ok(const HwSystemStream* stream);

#endif
