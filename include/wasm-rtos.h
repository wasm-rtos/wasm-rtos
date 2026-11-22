#ifndef WASM_RTOS_H_GUEST
#define WASM_RTOS_H_GUEST

/*
 * wasm-rtos ABI 1.8 for programs written in C or C++ (wasm32).
 *
 * Every call returns an int32_t: zero or a value on success, a negative WR_E*
 * error otherwise. Handles belong to the calling program and close when it
 * ends. Strings are (pointer, length) in UTF-8; the *_cstr helpers take a
 * NUL-terminated string instead. Time, sleeping and yielding are WASI
 * (clock_gettime, nanosleep/usleep, sched_yield). See docs/abi.md.
 *
 * ABI 1.x is frozen at 1.8: what is declared here does not change within
 * 1.x. A program built with a newer 1.x header still loads on this kernel;
 * calls this kernel lacks return -WR_EUNSUPPORTED.
 */

#include <stdint.h>

/* The devices, the errors, handle-close and wait-any: the device library's part of the ABI. */
#include "../hw/include/wasm-rtos-hw.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WR_EVENT_ALL    1u  /* event-wait: every bit, not any */
#define WR_EVENT_CLEAR  2u  /* event-wait: clear the bits waited for */
#define WR_EVENT_BITS   0x00FFFFFFu

#define WR_TIMER_PERIODIC 1u

/* One string argument for spawn: matches the ABI's (pointer, length) pair. */
typedef struct wr_str
{
    const char* text;
    uint32_t length;
} wr_str;

/* sync: a name of length 0 makes a private object; a name opens the object
 * of that name if one exists, or creates it. */
WR_IMPORT("sync", "mutex-create") int32_t wr_mutex_create(const char* name, uint32_t name_length);
WR_IMPORT("sync", "mutex-lock") int32_t wr_mutex_lock(int32_t mutex, uint32_t timeout_ms);
WR_IMPORT("sync", "mutex-unlock") int32_t wr_mutex_unlock(int32_t mutex);
WR_IMPORT("sync", "semaphore-create") int32_t wr_semaphore_create(const char* name, uint32_t name_length,
                                                                  uint32_t max_count, uint32_t initial_count);
WR_IMPORT("sync", "semaphore-take") int32_t wr_semaphore_take(int32_t semaphore, uint32_t timeout_ms);
WR_IMPORT("sync", "semaphore-give") int32_t wr_semaphore_give(int32_t semaphore);
WR_IMPORT("sync", "event-create") int32_t wr_event_create(const char* name, uint32_t name_length);
WR_IMPORT("sync", "event-set") int32_t wr_event_set(int32_t event, uint32_t bits);
WR_IMPORT("sync", "event-clear") int32_t wr_event_clear(int32_t event, uint32_t bits);
WR_IMPORT("sync", "event-get") int32_t wr_event_get(int32_t event);
WR_IMPORT("sync", "event-wait") int32_t wr_event_wait(int32_t event, uint32_t bits, uint32_t flags,
                                                      uint32_t timeout_ms);
WR_IMPORT("sync", "queue-create") int32_t wr_queue_create(const char* name, uint32_t name_length,
                                                          uint32_t item_size, uint32_t capacity);
WR_IMPORT("sync", "queue-send") int32_t wr_queue_send(int32_t queue, const void* item, uint32_t timeout_ms);
WR_IMPORT("sync", "queue-receive") int32_t wr_queue_receive(int32_t queue, void* item, uint32_t timeout_ms);

/* timer: expirations are counted; wait for the handle, then read the count. */
WR_IMPORT("timer", "create") int32_t wr_timer_create(uint32_t period_ms, uint32_t flags);
WR_IMPORT("timer", "start") int32_t wr_timer_start(int32_t timer);
WR_IMPORT("timer", "stop") int32_t wr_timer_stop(int32_t timer);
WR_IMPORT("timer", "set-period") int32_t wr_timer_set_period(int32_t timer, uint32_t period_ms);
WR_IMPORT("timer", "read") int32_t wr_timer_read(int32_t timer);

/* proc: children are handles; stack_size 0 lets the child choose. */
WR_IMPORT("proc", "spawn") int32_t wr_spawn(const void* module, uint32_t module_length, const wr_str* args,
                                            uint32_t argc, uint32_t priority, uint32_t stack_size);
