#ifndef WASM_RTOS_H
#define WASM_RTOS_H

#include <stdint.h>
#include <string.h>

#include "vm/source/wm.h"

/*
 * Build options (define to 0 to leave a feature out of the image):
 *   OS_HAS_PROCESSES  os_task_spawn() and the guest proc interface
 *   OS_HAS_FS         virtual file system behind the WASI file calls (fs.h)
 *   OS_FS_TAR         read-only tar image driver (needs OS_HAS_FS)
 *   OS_HAS_WASI       the WASI preview 1 calls programs built for it use
 * Off unless defined to 1:
 *   OS_HAS_DYLINK     dylink.0 programs and shared libraries (vm/source/wm_dylink.c)
 *   OS_HAS_MEMORY_ACCOUNTING  all interpreter memory per task in os_task_get_memory_usage
 */
#ifndef OS_HAS_PROCESSES
#define OS_HAS_PROCESSES 1
#endif
#ifndef OS_HAS_FS
#define OS_HAS_FS 1
#endif
#ifndef OS_FS_TAR
#define OS_FS_TAR OS_HAS_FS
#endif
#ifndef OS_HAS_WASI
#define OS_HAS_WASI 1
#endif
#ifndef OS_HAS_DYLINK
#define OS_HAS_DYLINK 0
#endif
#ifndef OS_HAS_MEMORY_ACCOUNTING
#define OS_HAS_MEMORY_ACCOUNTING 0
#endif
#if OS_HAS_DYLINK
#include "vm/source/wm_dylink.h"
#endif

/* Finite delays, timeouts, and timer periods must not exceed INT32_MAX. */
#define OS_WAIT_FOREVER UINT32_MAX

typedef struct OsTask* OsTaskHandle;
typedef struct OsQueue* OsQueueHandle;
typedef struct OsMutex* OsMutexHandle;
typedef struct OsSemaphore* OsSemaphoreHandle;
typedef struct OsEventGroup* OsEventGroupHandle;
typedef struct OsTimer* OsTimerHandle;
#if OS_HAS_DYLINK
typedef struct OsWasmLibrary* OsWasmLibraryHandle;
#endif

/*
 * Host functions (the kernel's imports and os_host_import_register's) get
 * their arguments in 32-bit slots, i64 and f64 taking two, and leave their
 * result in the first. OS_HOST_FUNCTION declares one. Inside it,
 * OS_HOST_RETURNS(type) comes first and names the result slot raw_return;
 * OS_HOST_ARG reads the next argument (a pointer into the program's memory
 * arrives as a uint32_t address). OS_HOST_RETURN sets the result and returns.
 *
 * The program's memory is reached by address, never through a pointer to
 * all of it: it may be paged, in pieces of a frame (4 KiB by default). OS_HOST_CHECK_ADDR traps a
 * range outside the memory; after it, OS_HOST_LOAD32/64 read a value at an
 * address, OS_HOST_STORE8/32/64 write one and OS_HOST_COPY_FROM/TO move a
 * block (linear memory is little-endian, like every target the kernel runs
 * on). A port that works on a buffer in place opens an OsGuestBuffer
 * (os_internal.h). In bounds, an access can still fail when the memory is
 * paged (its frame could not come back from the page store, or into RAM):
 * the program's call then ends with that trap once the host function
 * returns, as it would had the program touched the frame itself.
 */
typedef WmHostFunction OsHostImportFunction;

#define OS_HOST_FUNCTION(NAME) \
    WmResult NAME(WmRuntime* runtime, void* os_host_user, uint32_t* os_host_slot, WmMemory* os_host_memory)
#define OS_HOST_RETURNS(TYPE) TYPE* const raw_return = (TYPE*)(void*)os_host_slot
#define OS_HOST_ARG(TYPE, NAME) \
    TYPE NAME; \
    memcpy(&NAME, os_host_slot, sizeof(TYPE)); \
    os_host_slot += (sizeof(TYPE) + 3U) / 4U
#define OS_HOST_RETURN(VALUE) \
    do \
    { \
        *raw_return = (VALUE); \
        return NULL; \
    } while (0)
#define OS_HOST_CHECK_ADDR(ADDRESS, LENGTH) \
    do \
    { \
        if (!os_host_memory_in_bounds(os_host_memory, (uint32_t)(ADDRESS), (uint64_t)(LENGTH))) \
            return os_host_out_of_bounds; \
    } while (0)
#define OS_HOST_LOAD32(ADDRESS) os_host_load32(runtime, os_host_memory, (uint32_t)(ADDRESS))
#define OS_HOST_LOAD64(ADDRESS) os_host_load64(runtime, os_host_memory, (uint32_t)(ADDRESS))
#define OS_HOST_STORE8(ADDRESS, VALUE) os_host_store8(runtime, os_host_memory, (uint32_t)(ADDRESS), (uint8_t)(VALUE))
#define OS_HOST_STORE32(ADDRESS, VALUE) os_host_store32(runtime, os_host_memory, (uint32_t)(ADDRESS), (uint32_t)(VALUE))
#define OS_HOST_STORE64(ADDRESS, VALUE) os_host_store64(runtime, os_host_memory, (uint32_t)(ADDRESS), (uint64_t)(VALUE))
#define OS_HOST_COPY_FROM(ADDRESS, OUT, LENGTH) \
    os_host_fault(runtime, wm_memory_read(os_host_memory, (uint32_t)(ADDRESS), (OUT), (uint32_t)(LENGTH)))
#define OS_HOST_COPY_TO(ADDRESS, DATA, LENGTH) \
    os_host_fault(runtime, wm_memory_write(os_host_memory, (uint32_t)(ADDRESS), (DATA), (uint32_t)(LENGTH)))
/* Little-endian values in kernel memory (a record copied out of the program). */
#define OS_HOST_READ32(POINTER) os_host_read32(POINTER)
#define OS_HOST_READ64(POINTER) os_host_read64(POINTER)
#define OS_HOST_WRITE8(POINTER, VALUE) (*(uint8_t*)(POINTER) = (uint8_t)(VALUE))
#define OS_HOST_WRITE32(POINTER, VALUE) os_host_write32((POINTER), (uint32_t)(VALUE))
#define OS_HOST_WRITE64(POINTER, VALUE) os_host_write64((POINTER), (uint64_t)(VALUE))

/* Whether length bytes from address lie in the memory. */
static inline int os_host_memory_in_bounds(const WmMemory* memory, uint32_t address, uint64_t length)
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
static inline void os_host_fault(WmRuntime* runtime, WmResult result)
{
    if (result != NULL)
    {
        (void)wm_trap(runtime, result);
    }
}
static inline uint32_t os_host_load32(WmRuntime* runtime, const WmMemory* memory, uint32_t address)
{
    uint32_t value = 0U;
    os_host_fault(runtime, wm_memory_read(memory, address, &value, sizeof(value)));
    return value;
}
static inline uint64_t os_host_load64(WmRuntime* runtime, const WmMemory* memory, uint32_t address)
{
    uint64_t value = 0U;
    os_host_fault(runtime, wm_memory_read(memory, address, &value, sizeof(value)));
    return value;
}
static inline void os_host_store8(WmRuntime* runtime, WmMemory* memory, uint32_t address, uint8_t value)
{
    os_host_fault(runtime, wm_memory_write(memory, address, &value, sizeof(value)));
}
static inline void os_host_store32(WmRuntime* runtime, WmMemory* memory, uint32_t address, uint32_t value)
{
    os_host_fault(runtime, wm_memory_write(memory, address, &value, sizeof(value)));
}
static inline void os_host_store64(WmRuntime* runtime, WmMemory* memory, uint32_t address, uint64_t value)
{
    os_host_fault(runtime, wm_memory_write(memory, address, &value, sizeof(value)));
}
static inline uint32_t os_host_read32(const void* pointer)
{
    uint32_t value;
    memcpy(&value, pointer, sizeof(value));
    return value;
}
static inline uint64_t os_host_read64(const void* pointer)
{
    uint64_t value;
    memcpy(&value, pointer, sizeof(value));
    return value;
}
static inline void os_host_write32(void* pointer, uint32_t value) { memcpy(pointer, &value, sizeof(value)); }
static inline void os_host_write64(void* pointer, uint64_t value) { memcpy(pointer, &value, sizeof(value)); }
/* The trap OS_HOST_CHECK_ADDR returns. */
extern const char os_host_out_of_bounds[];

