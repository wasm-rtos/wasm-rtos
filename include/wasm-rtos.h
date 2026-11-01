#ifndef WASM_RTOS_H_GUEST
#define WASM_RTOS_H_GUEST

/*
 * wasm-rtos ABI 1.0 for programs written in C or C++ (wasm32).
 *
 * Every call returns an int32_t: zero or a value on success, a negative WR_E*
 * error otherwise. Handles belong to the calling program and close when it
 * ends. Strings are (pointer, length) in UTF-8; the *_cstr helpers take a
 * NUL-terminated string instead. Time, sleeping and yielding are WASI
 * (clock_gettime, nanosleep/usleep, sched_yield). See docs/abi.md.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WR_ABI_VERSION_MAJOR 1
#define WR_ABI_VERSION_MINOR 0

/* Errors; calls return their negatives. */
#define WR_EFAULT       1   /* a pointer or range lies outside the program's memory */
#define WR_EINVAL       2   /* an argument is invalid */
#define WR_EBADH        3   /* not an open handle of the right kind */
#define WR_ENOMEM       4   /* out of memory */
#define WR_ELIMIT       5   /* too many handles, processes or arguments */
#define WR_ENOTFOUND    6   /* no such file or name */
#define WR_ETIMEOUT     7   /* the timeout ran out */
#define WR_EABORTED     8   /* the wait was cancelled */
#define WR_EBUSY        9   /* the wait was cancelled because the program was suspended */
#define WR_ENOTOWNER    10  /* the mutex belongs to another program */
#define WR_EFULL        11  /* the queue or semaphore is full */
#define WR_EEMPTY       12  /* the queue is empty */
#define WR_EUNSUPPORTED 13  /* not available on this device or build */
#define WR_ETOOLONG     14  /* a string is too long */
#define WR_EBADPROGRAM  15  /* the module cannot be loaded */
#define WR_EISDIR       16  /* the path is a directory */
#define WR_ENOTDIR      17  /* a path component is not a directory */
#define WR_EDEAD        18  /* the process has already ended */
#define WR_EMISMATCH    19  /* a named object exists with other parameters */
#define WR_ETRAPPED     20  /* proc wait: the child trapped */
#define WR_EKILLED      21  /* proc wait: the child was killed */

#define WR_WAIT_FOREVER 0xFFFFFFFFu

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

#define WR_IMPORT(interface, name) \
    __attribute__((import_module("wasm-rtos:kernel/" interface "@1.0.0"), import_name(name)))

/* handle */
WR_IMPORT("handle", "close") int32_t wr_close(int32_t handle);

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

/* wait: the index of the first ready handle (queue with an item, semaphore
 * that can be taken, event with a bit set, expired timer, ended child). */
WR_IMPORT("wait", "any") int32_t wr_wait_any(const int32_t* handles, uint32_t count, uint32_t timeout_ms);

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

/* Helpers for NUL-terminated strings. */
static inline uint32_t wr_strlen(const char* text)
{
    uint32_t length = 0;
    while (text[length] != '\0')
        ++length;
    return length;
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
#define WR_APP_ABI_VERSION() WR_APP_U32_(1, 65536)
/* Interpreter stack in bytes, unless whoever starts the program picks one. */
#define WR_APP_STACK_SIZE(bytes) WR_APP_U32_(2, bytes)
/* Name shown for the program. */
#define WR_APP_NAME(text) WR_APP_TEXT_(3, text)

#ifdef __cplusplus
}
#endif

#endif