WR_IMPORT("proc", "spawn-path") int32_t wr_spawn_path(const char* path, uint32_t path_length, const wr_str* args,
                                                      uint32_t argc, uint32_t priority, uint32_t stack_size);
WR_IMPORT("proc", "wait") int32_t wr_wait(int32_t child, uint32_t timeout_ms);
WR_IMPORT("proc", "kill") int32_t wr_kill(int32_t child);
WR_IMPORT("proc", "self") int32_t wr_self(void);
WR_IMPORT("proc", "parent") int32_t wr_parent(void);

/* Time, sleep and yield through WASI, for programs without a C library. */
__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")))
int32_t wr_wasi_clock_time_get_(uint32_t clock, uint64_t precision, uint64_t* out_time);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("poll_oneoff")))
int32_t wr_wasi_poll_oneoff_(const void* subscriptions, void* events, uint32_t count, uint32_t* out_count);
__attribute__((import_module("wasi_snapshot_preview1"), import_name("sched_yield")))
int32_t wr_wasi_sched_yield_(void);

/* Monotonic kernel time in milliseconds. */
static inline uint64_t wr_time_ms(void)
{
    uint64_t now = 0;
    wr_wasi_clock_time_get_(1, 1000000, &now);
    return now / 1000000u;
}

/* Sleep; only the calling program waits. */
static inline void wr_sleep_ms(uint32_t milliseconds)
{
    /* WASI subscription_t (48 bytes, relative clock) and event_t (32 bytes). */
    uint64_t subscription[6] = { 0, 0, 1, (uint64_t)milliseconds * 1000000u, 0, 0 };
    uint64_t event[4];
    uint32_t count = 0;
    wr_wasi_poll_oneoff_(subscription, event, 1, &count);
}

static inline void wr_yield(void)
{
    wr_wasi_sched_yield_();
}

static inline wr_str wr_cstr(const char* text)
{
    wr_str value = { text, wr_strlen(text) };
    return value;
}

#define wr_mutex_open(name) wr_mutex_create((name), wr_strlen(name))
#define wr_semaphore_open(name, max, initial) wr_semaphore_create((name), wr_strlen(name), (max), (initial))
#define wr_event_open(name) wr_event_create((name), wr_strlen(name))
#define wr_queue_open(name, item_size, capacity) wr_queue_create((name), wr_strlen(name), (item_size), (capacity))

/* DS18B20 and alike (family 0x28): start a 12-bit conversion and wait the
 * 750 ms it takes, powering parasite-powered sensors through the strong
 * pull-up where the bus has one. */
#define WR_DS18B20_FAMILY 0x28u
static inline int32_t wr_ds18b20_convert(int32_t device)
{
    static const uint8_t convert = 0x44;
    wr_onewire_op ops[2] = { { WR_ONEWIRE_WRITE, (void*)&convert, 1u }, { WR_ONEWIRE_POWER, 0, 750u } };
    int32_t result = wr_onewire_transaction(device, ops, 2u);
    if (result != -WR_EUNSUPPORTED)
        return result;
    result = wr_onewire_transaction(device, ops, 1u);
    if (result == 0)
        wr_sleep_ms(750u);
    return result;
}

#define wr_spawn_path_cstr(path, args, argc, priority, stack) \
    wr_spawn_path((path), wr_strlen(path), (args), (argc), (priority), (stack))

/*
 * Program metadata, the "wasm-rtos.app" custom section: records of a key
 * byte, a LEB128 length and the value. Use each macro once, at file scope.
 */
#define WR_APP_SECTION_ ".section \".custom_section.wasm-rtos.app\",\"\",@\n"
#define WR_APP_U32_(key, value) __asm__(WR_APP_SECTION_ ".int8 " #key "\n.uleb128 4\n.int32 " #value "\n.text\n")
#define WR_APP_TEXT_(key, text) \
    __asm__(WR_APP_SECTION_ ".int8 " #key "\n.uleb128 2f-1f\n1:\n.ascii \"" text "\"\n2:\n.text\n")

/* The ABI version the program was written for (major << 16 | minor). */
#define WR_APP_ABI_VERSION() WR_APP_U32_(1, 65544)
/* Interpreter stack in bytes, unless whoever starts the program picks one. */
#define WR_APP_STACK_SIZE(bytes) WR_APP_U32_(2, bytes)
/* Name shown for the program. */
#define WR_APP_NAME(text) WR_APP_TEXT_(3, text)

#ifdef __cplusplus
}
#endif

#endif