typedef enum OsStatus
{
    OS_STATUS_OK = 0,
    OS_STATUS_ERROR,
    OS_STATUS_INVALID_ARGUMENT,
    OS_STATUS_OUT_OF_MEMORY,
    OS_STATUS_WASM_ERROR,
    OS_STATUS_TASK_DEAD,
    OS_STATUS_TASK_NOT_FOUND,
    OS_STATUS_NO_READY_TASKS,
    OS_STATUS_BUFFER_TOO_SMALL,
    OS_STATUS_UNSUPPORTED,
    OS_STATUS_OUT_OF_BOUNDS,
    OS_STATUS_QUEUE_FULL,
    OS_STATUS_QUEUE_EMPTY,
    OS_STATUS_QUEUE_NOT_FOUND,
    OS_STATUS_TIMEOUT,
    OS_STATUS_BUSY,
    OS_STATUS_NOT_OWNER,
    OS_STATUS_MUTEX_NOT_FOUND,
    OS_STATUS_SEMAPHORE_FULL,
    OS_STATUS_SEMAPHORE_NOT_FOUND,
    OS_STATUS_EVENT_GROUP_NOT_FOUND,
    OS_STATUS_TIMER_NOT_FOUND,
    OS_STATUS_ABORTED,
    OS_STATUS_NOT_WAITING,
    OS_STATUS_LIMIT_REACHED,
    OS_STATUS_FILE_NOT_FOUND,
    OS_STATUS_NOT_A_DIRECTORY,
    OS_STATUS_IS_A_DIRECTORY,
    OS_STATUS_READ_ONLY,
    OS_STATUS_END_OF_DIRECTORY,
    OS_STATUS_NACK,
    OS_STATUS_BUS_ERROR,
    OS_STATUS_PENDING
} OsStatus;

typedef enum OsTaskState
{
    OS_TASK_READY = 0,
    OS_TASK_RUNNING,
    OS_TASK_WAITING,
    OS_TASK_SUSPENDED,
    OS_TASK_DEAD
} OsTaskState;

typedef enum OsTaskExitReason
{
    OS_TASK_EXIT_NONE = 0,
    OS_TASK_EXIT_RETURNED,
    OS_TASK_EXIT_EXPLICIT,
    OS_TASK_EXIT_DELETED,
    OS_TASK_EXIT_WASM_ERROR
} OsTaskExitReason;

typedef enum OsTaskPriority
{
    OS_TASK_PRIORITY_IDLE = 0,
    OS_TASK_PRIORITY_LOW = 1,
    OS_TASK_PRIORITY_NORMAL = 5,
    OS_TASK_PRIORITY_HIGH = 10,
    OS_TASK_PRIORITY_REALTIME = 20
} OsTaskPriority;

typedef enum OsValueType
{
    OS_VALUE_TYPE_I32 = 1,
    OS_VALUE_TYPE_I64 = 2,
    OS_VALUE_TYPE_F32 = 3,
    OS_VALUE_TYPE_F64 = 4
} OsValueType;

typedef enum OsNotifyAction
{
    OS_NOTIFY_NO_ACTION = 0,
    OS_NOTIFY_SET_BITS,
    OS_NOTIFY_INCREMENT,
    OS_NOTIFY_SET_VALUE_WITH_OVERWRITE,
    OS_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE
} OsNotifyAction;

typedef uint32_t (*OsClockNowMsFunction)(void* context);
typedef void (*OsClockArmWakeupFunction)(void* context, uint32_t delay_ms);
typedef void (*OsClockCancelWakeupFunction)(void* context);

typedef struct OsClockPort
{
    OsClockNowMsFunction now_ms;
    OsClockArmWakeupFunction arm_wakeup;
    OsClockCancelWakeupFunction cancel_wakeup;
    void* context;
} OsClockPort;

typedef struct OsValue
{
    OsValueType type;
    union OsValueData
    {
        uint32_t i32;
        uint64_t i64;
        float f32;
        double f64;
    } value;
} OsValue;

OsStatus os_init(void);
void os_shutdown(void);

/*
 * Interpreter stack size. Whoever creates a task may pass stack_size; 0 takes
 * the size the program asks for in its "wasm-rtos.app" custom section, and
 * without one OS_DEFAULT_STACK_SIZE (8 KiB). A program overflowing its stack
 * traps with "stack overflow"; os_task_get_memory_usage reports the most it
 * has used (stack_peak_bytes).
 *
 * A program describes itself in a "wasm-rtos.app" custom section: records
 * of a key byte, a LEB128 length and that many bytes. Unknown keys are
 * skipped; a section that is not whole records fails the load, and so does
 * an ABI major version other than OS_ABI_VERSION_MAJOR. wasm-rtos.h has
 * macros that write the records (WR_APP_STACK_SIZE, WR_APP_NAME, ...).
 */
#define OS_ABI_VERSION_MAJOR 1U
#define OS_ABI_VERSION_MINOR 8U
#define OS_APP_KEY_ABI_VERSION 1U   /* u32: major << 16 | minor */
#define OS_APP_KEY_STACK_SIZE 2U    /* u32: interpreter stack bytes */
#define OS_APP_KEY_NAME 3U          /* UTF-8 */
#define OS_APP_KEY_ICON 4U          /* reserved: image */
#define OS_APP_KEY_FILE_TYPES 5U    /* reserved: UTF-8, ';'-separated extensions */
#define OS_APP_KEY_REQUIRES 6U      /* reserved: device requirements */

/*
 * entry_function_name may be NULL or "" to let the loader pick the entry
 * point: _start (WASI command), then app_main, then main. Every function
 * import must resolve when the task is created; otherwise creation fails
 * with OS_STATUS_WASM_ERROR and os_get_last_error_result() names the first
 * missing import.
 */
OsStatus os_task_create(
    OsTaskHandle* out_task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    const char* task_name,
    uint32_t stack_size,
    uint32_t priority
);

OsStatus os_task_create_with_args(
    OsTaskHandle* out_task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    OsValue* entry_args,
    uint32_t entry_arg_count,
    const char* task_name,
    uint32_t stack_size,
    uint32_t priority
);

OsStatus os_queue_create(
    OsQueueHandle* out_queue,
    uint32_t item_size,
    uint32_t item_count
);

void os_queue_delete(OsQueueHandle queue);
uint32_t os_queue_get_id(OsQueueHandle queue);
OsQueueHandle os_queue_find_by_id(uint32_t queue_id);
OsStatus os_queue_send(OsQueueHandle queue, const void* item);
OsStatus os_queue_receive(OsQueueHandle queue, void* out_item);
OsStatus os_queue_send_wait(
    OsQueueHandle queue,
    const void* item,
    uint32_t timeout_ms
);
/*
 * A receive that can complete immediately accepts any writable C buffer.
 * If it must block, out_item must point into the current WASM task's linear
 * memory so the kernel can retain a stable offset instead of a native pointer.
 * Native buffers return OS_STATUS_UNSUPPORTED rather than being retained.
 */
OsStatus os_queue_receive_wait(
    OsQueueHandle queue,
    void* out_item,
    uint32_t timeout_ms
);
uint32_t os_queue_get_count(OsQueueHandle queue);
uint32_t os_queue_get_space(OsQueueHandle queue);
uint32_t os_queue_get_item_size(OsQueueHandle queue);
/*
 * A name lets programs open the object (the guest sync interface). Objects
 * the host creates stay the host's: programs never delete them.
 */
OsStatus os_queue_set_name(OsQueueHandle queue, const char* name);
OsStatus os_mutex_set_name(OsMutexHandle mutex, const char* name);
OsStatus os_semaphore_set_name(OsSemaphoreHandle semaphore, const char* name);
OsStatus os_event_group_set_name(OsEventGroupHandle event_group, const char* name);

OsStatus os_mutex_create(OsMutexHandle* out_mutex);
void os_mutex_delete(OsMutexHandle mutex);
uint32_t os_mutex_get_id(OsMutexHandle mutex);
OsMutexHandle os_mutex_find_by_id(uint32_t mutex_id);
OsStatus os_mutex_lock(OsMutexHandle mutex, uint32_t timeout_ms);
OsStatus os_mutex_unlock(OsMutexHandle mutex);
OsTaskHandle os_mutex_get_owner(OsMutexHandle mutex);

OsStatus os_semaphore_create(
    OsSemaphoreHandle* out_semaphore,
    uint32_t max_count,
    uint32_t initial_count
);
void os_semaphore_delete(OsSemaphoreHandle semaphore);
uint32_t os_semaphore_get_id(OsSemaphoreHandle semaphore);
OsSemaphoreHandle os_semaphore_find_by_id(uint32_t semaphore_id);
OsStatus os_semaphore_take(OsSemaphoreHandle semaphore, uint32_t timeout_ms);
OsStatus os_semaphore_give(OsSemaphoreHandle semaphore);
uint32_t os_semaphore_get_count(OsSemaphoreHandle semaphore);
uint32_t os_semaphore_get_max_count(OsSemaphoreHandle semaphore);

OsStatus os_event_group_create(OsEventGroupHandle* out_event_group);
void os_event_group_delete(OsEventGroupHandle event_group);
uint32_t os_event_group_get_id(OsEventGroupHandle event_group);
OsEventGroupHandle os_event_group_find_by_id(uint32_t event_group_id);
uint32_t os_event_group_get_bits(OsEventGroupHandle event_group);
uint32_t os_event_group_set_bits(OsEventGroupHandle event_group, uint32_t bits);
uint32_t os_event_group_clear_bits(OsEventGroupHandle event_group, uint32_t bits);
OsStatus os_event_group_wait_bits(
    OsEventGroupHandle event_group,
    uint32_t bits_to_wait_for,
    uint8_t clear_on_exit,
    uint8_t wait_for_all,
    uint32_t timeout_ms
);

OsStatus os_host_import_register(
    const char* module_name,
    const char* import_name,
    const char* signature,
    OsHostImportFunction function
);

