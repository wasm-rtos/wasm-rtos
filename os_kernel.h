#ifndef WASM_RTOS_OS_KERNEL_H
#define WASM_RTOS_OS_KERNEL_H

/*
 * What the kernel's own files share: os.c (start, programs, guest calls),
 * task.c (tasks and processes), sync.c (queues, mutexes, semaphores, event
 * groups, notifications, timers), sched.c (clock, waits, scheduler) and
 * snapshot.c (snapshots and swap). Not for drivers or the ABI layer: they use
 * os_internal.h.
 */

#include "os.h"
#include "fs.h"
#include "driver.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OS_TASK_NAME_MAX 32U
#define OS_ERROR_DETAIL_MAX 96U
#if OS_HAS_PROCESSES
#ifndef OS_MAX_SPAWNED_TASKS
#define OS_MAX_SPAWNED_TASKS 16U
#endif
#define OS_PROC_MAX_ARGS 32U
#define OS_PROC_MAX_ARG_LENGTH 256U
#endif
/*
 * Interpreter stack for a program that neither asks for a size nor gets one
 * from its creator. Measured needs: 16 B for a trivial program, 0.5 KiB for a
 * printf hello world, 2 KiB for CoreMark; overflow traps cleanly.
 */
#ifndef OS_DEFAULT_STACK_SIZE
#define OS_DEFAULT_STACK_SIZE (8U * 1024U)
#endif
/* Fill for unused interpreter stack, so the high-water mark can be found. */
#define OS_STACK_PAINT 0xA5U
/* Functions a trap's error text names at most. */
#define OS_TRACE_FRAMES 8U
/* Interpreter work units per scheduler slice; override at build time. */
#ifndef OS_TASK_SLICE_FUEL
#define OS_TASK_SLICE_FUEL 10000ULL
#endif
#if OS_HAS_DYLINK
#define OS_DYLINK_LINEAR_STACK_SIZE (64U * 1024U)
#endif

typedef enum OsTaskRunResult
{
    OS_TASK_RUN_FINISHED = 0,
    OS_TASK_RUN_EXITED,
    OS_TASK_RUN_FUEL_EXHAUSTED,
    OS_TASK_RUN_SUSPENDED,
    OS_TASK_RUN_ERROR
} OsTaskRunResult;

typedef enum OsTaskWaitType
{
    OS_TASK_WAIT_NONE = 0,
    OS_TASK_WAIT_DELAY,
    OS_TASK_WAIT_QUEUE_SEND,
    OS_TASK_WAIT_QUEUE_RECEIVE,
    OS_TASK_WAIT_MUTEX,
    OS_TASK_WAIT_SEMAPHORE,
    OS_TASK_WAIT_EVENT_GROUP,
    OS_TASK_WAIT_NOTIFICATION,
    OS_TASK_WAIT_NOTIFICATION_TAKE,
    OS_TASK_WAIT_CHILD,
    OS_TASK_WAIT_ANY,
    /* A bus or pulse transfer in the background: the port uses the task's memory until it ends. */
    OS_TASK_WAIT_TRANSFER,
    OS_TASK_WAIT_PAGE,
    /* Something outside the kernel's objects, by key (os_signal_wait). */
    OS_TASK_WAIT_SIGNAL
} OsTaskWaitType;

typedef enum OsWaitReturnKind
{
    OS_WAIT_RETURN_STATUS = 0,
    OS_WAIT_RETURN_VALUE,
    /* Guest ABI call: the value on success, a negative ABI error otherwise. */
    OS_WAIT_RETURN_ABI
} OsWaitReturnKind;

typedef struct OsLastError
{
    OsStatus status;
    const char* result;
    const char* phase;
    char task_name[OS_TASK_NAME_MAX];
    uint8_t has_task_name;
    char detail[OS_ERROR_DETAIL_MAX];
} OsLastError;
#if OS_HAS_MEMORY_ACCOUNTING
/* Interpreter memory charged to a task: what it holds now and the most it has held. */
typedef struct OsMemoryAccount
{
    size_t bytes;
    size_t peak;
} OsMemoryAccount;

