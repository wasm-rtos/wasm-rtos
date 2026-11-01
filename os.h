#ifndef WASM_RTOS_H
#define WASM_RTOS_H

#include <stdint.h>

#include "wasm3/source/m3_config.h"
#include "wasm3/source/wasm3.h"
#if d_m3HasDylink
#include "wasm3/source/m3_dylink.h"
#endif

/*
 * Build options (define to 0 to leave a feature out of the image):
 *   OS_HAS_PROCESSES  os_task_spawn() and the guest proc interface
 *   OS_HAS_FS         virtual file system behind the WASI file calls (fs.h)
 *   OS_FS_TAR         read-only tar image driver (needs OS_HAS_FS)
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

/* Finite delays, timeouts, and timer periods must not exceed INT32_MAX. */
#define OS_WAIT_FOREVER UINT32_MAX

typedef struct OsTask* OsTaskHandle;
typedef struct OsQueue* OsQueueHandle;
typedef struct OsMutex* OsMutexHandle;
typedef struct OsSemaphore* OsSemaphoreHandle;
typedef struct OsEventGroup* OsEventGroupHandle;
typedef struct OsTimer* OsTimerHandle;
#if d_m3HasDylink
typedef struct OsWasmLibrary* OsWasmLibraryHandle;
#endif

typedef M3RawCall OsHostImportFunction;

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
    OS_STATUS_END_OF_DIRECTORY
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
M3Result os_host_wait_end(void);

#if d_m3HasDylink
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
#if OS_HAS_FS
/*
 * Start the program stored at an absolute file system path. A file whose
 * driver maps it into memory (the tar driver does) runs in place without a
 * copy, and its mount stays busy until the process is destroyed; any other
 * file is read into RAM first. With argc 0 the program gets argv = { path }.
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
 * Swapping moves a task's interpreter state (stack, linear memory, compiled
 * code) out of RAM into storage the platform provides, and brings it back
 * when the task runs again. Unlike a snapshot, the task stays where it is:
 * it keeps waiting, its timers and handles keep working, and whatever its
 * wait delivers meanwhile is applied when it comes back.
 *
 * The port stores one image per task id. write appends: offsets start at 0
 * and grow with each call. read returns bytes written earlier; discard
 * forgets the image. Calls happen from os_task_swap_out, os_task_swap_in and
 * the scheduler, never at the same time.
 */
typedef struct OsSwapPort
{
    void* context;
    OsStatus (*write)(void* context, uint32_t task_id, uint32_t offset, const void* data, uint32_t size);
    OsStatus (*read)(void* context, uint32_t task_id, uint32_t offset, void* data, uint32_t size);
    void (*discard)(void* context, uint32_t task_id);
} OsSwapPort;

/* NULL turns swapping off; tasks already swapped out keep their images. */
OsStatus os_swap_port_set(const OsSwapPort* port);

/*
 * Write the task out and free its interpreter state. Any task but the one
 * running can be swapped out, waiting or not; a task that has not started
 * frees its state without writing anything. OS_STATUS_UNSUPPORTED without a
 * port or for linked tasks; errors from the port leave the task as it was.
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
 * allocated for the task (those three plus module tables, names, types and
 * the like) and needs d_m3HasHeapAccounting=1; otherwise it is 0 and
 * total_bytes leaves that metadata out.
 */
typedef struct OsTaskMemoryUsage
{
    uint32_t stack_bytes;            /* interpreter value stack */
    uint32_t stack_peak_bytes;       /* the most of it the task has used, also after it ends */
    uint32_t linear_memory_bytes;    /* the program's linear memory, with spare room from memory.grow */
    uint32_t code_bytes;             /* compiled code pages */
    uint32_t interpreter_bytes;      /* all interpreter allocations, see above */
    uint32_t interpreter_peak_bytes; /* the most interpreter_bytes has been */
    uint32_t image_bytes;            /* kernel copy of the module; 0 when it runs in place or the host owns it */
    uint32_t swap_bytes;             /* size of the image in swap storage while swapped out, else 0 */
    uint32_t kernel_bytes;           /* task control block */
    uint32_t total_bytes;
} OsTaskMemoryUsage;

OsStatus os_task_get_memory_usage(OsTaskHandle task, OsTaskMemoryUsage* out_usage);

/*
 * Copy to or from a task's linear memory at a wasm address. The whole range
 * must lie inside the memory (OS_STATUS_OUT_OF_BOUNDS), including addresses
 * near 4 GiB. A swapped-out task has no memory to reach (OS_STATUS_BUSY).
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

uint32_t os_get_task_count(void);
uint32_t os_get_ready_task_count(void);
uint32_t os_get_waiting_task_count(void);
uint32_t os_get_timer_count(void);

#endif