void os_host_import_clear_all(void);

/*
 * Host imports that block: pass the import's result slot (raw_return) to
 * os_host_wait_begin before calling a waiting function such as
 * os_task_notify_wait from inside the import, and return os_host_wait_end()
 * from it. When the call started a wait the task blocks, and the slot gets
 * the wait's status (or value) when it wakes.
 */
OsStatus os_host_wait_begin(uint32_t* result_slot);
WmResult os_host_wait_end(void);

#if OS_HAS_DYLINK
/*
 * Register a dylink.0 side module (a shared library) under the name programs
 * list as needed. The byte buffer must remain valid until os_shutdown().
 *
 * A PIE program is loaded with its dependencies into its own runtime: every
 * process gets private copies of each library's memory, globals and
 * indirect-function table, like shared objects in an ELF process. Only the
 * library bytes are shared. Libraries may be registered at any time; a
 * program needing one that is missing fails to load.
 */
OsStatus os_wasm_library_register(
    OsWasmLibraryHandle* out_library,
    const char* module_name,
    uint8_t* wasm_bytes,
    uint32_t wasm_size
);
OsWasmLibraryHandle os_wasm_library_find(const char* module_name);
uint32_t os_wasm_library_get_id(OsWasmLibraryHandle library);
const char* os_wasm_library_get_name(OsWasmLibraryHandle library);
uint32_t os_wasm_library_get_count(void);
#endif

OsStatus os_task_delete(OsTaskHandle task);

#if OS_HAS_PROCESSES
/*
 * Start a WebAssembly program as a new process, like an executable loader.
 * The module bytes and argv are copied, so the caller may release them
 * afterwards. The entry point is detected (_start, app_main, main); a WASI
 * program receives argv through args_get. priority 0 inherits the calling
 * task's base priority, or OS_TASK_PRIORITY_NORMAL when called from the host.
 * stack_size 0 lets the program choose (see "Interpreter stack size"); guest
 * spawn calls always pass 0.
 *
 * A process spawned from inside a task becomes that task's child. A dead
 * child is reclaimed after its parent collects it (the guest proc wait
 * call), or as soon as it dies when its parent is gone. A process spawned
 * by the host has no parent and stays until the host deletes it, exactly
 * like a task from os_task_create. At most
 * OS_MAX_SPAWNED_TASKS spawned processes exist at once; further spawns
 * return OS_STATUS_LIMIT_REACHED.
 *
 * Processes of the same program share it: its bytes and its parsed module
 * (with the decoded code) are loaded once, and each process has its own
 * instance, memory and stack. A program is found by its contents, compared
 * byte by byte, so os_task_spawn copies the bytes only when no loaded
 * program has them, and the program goes with its last process.
 *
 * Programs start processes through the guest ABI's proc interface
 * (docs/abi.md), which gives them handles to their children.
 */
OsStatus os_task_spawn(
    OsTaskHandle* out_task,
    const uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* const* argv,
    uint32_t argc,
    uint32_t priority,
    uint32_t stack_size
);
uint32_t os_task_get_parent_id(OsTaskHandle task);
/* Programs loaded for processes, each once however many processes run it. */
uint32_t os_get_program_count(void);
#if OS_HAS_FS
/*
 * Start the program stored at an absolute file system path. A file whose
 * contents a running process already has is that process's program: it is
 * compared in pieces, not read whole, and a file that changed is a new
 * program. A file whose driver maps it into memory (the tar driver does)
 * runs in place without a copy, and its mount stays busy until the last
 * process of it is destroyed; any other file is read into RAM first. With
 * argc 0 the program gets argv = { path }.
 * Errors: OS_STATUS_FILE_NOT_FOUND, OS_STATUS_IS_A_DIRECTORY,
 * OS_STATUS_NOT_A_DIRECTORY, plus everything os_task_spawn returns.
 */
OsStatus os_task_spawn_path(
    OsTaskHandle* out_task,
    const char* path,
    const char* const* argv,
    uint32_t argc,
    uint32_t priority,
    uint32_t stack_size
);
#endif
#endif
OsStatus os_task_delay_ms(uint32_t delay_ms);
/*
 * Advance *previous_wake_time_ms by period_ms and block until that absolute
 * deadline when it is still in the future. The phase is advanced even when
 * the deadline was missed, matching FreeRTOS vTaskDelayUntil semantics.
 */
OsStatus os_task_delay_until(
    uint32_t* previous_wake_time_ms,
    uint32_t period_ms
);
/*
 * Wake a task blocked by a delay or synchronization wait. Blocking imports
 * that return a status resume with OS_STATUS_ABORTED.
 */
OsStatus os_task_abort_wait(OsTaskHandle task);
/*
 * Called from a host import: end the calling task's slice and keep it READY.
 * OS_STATUS_TASK_NOT_FOUND when no task is running.
 */
OsStatus os_task_yield(void);
/*
 * Safe from a host import or between slices (an interrupt handler's deferred
 * work): the running task stops at its next check, or else the next slice
 * stops at once. One request ends one slice.
 */
void os_request_preempt(void);
/*
 * A suspended task gets no slices until os_task_resume. Suspending a waiting
 * task cancels its wait: when resumed it continues at once, a delay ends
 * early and a blocking import returns OS_STATUS_BUSY. Resuming a task that is
 * not suspended changes nothing. Both return OS_STATUS_TASK_DEAD for a dead
 * task.
 */
OsStatus os_task_suspend(OsTaskHandle task);
OsStatus os_task_resume(OsTaskHandle task);

/*
 * A snapshot is a program's state between two slices: its position, stack,
 * linear memory and globals, without host addresses, so it loads into a task
 * running the same module on this or another device with the same kernel
 * version, whatever the pointer size. Load it into a new task created from
 * the same module; it continues where the snapshot was taken.
 *
 * Kernel state is not part of it: waits, timers, handles and open files stay
 * with the task they belong to. So saving needs a task that is neither
 * waiting nor the target of a timer (OS_STATUS_BUSY), and loading needs one
 * that has not run yet (OS_STATUS_BUSY). A load that fails leaves the task
 * as it was created. Linked (dylink.0) tasks return OS_STATUS_UNSUPPORTED.
 *
 * The stream calls write or read the snapshot in pieces through a callback;
 * a callback error stops the call and is returned as it is.
 */
typedef OsStatus (*OsSnapshotWrite)(void* context, const void* data, uint32_t size);
typedef OsStatus (*OsSnapshotRead)(void* context, void* data, uint32_t size);

OsStatus os_task_write_snapshot(OsTaskHandle task, OsSnapshotWrite write, void* context);
OsStatus os_task_read_snapshot(OsTaskHandle task, OsSnapshotRead read, void* context);

OsStatus os_task_get_snapshot_size(
    OsTaskHandle task,
    uint32_t* out_size
);

OsStatus os_task_save_snapshot(
    OsTaskHandle task,
    uint8_t* buffer,
    uint32_t buffer_size,
    uint32_t* out_size
);

OsStatus os_task_load_snapshot(
    OsTaskHandle task,
    const uint8_t* buffer,
    uint32_t buffer_size
);

/*
 * The whole system, for a device that loses its RAM in deep sleep or when
 * switched off: every task (its program's state, what it waits for, its
 * notifications, handles, open files, children) and every queue, mutex,
 * semaphore, event group and timer with what it holds, and every device a
 * task holds (each driver saves its own, by name), written to a stream
 * between two slices (not from inside a task). Modules are not written: a
 * process started by path is opened from it again, one the kernel copied
 * is in the stream, and for a task the host created the kernel asks find
 * by task name; a module of other bytes is refused.
 *
 * Saving returns OS_STATUS_BUSY while a transfer or page is on its way (try
 * after the next slice) and OS_STATUS_UNSUPPORTED while a task runs a
 * dylink.0 program or holds a display that loses its picture in the sleep
 * (OsDisplayInfo.keeps_image); the last error says which.
 *
 * Restore into a kernel set up as at start (os_init, clock, page store,
 * drivers, host imports, libraries) that has no task or object yet. The
 * clock goes on from where it was plus slept_ms, and what fell due during
 * the sleep is delivered at once. A stream that is cut, of another format
 * or that does not agree with itself restores nothing: an error, and the
 * kernel is as before. A task whose module was not found or changed, whose
 * open file is gone or changed, or whose device the firmware no longer
 * registers under its name or cannot set up, ends as a task that traps
 * (OS_TASK_EXIT_WASM_ERROR, its error text "restore: <why>"); every other
 * task goes on. The host finds its tasks and objects by their ids, which
 * are those they had.
 */
typedef OsStatus (*OsModuleFind)(void* context, const char* task_name, const uint8_t** out_bytes,
                                 uint32_t* out_size);
OsStatus os_system_save(OsSnapshotWrite write, void* context);
OsStatus os_system_restore(OsSnapshotRead read, void* context, OsModuleFind find, void* find_context,
                           uint32_t slept_ms);