/* Ahead of every interpreter allocation: its size and account, padded to keep the block aligned. */
typedef union OsMemoryHeader
{
    struct
    {
        size_t size;
        OsMemoryAccount* account;
    } info;
    long double align;
    void* pointer;
} OsMemoryHeader;
#endif

/*
 * Host functions made for import tables, shared by every task and kept
 * until os_shutdown: one per table entry that a program used.
 */
typedef struct OsHostFunctionNode
{
    char* signature;
    OsHostImportFunction function;
    WmFunc* func;
    struct OsHostFunctionNode* next;
} OsHostFunctionNode;

typedef struct OsHostImportNode
{
    char* module_name;
    char* import_name;
    char* signature;
    OsHostImportFunction function;
    struct OsHostImportNode* next;
} OsHostImportNode;
#if OS_HAS_DYLINK
struct OsWasmLibrary
{
    uint32_t id;
    char* module_name;
    uint8_t* wasm_bytes;
    uint32_t wasm_size;
    struct OsWasmLibrary* next;
};
#endif

struct OsQueue
{
    uint32_t id;
    uint32_t item_size;
    uint32_t item_count;
    uint32_t count;
    uint32_t head;
    uint32_t tail;
    uint8_t* storage;
    /* Set for objects created through the guest ABI (os_internal.h). */
    char* name;
    uint32_t guest_refs;
    uint8_t guest_owned;
    struct OsQueue* previous;
    struct OsQueue* next;
};

struct OsMutex
{
    uint32_t id;
    OsTaskHandle owner;
    /* Set for objects created through the guest ABI (os_internal.h). */
    char* name;
    uint32_t guest_refs;
    uint8_t guest_owned;
    struct OsMutex* previous;
    struct OsMutex* next;
};

struct OsSemaphore
{
    uint32_t id;
    uint32_t max_count;
    uint32_t count;
    /* Set for objects created through the guest ABI (os_internal.h). */
    char* name;
    uint32_t guest_refs;
    uint8_t guest_owned;
    struct OsSemaphore* previous;
    struct OsSemaphore* next;
};

struct OsEventGroup
{
    uint32_t id;
    uint32_t bits;
    /* Set for objects created through the guest ABI (os_internal.h). */
    char* name;
    uint32_t guest_refs;
    uint8_t guest_owned;
    struct OsEventGroup* previous;
    struct OsEventGroup* next;
};

struct OsTimer
{
    uint32_t id;
    uint32_t period_ms;
    uint32_t expiry_tick_ms;
    OsTaskHandle target_task;
    uint32_t notify_value;
    OsNotifyAction notify_action;
    uint8_t auto_reload;
    uint8_t active;
    /* Counting timers have no target task: they count expirations. */
    uint8_t counting;
    uint32_t expirations;
    /* Set for objects created through the guest ABI (os_internal.h). */
    char* name;
    uint32_t guest_refs;
    uint8_t guest_owned;
    struct OsTimer* previous;
    struct OsTimer* next;
};

typedef struct OsState
{
    OsTaskHandle task_list;
    OsTaskHandle current_task;
    OsTaskHandle last_scheduled_task;
    OsQueueHandle queue_list;
    OsMutexHandle mutex_list;
    OsSemaphoreHandle semaphore_list;
    OsEventGroupHandle event_group_list;
    OsTimerHandle timer_list;
    OsHostImportNode* host_import_list;
#if OS_HAS_DYLINK
    OsWasmLibraryHandle wasm_library_list;
    uint32_t next_wasm_library_id;
    uint32_t wasm_library_count;
#endif
    OsClockPort clock_port;
    uint32_t tick_ms;
    uint64_t tick_ms_extended;
    uint32_t last_time_update_ms;
    uint32_t next_task_id;
    uint32_t next_queue_id;
    uint32_t next_mutex_id;
    uint32_t next_semaphore_id;
    uint32_t next_event_group_id;
    uint32_t next_timer_id;
    uint64_t next_wait_sequence;
#if OS_HAS_PROCESSES
    uint32_t spawned_task_count;
#endif
    uint32_t task_count;
    uint32_t ready_task_count;
    uint32_t waiting_task_count;
    uint32_t timer_count;
    /* Result slot of the import running now, between its begin and end. */
    uint32_t* import_return_slot;
    OsOutputPort output_port;
    uint8_t initialized;
    uint8_t time_update_initialized;
    uint8_t preempt_requested;
    uint8_t clock_wakeup_armed;
    uint8_t processing_time_events;
    uint32_t clock_armed_tick_ms;
    OsLastError last_error;
} OsState;