/*
 * Paging moves single frames of programs' memory (4 KiB unless the
 * interpreter is built with another WM_FRAME_SIZE), so programs may
 * together have more memory than the board has RAM,
 * and a program may have more than RAM itself. With a page store, every
 * program's memory is paged: a frame comes into RAM when the program (or a
 * host call) touches it, and when RAM is full the kernel stores the frame
 * touched least recently (CLOCK) in one of the store's slots and brings it
 * back when it is touched again.
 *
 * Memory is promised only as far as it can be kept: creating a task or
 * growing its memory fails unless every frame of every program could have
 * a slot, so a program never runs out of memory it was given. A frame keeps
 * its slot once stored: one that came back and was only read leaves RAM
 * again without a write.
 *
 * The store has slots numbered from 0, each as large as a frame:
 * wm_frame_size() bytes (WM_FRAME_SIZE, 4096 unless built otherwise; with
 * 0, memory is never paged and there is no page store). ram_frames is how
 * many frames the kernel keeps in RAM (0: as many as the heap holds). Calls
 * happen from the scheduler or a host call, one at a time. Set the store
 * before creating tasks.
 */
typedef struct OsPageStore
{
    void* context;
    uint32_t slots;
    uint32_t ram_frames;
    OsStatus (*write)(void* context, uint32_t slot, const void* frame);
    OsStatus (*read)(void* context, uint32_t slot, void* frame);
    /*
     * Optional, for a store that reads in the background (DMA, another
     * thread): start reading slot into frame and return OS_STATUS_PENDING,
     * then call os_page_read_done when the read ends, also from an
     * interrupt or another thread; finish waits for a read in flight to end
     * and returns its status. Once finish has returned for a read, its
     * os_page_read_done must have come already or never come: a late one
     * could meet a new read of the same slot. Meanwhile only the program that touched the
     * frame waits, and the others run. OS_STATUS_OK: read at once;
     * OS_STATUS_BUSY: it cannot start another read now, and the kernel
     * reads the frame at once with read. Only predecoded code waits: code
     * run in place (WM_PREDECODE off, or a build too large to predecode)
     * reads frames at once.
     */
    OsStatus (*read_start)(void* context, uint32_t slot, void* frame);
    OsStatus (*finish)(void* context, uint32_t slot);
    /*
     * Optional, the same for writes: start writing frame to slot and return
     * OS_STATUS_PENDING, then call os_page_write_done when the write ends.
     * The frame leaves RAM only then, and nobody waits for it meanwhile;
     * finish waits for such a write too (it needs finish). OS_STATUS_OK:
     * written at once; OS_STATUS_BUSY: the kernel writes it at once.
     */
    OsStatus (*write_start)(void* context, uint32_t slot, const void* frame);
    /*
     * Optional: the region the frames in RAM come from (external RAM, for
     * one), os_page_ram_bytes(store) bytes. NULL: the kernel takes it from
     * the heap when the store is set. With a RAM budget (ram_frames not 0)
     * frames come only from this pool, never cut up the heap, and a
     * program's memory in one block is its frames in a row there.
     */
    void* ram;
} OsPageStore;

/* A background read or write the store started has ended. */
void os_page_read_done(uint32_t slot, OsStatus status);
void os_page_write_done(uint32_t slot, OsStatus status);

/* NULL turns paging off. OS_STATUS_BUSY while tasks exist. */
OsStatus os_page_store_set(const OsPageStore* store);
/*
 * The bytes of the frame pool for a store: ram_frames frames, and a few
 * more for frames on their way out when it writes in the background (they
 * do not count against the budget until their write ends). 0 without a RAM
 * budget.
 */
uint32_t os_page_ram_bytes(const OsPageStore* store);

/* What paging holds now, across all programs. */
typedef struct OsPageStats
{
    uint32_t reserved_frames; /* frames promised to programs */
    uint32_t limit_frames;    /* how many can be promised: the slots */
    uint32_t ram_frames;      /* frames in RAM */
    uint32_t stored_frames;   /* slots in use (a frame in RAM may keep one, holding a copy) */
    uint32_t stores;          /* frames written to the store so far */
    uint32_t loads;           /* frames read back so far */
} OsPageStats;

OsStatus os_page_get_stats(OsPageStats* out_stats);

/*
 * Where programs' stdout (fd 1) and stderr (fd 2) go. write gets the id of
 * the task that wrote, so a platform can keep a log per program, send it to
 * a UART, or both. Without a port, output goes to the C library's stdout
 * and stderr. Like the other ports, it is cleared by os_init.
 */
typedef struct OsOutputPort
{
    void* context;
    void (*write)(void* context, uint32_t task_id, uint32_t fd, const void* data, uint32_t size);
} OsOutputPort;

/* NULL restores the default. */
OsStatus os_output_port_set(const OsOutputPort* port);

/*
 * Swapping a task out frees all its RAM but the control block, at once,
 * through the page store: its state (stack, calls, globals, tables) is
 * written to the store's slots and its memory's frames go there as they
 * would one by one; the memory itself stays apart, paged. The task stays
 * where it is: it keeps waiting, its timers and handles keep working, and a
 * queue item its wait delivers goes straight into its memory. It comes back
 * from that state with the same memory, whose frames return as they are
 * touched. Any task but the one running can be swapped out, waiting or not;
 * one that has not started frees its state and starts afresh.
 * OS_STATUS_UNSUPPORTED without a page store or for linked tasks; errors
 * leave the task as it was.
 */
OsStatus os_task_swap_out(OsTaskHandle task);

/*
 * Bring a swapped-out task back now. The scheduler does this by itself before
 * the task's next slice; if that fails, the task ends with
 * OS_TASK_EXIT_WASM_ERROR and the error phase "swap_in".
 */
OsStatus os_task_swap_in(OsTaskHandle task);

uint8_t os_task_is_swapped(OsTaskHandle task);

/*
 * Sets the base priority; the scheduler always runs the highest priority
 * READY task and rotates tasks of equal priority. A mutex owner may run
 * higher while it inherits a waiter's priority (os_task_get_priority).
 */
OsStatus os_task_set_priority(
    OsTaskHandle task,
    uint32_t priority
);

uint32_t os_task_get_priority(OsTaskHandle task);
uint32_t os_task_get_base_priority(OsTaskHandle task);
uint32_t os_task_get_id(OsTaskHandle task);
OsTaskHandle os_task_find_by_id(uint32_t task_id);
uint32_t os_task_get_id_list(uint32_t* out_task_ids, uint32_t max_task_ids);
/* The name a program gives itself in its wasm-rtos.app section, or NULL. */
const char* os_task_get_app_name(OsTaskHandle task);
OsTaskState os_task_get_state(OsTaskHandle task);
OsTaskExitReason os_task_get_exit_reason(OsTaskHandle task);
uint32_t os_task_get_exit_code(OsTaskHandle task);
OsStatus os_task_get_return_value(OsTaskHandle task, OsValue* out_value);
uint32_t os_task_get_return_value_count(OsTaskHandle task);
OsStatus os_task_get_return_values(
    OsTaskHandle task,
    OsValue* out_values,
    uint32_t max_values,
    uint32_t* out_value_count
);
/*
 * RAM a task uses, in bytes. stack, linear memory and code are measured
 * exactly in every build. interpreter_bytes is everything the interpreter
 * allocated for the task (stack, memory, module tables, names, types and
 * the like) and needs OS_HAS_MEMORY_ACCOUNTING=1; otherwise it is 0 and
 * total_bytes leaves that metadata out. Predecoded code is a cache every
 * task shares: made at a function's first call, dropped when memory is
 * short; code_bytes is the part of it the task's module holds now, counted
 * in no task's interpreter_bytes.
 */
typedef struct OsTaskMemoryUsage
{
    uint32_t stack_bytes;            /* interpreter value stack */
    uint32_t stack_peak_bytes;       /* the most of it the task has used, also after it ends */
    uint32_t linear_memory_bytes;    /* the program's linear memory, with spare room from memory.grow */
    uint32_t resident_memory_bytes;  /* the part of it in RAM now: all of it, or the pages touched when paged */
    uint32_t stored_memory_bytes;    /* the part of it only a page store holds */
    uint32_t block_memory_bytes;     /* with a page store: all of it while it is one block, reached at full speed; else 0 */
    uint32_t code_bytes;             /* its module's predecoded code made so far (a cache tasks share, not in interpreter_bytes) */
    uint32_t interpreter_bytes;      /* all interpreter allocations, see above */
    uint32_t interpreter_peak_bytes; /* the most interpreter_bytes has been */
    uint32_t image_bytes;            /* kernel copy of the module; 0 when it runs in place or the host owns it */
    uint32_t swap_bytes;             /* size of its state in the page store while swapped out, else 0 */
    uint32_t kernel_bytes;           /* task control block */
    uint32_t total_bytes;
} OsTaskMemoryUsage;

OsStatus os_task_get_memory_usage(OsTaskHandle task, OsTaskMemoryUsage* out_usage);

/*
 * Copy to or from a task's linear memory at a wasm address. The whole range
 * must lie inside the memory (OS_STATUS_OUT_OF_BOUNDS), including addresses
 * near 4 GiB. A swapped-out task's memory is reached too (its frames come
 * back); one that never started has none (OS_STATUS_BUSY).
 */