struct OsTask
{
    uint32_t id;
    char name[OS_TASK_NAME_MAX];
    OsTaskState state;
    OsTaskExitReason exit_reason;
    uint32_t exit_code;
    uint32_t base_priority;
    uint32_t priority;
    uint32_t wake_tick_ms;
    OsTaskWaitType wait_type;
    union OsTaskWaitObject
    {
        OsQueueHandle queue;
        OsMutexHandle mutex;
        OsSemaphoreHandle semaphore;
        OsEventGroupHandle event_group;
    } wait_object;
    uint64_t wait_sequence;
    uint8_t* wait_queue_item;
    uint8_t wait_queue_has_wasm_output;
    uint32_t wait_queue_wasm_output_address;
    uint32_t wait_bits;
    uint8_t wait_clear_on_exit;
    uint8_t wait_for_all;
    uint8_t wait_has_timeout;
    uint8_t wait_has_return_slot;
    uint32_t wait_return_offset;
    OsWaitReturnKind wait_return_kind;
    uint32_t last_wait_value;
    OsStatus last_wait_status;
    /* OS_TASK_WAIT_ANY: the objects waited for. */
    OsWaitTarget* wait_targets;
    uint32_t wait_target_count;
    /* Owned by the guest ABI layer (handle table). */
    void* abi_state;

    uint32_t notification_value;
    uint32_t notification_clear_on_exit;
    uint8_t notification_pending;
    uint8_t notification_take_clear_count;

    uint8_t* wasm_bytes;
    uint32_t wasm_size;
    const char* entry_function_name;
    uint32_t stack_size;
    /* Most interpreter stack used, kept after the runtime is released. */
    uint32_t stack_peak_bytes;
    /* The program's name from its wasm-rtos.app section, or NULL. */
    char* app_name;
    /* Why the task failed (OS_TASK_EXIT_WASM_ERROR), or NULL. */
    char* error_text;
    OsValue* entry_args;
    uint32_t entry_arg_count;

    WmRuntime* wasm_runtime;
    uint8_t* wasm_stack_base;
    WmModule* wasm_module;
    WmInstance* wasm_instance;
    WmFunc* wasm_entry_function;
    /* The entry function's results, in slots, written when it returns. */
    uint32_t* wasm_results;
    /* The code WASI proc_exit gave. */
    uint32_t wasi_exit_code;
    uint8_t wasm_started;
    OsValue* entry_return_values;
    uint32_t entry_return_count;
    uint32_t entry_return_code;
    uint8_t wasm_needs_resume;
    uint8_t delete_requested;
    uint32_t run_count;
    uint64_t fuel_used;
    /* Fuel a stop request took back during the current slice; it was not spent. */
    uint64_t fuel_taken_back;

#if OS_HAS_PROCESSES
    /* Process state for tasks started through os_task_spawn */
    uint32_t parent_id;
    uint32_t wait_child_id;
    uint8_t spawned;
    uint8_t auto_reap;
    uint8_t exit_collected;
    uint8_t reap_pending;
    uint8_t* owned_wasm_bytes;
#if OS_HAS_FS
    /* Program started by path; released with the task. */
    OsFsImage image;
#endif
    char** argv;
    uint32_t argc;
    const char* default_argv[1];
#endif

    /* Swapped out: the interpreter state is in swap storage, not RAM. */
    uint8_t swapped;
    /* 0 when it was swapped out before it started: it starts afresh. */
    /* Swapped out: its state in the page store, its memory kept apart (paged out). */
    OsPageImage swap_image;
    WmMemory* swapped_memory;
    /* What a wait delivered while swapped out, applied when it comes back. */
    uint8_t swap_return_pending;
    uint32_t swap_return_offset;
    uint32_t swap_return_value;

#if OS_HAS_DYLINK
    /* The program and its libraries, for a dylink.0 program. */
    WmDylink* wasm_dylink;
#endif
#if OS_HAS_MEMORY_ACCOUNTING
    /* Interpreter memory allocated while working on this task. */
    OsMemoryAccount memory_account;
#endif

    struct OsTask* previous;
    struct OsTask* next;
};
struct OsImportLink;
#if OS_HAS_MEMORY_ACCOUNTING
/* Charge interpreter allocations to task until the previous account is restored. */
#define OS_HEAP_ENTER(task) \
    OsMemoryAccount* os_previous_memory_account = g_os_memory_account; \
    g_os_memory_account = &(task)->memory_account
#define OS_HEAP_LEAVE() (g_os_memory_account = os_previous_memory_account)
#else
#define OS_HEAP_ENTER(task) (void)(task)
#define OS_HEAP_LEAVE() ((void)0)
#endif

/* Settings a program declares about itself in its wasm-rtos.app section. */
typedef struct OsAppMetadata
{
    uint32_t abi_version;
    uint32_t stack_size;
    const uint8_t* name;
    uint32_t name_length;
} OsAppMetadata;

/* Carries an OsSnapshotWrite/Read callback through the interpreter's stream calls. */
typedef struct OsSnapshotStream
{
    OsSnapshotWrite write;
    OsSnapshotRead read;
    void* context;
    OsStatus status;
} OsSnapshotStream;

/* A snapshot in a caller's buffer; with no buffer it only counts. */
typedef struct OsSnapshotBuffer
{
    uint8_t* data;
    uint32_t size;
    uint32_t used;
} OsSnapshotBuffer;

/* Longest failure text a task keeps. */
#define OS_TASK_ERROR_MAX 192U

/* Imports no host function answers, counted while a program is linked; the first is named in the error. */
typedef struct OsImportLink
{
    /* The program being linked. */
    const WmModule* program;
    uint32_t missing;
    const char* module;
    uint32_t module_length;
    const char* name;
    uint32_t name_length;
} OsImportLink;
#if OS_HAS_PROCESSES

/*
 * The module a process runs and who owns it: heap bytes from os_task_spawn,
 * or a file system image from os_task_spawn_path.
 */
typedef struct OsProcessImage
{
    const uint8_t* bytes;
    uint32_t size;
    uint8_t* owned_bytes;
#if OS_HAS_FS
    OsFsImage file;
#endif
} OsProcessImage;
#endif

/*
 * A copy of a buffer's bytes, with what writing it back takes; a kept one is
 * in a list. A kept pinned buffer has no bytes, only its range to unpin.
 */
struct OsGuestKept
{
    struct OsGuestKept* next;
    uint32_t address;
    uint32_t length;
    uint32_t task_id;
    uint16_t access;
    uint16_t pinned;
    uint8_t data[];
};

typedef struct OsObjectFields
{
    char** name;
    uint32_t* guest_refs;
    uint8_t* guest_owned;
} OsObjectFields;

#define OS_OBJECT_FIELDS_OF(object, out) \
    do { (out)->name = &(object)->name; (out)->guest_refs = &(object)->guest_refs; \
         (out)->guest_owned = &(object)->guest_owned; } while (0)

#define OS_FIND_NAMED(list_head, type_name)                                         \
    do {                                                                            \
        type_name object = NULL;                                                    \
        for (object = (list_head); object != NULL; object = object->next)           \
        {                                                                           \
            if (os_object_name_matches(object->name, name, name_length))            \
            {                                                                       \
                return object->id;                                                  \
            }                                                                       \
        }                                                                           \
        return 0U;                                                                  \
    } while (0)