OsStatus os_task_read_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    uint8_t* out_buffer,
    uint32_t byte_count
);
OsStatus os_task_write_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    const uint8_t* buffer,
    uint32_t byte_count
);
const char* os_task_get_name(OsTaskHandle task);
OsTaskHandle os_task_get_current(void);
uint32_t os_task_get_run_count(OsTaskHandle task);
/*
 * Interpreter work units the task has spent, over all its slices; 0 in a
 * build without fuel. Deterministic, unlike time, so the same program uses
 * the same fuel on every device and in the browser.
 */
uint64_t os_task_get_fuel_used(OsTaskHandle task);
OsStatus os_task_get_last_wait_status(OsTaskHandle task);
uint32_t os_task_get_last_wait_value(OsTaskHandle task);

OsStatus os_task_notify(
    OsTaskHandle task,
    uint32_t value,
    OsNotifyAction action
);
OsStatus os_task_notify_wait(
    uint32_t clear_on_entry,
    uint32_t clear_on_exit,
    uint32_t timeout_ms
);
OsStatus os_task_notify_take(
    uint8_t clear_count_on_exit,
    uint32_t timeout_ms
);
uint32_t os_task_get_notification_value(OsTaskHandle task);
uint8_t os_task_is_notification_pending(OsTaskHandle task);

/* Timers notify their target task and are deleted when that task exits. */
OsStatus os_timer_create(
    OsTimerHandle* out_timer,
    uint32_t period_ms,
    uint8_t auto_reload,
    OsTaskHandle target_task,
    uint32_t notify_value,
    OsNotifyAction notify_action
);
void os_timer_delete(OsTimerHandle timer);
uint32_t os_timer_get_id(OsTimerHandle timer);
OsTimerHandle os_timer_find_by_id(uint32_t timer_id);
OsStatus os_timer_start(OsTimerHandle timer);
OsStatus os_timer_stop(OsTimerHandle timer);
OsStatus os_timer_reset(OsTimerHandle timer);
OsStatus os_timer_change_period(OsTimerHandle timer, uint32_t period_ms);
uint8_t os_timer_is_active(OsTimerHandle timer);
uint32_t os_timer_get_period_ms(OsTimerHandle timer);

/*
 * Time is the kernel's only platform dependency, and the kernel contains no
 * platform code: a board, the browser or a test either installs a clock port
 * or drives time itself with os_tick() / os_update_time_ms().
 *
 * now_ms is a monotonic platform clock. arm_wakeup and cancel_wakeup must
 * either both be supplied for tickless operation or both be NULL for polling.
 * A hardware ISR should only defer work; call os_clock_wakeup from a safe
 * platform context after the interrupt or event-loop callback is delivered.
 */
OsStatus os_clock_port_set(const OsClockPort* port);
void os_clock_port_clear(void);
OsStatus os_clock_poll(void);
OsStatus os_clock_wakeup(void);
uint8_t os_clock_is_tickless(void);

const char* os_get_last_error_phase(void);
const char* os_get_last_error_result(void);
const char* os_get_last_error_task_name(void);
/*
 * Why a task ended with OS_TASK_EXIT_WASM_ERROR, kept until it is deleted:
 * "phase: result", and the functions it was in when the interpreter keeps them,
 * innermost first ("call: [trap] unreachable executed in parse < main").
 * NULL for tasks that did not fail.
 */
const char* os_task_get_error(OsTaskHandle task);
OsStatus os_get_last_error_status(void);

/* Advance kernel time by milliseconds; the next os_update_time_ms only re-baselines. */
void os_tick(uint32_t milliseconds);
/*
 * Feed an absolute monotonic clock: time advances by the difference from the
 * previous call, wrapping at 32 bits. The first call only sets the baseline.
 */
OsStatus os_update_time_ms(uint32_t now_ms);
uint32_t os_get_tick_ms(void);
uint32_t os_get_next_wakeup_ms(void);

OsStatus os_schedule(void);

/*
 * GPIO. The platform drives the pins through a port; the kernel lends them
 * to programs by name. Register every pin a program may use, under the names
 * of the device profile ("led", "button-a", "gpio5"); a pin may have several
 * names. A program owns a pin from open until it closes it or ends; then the
 * kernel configures the pin as an input without pulls.
 *
 * Port flags describe the physical pin: OS_GPIO_INIT_HIGH is the level an
 * output starts at, the edge flags enable edge reports through os_gpio_edge.
 */
#define OS_GPIO_OUTPUT       0x01U
#define OS_GPIO_PULL_UP      0x02U
#define OS_GPIO_PULL_DOWN    0x04U
#define OS_GPIO_OPEN_DRAIN   0x08U
#define OS_GPIO_INIT_HIGH    0x20U
#define OS_GPIO_EDGE_RISING  0x40U
#define OS_GPIO_EDGE_FALLING 0x80U

/* What a pin can do (os_gpio_add_pin capabilities). */
#define OS_GPIO_CAN_INPUT      0x01U
#define OS_GPIO_CAN_OUTPUT     0x02U
#define OS_GPIO_CAN_PULL_UP    0x04U
#define OS_GPIO_CAN_PULL_DOWN  0x08U
#define OS_GPIO_CAN_OPEN_DRAIN 0x10U
#define OS_GPIO_CAN_EDGES      0x20U
#define OS_GPIO_CAN_PWM        0x40U
#define OS_GPIO_CAN_PULSE      0x80U
#define OS_GPIO_CAN_ALL        0xFFU

/* os_gpio_add_pin options: the board inverts the pin, so programs see 1 for "on". */
#define OS_GPIO_ACTIVE_LOW 0x01U

#define OS_GPIO_NAME_MAX 32U
#ifndef OS_GPIO_MAX_PINS
#define OS_GPIO_MAX_PINS 32U
#endif
#ifndef OS_GPIO_MAX_NAMES
#define OS_GPIO_MAX_NAMES 64U
#endif

typedef struct OsGpioPort
{
    void* context;
    OsStatus (*configure)(void* context, uint32_t pin, uint32_t flags);
    OsStatus (*write)(void* context, uint32_t pin, uint8_t level);
    OsStatus (*read)(void* context, uint32_t pin, uint8_t* out_level);
} OsGpioPort;

/* NULL removes the port; programs then get -WR_EUNSUPPORTED. */
OsStatus os_gpio_port_set(const OsGpioPort* port);

/*
 * Make a pin available to programs under name. Registering another name for
 * the same pin needs the same capabilities and options. OS_STATUS_BUSY when
 * the name is taken, OS_STATUS_LIMIT_REACHED past OS_GPIO_MAX_PINS or
 * OS_GPIO_MAX_NAMES.
 */
OsStatus os_gpio_add_pin(const char* name, uint32_t pin, uint32_t capabilities, uint32_t options);

/*
 * Report edges on a pin (OS_GPIO_EDGE_RISING and/or OS_GPIO_EDGE_FALLING).
 * Safe to call from an interrupt handler: it only counts them; the next
 * os_schedule delivers them to the program watching the pin.
 */
void os_gpio_edge(uint32_t pin, uint32_t edges);

/* The id of the task that owns a pin, 0 when it is free or unknown. */
uint32_t os_gpio_get_owner(uint32_t pin);

/*
 * The pins programs wait on, for a board about to sleep: each pin (the
 * port's number) with the edges watched on it as the port sees them
 * (OS_GPIO_EDGE_RISING, OS_GPIO_EDGE_FALLING). Fills up to max and returns
 * how many there are. The port arms them as wake sources; after the
 * restore, it reports the edge that woke the board with os_gpio_edge.
 */
typedef struct OsGpioWake
{
    uint32_t pin;
    uint32_t edges;
} OsGpioWake;
uint32_t os_gpio_wake_pins(OsGpioWake* out, uint32_t max);

/*
 * Displays. The platform registers each display a program may draw on under
 * the device profile's name ("main" for the primary one) and draws through a
 * port; the kernel lends a display to one program at a time and checks every
 * write, so the port only ever sees whole rectangles inside the display in a
 * format it accepts.
 *
 * Pixel formats, as programs write them:
 *   OS_DISPLAY_MONO1     1 bit per pixel, 1 = lit; rows of whole bytes, the
 *                        leftmost pixel in the most significant bit
 *   OS_DISPLAY_RGB565    16-bit little-endian, red in the top 5 bits
 *   OS_DISPLAY_XRGB8888  32-bit little-endian 0x00RRGGBB
 * A port converts to the panel's own order (such as big-endian RGB565 over
 * SPI) itself.
 */
#define OS_DISPLAY_MONO1    1U
#define OS_DISPLAY_RGB565   2U
#define OS_DISPLAY_XRGB8888 3U
#define OS_DISPLAY_FORMAT_BIT(format) (1U << (format))

#define OS_DISPLAY_RECTANGLE 0U
/* A circle inscribed in width x height; pixels outside it are not shown. */
#define OS_DISPLAY_ROUND     1U

#ifndef OS_DISPLAY_MAX
#define OS_DISPLAY_MAX 2U
#endif
#define OS_DISPLAY_NAME_MAX 32U