#if OS_HAS_MEMORY_ACCOUNTING
extern OsMemoryAccount* g_os_memory_account;
#endif
extern const char os_exit_trap[];
extern OsState g_os;

OsStatus os_read_app_metadata(const uint8_t* bytes, uint32_t size, OsAppMetadata* out_metadata);
uint32_t os_task_stack_bytes(OsTaskHandle task);
uint32_t os_task_stack_peak(OsTaskHandle task);
void os_swap_discard(OsTaskHandle task);
void os_swap_forget_deliveries(OsTaskHandle task);
void os_task_record_error(OsTaskHandle task);
void os_queue_free_all(void);
void os_mutex_free_all(void);
void os_semaphore_free_all(void);
int os_event_group_is_in_list(OsEventGroupHandle event_group);
void os_event_group_free_all(void);
void os_timer_free_all(void);
void os_timer_delete_for_task(OsTaskHandle task);
void os_task_free(OsTaskHandle task);
int os_task_is_in_list(OsTaskHandle task);
uint8_t os_task_is_dead(OsTaskHandle task);
uint8_t os_task_snapshot_is_busy(OsTaskHandle task);
void os_recalculate_task_counters(void);
void os_task_free_return_values(OsTaskHandle task);
OsStatus os_task_create_wasm(
    OsTaskHandle task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    uint32_t stack_size
);
OsStatus os_capture_entry_return_value(OsTaskHandle task);
OsStatus os_call_entry_function(OsTaskHandle task, WmResult* out_result);
OsStatus os_task_get_memory_address(
    OsTaskHandle task,
    const void* memory,
    uint32_t byte_count,
    uint32_t* out_wasm_address
);
void os_task_cleanup_wasm(OsTaskHandle task);
OsStatus os_wasm_snapshot_result_to_status(WmResult result);
void os_preempt_for_higher_priority_ready_task(void);
OsStatus os_begin_task_wait(
    OsTaskHandle task,
    OsTaskWaitType wait_type,
    uint32_t timeout_ms
);
void os_complete_task_wait(
    OsTaskHandle task,
    OsStatus status,
    uint32_t value
);
void os_cancel_task_wait(OsTaskHandle task, OsStatus status);
void os_clear_task_wait(OsTaskHandle task);
void os_clear_queue_wait(OsTaskHandle task);
OsStatus os_prepare_wait_return(OsTaskHandle task, uint32_t* raw_return);
void os_clear_wait_return(OsTaskHandle task);
void os_recompute_task_priorities(void);
void os_release_task_mutexes(OsTaskHandle task);
void os_process_timers(void);
OsStatus os_sync_clock_before_deadline(void);
void os_clock_rearm(void);
void os_set_task_ready(OsTaskHandle task);
void os_set_task_waiting(OsTaskHandle task, uint32_t wake_tick_ms);
void os_set_task_suspended(OsTaskHandle task);
#if OS_HAS_PROCESSES
void os_reap_processes(void);
#endif
void os_set_task_dead(OsTaskHandle task);
void os_record_task_exit(OsTaskHandle task, OsTaskExitReason reason, uint32_t code);
void os_request_task_stop(OsTaskHandle task);
void os_destroy_task(OsTaskHandle task);
uint8_t os_time_reached(uint32_t now, uint32_t target);
void os_clear_last_error(void);
void os_set_last_error(OsTaskHandle task, const char* phase, WmResult result, OsStatus status);
OsStatus os_import_wait_begin(uint32_t* raw_return);
WmResult os_import_wait_end(uint8_t abi);
uint8_t os_object_fields(OsObjectType type, uint32_t id, OsObjectFields* out);
uint8_t os_object_name_matches(const char* object_name, const char* name, uint32_t name_length);
void os_wake_any_waiters(void);

#endif