typedef struct OsDisplayInfo
{
    uint16_t width;
    uint16_t height;
    uint8_t shape;            /* OS_DISPLAY_RECTANGLE or OS_DISPLAY_ROUND */
    uint8_t format;           /* the panel's own format, the fastest to write */
    uint16_t dpi;             /* pixels per inch, 0 when unknown */
    uint32_t formats;         /* OS_DISPLAY_FORMAT_BIT of each format write accepts; includes format */
    uint8_t has_brightness;   /* set_brightness works */
    uint8_t brightness;       /* the level at registration, 0-255 */
    uint8_t keeps_image;      /* keeps its picture while the board sleeps: a saved system may hold it */
} OsDisplayInfo;

/*
 * write puts a rectangle of pixels on the display; pixels is valid only
 * during the call. fill paints a rectangle one color (a pixel value in
 * format); without it, or when it returns OS_STATUS_UNSUPPORTED for that
 * display or format, the kernel writes rows. flush makes everything written
 * visible: a port that buffers (e-paper, a frame it sends whole) shows it
 * now; one that writes the panel directly may leave it NULL.
 */
typedef struct OsDisplayPort
{
    void* context;
    OsStatus (*write)(void* context, uint32_t display, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t format, const void* pixels, uint32_t size);
    OsStatus (*fill)(void* context, uint32_t display, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                     uint32_t format, uint32_t color);
    OsStatus (*flush)(void* context, uint32_t display);
    OsStatus (*set_brightness)(void* context, uint32_t display, uint8_t level);
} OsDisplayPort;

/* NULL removes the port; programs then get -WR_EUNSUPPORTED. */
OsStatus os_display_port_set(const OsDisplayPort* port);
/*
 * Make display number display available to programs under name.
 * OS_STATUS_BUSY when the name or number is taken, OS_STATUS_LIMIT_REACHED
 * past OS_DISPLAY_MAX.
 */
OsStatus os_display_add(const char* name, uint32_t display, const OsDisplayInfo* info);
/* The id of the task that owns a display, 0 when it is free or unknown. */
uint32_t os_display_get_owner(uint32_t display);

/*
 * Input devices: a group of buttons, a rotary control such as a bezel or a
 * knob, a touch surface, a keyboard. The platform registers each under the
 * device profile's name ("buttons", "bezel", "touch", "keyboard") and reports
 * what happens; the kernel queues events for the program that owns the
 * device and makes long presses and key repeat from the timings given here,
 * so they feel the same in every program.
 *
 * Keys and buttons use Linux input event codes (KEY_*, BTN_*); rotation is
 * reported for a REL_* code, such as REL_DIAL for a bezel or REL_WHEEL.
 * Report from the context that calls os_schedule, not from an interrupt
 * handler.
 */
#define OS_INPUT_BUTTONS  1U
#define OS_INPUT_ROTARY   2U
#define OS_INPUT_TOUCH    3U
#define OS_INPUT_KEYBOARD 4U

#ifndef OS_INPUT_MAX
#define OS_INPUT_MAX 8U
#endif
/* Events a device holds for its program before old ones are dropped. */
#ifndef OS_INPUT_QUEUE
#define OS_INPUT_QUEUE 32U
#endif
/* Keys a device tracks as held, for long presses and repeat. */
#ifndef OS_INPUT_MAX_HELD
#define OS_INPUT_MAX_HELD 4U
#endif
#define OS_INPUT_NAME_MAX 32U

typedef struct OsInputInfo
{
    uint8_t kind;                /* OS_INPUT_* */
    uint16_t width;              /* touch: coordinates run 0..width-1, 0..height-1 */
    uint16_t height;
    uint16_t steps_per_turn;     /* rotary: detents in a full turn, 0 when unknown */
    uint16_t long_press_ms;      /* keys held this long report a long press; 0: never */
    uint16_t repeat_delay_ms;    /* keys held this long start repeating; 0: never */
    uint16_t repeat_interval_ms; /* then repeat this often */
} OsInputInfo;

/* OS_STATUS_BUSY when the name or number is taken, OS_STATUS_LIMIT_REACHED past OS_INPUT_MAX. */
OsStatus os_input_add(const char* name, uint32_t device, const OsInputInfo* info);
/* A key or button went down (1) or up (0). */
OsStatus os_input_key(uint32_t device, uint32_t code, uint8_t down);
/* steps detents on axis code (REL_*), positive clockwise. */
OsStatus os_input_rotate(uint32_t device, uint32_t code, int32_t steps);
/* Contact contact touched (OS_INPUT_TOUCH_DOWN), moved, or lifted at x, y. */
#define OS_INPUT_TOUCH_UP   0U
#define OS_INPUT_TOUCH_DOWN 1U
#define OS_INPUT_TOUCH_MOVE 2U
OsStatus os_input_touch(uint32_t device, uint32_t contact, uint32_t phase, uint32_t x, uint32_t y);
/* Text a keyboard produced, one Unicode code point, besides its key events. */
OsStatus os_input_text(uint32_t device, uint32_t code_point);
/* The id of the task that owns a device, 0 when it is free or unknown. */
uint32_t os_input_get_owner(uint32_t device);

/*
 * Buses. The platform registers each bus under the device profile's name
 * ("i2c0", "spi0", "ow0") and moves the bytes through a port; the kernel lends
 * devices on a bus to programs and checks every transaction, so a port only
 * ever sees whole, valid transactions. Data pointers point into the
 * program's memory and are valid during the call.
 *
 * A transaction runs to the end before the next one starts: one program's
 * bytes never mix with another's.
 *
 * A port may work in the background (DMA): transaction starts the transfer
 * and returns OS_STATUS_PENDING, and the port reports the end with
 * os_bus_complete. Only the calling program waits meanwhile; the others
 * run. Until then the ops array and every buffer stay valid and untouched by
 * the kernel: the program is not swapped out, and before it is paused,
 * cancelled or ended the kernel calls finish, which returns only once the
 * transfer is over and reported. A port that never returns
 * OS_STATUS_PENDING may leave finish NULL.
 */
#ifndef OS_BUS_MAX_OPS
#define OS_BUS_MAX_OPS 16U
#endif
#define OS_BUS_NAME_MAX 32U

/* The ops of a transaction; see I2C, SPI and 1-Wire below. */
#define OS_I2C_WRITE 1U
#define OS_I2C_READ  2U

typedef struct OsI2cOp
{
    uint8_t kind;      /* OS_I2C_WRITE or OS_I2C_READ */
    uint8_t* data;     /* written from, or read into */
    uint32_t length;
} OsI2cOp;

#define OS_SPI_WRITE    1U
#define OS_SPI_READ     2U
#define OS_SPI_TRANSFER 3U
#define OS_SPI_DELAY_US 4U

typedef struct OsSpiOp
{
    uint8_t kind;          /* OS_SPI_* */
    const uint8_t* write;  /* WRITE and TRANSFER */
    uint8_t* read;         /* READ and TRANSFER */
    uint32_t length;       /* bytes, or microseconds for DELAY_US */
} OsSpiOp;

#define OS_ONEWIRE_WRITE      1U
#define OS_ONEWIRE_READ       2U
#define OS_ONEWIRE_WRITE_BITS 3U
#define OS_ONEWIRE_READ_BITS  4U
#define OS_ONEWIRE_POWER      5U
#define OS_ONEWIRE_RESET      6U

typedef struct OsOneWireOp
{
    uint8_t kind;      /* OS_ONEWIRE_* */
    uint8_t* data;     /* written from, or read into; bits least significant first */
    uint32_t length;   /* bytes, bits for the _BITS kinds, milliseconds for POWER */
} OsOneWireOp;

/*
 * I2C: ops in order, a repeated start where a read follows a write or a
 * write follows a read, a stop at the end. Adjacent ops of the same kind
 * continue without a start. No ops: just the address, to see whether a
 * device answers. Return OS_STATUS_NACK when the address or a byte is not
 * acknowledged, OS_STATUS_BUS_ERROR for a lost arbitration or a stuck bus,
 * OS_STATUS_TIMEOUT when a device stretches the clock too long.
 */
#ifndef OS_I2C_MAX_BUSES
#define OS_I2C_MAX_BUSES 2U
#endif
/* Devices programs may hold open at once, over all buses. */
#ifndef OS_I2C_MAX_DEVICES
#define OS_I2C_MAX_DEVICES 16U
#endif

typedef struct OsI2cPort
{
    void* context;
    OsStatus (*transaction)(void* context, uint32_t bus, uint32_t address, uint32_t frequency_hz,
                            const OsI2cOp* ops, uint32_t count);
    void (*finish)(void* context, uint32_t bus);
} OsI2cPort;

/* NULL removes the port; programs then get -WR_EUNSUPPORTED. */
OsStatus os_i2c_port_set(const OsI2cPort* port);
/* OS_STATUS_BUSY when the name or number is taken, OS_STATUS_LIMIT_REACHED past OS_I2C_MAX_BUSES. */
OsStatus os_i2c_add_bus(const char* name, uint32_t bus, uint32_t frequency_hz);
/* The id of the task that has address open on bus, 0 when none. */
uint32_t os_i2c_get_owner(uint32_t bus, uint32_t address);

/*
 * SPI: chip select (when the kernel drives it) goes low before the first op
 * and high after the last, also after an error. A read sends zeros; a
 * transfer sends and receives at once, and may read into the bytes it
 * sends. A delay waits length microseconds with chip select held.
 * mode: OS_SPI_CPHA | OS_SPI_CPOL (SPI modes 0-3), OS_SPI_LSB_FIRST.
 */
#define OS_SPI_CPHA      0x01U
#define OS_SPI_CPOL      0x02U
#define OS_SPI_LSB_FIRST 0x04U

#ifndef OS_SPI_MAX_BUSES
#define OS_SPI_MAX_BUSES 2U
#endif
#ifndef OS_SPI_MAX_DEVICES
#define OS_SPI_MAX_DEVICES 8U
#endif

typedef struct OsSpiPort
{
    void* context;
    OsStatus (*transaction)(void* context, uint32_t bus, uint32_t mode, uint32_t frequency_hz,
                            const OsSpiOp* ops, uint32_t count);
    void (*finish)(void* context, uint32_t bus);
} OsSpiPort;

OsStatus os_spi_port_set(const OsSpiPort* port);
/* max_hz caps what programs ask for. */
OsStatus os_spi_add_bus(const char* name, uint32_t bus, uint32_t max_hz);
/* The id of the task that holds bus exclusively or has a device on it with chip select pin cs_pin; 0 when none. */
uint32_t os_spi_get_owner(uint32_t bus, uint32_t cs_pin);

/*
 * 1-Wire: one data line with a pull-up, devices told apart by a 64-bit ROM
 * code (family code in the low byte, CRC in the high one). Ops in order:
 *   RESET       a reset pulse; OS_STATUS_NACK when no device answers with a
 *               presence pulse
 *   WRITE/READ  bytes, least significant bit first; a read sends 1 slots
 *   WRITE_BITS/READ_BITS  single time slots: length bits, packed least
 *               significant first
 *   POWER       right after a write, hold the line high through a strong
 *               pull-up (parasite-powered devices converting or copying)
 *               for length milliseconds; 0 keeps it high until the next
 *               transaction on the bus (POWER 0 comes last). Ports that
 *               cannot drive the line return OS_STATUS_UNSUPPORTED.
 * A transaction without ops only ends a strong pull-up left on.
 *
 * For a program's device the kernel sends RESET and the ROM command (match
 * ROM with the code) itself before the program's ops. triplet, when the
 * port has it, runs one step of a ROM search: it reads a bit and its
 * complement, writes the bit chosen (that bit when they differ, direction
 * when both are 0) and returns bit 0 = the bit, bit 1 = the complement,
 * bit 2 = the direction written. Without it the kernel searches with bit
 * ops. A search runs at once: a port returning OS_STATUS_PENDING is
 * finished on the spot.
 */
#ifndef OS_ONEWIRE_MAX_BUSES
#define OS_ONEWIRE_MAX_BUSES 2U
#endif
/* Devices programs may hold open at once, over all buses. */
#ifndef OS_ONEWIRE_MAX_DEVICES
#define OS_ONEWIRE_MAX_DEVICES 8U
#endif
#define OS_ONEWIRE_MAX_POWER_MS 65535U
/* os_onewire_get_owner: the program that holds the whole bus. */
#define OS_ONEWIRE_WHOLE_BUS 0U

typedef struct OsOneWirePort
{
    void* context;
    OsStatus (*transaction)(void* context, uint32_t bus, const OsOneWireOp* ops, uint32_t count);
    OsStatus (*triplet)(void* context, uint32_t bus, uint8_t direction, uint8_t* out_bits);
    void (*finish)(void* context, uint32_t bus);
} OsOneWirePort;

OsStatus os_onewire_port_set(const OsOneWirePort* port);
/*
 * A bus on a pin is best registered under the pin's name ("gpio4"), which
 * is what Arduino's OneWire(4) opens.
 */
OsStatus os_onewire_add_bus(const char* name, uint32_t bus);
/* The id of the task that has rom open on bus (OS_ONEWIRE_WHOLE_BUS: holds the bus), 0 when none. */
uint32_t os_onewire_get_owner(uint32_t bus, uint64_t rom);
/* The 1-Wire CRC-8 (polynomial x^8 + x^5 + x^4 + 1) of length bytes. */
uint8_t os_onewire_crc8(const uint8_t* data, uint32_t length);

/*
 * Every finished transaction, for bus analyzers and logs: which program,
 * which bus and device, the ops with the bytes written and read (valid
 * during the call) and the result. device is the I2C address, or the chip
 * select pin (OS_BUS_NO_CS when the program drives it), or for 1-Wire 0:
 * the ROM goes out in the ops (the kernel's RESET and match ROM included).
 */
#define OS_BUS_I2C 1U
#define OS_BUS_SPI 2U
#define OS_BUS_ONEWIRE 3U
#define OS_BUS_NO_CS 0xFFFFFFFFU

typedef struct OsBusTrace
{
    uint8_t kind;           /* OS_BUS_I2C, OS_BUS_SPI or OS_BUS_ONEWIRE */
    uint32_t bus;
    uint32_t device;
    uint32_t task_id;
    uint32_t frequency_hz;
    const void* ops;        /* const OsI2cOp*, const OsSpiOp* or const OsOneWireOp* */
    uint32_t count;
    OsStatus status;
} OsBusTrace;

typedef void (*OsBusTraceFunction)(void* context, const OsBusTrace* trace);
/* NULL stops tracing. */
void os_bus_trace_set(OsBusTraceFunction function, void* context);

/*
 * The end of a transfer a port started with OS_STATUS_PENDING, with its
 * result (kind is OS_BUS_I2C, OS_BUS_SPI or OS_BUS_ONEWIRE). Safe to call
 * from an interrupt handler: it only records the result; the next
 * os_schedule (or finish) delivers it to the waiting program. With a tickless clock, wake the
 * kernel afterwards as for os_gpio_edge.
 */
void os_bus_complete(uint32_t kind, uint32_t bus, OsStatus status);
/* Whether a background transfer is in progress on a bus. */
uint8_t os_bus_is_busy(uint32_t kind, uint32_t bus);

/*
 * Analog inputs, registered under the device profile's names ("a0",
 * "battery"). Reading changes nothing, so programs share a channel. read
 * returns the converter's raw value; read_mv, when the port has one, a
 * calibrated voltage. Without it, or when it returns OS_STATUS_UNSUPPORTED
 * for a channel, the kernel scales the raw value.
 */
#ifndef OS_ADC_MAX
#define OS_ADC_MAX 8U
#endif
#define OS_ADC_NAME_MAX 32U

typedef struct OsAdcInfo
{
    uint8_t resolution_bits;   /* raw values run 0 .. 2^bits - 1 */
    uint32_t full_scale_mv;    /* the voltage of the largest raw value */
} OsAdcInfo;

typedef struct OsAdcPort
{
    void* context;
    OsStatus (*read)(void* context, uint32_t channel, uint32_t* out_raw);
    OsStatus (*read_mv)(void* context, uint32_t channel, uint32_t* out_mv);
} OsAdcPort;

OsStatus os_adc_port_set(const OsAdcPort* port);
/* OS_STATUS_BUSY when the name or number is taken, OS_STATUS_LIMIT_REACHED past OS_ADC_MAX. */
OsStatus os_adc_add_channel(const char* name, uint32_t channel, const OsAdcInfo* info);

/*
 * Analog outputs, registered under the device profile's names. One program
 * owns an output at a time. A channel named like a GPIO pin ("gpio25") is
 * that pin: opening it takes the pin as an opened GPIO would, so nothing
 * else drives it, and closing gives it back as an input. Other names are
 * outputs of their own (an external DAC chip).
 *
 * start (may be NULL) switches the channel on; write sets a raw value,
 * 0 .. 2^resolution_bits - 1, already checked; stop switches it off. The
 * output is 0 when a program opens it. Voltages are scaled by the kernel
 * from full_scale_mv, the voltage of the largest raw value.
 */
#ifndef OS_DAC_MAX
#define OS_DAC_MAX 4U
#endif
#define OS_DAC_NAME_MAX 32U

typedef struct OsDacInfo
{
    uint8_t resolution_bits;
    uint32_t full_scale_mv;
} OsDacInfo;

typedef struct OsDacPort
{
    void* context;
    OsStatus (*start)(void* context, uint32_t channel);
    OsStatus (*write)(void* context, uint32_t channel, uint32_t raw);
    void (*stop)(void* context, uint32_t channel);
} OsDacPort;

OsStatus os_dac_port_set(const OsDacPort* port);
/* OS_STATUS_BUSY when the name or number is taken, OS_STATUS_LIMIT_REACHED past OS_DAC_MAX. */
OsStatus os_dac_add_channel(const char* name, uint32_t channel, const OsDacInfo* info);
/* The id of the task that owns a channel, 0 when it is free. */
uint32_t os_dac_get_owner(uint32_t channel);

/*
 * Pulse-width outputs on GPIO pins that have OS_GPIO_CAN_PWM. A program owns
 * the pin as it would through gpio; closing gives it back as an input.
 * Duty is 0 (always low) to 65535 (always high) on the pin; the kernel has
 * already applied the board's and the program's inversion. A port without a
 * free channel or timer for the pin returns OS_STATUS_LIMIT_REACHED; it may
 * round the frequency and reports the one it set.
 */
#define OS_PWM_DUTY_MAX 65535U

typedef struct OsPwmPort
{
    void* context;
    OsStatus (*start)(void* context, uint32_t pin, uint32_t frequency_hz, uint32_t duty, uint32_t* out_frequency_hz);
    OsStatus (*set_duty)(void* context, uint32_t pin, uint32_t duty);
    OsStatus (*set_frequency)(void* context, uint32_t pin, uint32_t frequency_hz, uint32_t* out_frequency_hz);
    void (*stop)(void* context, uint32_t pin);
} OsPwmPort;

OsStatus os_pwm_port_set(const OsPwmPort* port);

/*
 * Serial ports, registered under the device profile's names ("uart1",
 * "gps"), one owner each. Received bytes go into a kernel buffer the port
 * fills with os_uart_receive; sending hands bytes to the port's write,
 * which takes what fits in its own buffer and returns how many. When it
 * has room again it calls os_uart_writable. Both are safe in interrupt
 * handlers; the next os_schedule wakes the programs waiting for them.
 */
#ifndef OS_UART_MAX
#define OS_UART_MAX 4U
#endif
/* Bytes the kernel keeps for a program between reads; a power of two. */
#ifndef OS_UART_RX_BUFFER
#define OS_UART_RX_BUFFER 256U
#endif
#define OS_UART_NAME_MAX 32U

/* Frame format for configure; 8 data bits, no parity, 1 stop bit when 0. */
#define OS_UART_PARITY_EVEN 0x01U
#define OS_UART_PARITY_ODD  0x02U
#define OS_UART_STOP_2      0x04U
#define OS_UART_DATA_7      0x08U
#define OS_UART_FLOW_RTS_CTS 0x10U

typedef struct OsUartPort
{
    void* context;
    OsStatus (*configure)(void* context, uint32_t uart, uint32_t baud, uint32_t format, uint32_t* out_baud);
    /* Takes up to size bytes into the port's own buffer; how many in *out_written. */
    OsStatus (*write)(void* context, uint32_t uart, const uint8_t* data, uint32_t size, uint32_t* out_written);
    /* The port stops receiving: the program closed the port. May be NULL. */
    void (*close)(void* context, uint32_t uart);
} OsUartPort;

OsStatus os_uart_port_set(const OsUartPort* port);
/* max_baud caps what programs ask for (0: no cap). OS_STATUS_BUSY or OS_STATUS_LIMIT_REACHED as above. */
OsStatus os_uart_add(const char* name, uint32_t uart, uint32_t max_baud);
/* Received bytes; those that do not fit are dropped and counted. */
void os_uart_receive(uint32_t uart, const uint8_t* data, uint32_t size);
/* A frame error, parity error or overrun in the port: counted. */
void os_uart_error(uint32_t uart);
/* The port's send buffer has room again. */
void os_uart_writable(uint32_t uart);
/* The id of the task that owns a port, 0 when it is free. */
uint32_t os_uart_get_owner(uint32_t uart);

/*
 * Counters, registered under the device profile's names ("enc0", "flow"):
 * a pulse counter on one pin, or a quadrature encoder on two. Hardware
 * counts (a pulse counter unit, a timer in encoder mode), so no edge is
 * lost to a busy program; one owner each.
 *
 * start begins counting from 0 in a mode (the edges counted, or the
 * encoder's resolution) with a glitch filter the port rounds to what it has
 * (0: none) and reports. read returns the count as 32 bits that wrap: a
 * port with a 16-bit counter extends it on its overflow interrupt. write
 * sets the count.
 *
 * watch, when the port has it, arms the hardware to call
 * os_counter_event once the count goes below low or above high (extra
 * calls are harmless: the kernel reads the count again). Without it the
 * kernel reads watched counters every OS_COUNTER_POLL_MS.
 */
#ifndef OS_COUNTER_MAX
#define OS_COUNTER_MAX 4U
#endif
#ifndef OS_COUNTER_POLL_MS
#define OS_COUNTER_POLL_MS 10U
#endif
#define OS_COUNTER_NAME_MAX 32U

#define OS_COUNTER_PULSE      1U  /* one pin */
#define OS_COUNTER_QUADRATURE 2U  /* two pins, A and B */

/* Modes: the edges a pulse counter counts, or an encoder's steps per cycle. */
#define OS_COUNTER_RISING  1U
#define OS_COUNTER_FALLING 2U
#define OS_COUNTER_BOTH    3U
#define OS_COUNTER_X1      4U
#define OS_COUNTER_X2      5U
#define OS_COUNTER_X4      6U

typedef struct OsCounterInfo
{
    uint8_t kind;      /* OS_COUNTER_PULSE or OS_COUNTER_QUADRATURE */
    uint32_t max_hz;   /* the fastest edges it counts, 0 when unknown */
} OsCounterInfo;

typedef struct OsCounterPort
{
    void* context;
    OsStatus (*start)(void* context, uint32_t unit, uint32_t mode, uint32_t filter_ns, uint32_t* out_filter_ns);
    OsStatus (*read)(void* context, uint32_t unit, int32_t* out_count);
    OsStatus (*write)(void* context, uint32_t unit, int32_t count);
    /* May be NULL. */
    OsStatus (*watch)(void* context, uint32_t unit, int32_t low, int32_t high);
    void (*stop)(void* context, uint32_t unit);
} OsCounterPort;

OsStatus os_counter_port_set(const OsCounterPort* port);
/* OS_STATUS_BUSY when the name or number is taken, OS_STATUS_LIMIT_REACHED past OS_COUNTER_MAX. */
OsStatus os_counter_add(const char* name, uint32_t unit, const OsCounterInfo* info);
/* The count left the window given to watch. Safe in an interrupt handler. */
void os_counter_event(uint32_t unit);
/* The id of the task that owns a counter, 0 when it is free. */
uint32_t os_counter_get_owner(uint32_t unit);

/*
 * Timed pulse trains on GPIO pins that have OS_GPIO_CAN_PULSE, for what needs
 * timing finer than a program can keep: addressable LEDs, infrared remotes,
 * echo and one-wire-like sensors. Hardware made for it (a remote-control
 * peripheral, programmable I/O, a timer with DMA) does the work.
 *
 * An item is 32 bits: OS_PULSE_HIGH for the level, the low 31 bits the time
 * in nanoseconds. The port rounds to its resolution.
 *
 * send, send_bits and receive may return OS_STATUS_PENDING and report the
 * end with os_pulse_complete; meanwhile the items and data stay valid and
 * belong to the port, exactly as for background bus transfers, and finish
 * must end the work at once. receive writes captured items into the
 * program's buffer: it starts at the first edge, ends after idle_ns without
 * one or when the buffer is full, and fails with OS_STATUS_TIMEOUT when no
 * edge comes within timeout_ms (the port times it, so the buffer is never
 * left to it after the kernel has moved on).
 */
#define OS_PULSE_HIGH       0x80000000U
#define OS_PULSE_TIME_MASK  0x7FFFFFFFU
#define OS_PULSE_RECEIVE    0x01U  /* an input that captures; otherwise an output */
#define OS_PULSE_IDLE_HIGH  0x02U  /* outputs: the level between trains */
#ifndef OS_PULSE_MAX
#define OS_PULSE_MAX 4U
#endif

typedef struct OsPulsePort
{
    void* context;
    /* The pin's direction and idle level; OS_STATUS_LIMIT_REACHED when no channel is free. */
    OsStatus (*configure)(void* context, uint32_t pin, uint32_t flags);
    /* carrier_hz 0: plain levels; otherwise high items are that carrier (infrared). */
    OsStatus (*send)(void* context, uint32_t pin, const uint32_t* items, uint32_t count, uint32_t carrier_hz);
    /*
     * Bytes, most significant bit first: a 0 bit is timing[0] ns high then
     * timing[1] ns low, a 1 bit timing[2] high then timing[3] low. May be
     * NULL: programs then get -WR_EUNSUPPORTED.
     */
    OsStatus (*send_bits)(void* context, uint32_t pin, const uint8_t* data, uint32_t length, const uint32_t timing[4]);
    /* May be NULL when no pin can capture. Done at once: the items written in *out_count. */
    OsStatus (*receive)(void* context, uint32_t pin, uint32_t* items, uint32_t max, uint32_t idle_ns,
                        uint32_t timeout_ms, uint32_t* out_count);
    void (*finish)(void* context, uint32_t pin);
    /* The pin goes back to the kernel: stop and free its channel. */
    void (*release)(void* context, uint32_t pin);
    /* Nanoseconds of one tick of the hardware, for programs that want to know. */
    uint32_t resolution_ns;
} OsPulsePort;

OsStatus os_pulse_port_set(const OsPulsePort* port);
/*
 * The end of work a port started with OS_STATUS_PENDING: its result and,
 * for receive, how many items it wrote. Safe in an interrupt handler.
 */
void os_pulse_complete(uint32_t pin, OsStatus status, uint32_t count);

uint32_t os_get_task_count(void);
uint32_t os_get_ready_task_count(void);
uint32_t os_get_waiting_task_count(void);
uint32_t os_get_timer_count(void);

#endif
