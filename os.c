#include "os.h"
#include "fs.h"
#include "os_internal.h"

#include "wasm3/source/wasm3.h"
#include "wasm3/source/m3_env.h"
#include "wasm3/source/m3_api_wasi.h"
#if d_m3HasDylink
#include "wasm3/source/m3_dylink.h"
#endif

#include <stddef.h>
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
/*
 * Native C stack that nested Wasm calls may take in one slice; past it the
 * slice ends early (resuming starts over at the top of the native stack).
 * Each nested call takes about 100 bytes. 0 = unlimited.
 */
#ifndef OS_NATIVE_STACK_BUDGET
#define OS_NATIVE_STACK_BUDGET (16U * 1024U)
#endif
/* Fill for unused interpreter stack, so the high-water mark can be found. */
#define OS_STACK_PAINT 0xA5U
/* Interpreter work units per scheduler slice; override at build time. */
#ifndef OS_TASK_SLICE_FUEL
#define OS_TASK_SLICE_FUEL 10000ULL
#endif
#if d_m3HasDylink
#define OS_DYLINK_LINEAR_STACK_SIZE (64U * 1024U)
#endif

#if defined(d_m3HasWASI) || defined(d_m3HasMetaWASI) || defined(d_m3HasUVWASI)
#define OS_HAS_WASM3_WASI 1
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
    OS_TASK_WAIT_ANY
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

typedef struct OsHostImportNode
{
    char* module_name;
    char* import_name;
    char* signature;
    OsHostImportFunction function;
    struct OsHostImportNode* next;
} OsHostImportNode;

#if d_m3HasDylink
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
#if d_m3HasDylink
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
    OsSwapPort swap_port;
    uint8_t has_swap_port;
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
    OsValue* entry_args;
    uint32_t entry_arg_count;

    IM3Environment wasm_environment;
    IM3Runtime wasm_runtime;
    uint8_t* wasm_stack_base;
    IM3Module wasm_module;
    IM3Function wasm_entry_function;
    uint8_t wasm_module_loaded;
    uint8_t wasm_started;
    OsValue* entry_return_values;
    uint32_t entry_return_count;
    uint32_t entry_return_code;
    uint8_t wasm_needs_resume;
    uint8_t delete_requested;
    uint32_t run_count;

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
    uint8_t swap_has_image;
    uint32_t swap_bytes;
    /* What a wait delivered while swapped out, applied when it comes back. */
    uint8_t swap_return_pending;
    uint32_t swap_return_offset;
    uint32_t swap_return_value;
    uint8_t* swap_item;
    uint32_t swap_item_address;
    uint32_t swap_item_size;

#if d_m3HasDylink
    uint8_t wasm_dylink_task;
#endif
#if d_m3HasHeapAccounting
    /* Interpreter memory allocated while working on this task. */
    M3HeapAccount* heap_account;
#endif

    struct OsTask* previous;
    struct OsTask* next;
};

static OsState g_os;


static OsQueueHandle os_queue_allocate(void);
static void os_queue_free(OsQueueHandle queue);
static void os_queue_list_insert(OsQueueHandle queue);
static void os_queue_list_remove(OsQueueHandle queue);
static int os_queue_is_in_list(OsQueueHandle queue);
static void os_queue_free_all(void);
static OsMutexHandle os_mutex_allocate(void);
static void os_mutex_free(OsMutexHandle mutex);
static void os_mutex_list_insert(OsMutexHandle mutex);
static void os_mutex_list_remove(OsMutexHandle mutex);
static int os_mutex_is_in_list(OsMutexHandle mutex);
static void os_mutex_free_all(void);
static OsSemaphoreHandle os_semaphore_allocate(void);
static void os_semaphore_free(OsSemaphoreHandle semaphore);
static void os_semaphore_list_insert(OsSemaphoreHandle semaphore);
static void os_semaphore_list_remove(OsSemaphoreHandle semaphore);
static int os_semaphore_is_in_list(OsSemaphoreHandle semaphore);
static void os_semaphore_free_all(void);
static OsEventGroupHandle os_event_group_allocate(void);
static void os_event_group_free(OsEventGroupHandle event_group);
static void os_event_group_list_insert(OsEventGroupHandle event_group);
static void os_event_group_list_remove(OsEventGroupHandle event_group);
static int os_event_group_is_in_list(OsEventGroupHandle event_group);
static void os_event_group_free_all(void);
static OsTimerHandle os_timer_allocate(void);
static void os_timer_free(OsTimerHandle timer);
static void os_timer_list_insert(OsTimerHandle timer);
static void os_timer_list_remove(OsTimerHandle timer);
static int os_timer_is_in_list(OsTimerHandle timer);
static void os_timer_free_all(void);
static void os_timer_delete_for_task(OsTaskHandle task);
static void os_process_timers(void);
static OsTaskHandle os_task_allocate(void);
static void os_task_free(OsTaskHandle task);
static void os_task_list_insert(OsTaskHandle task);
static void os_task_list_remove(OsTaskHandle task);
static int os_task_is_in_list(OsTaskHandle task);
static uint8_t os_task_is_dead(OsTaskHandle task);
static uint8_t os_task_snapshot_is_busy(OsTaskHandle task);
static void os_recalculate_task_counters(void);
static void os_copy_task_name(OsTaskHandle task, const char* task_name);
static OsStatus os_task_initialize(
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
static OsStatus os_task_copy_entry_args(OsTaskHandle task, OsValue* entry_args, uint32_t entry_arg_count);
static void os_task_free_entry_args(OsTaskHandle task);
static void os_task_free_return_values(OsTaskHandle task);
static char* os_duplicate_string(const char* value);
static void os_host_import_free_list(void);
static int os_host_import_exists(const char* module_name, const char* import_name);
#if d_m3HasDylink
static void os_wasm_library_free_all(void);
static M3Result os_dylink_resolve_module(
    void* context,
    const char* name,
    IM3Module* out_module
);
static M3Result os_dylink_link_host_imports(void* context, IM3Module module);
#endif
static OsStatus os_task_create_wasm(
    OsTaskHandle task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    uint32_t stack_size
);
static void os_task_cleanup_wasm(OsTaskHandle task);
static OsStatus os_wasm_result_to_status(M3Result result);
static OsStatus os_wasm_snapshot_result_to_status(M3Result result);
static M3Result os_link_task_host_imports(OsTaskHandle task);
static OsStatus os_check_module_imports(OsTaskHandle task, IM3Module module);
static const char* os_detect_entry_function(IM3Module module);
static M3Result os_link_module_host_imports(IM3Module module);
static M3Result os_link_registered_host_imports(IM3Module module);
static OsTaskHandle os_select_next_ready_task(void);
static void os_preempt_for_higher_priority_ready_task(void);
static OsTaskHandle os_select_queue_waiter(
    OsQueueHandle queue,
    OsTaskWaitType wait_type
);
static OsStatus os_queue_deliver_to_waiting_receiver(
    OsTaskHandle task,
    const void* item
);
static void os_queue_fill_from_waiting_sender(OsQueueHandle queue);
static OsTaskHandle os_select_mutex_waiter(OsMutexHandle mutex);
static OsTaskHandle os_select_semaphore_waiter(OsSemaphoreHandle semaphore);
static void os_update_time_events(void);
static void os_update_waiting_tasks(void);
static void os_advance_tick_ms(uint32_t milliseconds);
static OsStatus os_sync_clock_before_deadline(void);
static void os_clock_rearm(void);
static OsTaskRunResult os_run_task_slice(OsTaskHandle task);
#if d_m3HasHeapAccounting
/* Charge interpreter allocations to task until the returned account is restored. */
#define OS_HEAP_ENTER(task) M3HeapAccount* os_previous_heap_account = m3_SetHeapAccount((task)->heap_account)
#define OS_HEAP_LEAVE() (void)m3_SetHeapAccount(os_previous_heap_account)
#else
#define OS_HEAP_ENTER(task) (void)(task)
#define OS_HEAP_LEAVE() ((void)0)
#endif
static OsStatus os_validate_entry_function(OsTaskHandle task);
static OsStatus os_capture_entry_return_value(OsTaskHandle task);
static OsStatus os_call_entry_function(OsTaskHandle task, M3Result* out_result);
static OsStatus os_task_get_memory_pointer(
    OsTaskHandle task,
    uint32_t wasm_address,
    uint32_t byte_count,
    uint8_t** out_memory
);
static OsStatus os_task_get_memory_address(
    OsTaskHandle task,
    const void* memory,
    uint32_t byte_count,
    uint32_t* out_wasm_address
);
static void os_set_task_ready(OsTaskHandle task);
static void os_set_task_waiting(OsTaskHandle task, uint32_t wake_tick_ms);
static void os_set_task_suspended(OsTaskHandle task);
static void os_set_task_dead(OsTaskHandle task);
#if OS_HAS_PROCESSES
static void os_process_on_task_dead(OsTaskHandle task);
static void os_reap_processes(void);
#if defined(OS_HAS_WASM3_WASI)
static void os_apply_task_wasi_args(OsTaskHandle task);
#endif
#endif
static OsStatus os_begin_task_wait(
    OsTaskHandle task,
    OsTaskWaitType wait_type,
    uint32_t timeout_ms
);
static void os_complete_task_wait(
    OsTaskHandle task,
    OsStatus status,
    uint32_t value
);
static void os_cancel_task_wait(OsTaskHandle task, OsStatus status);
static void os_clear_task_wait(OsTaskHandle task);
static void os_clear_queue_wait(OsTaskHandle task);
static OsStatus os_prepare_wait_return(OsTaskHandle task, uint32_t* raw_return);
static void os_clear_wait_return(OsTaskHandle task);
static void os_write_wait_return(OsTaskHandle task, uint32_t value);
static void os_recompute_task_priorities(void);
static void os_release_task_mutexes(OsTaskHandle task);
static uint8_t os_event_group_condition_met(
    uint32_t current_bits,
    uint32_t bits_to_wait_for,
    uint8_t wait_for_all
);
static void os_event_group_wake_waiters(OsEventGroupHandle event_group);
static void os_complete_notification_wait(OsTaskHandle task);
static void os_record_task_exit(OsTaskHandle task, OsTaskExitReason reason, uint32_t code);
static void os_wake_any_waiters(void);
static void os_request_task_stop(OsTaskHandle task);
static void os_request_task_delete(OsTaskHandle task);
static void os_destroy_task(OsTaskHandle task);
static uint8_t os_time_reached(uint32_t now, uint32_t target);
static void os_clear_last_error(void);
static void os_set_last_error(OsTaskHandle task, const char* phase, M3Result result, OsStatus status);

OsStatus os_init(void)
{
    os_shutdown();

    g_os.next_task_id = 1U;
    g_os.next_queue_id = 1U;
    g_os.next_mutex_id = 1U;
    g_os.next_semaphore_id = 1U;
    g_os.next_event_group_id = 1U;
    g_os.next_timer_id = 1U;
#if d_m3HasDylink
    g_os.next_wasm_library_id = 1U;
#endif
    g_os.next_wait_sequence = 1U;
#if OS_HAS_PROCESSES
    g_os.spawned_task_count = 0U;
#endif
    g_os.initialized = 1U;
    os_clear_last_error();

    return OS_STATUS_OK;
}

void os_shutdown(void)
{
    OsTaskHandle task = g_os.task_list;

    if (g_os.clock_wakeup_armed && g_os.clock_port.cancel_wakeup != NULL)
    {
        g_os.clock_port.cancel_wakeup(g_os.clock_port.context);
    }

    while (task != NULL)
    {
        OsTaskHandle next = task->next;
        os_task_free(task);
        task = next;
    }

#if d_m3HasDylink
    os_wasm_library_free_all();
#endif

    os_queue_free_all();
    os_mutex_free_all();
    os_semaphore_free_all();
    os_event_group_free_all();
    os_timer_free_all();
    os_host_import_free_list();
#if OS_HAS_FS
    os_fs_shutdown();
#endif
#if OS_HAS_GPIO
    os_gpio_shutdown();
#endif

    memset(&g_os, 0, sizeof(g_os));
}

OsStatus os_task_create(
    OsTaskHandle* out_task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    const char* task_name,
    uint32_t stack_size,
    uint32_t priority
)
{
    return os_task_initialize(
        out_task,
        wasm_bytes,
        wasm_size,
        entry_function_name,
        NULL,
        0U,
        task_name,
        stack_size,
        priority
    );
}

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
)
{
    return os_task_initialize(
        out_task,
        wasm_bytes,
        wasm_size,
        entry_function_name,
        entry_args,
        entry_arg_count,
        task_name,
        stack_size,
        priority
    );
}


OsStatus os_queue_create(
    OsQueueHandle* out_queue,
    uint32_t item_size,
    uint32_t item_count
)
{
    OsQueueHandle queue = NULL;
    size_t storage_size = 0U;

    if (out_queue == NULL || item_size == 0U || item_count == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        OsStatus status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    if ((size_t)item_count > ((size_t)-1) / (size_t)item_size)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    *out_queue = NULL;
    storage_size = (size_t)item_size * (size_t)item_count;

    queue = os_queue_allocate();
    if (queue == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    queue->storage = (uint8_t*)calloc(1U, storage_size);
    if (queue->storage == NULL)
    {
        os_queue_free(queue);
        return OS_STATUS_OUT_OF_MEMORY;
    }

    queue->id = g_os.next_queue_id++;
    queue->item_size = item_size;
    queue->item_count = item_count;
    os_queue_list_insert(queue);

    *out_queue = queue;
    return OS_STATUS_OK;
}

void os_queue_delete(OsQueueHandle queue)
{
    OsTaskHandle task = NULL;

    if (queue == NULL || !os_queue_is_in_list(queue))
    {
        return;
    }

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if ((task->wait_type == OS_TASK_WAIT_QUEUE_SEND ||
             task->wait_type == OS_TASK_WAIT_QUEUE_RECEIVE) &&
            task->wait_object.queue == queue)
        {
            os_complete_task_wait(task, OS_STATUS_QUEUE_NOT_FOUND, 0U);
        }
    }

    os_queue_list_remove(queue);
    os_queue_free(queue);
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
}

uint32_t os_queue_get_id(OsQueueHandle queue)
{
    if (queue == NULL || !os_queue_is_in_list(queue))
    {
        return 0U;
    }

    return queue->id;
}

OsQueueHandle os_queue_find_by_id(uint32_t queue_id)
{
    OsQueueHandle queue = NULL;

    if (queue_id == 0U)
    {
        return NULL;
    }

    for (queue = g_os.queue_list; queue != NULL; queue = queue->next)
    {
        if (queue->id == queue_id)
        {
            return queue;
        }
    }

    return NULL;
}

OsStatus os_queue_send(OsQueueHandle queue, const void* item)
{
    OsTaskHandle receiver = NULL;
    uint8_t* destination = NULL;
    OsStatus status = OS_STATUS_OK;

    if (queue == NULL || item == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_queue_is_in_list(queue))
    {
        return OS_STATUS_QUEUE_NOT_FOUND;
    }

    receiver = os_select_queue_waiter(queue, OS_TASK_WAIT_QUEUE_RECEIVE);
    if (receiver != NULL)
    {
        status = os_queue_deliver_to_waiting_receiver(receiver, item);
        os_recalculate_task_counters();
        os_preempt_for_higher_priority_ready_task();
        return status;
    }

    if (queue->count >= queue->item_count)
    {
        return OS_STATUS_QUEUE_FULL;
    }

    destination = queue->storage + ((size_t)queue->tail * (size_t)queue->item_size);
    memcpy(destination, item, queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->item_count;
    ++queue->count;
    os_wake_any_waiters();

    return OS_STATUS_OK;
}

OsStatus os_queue_receive(OsQueueHandle queue, void* out_item)
{
    uint8_t* source = NULL;

    if (queue == NULL || out_item == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_queue_is_in_list(queue))
    {
        return OS_STATUS_QUEUE_NOT_FOUND;
    }

    if (queue->count == 0U)
    {
        return OS_STATUS_QUEUE_EMPTY;
    }

    source = queue->storage + ((size_t)queue->head * (size_t)queue->item_size);
    memcpy(out_item, source, queue->item_size);
    queue->head = (queue->head + 1U) % queue->item_count;
    --queue->count;

    os_queue_fill_from_waiting_sender(queue);
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();

    return OS_STATUS_OK;
}

OsStatus os_queue_send_wait(
    OsQueueHandle queue,
    const void* item,
    uint32_t timeout_ms
)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;

    status = os_queue_send(queue, item);
    if (status != OS_STATUS_QUEUE_FULL)
    {
        if (task != NULL)
        {
            task->last_wait_status = status;
            task->last_wait_value = 0U;
        }
        return status;
    }

    if (timeout_ms == 0U)
    {
        if (task != NULL)
        {
            task->last_wait_status = OS_STATUS_TIMEOUT;
            task->last_wait_value = 0U;
        }
        return OS_STATUS_TIMEOUT;
    }

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    task->wait_queue_item = (uint8_t*)malloc(queue->item_size);
    if (task->wait_queue_item == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    memcpy(task->wait_queue_item, item, queue->item_size);
    task->wait_object.queue = queue;
    task->wait_return_kind = OS_WAIT_RETURN_STATUS;
    status = os_begin_task_wait(task, OS_TASK_WAIT_QUEUE_SEND, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        os_clear_queue_wait(task);
    }

    return status;
}

OsStatus os_queue_receive_wait(
    OsQueueHandle queue,
    void* out_item,
    uint32_t timeout_ms
)
{
    OsTaskHandle task = g_os.current_task;
    uint32_t wasm_output_address = 0U;
    OsStatus status = os_queue_receive(queue, out_item);

    if (status != OS_STATUS_QUEUE_EMPTY)
    {
        if (task != NULL)
        {
            task->last_wait_status = status;
            task->last_wait_value = 0U;
        }
        return status;
    }

    if (timeout_ms == 0U)
    {
        if (task != NULL)
        {
            task->last_wait_status = OS_STATUS_TIMEOUT;
            task->last_wait_value = 0U;
        }
        return OS_STATUS_TIMEOUT;
    }

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    status = os_task_get_memory_address(
        task,
        out_item,
        queue->item_size,
        &wasm_output_address
    );
    if (status != OS_STATUS_OK)
    {
        task->last_wait_status = OS_STATUS_UNSUPPORTED;
        task->last_wait_value = 0U;
        return OS_STATUS_UNSUPPORTED;
    }

    task->wait_object.queue = queue;
    task->wait_queue_has_wasm_output = 1U;
    task->wait_queue_wasm_output_address = wasm_output_address;
    task->wait_return_kind = OS_WAIT_RETURN_STATUS;
    status = os_begin_task_wait(task, OS_TASK_WAIT_QUEUE_RECEIVE, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        os_clear_queue_wait(task);
    }

    return status;
}

uint32_t os_queue_get_count(OsQueueHandle queue)
{
    if (queue == NULL || !os_queue_is_in_list(queue))
    {
        return 0U;
    }

    return queue->count;
}

uint32_t os_queue_get_item_size(OsQueueHandle queue)
{
    return queue != NULL && os_queue_is_in_list(queue) ? queue->item_size : 0U;
}

uint32_t os_queue_get_space(OsQueueHandle queue)
{
    if (queue == NULL || !os_queue_is_in_list(queue))
    {
        return 0U;
    }

    return queue->item_count - queue->count;
}

OsStatus os_mutex_create(OsMutexHandle* out_mutex)
{
    OsMutexHandle mutex = NULL;

    if (out_mutex == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        OsStatus status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    *out_mutex = NULL;
    mutex = os_mutex_allocate();
    if (mutex == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    mutex->id = g_os.next_mutex_id++;
    os_mutex_list_insert(mutex);
    *out_mutex = mutex;
    return OS_STATUS_OK;
}

void os_mutex_delete(OsMutexHandle mutex)
{
    OsTaskHandle task = NULL;

    if (mutex == NULL || !os_mutex_is_in_list(mutex))
    {
        return;
    }

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->wait_type == OS_TASK_WAIT_MUTEX &&
            task->wait_object.mutex == mutex)
        {
            os_complete_task_wait(task, OS_STATUS_MUTEX_NOT_FOUND, 0U);
        }
    }

    mutex->owner = NULL;
    os_mutex_list_remove(mutex);
    os_mutex_free(mutex);
    os_recompute_task_priorities();
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
}

uint32_t os_mutex_get_id(OsMutexHandle mutex)
{
    if (mutex == NULL || !os_mutex_is_in_list(mutex))
    {
        return 0U;
    }

    return mutex->id;
}

OsMutexHandle os_mutex_find_by_id(uint32_t mutex_id)
{
    OsMutexHandle mutex = NULL;

    if (mutex_id == 0U)
    {
        return NULL;
    }

    for (mutex = g_os.mutex_list; mutex != NULL; mutex = mutex->next)
    {
        if (mutex->id == mutex_id)
        {
            return mutex;
        }
    }

    return NULL;
}

OsStatus os_mutex_lock(OsMutexHandle mutex, uint32_t timeout_ms)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;

    if (mutex == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_mutex_is_in_list(mutex))
    {
        return OS_STATUS_MUTEX_NOT_FOUND;
    }

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (mutex->owner == NULL)
    {
        mutex->owner = task;
        task->last_wait_status = OS_STATUS_OK;
        task->last_wait_value = 0U;
        return OS_STATUS_OK;
    }

    if (mutex->owner == task)
    {
        return OS_STATUS_BUSY;
    }

    if (timeout_ms == 0U)
    {
        task->last_wait_status = OS_STATUS_TIMEOUT;
        task->last_wait_value = 0U;
        return OS_STATUS_TIMEOUT;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    task->wait_object.mutex = mutex;
    task->wait_return_kind = OS_WAIT_RETURN_STATUS;
    status = os_begin_task_wait(task, OS_TASK_WAIT_MUTEX, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        task->wait_object.mutex = NULL;
        return status;
    }

    os_recompute_task_priorities();
    return OS_STATUS_OK;
}

OsStatus os_mutex_unlock(OsMutexHandle mutex)
{
    OsTaskHandle task = g_os.current_task;
    OsTaskHandle waiter = NULL;

    if (mutex == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_mutex_is_in_list(mutex))
    {
        return OS_STATUS_MUTEX_NOT_FOUND;
    }

    if (task == NULL || mutex->owner != task)
    {
        return OS_STATUS_NOT_OWNER;
    }

    waiter = os_select_mutex_waiter(mutex);
    mutex->owner = waiter;
    if (waiter != NULL)
    {
        os_complete_task_wait(waiter, OS_STATUS_OK, 0U);
    }

    os_recompute_task_priorities();
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
    return OS_STATUS_OK;
}

OsTaskHandle os_mutex_get_owner(OsMutexHandle mutex)
{
    if (mutex == NULL || !os_mutex_is_in_list(mutex))
    {
        return NULL;
    }

    return mutex->owner;
}

OsStatus os_semaphore_create(
    OsSemaphoreHandle* out_semaphore,
    uint32_t max_count,
    uint32_t initial_count
)
{
    OsSemaphoreHandle semaphore = NULL;

    if (out_semaphore == NULL || max_count == 0U || initial_count > max_count)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        OsStatus status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    *out_semaphore = NULL;
    semaphore = os_semaphore_allocate();
    if (semaphore == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    semaphore->id = g_os.next_semaphore_id++;
    semaphore->max_count = max_count;
    semaphore->count = initial_count;
    os_semaphore_list_insert(semaphore);
    *out_semaphore = semaphore;
    return OS_STATUS_OK;
}

void os_semaphore_delete(OsSemaphoreHandle semaphore)
{
    OsTaskHandle task = NULL;

    if (semaphore == NULL || !os_semaphore_is_in_list(semaphore))
    {
        return;
    }

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->wait_type == OS_TASK_WAIT_SEMAPHORE &&
            task->wait_object.semaphore == semaphore)
        {
            os_complete_task_wait(task, OS_STATUS_SEMAPHORE_NOT_FOUND, 0U);
        }
    }

    os_semaphore_list_remove(semaphore);
    os_semaphore_free(semaphore);
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
}

uint32_t os_semaphore_get_id(OsSemaphoreHandle semaphore)
{
    if (semaphore == NULL || !os_semaphore_is_in_list(semaphore))
    {
        return 0U;
    }

    return semaphore->id;
}

OsSemaphoreHandle os_semaphore_find_by_id(uint32_t semaphore_id)
{
    OsSemaphoreHandle semaphore = NULL;

    if (semaphore_id == 0U)
    {
        return NULL;
    }

    for (semaphore = g_os.semaphore_list;
         semaphore != NULL;
         semaphore = semaphore->next)
    {
        if (semaphore->id == semaphore_id)
        {
            return semaphore;
        }
    }

    return NULL;
}

OsStatus os_semaphore_take(OsSemaphoreHandle semaphore, uint32_t timeout_ms)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;

    if (semaphore == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_semaphore_is_in_list(semaphore))
    {
        return OS_STATUS_SEMAPHORE_NOT_FOUND;
    }

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (semaphore->count > 0U)
    {
        --semaphore->count;
        task->last_wait_status = OS_STATUS_OK;
        task->last_wait_value = 0U;
        return OS_STATUS_OK;
    }

    if (timeout_ms == 0U)
    {
        task->last_wait_status = OS_STATUS_TIMEOUT;
        task->last_wait_value = 0U;
        return OS_STATUS_TIMEOUT;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    task->wait_object.semaphore = semaphore;
    task->wait_return_kind = OS_WAIT_RETURN_STATUS;
    status = os_begin_task_wait(task, OS_TASK_WAIT_SEMAPHORE, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        task->wait_object.semaphore = NULL;
        return status;
    }

    return OS_STATUS_OK;
}

OsStatus os_semaphore_give(OsSemaphoreHandle semaphore)
{
    OsTaskHandle waiter = NULL;

    if (semaphore == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_semaphore_is_in_list(semaphore))
    {
        return OS_STATUS_SEMAPHORE_NOT_FOUND;
    }

    waiter = os_select_semaphore_waiter(semaphore);
    if (waiter != NULL)
    {
        os_complete_task_wait(waiter, OS_STATUS_OK, 0U);
        os_recalculate_task_counters();
        os_preempt_for_higher_priority_ready_task();
        return OS_STATUS_OK;
    }

    if (semaphore->count >= semaphore->max_count)
    {
        return OS_STATUS_SEMAPHORE_FULL;
    }

    ++semaphore->count;
    os_wake_any_waiters();
    return OS_STATUS_OK;
}

uint32_t os_semaphore_get_count(OsSemaphoreHandle semaphore)
{
    if (semaphore == NULL || !os_semaphore_is_in_list(semaphore))
    {
        return 0U;
    }

    return semaphore->count;
}

uint32_t os_semaphore_get_max_count(OsSemaphoreHandle semaphore)
{
    if (semaphore == NULL || !os_semaphore_is_in_list(semaphore))
    {
        return 0U;
    }

    return semaphore->max_count;
}

OsStatus os_event_group_create(OsEventGroupHandle* out_event_group)
{
    OsEventGroupHandle event_group = NULL;

    if (out_event_group == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        OsStatus status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    *out_event_group = NULL;
    event_group = os_event_group_allocate();
    if (event_group == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    event_group->id = g_os.next_event_group_id++;
    os_event_group_list_insert(event_group);
    *out_event_group = event_group;
    return OS_STATUS_OK;
}

void os_event_group_delete(OsEventGroupHandle event_group)
{
    OsTaskHandle task = NULL;

    if (event_group == NULL || !os_event_group_is_in_list(event_group))
    {
        return;
    }

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->wait_type == OS_TASK_WAIT_EVENT_GROUP &&
            task->wait_object.event_group == event_group)
        {
            os_complete_task_wait(task, OS_STATUS_EVENT_GROUP_NOT_FOUND, 0U);
        }
    }

    os_event_group_list_remove(event_group);
    os_event_group_free(event_group);
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
}

uint32_t os_event_group_get_id(OsEventGroupHandle event_group)
{
    if (event_group == NULL || !os_event_group_is_in_list(event_group))
    {
        return 0U;
    }

    return event_group->id;
}

OsEventGroupHandle os_event_group_find_by_id(uint32_t event_group_id)
{
    OsEventGroupHandle event_group = NULL;

    if (event_group_id == 0U)
    {
        return NULL;
    }

    for (event_group = g_os.event_group_list;
         event_group != NULL;
         event_group = event_group->next)
    {
        if (event_group->id == event_group_id)
        {
            return event_group;
        }
    }

    return NULL;
}

uint32_t os_event_group_get_bits(OsEventGroupHandle event_group)
{
    if (event_group == NULL || !os_event_group_is_in_list(event_group))
    {
        return 0U;
    }

    return event_group->bits;
}

uint32_t os_event_group_set_bits(OsEventGroupHandle event_group, uint32_t bits)
{
    if (event_group == NULL || !os_event_group_is_in_list(event_group))
    {
        return 0U;
    }

    event_group->bits |= bits;
    os_event_group_wake_waiters(event_group);
    os_wake_any_waiters();
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
    return event_group->bits;
}

uint32_t os_event_group_clear_bits(OsEventGroupHandle event_group, uint32_t bits)
{
    uint32_t previous_bits = 0U;

    if (event_group == NULL || !os_event_group_is_in_list(event_group))
    {
        return 0U;
    }

    previous_bits = event_group->bits;
    event_group->bits &= ~bits;
    return previous_bits;
}

OsStatus os_event_group_wait_bits(
    OsEventGroupHandle event_group,
    uint32_t bits_to_wait_for,
    uint8_t clear_on_exit,
    uint8_t wait_for_all,
    uint32_t timeout_ms
)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;

    if (event_group == NULL || bits_to_wait_for == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_event_group_is_in_list(event_group))
    {
        return OS_STATUS_EVENT_GROUP_NOT_FOUND;
    }

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_event_group_condition_met(
            event_group->bits,
            bits_to_wait_for,
            wait_for_all))
    {
        task->last_wait_status = OS_STATUS_OK;
        task->last_wait_value = event_group->bits;
        if (clear_on_exit)
        {
            event_group->bits &= ~bits_to_wait_for;
        }
        return OS_STATUS_OK;
    }

    if (timeout_ms == 0U)
    {
        task->last_wait_status = OS_STATUS_TIMEOUT;
        task->last_wait_value = event_group->bits;
        return OS_STATUS_TIMEOUT;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    task->wait_object.event_group = event_group;
    task->wait_bits = bits_to_wait_for;
    task->wait_clear_on_exit = clear_on_exit ? 1U : 0U;
    task->wait_for_all = wait_for_all ? 1U : 0U;
    task->wait_return_kind = OS_WAIT_RETURN_VALUE;
    status = os_begin_task_wait(task, OS_TASK_WAIT_EVENT_GROUP, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        task->wait_object.event_group = NULL;
        return status;
    }

    return OS_STATUS_OK;
}

OsStatus os_host_import_register(
    const char* module_name,
    const char* import_name,
    const char* signature,
    OsHostImportFunction function
)
{
    OsHostImportNode* node = NULL;

    if (module_name == NULL || module_name[0] == '\0' ||
        import_name == NULL || import_name[0] == '\0' ||
        signature == NULL || signature[0] == '\0' ||
        function == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        return OS_STATUS_ERROR;
    }

    if (os_host_import_exists(module_name, import_name))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    node = (OsHostImportNode*)calloc(1U, sizeof(OsHostImportNode));
    if (node == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    node->module_name = os_duplicate_string(module_name);
    node->import_name = os_duplicate_string(import_name);
    node->signature = os_duplicate_string(signature);
    node->function = function;

    if (node->module_name == NULL || node->import_name == NULL || node->signature == NULL)
    {
        free(node->module_name);
        free(node->import_name);
        free(node->signature);
        free(node);
        return OS_STATUS_OUT_OF_MEMORY;
    }

    node->next = g_os.host_import_list;
    g_os.host_import_list = node;

    return OS_STATUS_OK;
}

void os_host_import_clear_all(void)
{
    os_host_import_free_list();
}

#if d_m3HasDylink
OsStatus os_wasm_library_register(
    OsWasmLibraryHandle* out_library,
    const char* module_name,
    uint8_t* wasm_bytes,
    uint32_t wasm_size
)
{
    OsWasmLibraryHandle library = NULL;

    if (out_library == NULL || module_name == NULL || module_name[0] == '\0' ||
        wasm_bytes == NULL || wasm_size == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    *out_library = NULL;
    if (!g_os.initialized)
    {
        OsStatus status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }
    if (os_wasm_library_find(module_name) != NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    library = (OsWasmLibraryHandle)calloc(1U, sizeof(struct OsWasmLibrary));
    if (library == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    library->module_name = os_duplicate_string(module_name);
    if (library->module_name == NULL)
    {
        free(library);
        return OS_STATUS_OUT_OF_MEMORY;
    }
    library->id = g_os.next_wasm_library_id++;
    library->wasm_bytes = wasm_bytes;
    library->wasm_size = wasm_size;
    library->next = g_os.wasm_library_list;
    g_os.wasm_library_list = library;
    ++g_os.wasm_library_count;
    *out_library = library;
    return OS_STATUS_OK;
}

OsWasmLibraryHandle os_wasm_library_find(const char* module_name)
{
    OsWasmLibraryHandle library = NULL;
    if (module_name == NULL)
    {
        return NULL;
    }
    for (library = g_os.wasm_library_list;
         library != NULL;
         library = library->next)
    {
        if (strcmp(library->module_name, module_name) == 0)
        {
            return library;
        }
    }
    return NULL;
}

uint32_t os_wasm_library_get_id(OsWasmLibraryHandle library)
{
    return library != NULL ? library->id : 0U;
}

const char* os_wasm_library_get_name(OsWasmLibraryHandle library)
{
    return library != NULL ? library->module_name : NULL;
}

uint32_t os_wasm_library_get_count(void)
{
    return g_os.wasm_library_count;
}

static M3Result os_dylink_resolve_module(
    void* context,
    const char* name,
    IM3Module* out_module
)
{
    OsTaskHandle task = (OsTaskHandle)context;
    OsWasmLibraryHandle library = NULL;
    if (out_module == NULL)
    {
        return m3Err_dylinkDependencyMissing;
    }
    *out_module = NULL;
    library = os_wasm_library_find(name);
    if (library == NULL || task == NULL)
    {
        return m3Err_dylinkDependencyMissing;
    }
    /* Each process parses its own instance; only the bytes are shared. */
    return m3_ParseModule(
        task->wasm_environment,
        out_module,
        library->wasm_bytes,
        library->wasm_size
    );
}

static M3Result os_dylink_link_host_imports(void* context, IM3Module module)
{
    (void)context;
    return os_link_module_host_imports(module);
}
#endif

/* Settings a program declares about itself in its wasm-rtos.app section. */
typedef struct OsAppMetadata
{
    uint32_t abi_version;
    uint32_t stack_size;
    const uint8_t* name;
    uint32_t name_length;
} OsAppMetadata;

static uint8_t os_read_leb_u32(const uint8_t** cursor, const uint8_t* end, uint32_t* out_value)
{
    uint32_t value = 0U;
    uint32_t shift = 0U;

    while (*cursor < end && shift < 35U)
    {
        uint8_t byte = *(*cursor)++;
        value |= (uint32_t)(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U)
        {
            *out_value = value;
            return 1U;
        }
        shift += 7U;
    }
    return 0U;
}

static uint32_t os_read_le_u32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

/*
 * Find the custom section "wasm-rtos.app": records of a key byte, a LEB128
 * length and that many bytes. Unknown keys are skipped. A module the parser
 * cannot walk is left to wasm3 to reject; a malformed section is an error.
 */
static OsStatus os_read_app_metadata(const uint8_t* bytes, uint32_t size, OsAppMetadata* out_metadata)
{
    static const char name[] = "wasm-rtos.app";
    const uint8_t* cursor = bytes + 8U;
    const uint8_t* end = bytes + size;

    memset(out_metadata, 0, sizeof(*out_metadata));
    if (size < 8U)
    {
        return OS_STATUS_OK;
    }

    while (cursor < end)
    {
        uint8_t id = *cursor++;
        uint32_t section_size = 0U;
        const uint8_t* section_end = NULL;
        uint32_t name_length = 0U;

        if (!os_read_leb_u32(&cursor, end, &section_size) || section_size > (uint32_t)(end - cursor))
        {
            return OS_STATUS_OK;
        }
        section_end = cursor + section_size;
        if (id == 0U && os_read_leb_u32(&cursor, section_end, &name_length) &&
            name_length == sizeof(name) - 1U && name_length <= (uint32_t)(section_end - cursor) &&
            memcmp(cursor, name, name_length) == 0)
        {
            cursor += name_length;
            while (cursor < section_end)
            {
                uint8_t key = *cursor++;
                uint32_t length = 0U;

                if (!os_read_leb_u32(&cursor, section_end, &length) || length > (uint32_t)(section_end - cursor))
                {
                    return OS_STATUS_INVALID_ARGUMENT;
                }
                if (key == OS_APP_KEY_ABI_VERSION || key == OS_APP_KEY_STACK_SIZE)
                {
                    if (length != 4U)
                    {
                        return OS_STATUS_INVALID_ARGUMENT;
                    }
                    if (key == OS_APP_KEY_ABI_VERSION)
                    {
                        out_metadata->abi_version = os_read_le_u32(cursor);
                    }
                    else
                    {
                        out_metadata->stack_size = os_read_le_u32(cursor);
                    }
                }
                else if (key == OS_APP_KEY_NAME)
                {
                    out_metadata->name = cursor;
                    out_metadata->name_length = length;
                }
                cursor += length;
            }
        }
        cursor = section_end;
    }
    return OS_STATUS_OK;
}

/* Highest byte of the interpreter stack the task has written. */
static uint32_t os_task_stack_peak(OsTaskHandle task)
{
    const uint8_t* stack = NULL;
    uint32_t top = 0U;

    if (task->wasm_runtime == NULL || task->wasm_runtime->originStack == NULL)
    {
        return task->stack_peak_bytes;
    }
    stack = (const uint8_t*)task->wasm_runtime->stack;
    top = task->wasm_runtime->stackSize;
    while (top > task->stack_peak_bytes && stack[top - 1U] == OS_STACK_PAINT)
    {
        --top;
    }
    if (top > task->stack_peak_bytes)
    {
        task->stack_peak_bytes = top;
    }
    return task->stack_peak_bytes;
}

static OsStatus os_task_initialize(
    OsTaskHandle* out_task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    OsValue* entry_args,
    uint32_t entry_arg_count,
    const char* task_name,
    uint32_t stack_size,
    uint32_t priority
)
{
    OsTaskHandle task = NULL;
    OsStatus status = OS_STATUS_OK;

    if (out_task == NULL || wasm_bytes == NULL || wasm_size == 0U ||
        (entry_arg_count > 0U && entry_args == NULL))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    *out_task = NULL;

    os_clear_last_error();

    task = os_task_allocate();
    if (task == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    task->id = g_os.next_task_id++;
    task->base_priority = priority;
    task->priority = priority;
    task->state = OS_TASK_READY;
    task->exit_reason = OS_TASK_EXIT_NONE;
    task->exit_code = 0U;
    task->last_wait_status = OS_STATUS_OK;
    task->wasm_bytes = wasm_bytes;
    task->wasm_size = wasm_size;
    task->entry_function_name = entry_function_name;
    os_copy_task_name(task, task_name);

    /* The creator's size wins, then the program's own request, then the default. */
    {
        OsAppMetadata metadata;
        status = os_read_app_metadata(wasm_bytes, wasm_size, &metadata);
        if (status != OS_STATUS_OK)
        {
            os_set_last_error(task, "app_metadata", "malformed wasm-rtos.app section", OS_STATUS_WASM_ERROR);
            os_task_free(task);
            return OS_STATUS_WASM_ERROR;
        }
        /* The ABI's major version is in the high 16 bits; 0 means not stated. */
        if (metadata.abi_version != 0U && (metadata.abi_version >> 16U) != OS_ABI_VERSION_MAJOR)
        {
            os_set_last_error(task, "app_metadata", "program needs another ABI version", OS_STATUS_UNSUPPORTED);
            os_task_free(task);
            return OS_STATUS_UNSUPPORTED;
        }
        if (metadata.name_length > 0U)
        {
            task->app_name = (char*)malloc((size_t)metadata.name_length + 1U);
            if (task->app_name != NULL)
            {
                memcpy(task->app_name, metadata.name, metadata.name_length);
                task->app_name[metadata.name_length] = '\0';
            }
        }
        task->stack_size = stack_size != 0U ? stack_size
            : metadata.stack_size != 0U ? metadata.stack_size : OS_DEFAULT_STACK_SIZE;
    }

    status = os_task_copy_entry_args(task, entry_args, entry_arg_count);
    if (status != OS_STATUS_OK)
    {
        os_task_free(task);
        return status;
    }

#if d_m3HasHeapAccounting
    task->heap_account = m3_NewHeapAccount();
    if (task->heap_account == NULL)
    {
        os_task_free(task);
        return OS_STATUS_OUT_OF_MEMORY;
    }
#endif

    {
        OS_HEAP_ENTER(task);
        status = os_task_create_wasm(
            task,
            wasm_bytes,
            wasm_size,
            entry_function_name,
            task->stack_size
        );
        OS_HEAP_LEAVE();
    }

    if (status != OS_STATUS_OK)
    {
        os_task_free(task);
        return status;
    }

    os_task_list_insert(task);
    os_recalculate_task_counters();

    os_clear_last_error();

    *out_task = task;
    return OS_STATUS_OK;
}

OsStatus os_task_delete(OsTaskHandle task)
{
    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (g_os.current_task == task && task->state == OS_TASK_RUNNING)
    {
        os_request_task_delete(task);
        os_recalculate_task_counters();
        return OS_STATUS_OK;
    }

    os_destroy_task(task);

    return OS_STATUS_OK;
}

OsStatus os_task_delay_ms(uint32_t delay_ms)
{
    OsStatus status = OS_STATUS_OK;

    if (g_os.current_task == NULL)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (delay_ms == 0U)
    {
        return os_task_yield();
    }

    if (delay_ms > (uint32_t)INT32_MAX)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    os_set_task_waiting(g_os.current_task, g_os.tick_ms + delay_ms);
    os_request_task_stop(g_os.current_task);
    os_recalculate_task_counters();

    return OS_STATUS_OK;
}

OsStatus os_task_delay_until(
    uint32_t* previous_wake_time_ms,
    uint32_t period_ms
)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;
    uint32_t elapsed_since_phase_ms = 0U;
    uint32_t wake_tick_ms = 0U;
    uint64_t previous_wake_time_extended_ms = 0U;
    uint64_t wake_time_extended_ms = 0U;

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (previous_wake_time_ms == NULL || period_ms == 0U ||
        period_ms > (uint32_t)INT32_MAX)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    elapsed_since_phase_ms = g_os.tick_ms - *previous_wake_time_ms;
    if (g_os.tick_ms_extended < (uint64_t)elapsed_since_phase_ms)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    previous_wake_time_extended_ms =
        g_os.tick_ms_extended - (uint64_t)elapsed_since_phase_ms;
    wake_time_extended_ms =
        previous_wake_time_extended_ms + (uint64_t)period_ms;
    wake_tick_ms = (uint32_t)wake_time_extended_ms;
    *previous_wake_time_ms = wake_tick_ms;
    task->last_wait_status = OS_STATUS_OK;
    task->last_wait_value = 0U;

    if (wake_time_extended_ms <= g_os.tick_ms_extended)
    {
        return OS_STATUS_OK;
    }

    os_set_task_waiting(task, wake_tick_ms);
    os_request_task_stop(task);
    os_recalculate_task_counters();

    return OS_STATUS_OK;
}

OsStatus os_task_abort_wait(OsTaskHandle task)
{
    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task))
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (task->state != OS_TASK_WAITING ||
        task->wait_type == OS_TASK_WAIT_NONE)
    {
        return OS_STATUS_NOT_WAITING;
    }

    os_cancel_task_wait(task, OS_STATUS_ABORTED);
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
    os_clock_rearm();
    return OS_STATUS_OK;
}

OsStatus os_task_yield(void)
{
    if (g_os.current_task == NULL)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (g_os.current_task->state == OS_TASK_RUNNING)
    {
        os_set_task_ready(g_os.current_task);
        os_recalculate_task_counters();
    }

    os_request_task_stop(g_os.current_task);

    return OS_STATUS_OK;
}

void os_request_preempt(void)
{
    OsTaskHandle task = NULL;

    if (!g_os.initialized)
    {
        return;
    }

    g_os.preempt_requested = 1U;

    task = g_os.current_task;
    if (task != NULL && task->state == OS_TASK_RUNNING)
    {
        os_request_task_stop(task);
    }
}

OsStatus os_task_suspend(OsTaskHandle task)
{
    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task))
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (g_os.current_task == task && task->state == OS_TASK_RUNNING)
    {
        os_request_task_stop(task);
    }

    if (task->state == OS_TASK_WAITING)
    {
        os_cancel_task_wait(task, OS_STATUS_BUSY);
    }

    os_set_task_suspended(task);
    os_recompute_task_priorities();
    os_recalculate_task_counters();

    return OS_STATUS_OK;
}

OsStatus os_task_resume(OsTaskHandle task)
{
    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task))
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (task->state == OS_TASK_SUSPENDED)
    {
        os_set_task_ready(task);
        os_recalculate_task_counters();
        os_preempt_for_higher_priority_ready_task();
    }

    return OS_STATUS_OK;
}

/* Carries an OsSnapshotWrite/Read callback through wasm3's stream calls. */
typedef struct OsSnapshotStream
{
    OsSnapshotWrite write;
    OsSnapshotRead read;
    void* context;
    OsStatus status;
} OsSnapshotStream;

static const char os_snapshot_stream_failed[] = "snapshot stream failed";

static M3Result os_snapshot_stream_write(void* context, const void* data, uint32_t size)
{
    OsSnapshotStream* stream = (OsSnapshotStream*)context;
    stream->status = stream->write(stream->context, data, size);
    return stream->status == OS_STATUS_OK ? m3Err_none : os_snapshot_stream_failed;
}

static M3Result os_snapshot_stream_read(void* context, void* data, uint32_t size)
{
    OsSnapshotStream* stream = (OsSnapshotStream*)context;
    stream->status = stream->read(stream->context, data, size);
    return stream->status == OS_STATUS_OK ? m3Err_none : os_snapshot_stream_failed;
}

static OsStatus os_snapshot_stream_status(const OsSnapshotStream* stream, M3Result result)
{
    return result == os_snapshot_stream_failed ? stream->status : os_wasm_snapshot_result_to_status(result);
}

/* Whether a task's runtime may be saved to or loaded from a snapshot now. */
static OsStatus os_task_check_snapshot(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }
    if (os_task_is_dead(task))
    {
        return OS_STATUS_TASK_DEAD;
    }
#if d_m3HasDylink
    if (task->wasm_dylink_task)
    {
        return OS_STATUS_UNSUPPORTED;
    }
#endif
    if (task->swapped)
    {
        return OS_STATUS_BUSY;
    }
    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }
    if (g_os.current_task == task)
    {
        return OS_STATUS_ERROR;
    }
    return os_task_snapshot_is_busy(task) ? OS_STATUS_BUSY : OS_STATUS_OK;
}

OsStatus os_task_write_snapshot(OsTaskHandle task, OsSnapshotWrite write, void* context)
{
    OsSnapshotStream stream = { write, NULL, context, OS_STATUS_OK };
    OsStatus status = OS_STATUS_OK;
    M3Result result = m3Err_none;

    if (task == NULL || write == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_task_check_snapshot(task);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    result = m3_WriteRuntimeSnapshot(task->wasm_runtime, os_snapshot_stream_write, &stream);
    status = os_snapshot_stream_status(&stream, result);
    if (status == OS_STATUS_OK)
    {
        os_clear_last_error();
    }
    else
    {
        os_set_last_error(task, "snapshot_save", result, status);
    }
    return status;
}

/* The task continues from a loaded snapshot at its next slice. */
static void os_task_continue_from_snapshot(OsTaskHandle task)
{
    task->wasm_started = 1U;
    task->wasm_needs_resume = 1U;
    task->entry_return_code = 0U;
}

OsStatus os_task_read_snapshot(OsTaskHandle task, OsSnapshotRead read, void* context)
{
    OsSnapshotStream stream = { NULL, read, context, OS_STATUS_OK };
    OsStatus status = OS_STATUS_OK;
    M3Result result = m3Err_none;

    if (task == NULL || read == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_task_check_snapshot(task);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (task->wasm_started)
    {
        return OS_STATUS_BUSY;
    }

    {
        OS_HEAP_ENTER(task);
        result = m3_ReadRuntimeSnapshot(task->wasm_runtime, os_snapshot_stream_read, &stream);
        OS_HEAP_LEAVE();
    }
    status = os_snapshot_stream_status(&stream, result);
    if (status == OS_STATUS_OK)
    {
        os_clear_last_error();
        os_task_continue_from_snapshot(task);
        os_set_task_ready(task);
        os_recalculate_task_counters();
        return OS_STATUS_OK;
    }

    os_set_last_error(task, "snapshot_load", result, status);
    /* A load that failed partway may have written some memory: start over. */
    {
        OsLastError error = g_os.last_error;
        OsStatus recreated = OS_STATUS_OK;
        os_task_cleanup_wasm(task);
        {
            OS_HEAP_ENTER(task);
            recreated = os_task_create_wasm(task, task->wasm_bytes, task->wasm_size, task->entry_function_name,
                                            task->stack_size);
            OS_HEAP_LEAVE();
        }
        g_os.last_error = error;
        if (recreated != OS_STATUS_OK)
        {
            os_task_cleanup_wasm(task);
        }
    }
    return status;
}

/* A snapshot in a caller's buffer; with no buffer it only counts. */
typedef struct OsSnapshotBuffer
{
    uint8_t* data;
    uint32_t size;
    uint32_t used;
} OsSnapshotBuffer;

static OsStatus os_snapshot_buffer_write(void* context, const void* data, uint32_t size)
{
    OsSnapshotBuffer* buffer = (OsSnapshotBuffer*)context;
    if (buffer->data != NULL)
    {
        if (size > buffer->size - buffer->used)
        {
            return OS_STATUS_BUFFER_TOO_SMALL;
        }
        memcpy(buffer->data + buffer->used, data, size);
    }
    buffer->used += size;
    return OS_STATUS_OK;
}

static OsStatus os_snapshot_buffer_read(void* context, void* data, uint32_t size)
{
    OsSnapshotBuffer* buffer = (OsSnapshotBuffer*)context;
    if (size > buffer->size - buffer->used)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    memcpy(data, buffer->data + buffer->used, size);
    buffer->used += size;
    return OS_STATUS_OK;
}

OsStatus os_task_get_snapshot_size(OsTaskHandle task, uint32_t* out_size)
{
    OsSnapshotBuffer counter = { NULL, 0U, 0U };
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || out_size == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_task_write_snapshot(task, os_snapshot_buffer_write, &counter);
    *out_size = counter.used;
    return status;
}

OsStatus os_task_save_snapshot(
    OsTaskHandle task,
    uint8_t* buffer,
    uint32_t buffer_size,
    uint32_t* out_size
)
{
    OsSnapshotBuffer target = { buffer, buffer_size, 0U };
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || buffer == NULL || buffer_size == 0U || out_size == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_task_get_snapshot_size(task, out_size);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (buffer_size < *out_size)
    {
        return OS_STATUS_BUFFER_TOO_SMALL;
    }
    return os_task_write_snapshot(task, os_snapshot_buffer_write, &target);
}

OsStatus os_task_load_snapshot(OsTaskHandle task, const uint8_t* buffer, uint32_t buffer_size)
{
    OsSnapshotBuffer source = { (uint8_t*)buffer, buffer_size, 0U };

    if (task == NULL || buffer == NULL || buffer_size == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    return os_task_read_snapshot(task, os_snapshot_buffer_read, &source);
}

/* ---- Swapping ------------------------------------------------------------ */

/* Where the next piece of a task's swap image goes or comes from. */
typedef struct OsSwapCursor
{
    uint32_t task_id;
    uint32_t offset;
} OsSwapCursor;

static OsStatus os_swap_write(void* context, const void* data, uint32_t size)
{
    OsSwapCursor* cursor = (OsSwapCursor*)context;
    OsStatus status = g_os.swap_port.write(g_os.swap_port.context, cursor->task_id, cursor->offset, data, size);
    cursor->offset += size;
    return status;
}

static OsStatus os_swap_read(void* context, void* data, uint32_t size)
{
    OsSwapCursor* cursor = (OsSwapCursor*)context;
    OsStatus status = g_os.swap_port.read(g_os.swap_port.context, cursor->task_id, cursor->offset, data, size);
    cursor->offset += size;
    return status;
}

static void os_swap_discard(OsTaskHandle task)
{
    if (task->swap_has_image && g_os.has_swap_port && g_os.swap_port.discard != NULL)
    {
        g_os.swap_port.discard(g_os.swap_port.context, task->id);
    }
    task->swap_has_image = 0U;
    task->swap_bytes = 0U;
}

static void os_swap_forget_deliveries(OsTaskHandle task)
{
    free(task->swap_item);
    task->swap_item = NULL;
    task->swap_item_size = 0U;
    task->swap_return_pending = 0U;
}

OsStatus os_swap_port_set(const OsSwapPort* port)
{
    if (port == NULL)
    {
        g_os.has_swap_port = 0U;
        memset(&g_os.swap_port, 0, sizeof(g_os.swap_port));
        return OS_STATUS_OK;
    }
    if (port->write == NULL || port->read == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_os.swap_port = *port;
    g_os.has_swap_port = 1U;
    return OS_STATUS_OK;
}

uint8_t os_task_is_swapped(OsTaskHandle task)
{
    return task != NULL && os_task_is_in_list(task) && task->swapped ? 1U : 0U;
}

OsStatus os_task_swap_out(OsTaskHandle task)
{
    OsSwapCursor cursor;
    OsSnapshotStream stream = { os_swap_write, NULL, &cursor, OS_STATUS_OK };
    M3Result result = m3Err_none;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }
    if (os_task_is_dead(task))
    {
        return OS_STATUS_TASK_DEAD;
    }
    if (task->swapped)
    {
        return OS_STATUS_OK;
    }
    if (!g_os.has_swap_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
#if d_m3HasDylink
    if (task->wasm_dylink_task)
    {
        return OS_STATUS_UNSUPPORTED;
    }
#endif
    if (g_os.current_task == task)
    {
        return OS_STATUS_ERROR;
    }
    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (task->wasm_started)
    {
        cursor.task_id = task->id;
        cursor.offset = 0U;
        task->swap_has_image = 1U;
        result = m3_WriteRuntimeSnapshot(task->wasm_runtime, os_snapshot_stream_write, &stream);
        status = os_snapshot_stream_status(&stream, result);
        if (status != OS_STATUS_OK)
        {
            os_swap_discard(task);
            os_set_last_error(task, "swap_out", result, status);
            return status;
        }
        task->swap_bytes = cursor.offset;
    }

    os_task_cleanup_wasm(task);
    task->swapped = 1U;
    os_clear_last_error();
    return OS_STATUS_OK;
}

OsStatus os_task_swap_in(OsTaskHandle task)
{
    OsSwapCursor cursor;
    OsSnapshotStream stream = { NULL, os_swap_read, &cursor, OS_STATUS_OK };
    M3Result result = m3Err_none;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }
    if (!task->swapped)
    {
        return OS_STATUS_OK;
    }
    if (task->swap_has_image && !g_os.has_swap_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }

    {
        OS_HEAP_ENTER(task);
        status = os_task_create_wasm(task, task->wasm_bytes, task->wasm_size, task->entry_function_name,
                                     task->stack_size);
        if (status == OS_STATUS_OK && task->swap_has_image)
        {
            cursor.task_id = task->id;
            cursor.offset = 0U;
            result = m3_ReadRuntimeSnapshot(task->wasm_runtime, os_snapshot_stream_read, &stream);
            status = os_snapshot_stream_status(&stream, result);
        }
        OS_HEAP_LEAVE();
    }
    if (status != OS_STATUS_OK)
    {
        /* The image stays; the task can be brought back later. */
        os_task_cleanup_wasm(task);
        os_set_last_error(task, "swap_in", result != m3Err_none ? result : "cannot recreate the runtime", status);
        return status;
    }

    if (task->swap_has_image)
    {
        os_task_continue_from_snapshot(task);
    }
    task->swapped = 0U;
    task->wasm_stack_base = (uint8_t*)task->wasm_runtime->stack;
    if (task->swap_return_pending && task->swap_return_offset <= task->wasm_runtime->stackSize - sizeof(uint32_t))
    {
        memcpy(task->wasm_stack_base + task->swap_return_offset, &task->swap_return_value, sizeof(uint32_t));
    }
    if (task->swap_item != NULL)
    {
        (void)os_task_write_memory(task, task->swap_item_address, task->swap_item, task->swap_item_size);
    }
    os_swap_forget_deliveries(task);
    os_swap_discard(task);
    os_clear_last_error();
    return OS_STATUS_OK;
}

OsStatus os_task_set_priority(OsTaskHandle task, uint32_t priority)
{
    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task))
    {
        return OS_STATUS_TASK_DEAD;
    }

    task->base_priority = priority;
    os_recompute_task_priorities();
    os_preempt_for_higher_priority_ready_task();
    return OS_STATUS_OK;
}

uint32_t os_task_get_priority(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->priority;
}

uint32_t os_task_get_base_priority(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->base_priority;
}

uint32_t os_task_get_id(OsTaskHandle task)
{
    if (task == NULL || !os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->id;
}

OsTaskHandle os_task_find_by_id(uint32_t task_id)
{
    OsTaskHandle task = NULL;

    if (task_id == 0U)
    {
        return NULL;
    }

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->id == task_id)
        {
            return task;
        }
    }

    return NULL;
}

uint32_t os_task_get_id_list(uint32_t* out_task_ids, uint32_t max_task_ids)
{
    OsTaskHandle task = NULL;
    uint32_t written = 0U;

    if (out_task_ids == NULL || max_task_ids == 0U)
    {
        return 0U;
    }

    for (task = g_os.task_list; task != NULL && written < max_task_ids; task = task->next)
    {
        if (task->state != OS_TASK_DEAD)
        {
            out_task_ids[written] = task->id;
            ++written;
        }
    }

    return written;
}

const char* os_task_get_app_name(OsTaskHandle task)
{
    return os_task_is_in_list(task) ? task->app_name : NULL;
}

OsTaskState os_task_get_state(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return OS_TASK_DEAD;
    }

    return task->state;
}

const char* os_task_get_name(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return NULL;
    }

    return task->name;
}

OsTaskHandle os_task_get_current(void)
{
    return g_os.current_task;
}

uint32_t os_task_get_run_count(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->run_count;
}

OsStatus os_task_get_last_wait_status(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    return task->last_wait_status;
}

uint32_t os_task_get_last_wait_value(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->last_wait_value;
}

OsStatus os_task_notify(
    OsTaskHandle task,
    uint32_t value,
    OsNotifyAction action
)
{
    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task))
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (action == OS_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE &&
        task->notification_pending)
    {
        return OS_STATUS_BUSY;
    }

    switch (action)
    {
        case OS_NOTIFY_NO_ACTION:
            break;

        case OS_NOTIFY_SET_BITS:
            task->notification_value |= value;
            break;

        case OS_NOTIFY_INCREMENT:
            ++task->notification_value;
            break;

        case OS_NOTIFY_SET_VALUE_WITH_OVERWRITE:
        case OS_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE:
            task->notification_value = value;
            break;

        default:
            return OS_STATUS_INVALID_ARGUMENT;
    }

    task->notification_pending = 1U;
    os_complete_notification_wait(task);
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
    os_clock_rearm();
    return OS_STATUS_OK;
}

OsStatus os_task_notify_wait(
    uint32_t clear_on_entry,
    uint32_t clear_on_exit,
    uint32_t timeout_ms
)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    if (task->notification_pending)
    {
        task->last_wait_value = task->notification_value;
        task->last_wait_status = OS_STATUS_OK;
        task->notification_value &= ~clear_on_exit;
        task->notification_pending = 0U;
        return OS_STATUS_OK;
    }

    task->notification_value &= ~clear_on_entry;

    if (timeout_ms == 0U)
    {
        task->last_wait_value = 0U;
        task->last_wait_status = OS_STATUS_TIMEOUT;
        return OS_STATUS_TIMEOUT;
    }

    task->notification_clear_on_exit = clear_on_exit;
    task->wait_return_kind = OS_WAIT_RETURN_STATUS;
    status = os_begin_task_wait(task, OS_TASK_WAIT_NOTIFICATION, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return OS_STATUS_OK;
}

OsStatus os_task_notify_take(
    uint8_t clear_count_on_exit,
    uint32_t timeout_ms
)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    if (task->notification_value > 0U)
    {
        task->last_wait_value = task->notification_value;
        task->last_wait_status = OS_STATUS_OK;
        if (clear_count_on_exit)
        {
            task->notification_value = 0U;
        }
        else
        {
            --task->notification_value;
        }
        task->notification_pending = 0U;
        return OS_STATUS_OK;
    }

    if (timeout_ms == 0U)
    {
        task->last_wait_value = 0U;
        task->last_wait_status = OS_STATUS_TIMEOUT;
        return OS_STATUS_TIMEOUT;
    }

    task->notification_take_clear_count = clear_count_on_exit ? 1U : 0U;
    task->wait_return_kind = OS_WAIT_RETURN_VALUE;
    status = os_begin_task_wait(task, OS_TASK_WAIT_NOTIFICATION_TAKE, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return OS_STATUS_OK;
}

uint32_t os_task_get_notification_value(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->notification_value;
}

uint8_t os_task_is_notification_pending(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->notification_pending;
}

OsStatus os_timer_create(
    OsTimerHandle* out_timer,
    uint32_t period_ms,
    uint8_t auto_reload,
    OsTaskHandle target_task,
    uint32_t notify_value,
    OsNotifyAction notify_action
)
{
    OsTimerHandle timer = NULL;

    if (out_timer == NULL || period_ms == 0U || period_ms > (uint32_t)INT32_MAX ||
        target_task == NULL ||
        notify_action < OS_NOTIFY_NO_ACTION ||
        notify_action > OS_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    *out_timer = NULL;

    if (!os_task_is_in_list(target_task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(target_task))
    {
        return OS_STATUS_TASK_DEAD;
    }

    timer = os_timer_allocate();
    if (timer == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    timer->id = g_os.next_timer_id++;
    timer->period_ms = period_ms;
    timer->target_task = target_task;
    timer->notify_value = notify_value;
    timer->notify_action = notify_action;
    timer->auto_reload = auto_reload ? 1U : 0U;
    os_timer_list_insert(timer);
    ++g_os.timer_count;
    *out_timer = timer;
    return OS_STATUS_OK;
}

void os_timer_delete(OsTimerHandle timer)
{
    if (!os_timer_is_in_list(timer))
    {
        return;
    }

    os_timer_list_remove(timer);
    os_timer_free(timer);
    if (g_os.timer_count > 0U)
    {
        --g_os.timer_count;
    }
    os_clock_rearm();
}

uint32_t os_timer_get_id(OsTimerHandle timer)
{
    return os_timer_is_in_list(timer) ? timer->id : 0U;
}

OsTimerHandle os_timer_find_by_id(uint32_t timer_id)
{
    OsTimerHandle timer = NULL;

    for (timer = g_os.timer_list; timer != NULL; timer = timer->next)
    {
        if (timer->id == timer_id)
        {
            return timer;
        }
    }

    return NULL;
}

OsStatus os_timer_start(OsTimerHandle timer)
{
    OsStatus status = OS_STATUS_OK;

    if (timer == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_timer_is_in_list(timer))
    {
        return OS_STATUS_TIMER_NOT_FOUND;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    if (!os_timer_is_in_list(timer))
    {
        return OS_STATUS_TIMER_NOT_FOUND;
    }

    timer->expiry_tick_ms = g_os.tick_ms + timer->period_ms;
    timer->active = 1U;
    os_clock_rearm();
    return OS_STATUS_OK;
}

OsStatus os_timer_stop(OsTimerHandle timer)
{
    if (timer == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_timer_is_in_list(timer))
    {
        return OS_STATUS_TIMER_NOT_FOUND;
    }

    timer->active = 0U;
    timer->expiry_tick_ms = 0U;
    os_clock_rearm();
    return OS_STATUS_OK;
}

OsStatus os_timer_reset(OsTimerHandle timer)
{
    return os_timer_start(timer);
}

OsStatus os_timer_change_period(OsTimerHandle timer, uint32_t period_ms)
{
    OsStatus status = OS_STATUS_OK;

    if (timer == NULL || period_ms == 0U || period_ms > (uint32_t)INT32_MAX)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_timer_is_in_list(timer))
    {
        return OS_STATUS_TIMER_NOT_FOUND;
    }

    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    if (!os_timer_is_in_list(timer))
    {
        return OS_STATUS_TIMER_NOT_FOUND;
    }

    timer->period_ms = period_ms;
    timer->expiry_tick_ms = g_os.tick_ms + period_ms;
    timer->active = 1U;
    os_clock_rearm();
    return OS_STATUS_OK;
}

uint8_t os_timer_is_active(OsTimerHandle timer)
{
    return os_timer_is_in_list(timer) ? timer->active : 0U;
}

uint32_t os_timer_get_period_ms(OsTimerHandle timer)
{
    return os_timer_is_in_list(timer) ? timer->period_ms : 0U;
}

OsTaskExitReason os_task_get_exit_reason(OsTaskHandle task)
{
    if (task == NULL || !os_task_is_in_list(task))
    {
        return OS_TASK_EXIT_NONE;
    }

    return task->exit_reason;
}

uint32_t os_task_get_exit_code(OsTaskHandle task)
{
    if (task == NULL || !os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->exit_code;
}

OsStatus os_task_get_return_value(OsTaskHandle task, OsValue* out_value)
{
    uint32_t value_count = 0U;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || out_value == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_task_get_return_values(task, out_value, 1U, &value_count);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    return value_count == 1U ? OS_STATUS_OK : OS_STATUS_ERROR;
}

uint32_t os_task_get_return_value_count(OsTaskHandle task)
{
    if (task == NULL || !os_task_is_in_list(task) ||
        task->exit_reason != OS_TASK_EXIT_RETURNED)
    {
        return 0U;
    }

    return task->entry_return_count;
}

OsStatus os_task_get_return_values(
    OsTaskHandle task,
    OsValue* out_values,
    uint32_t max_values,
    uint32_t* out_value_count
)
{
    if (task == NULL || out_value_count == NULL ||
        (max_values > 0U && out_values == NULL))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    *out_value_count = task->entry_return_count;

    if (task->exit_reason != OS_TASK_EXIT_RETURNED || task->entry_return_count == 0U)
    {
        return OS_STATUS_ERROR;
    }

    if (max_values < task->entry_return_count)
    {
        return OS_STATUS_BUFFER_TOO_SMALL;
    }

    memcpy(out_values, task->entry_return_values, (size_t)task->entry_return_count * sizeof(OsValue));
    return OS_STATUS_OK;
}


static uint32_t os_code_page_list_bytes(IM3CodePage page)
{
    uint32_t bytes = 0U;

    for (; page != NULL; page = page->info.next)
    {
        bytes += (uint32_t)(sizeof(M3CodePageHeader) + (size_t)page->info.numLines * sizeof(code_t));
    }
    return bytes;
}

OsStatus os_task_get_memory_usage(OsTaskHandle task, OsTaskMemoryUsage* out_usage)
{
    IM3Runtime runtime = NULL;

    if (task == NULL || out_usage == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    memset(out_usage, 0, sizeof(*out_usage));
    runtime = task->wasm_runtime;
    if (runtime != NULL)
    {
        /* m3_NewRuntime allocates four guard slots past the stack. */
        out_usage->stack_bytes = runtime->originStack != NULL
            ? runtime->stackSize + 4U * (uint32_t)sizeof(m3slot_t) : 0U;
        /* What is allocated, including spare room kept by memory.grow. */
        out_usage->linear_memory_bytes = runtime->memory.mallocated != NULL
            ? (uint32_t)(sizeof(M3MemoryHeader) + runtime->memory.mallocated->capacity) : 0U;
        out_usage->code_bytes = os_code_page_list_bytes(runtime->pagesOpen) +
                                os_code_page_list_bytes(runtime->pagesFull);
    }
    out_usage->stack_peak_bytes = os_task_stack_peak(task);
#if d_m3HasHeapAccounting
    out_usage->interpreter_bytes = (uint32_t)m3_GetHeapAccountBytes(task->heap_account);
    out_usage->interpreter_peak_bytes = (uint32_t)m3_GetHeapAccountPeakBytes(task->heap_account);
#endif
#if OS_HAS_PROCESSES
    if (task->owned_wasm_bytes != NULL)
    {
        out_usage->image_bytes = task->wasm_size;
    }
#if OS_HAS_FS
    if (task->image.copy != NULL)
    {
        out_usage->image_bytes = task->image.size;
    }
#endif
#endif
    out_usage->swap_bytes = task->swapped ? task->swap_bytes : 0U;
    out_usage->kernel_bytes = (uint32_t)sizeof(struct OsTask);
    out_usage->total_bytes = out_usage->image_bytes + out_usage->kernel_bytes +
        (out_usage->interpreter_bytes != 0U
            ? out_usage->interpreter_bytes
            : out_usage->stack_bytes + out_usage->linear_memory_bytes + out_usage->code_bytes);
    return OS_STATUS_OK;
}

OsStatus os_task_read_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    uint8_t* out_buffer,
    uint32_t byte_count
)
{
    uint8_t* memory = NULL;
    OsStatus status = OS_STATUS_OK;

    if (byte_count > 0U && out_buffer == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_task_get_memory_pointer(task, wasm_address, byte_count, &memory);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    if (byte_count > 0U)
    {
        memcpy(out_buffer, memory, byte_count);
    }

    return OS_STATUS_OK;
}

OsStatus os_task_write_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    const uint8_t* buffer,
    uint32_t byte_count
)
{
    uint8_t* memory = NULL;
    OsStatus status = OS_STATUS_OK;

    if (byte_count > 0U && buffer == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_task_get_memory_pointer(task, wasm_address, byte_count, &memory);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    if (byte_count > 0U)
    {
        memcpy(memory, buffer, byte_count);
    }

    return OS_STATUS_OK;
}

const char* os_get_last_error_phase(void)
{
    return g_os.last_error.phase != NULL ? g_os.last_error.phase : "none";
}

const char* os_get_last_error_result(void)
{
    return g_os.last_error.result;
}

const char* os_get_last_error_task_name(void)
{
    return g_os.last_error.has_task_name ? g_os.last_error.task_name : NULL;
}

OsStatus os_get_last_error_status(void)
{
    return g_os.last_error.status;
}

OsStatus os_clock_port_set(const OsClockPort* port)
{
    OsStatus status = OS_STATUS_OK;

    if (port == NULL || port->now_ms == NULL ||
        ((port->arm_wakeup == NULL) != (port->cancel_wakeup == NULL)))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    os_clock_port_clear();
    g_os.clock_port = *port;
    g_os.time_update_initialized = 0U;
    return os_clock_poll();
}

void os_clock_port_clear(void)
{
    if (g_os.clock_wakeup_armed && g_os.clock_port.cancel_wakeup != NULL)
    {
        g_os.clock_port.cancel_wakeup(g_os.clock_port.context);
    }

    memset(&g_os.clock_port, 0, sizeof(g_os.clock_port));
    g_os.clock_wakeup_armed = 0U;
    g_os.clock_armed_tick_ms = 0U;
    g_os.time_update_initialized = 0U;
}

OsStatus os_clock_poll(void)
{
    uint32_t now_ms = 0U;

    if (!g_os.initialized)
    {
        return OS_STATUS_ERROR;
    }

    if (g_os.clock_port.now_ms == NULL)
    {
        return OS_STATUS_UNSUPPORTED;
    }

    now_ms = g_os.clock_port.now_ms(g_os.clock_port.context);
    return os_update_time_ms(now_ms);
}

OsStatus os_clock_wakeup(void)
{
    if (!os_clock_is_tickless())
    {
        return OS_STATUS_UNSUPPORTED;
    }

    g_os.clock_wakeup_armed = 0U;
    g_os.clock_armed_tick_ms = 0U;
    return os_clock_poll();
}

uint8_t os_clock_is_tickless(void)
{
    return g_os.clock_port.now_ms != NULL &&
           g_os.clock_port.arm_wakeup != NULL &&
           g_os.clock_port.cancel_wakeup != NULL;
}

void os_tick(uint32_t milliseconds)
{
    g_os.time_update_initialized = 0U;
    os_advance_tick_ms(milliseconds);
}

OsStatus os_update_time_ms(uint32_t now_ms)
{
    OsStatus status = OS_STATUS_OK;

    if (!g_os.initialized)
    {
        status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    if (!g_os.time_update_initialized)
    {
        g_os.last_time_update_ms = now_ms;
        g_os.time_update_initialized = 1U;
        os_update_time_events();
        return OS_STATUS_OK;
    }

    {
        uint32_t elapsed_ms = now_ms - g_os.last_time_update_ms;
        g_os.last_time_update_ms = now_ms;

        if (elapsed_ms > 0U)
        {
            os_advance_tick_ms(elapsed_ms);
        }
        else
        {
            os_update_time_events();
        }
    }

    return OS_STATUS_OK;
}

uint32_t os_get_tick_ms(void)
{
    return g_os.tick_ms;
}

uint32_t os_get_next_wakeup_ms(void)
{
    OsTaskHandle task = NULL;
    OsTimerHandle timer = NULL;
    uint32_t next_wakeup_ms = OS_WAIT_FOREVER;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        uint32_t remaining_ms = 0U;

        if (task->state != OS_TASK_WAITING || !task->wait_has_timeout)
        {
            continue;
        }

        remaining_ms = os_time_reached(g_os.tick_ms, task->wake_tick_ms)
            ? 0U
            : task->wake_tick_ms - g_os.tick_ms;
        if (remaining_ms < next_wakeup_ms)
        {
            next_wakeup_ms = remaining_ms;
        }
    }

    for (timer = g_os.timer_list; timer != NULL; timer = timer->next)
    {
        uint32_t remaining_ms = 0U;

        if (!timer->active)
        {
            continue;
        }

        remaining_ms = os_time_reached(g_os.tick_ms, timer->expiry_tick_ms)
            ? 0U
            : timer->expiry_tick_ms - g_os.tick_ms;
        if (remaining_ms < next_wakeup_ms)
        {
            next_wakeup_ms = remaining_ms;
        }
    }

    return next_wakeup_ms;
}

OsStatus os_schedule(void)
{
    OsTaskHandle task = NULL;
    OsTaskRunResult run_result = OS_TASK_RUN_ERROR;
    OsStatus schedule_status = OS_STATUS_OK;

    if (!g_os.initialized)
    {
        return OS_STATUS_NO_READY_TASKS;
    }

#if OS_HAS_GPIO
    /* Edges reported since the last call, also from interrupt handlers. */
    os_gpio_poll();
#endif

    if (g_os.clock_port.now_ms != NULL)
    {
        (void)os_clock_poll();
    }
    else
    {
        os_update_time_events();
    }

#if OS_HAS_PROCESSES
    os_reap_processes();
#endif

    task = os_select_next_ready_task();
    if (task == NULL)
    {
        g_os.current_task = NULL;
        return OS_STATUS_NO_READY_TASKS;
    }

    g_os.current_task = task;
    g_os.last_scheduled_task = task;
    task->state = OS_TASK_RUNNING;
    os_recalculate_task_counters();

    {
        /* Lazy compilation and memory.grow happen while the task runs. */
        OS_HEAP_ENTER(task);
        run_result = os_run_task_slice(task);
        OS_HEAP_LEAVE();
    }
    ++task->run_count;

    if (g_os.current_task == task)
    {
        g_os.current_task = NULL;
    }

    if (!os_task_is_in_list(task))
    {
        os_recalculate_task_counters();
        return schedule_status;
    }

    if (task->delete_requested)
    {
        os_destroy_task(task);
        return OS_STATUS_OK;
    }

    if (task->state != OS_TASK_RUNNING)
    {
        if (task->state == OS_TASK_DEAD)
        {
            os_task_cleanup_wasm(task);
        }

        os_recalculate_task_counters();
        return schedule_status;
    }

    switch (run_result)
    {
        case OS_TASK_RUN_FINISHED:
            os_record_task_exit(task, OS_TASK_EXIT_RETURNED, task->entry_return_code);
            os_set_task_dead(task);
            break;

        case OS_TASK_RUN_EXITED:
            os_record_task_exit(task, OS_TASK_EXIT_EXPLICIT, task->entry_return_code);
            os_set_task_dead(task);
            break;

        case OS_TASK_RUN_FUEL_EXHAUSTED:
            os_set_task_ready(task);
            break;

        case OS_TASK_RUN_SUSPENDED:
            os_set_task_suspended(task);
            break;

        case OS_TASK_RUN_ERROR:
        default:
            os_record_task_exit(task, OS_TASK_EXIT_WASM_ERROR, 1U);
            os_set_task_dead(task);
            schedule_status = OS_STATUS_WASM_ERROR;
            break;
    }

    os_recalculate_task_counters();
    return schedule_status;
}

uint32_t os_get_task_count(void)
{
    return g_os.task_count;
}

uint32_t os_get_ready_task_count(void)
{
    return g_os.ready_task_count;
}

uint32_t os_get_waiting_task_count(void)
{
    return g_os.waiting_task_count;
}

uint32_t os_get_timer_count(void)
{
    return g_os.timer_count;
}


static OsQueueHandle os_queue_allocate(void)
{
    return (OsQueueHandle)calloc(1U, sizeof(struct OsQueue));
}

static void os_queue_free(OsQueueHandle queue)
{
    if (queue == NULL)
    {
        return;
    }

    free(queue->storage);
    queue->storage = NULL;
    free(queue->name);
    free(queue);
}

static void os_queue_list_insert(OsQueueHandle queue)
{
    OsQueueHandle cursor = NULL;

    if (queue == NULL)
    {
        return;
    }

    queue->previous = NULL;
    queue->next = NULL;

    if (g_os.queue_list == NULL)
    {
        g_os.queue_list = queue;
        return;
    }

    for (cursor = g_os.queue_list; cursor->next != NULL; cursor = cursor->next)
    {
    }

    cursor->next = queue;
    queue->previous = cursor;
}

static void os_queue_list_remove(OsQueueHandle queue)
{
    if (queue == NULL)
    {
        return;
    }

    if (queue->previous != NULL)
    {
        queue->previous->next = queue->next;
    }
    else if (g_os.queue_list == queue)
    {
        g_os.queue_list = queue->next;
    }

    if (queue->next != NULL)
    {
        queue->next->previous = queue->previous;
    }

    queue->previous = NULL;
    queue->next = NULL;
}

static int os_queue_is_in_list(OsQueueHandle queue)
{
    OsQueueHandle cursor = NULL;

    if (queue == NULL)
    {
        return 0;
    }

    for (cursor = g_os.queue_list; cursor != NULL; cursor = cursor->next)
    {
        if (cursor == queue)
        {
            return 1;
        }
    }

    return 0;
}

static void os_queue_free_all(void)
{
    OsQueueHandle queue = g_os.queue_list;

    while (queue != NULL)
    {
        OsQueueHandle next = queue->next;
        os_queue_free(queue);
        queue = next;
    }

    g_os.queue_list = NULL;
}

static OsMutexHandle os_mutex_allocate(void)
{
    return (OsMutexHandle)calloc(1U, sizeof(struct OsMutex));
}

static void os_mutex_free(OsMutexHandle mutex)
{
    if (mutex != NULL)
    {
        free(mutex->name);
    }
    free(mutex);
}

static void os_mutex_list_insert(OsMutexHandle mutex)
{
    OsMutexHandle cursor = NULL;

    if (mutex == NULL)
    {
        return;
    }

    mutex->previous = NULL;
    mutex->next = NULL;

    if (g_os.mutex_list == NULL)
    {
        g_os.mutex_list = mutex;
        return;
    }

    for (cursor = g_os.mutex_list; cursor->next != NULL; cursor = cursor->next)
    {
    }

    cursor->next = mutex;
    mutex->previous = cursor;
}

static void os_mutex_list_remove(OsMutexHandle mutex)
{
    if (mutex == NULL)
    {
        return;
    }

    if (mutex->previous != NULL)
    {
        mutex->previous->next = mutex->next;
    }
    else if (g_os.mutex_list == mutex)
    {
        g_os.mutex_list = mutex->next;
    }

    if (mutex->next != NULL)
    {
        mutex->next->previous = mutex->previous;
    }

    mutex->previous = NULL;
    mutex->next = NULL;
}

static int os_mutex_is_in_list(OsMutexHandle mutex)
{
    OsMutexHandle cursor = NULL;

    for (cursor = g_os.mutex_list; cursor != NULL; cursor = cursor->next)
    {
        if (cursor == mutex)
        {
            return 1;
        }
    }

    return 0;
}

static void os_mutex_free_all(void)
{
    OsMutexHandle mutex = g_os.mutex_list;

    while (mutex != NULL)
    {
        OsMutexHandle next = mutex->next;
        os_mutex_free(mutex);
        mutex = next;
    }

    g_os.mutex_list = NULL;
}

static OsSemaphoreHandle os_semaphore_allocate(void)
{
    return (OsSemaphoreHandle)calloc(1U, sizeof(struct OsSemaphore));
}

static void os_semaphore_free(OsSemaphoreHandle semaphore)
{
    if (semaphore != NULL)
    {
        free(semaphore->name);
    }
    free(semaphore);
}

static void os_semaphore_list_insert(OsSemaphoreHandle semaphore)
{
    OsSemaphoreHandle cursor = NULL;

    if (semaphore == NULL)
    {
        return;
    }

    semaphore->previous = NULL;
    semaphore->next = NULL;

    if (g_os.semaphore_list == NULL)
    {
        g_os.semaphore_list = semaphore;
        return;
    }

    for (cursor = g_os.semaphore_list;
         cursor->next != NULL;
         cursor = cursor->next)
    {
    }

    cursor->next = semaphore;
    semaphore->previous = cursor;
}

static void os_semaphore_list_remove(OsSemaphoreHandle semaphore)
{
    if (semaphore == NULL)
    {
        return;
    }

    if (semaphore->previous != NULL)
    {
        semaphore->previous->next = semaphore->next;
    }
    else if (g_os.semaphore_list == semaphore)
    {
        g_os.semaphore_list = semaphore->next;
    }

    if (semaphore->next != NULL)
    {
        semaphore->next->previous = semaphore->previous;
    }

    semaphore->previous = NULL;
    semaphore->next = NULL;
}

static int os_semaphore_is_in_list(OsSemaphoreHandle semaphore)
{
    OsSemaphoreHandle cursor = NULL;

    for (cursor = g_os.semaphore_list; cursor != NULL; cursor = cursor->next)
    {
        if (cursor == semaphore)
        {
            return 1;
        }
    }

    return 0;
}

static void os_semaphore_free_all(void)
{
    OsSemaphoreHandle semaphore = g_os.semaphore_list;

    while (semaphore != NULL)
    {
        OsSemaphoreHandle next = semaphore->next;
        os_semaphore_free(semaphore);
        semaphore = next;
    }

    g_os.semaphore_list = NULL;
}

static OsEventGroupHandle os_event_group_allocate(void)
{
    return (OsEventGroupHandle)calloc(1U, sizeof(struct OsEventGroup));
}

static void os_event_group_free(OsEventGroupHandle event_group)
{
    if (event_group != NULL)
    {
        free(event_group->name);
    }
    free(event_group);
}

static void os_event_group_list_insert(OsEventGroupHandle event_group)
{
    OsEventGroupHandle cursor = NULL;

    if (event_group == NULL)
    {
        return;
    }

    event_group->previous = NULL;
    event_group->next = NULL;

    if (g_os.event_group_list == NULL)
    {
        g_os.event_group_list = event_group;
        return;
    }

    for (cursor = g_os.event_group_list;
         cursor->next != NULL;
         cursor = cursor->next)
    {
    }

    cursor->next = event_group;
    event_group->previous = cursor;
}

static void os_event_group_list_remove(OsEventGroupHandle event_group)
{
    if (event_group == NULL)
    {
        return;
    }

    if (event_group->previous != NULL)
    {
        event_group->previous->next = event_group->next;
    }
    else if (g_os.event_group_list == event_group)
    {
        g_os.event_group_list = event_group->next;
    }

    if (event_group->next != NULL)
    {
        event_group->next->previous = event_group->previous;
    }

    event_group->previous = NULL;
    event_group->next = NULL;
}

static int os_event_group_is_in_list(OsEventGroupHandle event_group)
{
    OsEventGroupHandle cursor = NULL;

    for (cursor = g_os.event_group_list;
         cursor != NULL;
         cursor = cursor->next)
    {
        if (cursor == event_group)
        {
            return 1;
        }
    }

    return 0;
}

static void os_event_group_free_all(void)
{
    OsEventGroupHandle event_group = g_os.event_group_list;

    while (event_group != NULL)
    {
        OsEventGroupHandle next = event_group->next;
        os_event_group_free(event_group);
        event_group = next;
    }

    g_os.event_group_list = NULL;
}

static OsTimerHandle os_timer_allocate(void)
{
    return (OsTimerHandle)calloc(1U, sizeof(struct OsTimer));
}

static void os_timer_free(OsTimerHandle timer)
{
    if (timer != NULL)
    {
        free(timer->name);
    }
    free(timer);
}

static void os_timer_list_insert(OsTimerHandle timer)
{
    OsTimerHandle cursor = NULL;

    if (timer == NULL)
    {
        return;
    }

    timer->previous = NULL;
    timer->next = NULL;

    if (g_os.timer_list == NULL)
    {
        g_os.timer_list = timer;
        return;
    }

    for (cursor = g_os.timer_list; cursor->next != NULL; cursor = cursor->next)
    {
    }

    cursor->next = timer;
    timer->previous = cursor;
}

static void os_timer_list_remove(OsTimerHandle timer)
{
    if (timer == NULL)
    {
        return;
    }

    if (timer->previous != NULL)
    {
        timer->previous->next = timer->next;
    }
    else if (g_os.timer_list == timer)
    {
        g_os.timer_list = timer->next;
    }

    if (timer->next != NULL)
    {
        timer->next->previous = timer->previous;
    }

    timer->previous = NULL;
    timer->next = NULL;
}

static int os_timer_is_in_list(OsTimerHandle timer)
{
    OsTimerHandle cursor = NULL;

    for (cursor = g_os.timer_list; cursor != NULL; cursor = cursor->next)
    {
        if (cursor == timer)
        {
            return 1;
        }
    }

    return 0;
}

static void os_timer_free_all(void)
{
    OsTimerHandle timer = g_os.timer_list;

    while (timer != NULL)
    {
        OsTimerHandle next = timer->next;
        os_timer_free(timer);
        timer = next;
    }

    g_os.timer_list = NULL;
    g_os.timer_count = 0U;
}

static void os_timer_delete_for_task(OsTaskHandle task)
{
    OsTimerHandle timer = g_os.timer_list;

    while (timer != NULL)
    {
        OsTimerHandle next = timer->next;

        if (timer->target_task == task)
        {
            os_timer_list_remove(timer);
            os_timer_free(timer);
            if (g_os.timer_count > 0U)
            {
                --g_os.timer_count;
            }
        }

        timer = next;
    }

    os_clock_rearm();
}

static OsTaskHandle os_task_allocate(void)
{
    return (OsTaskHandle)calloc(1U, sizeof(struct OsTask));
}

static void os_task_free(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    os_task_cleanup_wasm(task);
    os_swap_discard(task);
    os_swap_forget_deliveries(task);
    os_abi_release_task(task);
#if d_m3HasHeapAccounting
    m3_ReleaseHeapAccount(task->heap_account);
#endif
    os_clear_task_wait(task);
    os_task_free_entry_args(task);
    os_task_free_return_values(task);
    free(task->app_name);
#if OS_HAS_PROCESSES
    /* The module was parsed in place, so its bytes outlive the runtime. */
    free(task->owned_wasm_bytes);
#if OS_HAS_FS
    os_fs_image_close(&task->image);
#endif
    free(task->argv);
    if (task->spawned && g_os.spawned_task_count > 0U)
    {
        --g_os.spawned_task_count;
    }
#endif
#if OS_HAS_FS
    os_fs_release_task(task->id);
#endif
    free(task);
}

static void os_task_list_insert(OsTaskHandle task)
{
    OsTaskHandle cursor = NULL;

    if (task == NULL)
    {
        return;
    }

    task->previous = NULL;
    task->next = NULL;

    if (g_os.task_list == NULL)
    {
        g_os.task_list = task;
        return;
    }

    for (cursor = g_os.task_list; cursor->next != NULL; cursor = cursor->next)
    {
    }

    cursor->next = task;
    task->previous = cursor;
}

static void os_task_list_remove(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    if (task->previous != NULL)
    {
        task->previous->next = task->next;
    }
    else if (g_os.task_list == task)
    {
        g_os.task_list = task->next;
    }

    if (task->next != NULL)
    {
        task->next->previous = task->previous;
    }

    task->previous = NULL;
    task->next = NULL;
}

static int os_task_is_in_list(OsTaskHandle task)
{
    OsTaskHandle cursor = NULL;

    if (task == NULL)
    {
        return 0;
    }

    for (cursor = g_os.task_list; cursor != NULL; cursor = cursor->next)
    {
        if (cursor == task)
        {
            return 1;
        }
    }

    return 0;
}

static uint8_t os_task_is_dead(OsTaskHandle task)
{
    return (task != NULL && task->state == OS_TASK_DEAD) ? 1U : 0U;
}

static uint8_t os_task_snapshot_is_busy(OsTaskHandle task)
{
    OsTimerHandle timer = NULL;

    if (task == NULL)
    {
        return 0U;
    }

    if (task->state == OS_TASK_WAITING || task->wait_type != OS_TASK_WAIT_NONE)
    {
        return 1U;
    }

    for (timer = g_os.timer_list; timer != NULL; timer = timer->next)
    {
        if (timer->target_task == task)
        {
            return 1U;
        }
    }

    return 0U;
}

static void os_recalculate_task_counters(void)
{
    OsTaskHandle task = NULL;

    g_os.task_count = 0U;
    g_os.ready_task_count = 0U;
    g_os.waiting_task_count = 0U;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_DEAD)
        {
            continue;
        }

        ++g_os.task_count;

        if (task->state == OS_TASK_READY)
        {
            ++g_os.ready_task_count;
        }
        else if (task->state == OS_TASK_WAITING)
        {
            ++g_os.waiting_task_count;
        }
    }
}

static void os_copy_task_name(OsTaskHandle task, const char* task_name)
{
    const char* source_name = task_name;

    if (source_name == NULL || source_name[0] == '\0')
    {
        source_name = "wasm-task";
    }

    strncpy(task->name, source_name, OS_TASK_NAME_MAX - 1U);
    task->name[OS_TASK_NAME_MAX - 1U] = '\0';
}

static OsStatus os_task_copy_entry_args(OsTaskHandle task, OsValue* entry_args, uint32_t entry_arg_count)
{
    if (task == NULL || (entry_arg_count > 0U && entry_args == NULL))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    task->entry_args = NULL;
    task->entry_arg_count = 0U;

    if (entry_arg_count == 0U)
    {
        return OS_STATUS_OK;
    }

    task->entry_args = (OsValue*)calloc(entry_arg_count, sizeof(OsValue));
    if (task->entry_args == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    memcpy(task->entry_args, entry_args, (size_t)entry_arg_count * sizeof(OsValue));
    task->entry_arg_count = entry_arg_count;
    return OS_STATUS_OK;
}

static void os_task_free_entry_args(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    free(task->entry_args);
    task->entry_args = NULL;
    task->entry_arg_count = 0U;
}

static void os_task_free_return_values(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    free(task->entry_return_values);
    task->entry_return_values = NULL;
    task->entry_return_count = 0U;
    task->entry_return_code = 0U;
}

static char* os_duplicate_string(const char* value)
{
    size_t length = 0U;
    char* copy = NULL;

    if (value == NULL)
    {
        return NULL;
    }

    length = strlen(value);
    copy = (char*)malloc(length + 1U);
    if (copy == NULL)
    {
        return NULL;
    }

    memcpy(copy, value, length + 1U);
    return copy;
}

static void os_host_import_free_list(void)
{
    OsHostImportNode* node = g_os.host_import_list;

    while (node != NULL)
    {
        OsHostImportNode* next = node->next;
        free(node->module_name);
        free(node->import_name);
        free(node->signature);
        free(node);
        node = next;
    }

    g_os.host_import_list = NULL;
}

#if d_m3HasDylink
static void os_wasm_library_free_all(void)
{
    OsWasmLibraryHandle library = g_os.wasm_library_list;
    while (library != NULL)
    {
        OsWasmLibraryHandle next = library->next;
        free(library->module_name);
        free(library);
        library = next;
    }
    g_os.wasm_library_list = NULL;
    g_os.wasm_library_count = 0U;
}
#endif

static int os_host_import_exists(const char* module_name, const char* import_name)
{
    OsHostImportNode* node = NULL;

    for (node = g_os.host_import_list; node != NULL; node = node->next)
    {
        if (strcmp(node->module_name, module_name) == 0 &&
            strcmp(node->import_name, import_name) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static OsStatus os_task_create_wasm(
    OsTaskHandle task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    uint32_t stack_size
)
{
    M3Result result = m3Err_none;

    task->wasm_environment = m3_NewEnvironment();
    if (task->wasm_environment == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    task->wasm_runtime = m3_NewRuntime(task->wasm_environment, stack_size, task);
    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memset(task->wasm_runtime->stack, OS_STACK_PAINT, task->wasm_runtime->stackSize);
    m3_SetNativeStackBudget(task->wasm_runtime, OS_NATIVE_STACK_BUDGET);
    task->wasm_stack_base = (uint8_t*)task->wasm_runtime->stack;

    result = m3_ParseModule(
        task->wasm_environment,
        &task->wasm_module,
        wasm_bytes,
        wasm_size
    );
    if (result != m3Err_none)
    {
        OsStatus status = os_wasm_result_to_status(result);
        os_set_last_error(task, "parse_module", result, status);
        return status;
    }

#if d_m3HasDylink
    if (task->wasm_module->dylink != NULL)
    {
        /*
         * A PIE program: load it and its dylink.0 dependencies into the
         * task's own runtime. Every process gets private library memory,
         * globals and table, like shared objects in an ELF process.
         */
        M3DylinkOptions options;
        IM3Module module = task->wasm_module;

        memset(&options, 0, sizeof(options));
        options.context = task;
        options.resolveModule = os_dylink_resolve_module;
        options.linkHostImports = os_dylink_link_host_imports;
        options.linearStackSize = OS_DYLINK_LINEAR_STACK_SIZE;
        result = m3_DylinkLoad(task->wasm_runtime, module, task->name, &options);
        /* Once the module joined the runtime, the runtime frees it. */
        task->wasm_module_loaded = module->runtime == task->wasm_runtime ? 1U : 0U;
        if (result != m3Err_none)
        {
            OsStatus status = os_wasm_result_to_status(result);
            os_set_last_error(task, "dylink_load", result, status);
            return status;
        }
        task->wasm_dylink_task = 1U;

        if (entry_function_name == NULL || entry_function_name[0] == '\0')
        {
            entry_function_name = os_detect_entry_function(module);
            if (entry_function_name == NULL)
            {
                os_set_last_error(task, "find_function",
                                  "no entry point: export _start, app_main or main",
                                  OS_STATUS_WASM_ERROR);
                return OS_STATUS_WASM_ERROR;
            }
            task->entry_function_name = entry_function_name;
        }
        /* Look only in the program: a library may export the same name. */
        result = m3_FindFunctionInModule(&task->wasm_entry_function, module, entry_function_name);
        if (result != m3Err_none)
        {
            OsStatus status = os_wasm_result_to_status(result);
            os_set_last_error(task, "find_function", result, status);
            return status;
        }
        return os_validate_entry_function(task);
    }
#endif

    result = m3_LoadModule(task->wasm_runtime, task->wasm_module);
    if (result != m3Err_none)
    {
        OsStatus status = os_wasm_result_to_status(result);
        os_set_last_error(task, "load_module", result, status);
        return status;
    }
    task->wasm_module_loaded = 1U;

    result = os_link_task_host_imports(task);
    if (result != m3Err_none)
    {
        OsStatus status = os_wasm_result_to_status(result);
        os_set_last_error(task, "link_imports", result, status);
        return status;
    }

    {
        OsStatus status = os_check_module_imports(task, task->wasm_module);
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    if (entry_function_name == NULL || entry_function_name[0] == '\0')
    {
        entry_function_name = os_detect_entry_function(task->wasm_module);
        if (entry_function_name == NULL)
        {
            os_set_last_error(task, "find_function",
                              "no entry point: export _start, app_main or main",
                              OS_STATUS_WASM_ERROR);
            return OS_STATUS_WASM_ERROR;
        }
        task->entry_function_name = entry_function_name;
    }

    result = m3_FindFunction(
        &task->wasm_entry_function,
        task->wasm_runtime,
        entry_function_name
    );
    if (result != m3Err_none)
    {
        OsStatus status = os_wasm_result_to_status(result);
        os_set_last_error(task, "find_function", result, status);
        return status;
    }

    return os_validate_entry_function(task);
}

static OsStatus os_validate_entry_function(OsTaskHandle task)
{
    uint32_t argument_count = 0U;
    uint32_t return_count = 0U;

    if (task == NULL || task->wasm_entry_function == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    argument_count = m3_GetArgCount(task->wasm_entry_function);
    return_count = m3_GetRetCount(task->wasm_entry_function);

    if (argument_count != task->entry_arg_count)
    {
        os_set_last_error(task, "validate_entry_signature", "entry argument count mismatch", OS_STATUS_INVALID_ARGUMENT);
        return OS_STATUS_INVALID_ARGUMENT;
    }

    for (uint32_t argument_index = 0U; argument_index < argument_count; ++argument_index)
    {
        M3ValueType wasm_type = m3_GetArgType(task->wasm_entry_function, argument_index);
        OsValueType os_type = task->entry_args[argument_index].type;

        if (wasm_type == c_m3Type_i32)
        {
            if (os_type != OS_VALUE_TYPE_I32)
            {
                os_set_last_error(task, "validate_entry_signature", "entry argument type mismatch", OS_STATUS_INVALID_ARGUMENT);
                return OS_STATUS_INVALID_ARGUMENT;
            }
        }
        else if (wasm_type == c_m3Type_i64)
        {
            if (os_type != OS_VALUE_TYPE_I64)
            {
                os_set_last_error(task, "validate_entry_signature", "entry argument type mismatch", OS_STATUS_INVALID_ARGUMENT);
                return OS_STATUS_INVALID_ARGUMENT;
            }
        }
        else if (wasm_type == c_m3Type_f32)
        {
            if (os_type != OS_VALUE_TYPE_F32)
            {
                os_set_last_error(task, "validate_entry_signature", "entry argument type mismatch", OS_STATUS_INVALID_ARGUMENT);
                return OS_STATUS_INVALID_ARGUMENT;
            }
        }
        else if (wasm_type == c_m3Type_f64)
        {
            if (os_type != OS_VALUE_TYPE_F64)
            {
                os_set_last_error(task, "validate_entry_signature", "entry argument type mismatch", OS_STATUS_INVALID_ARGUMENT);
                return OS_STATUS_INVALID_ARGUMENT;
            }
        }
        else
        {
            os_set_last_error(task, "validate_entry_signature", "unsupported entry argument type", OS_STATUS_UNSUPPORTED);
            return OS_STATUS_UNSUPPORTED;
        }
    }

    if (return_count == 0U)
    {
        os_task_free_return_values(task);
        return OS_STATUS_OK;
    }

    os_task_free_return_values(task);
    task->entry_return_values = (OsValue*)calloc(return_count, sizeof(OsValue));
    if (task->entry_return_values == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    task->entry_return_count = return_count;
    task->entry_return_code = 0U;

    for (uint32_t return_index = 0U; return_index < return_count; ++return_index)
    {
        M3ValueType return_type = m3_GetRetType(task->wasm_entry_function, return_index);
        if (return_type == c_m3Type_i32)
        {
            task->entry_return_values[return_index].type = OS_VALUE_TYPE_I32;
        }
        else if (return_type == c_m3Type_i64)
        {
            task->entry_return_values[return_index].type = OS_VALUE_TYPE_I64;
        }
        else if (return_type == c_m3Type_f32)
        {
            task->entry_return_values[return_index].type = OS_VALUE_TYPE_F32;
        }
        else if (return_type == c_m3Type_f64)
        {
            task->entry_return_values[return_index].type = OS_VALUE_TYPE_F64;
        }
        else
        {
            os_task_free_return_values(task);
            os_set_last_error(task, "validate_entry_signature", "unsupported entry return type", OS_STATUS_WASM_ERROR);
            return OS_STATUS_WASM_ERROR;
        }
    }

    return OS_STATUS_OK;
}

static OsStatus os_capture_entry_return_value(OsTaskHandle task)
{
    const void** result_values = NULL;
    M3Result result = m3Err_none;

    if (task == NULL || task->wasm_entry_function == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (task->entry_return_count == 0U)
    {
        task->entry_return_code = 0U;
        return OS_STATUS_OK;
    }

    result_values = (const void**)calloc(task->entry_return_count, sizeof(const void*));
    if (result_values == NULL)
    {
        task->entry_return_code = 0U;
        return OS_STATUS_OUT_OF_MEMORY;
    }

    for (uint32_t return_index = 0U; return_index < task->entry_return_count; ++return_index)
    {
        OsValue* return_value = &task->entry_return_values[return_index];

        if (return_value->type == OS_VALUE_TYPE_I32)
        {
            result_values[return_index] = &return_value->value.i32;
        }
        else if (return_value->type == OS_VALUE_TYPE_I64)
        {
            result_values[return_index] = &return_value->value.i64;
        }
        else if (return_value->type == OS_VALUE_TYPE_F32)
        {
            result_values[return_index] = &return_value->value.f32;
        }
        else if (return_value->type == OS_VALUE_TYPE_F64)
        {
            result_values[return_index] = &return_value->value.f64;
        }
        else
        {
            free(result_values);
            task->entry_return_code = 0U;
            return OS_STATUS_UNSUPPORTED;
        }
    }

    result = m3_GetResults(task->wasm_entry_function, task->entry_return_count, result_values);
    free(result_values);
    if (result != m3Err_none)
    {
        os_set_last_error(task, "get_results", result, OS_STATUS_WASM_ERROR);
        task->entry_return_code = 0U;
        return OS_STATUS_WASM_ERROR;
    }

    if (task->entry_return_values[0].type == OS_VALUE_TYPE_I32)
    {
        task->entry_return_code = task->entry_return_values[0].value.i32;
    }
    else
    {
        task->entry_return_code = 0U;
    }

    return OS_STATUS_OK;
}

static OsStatus os_call_entry_function(OsTaskHandle task, M3Result* out_result)
{
    const void** argument_pointers = NULL;
    M3Result result = m3Err_none;

    if (task == NULL || task->wasm_entry_function == NULL || out_result == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (task->entry_arg_count > 0U)
    {
        argument_pointers = (const void**)calloc(task->entry_arg_count, sizeof(const void*));
        if (argument_pointers == NULL)
        {
            *out_result = "entry argument pointer allocation failed";
            return OS_STATUS_OUT_OF_MEMORY;
        }

        for (uint32_t argument_index = 0U; argument_index < task->entry_arg_count; ++argument_index)
        {
            argument_pointers[argument_index] = &task->entry_args[argument_index].value;
        }
    }

    result = m3_Call(task->wasm_entry_function, task->entry_arg_count, argument_pointers);
    free(argument_pointers);

    *out_result = result;
    return OS_STATUS_OK;
}


static OsStatus os_task_get_memory_pointer(
    OsTaskHandle task,
    uint32_t wasm_address,
    uint32_t byte_count,
    uint8_t** out_memory
)
{
    uint8_t* memory = NULL;
    uint32_t memory_size = 0U;

    if (task == NULL || out_memory == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    *out_memory = NULL;

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (task->swapped)
    {
        return OS_STATUS_BUSY;
    }

    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }

    memory = m3_GetMemory(task->wasm_runtime, &memory_size, 0U);
    if (byte_count > 0U && memory == NULL)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    if (wasm_address > memory_size)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    if (byte_count > memory_size - wasm_address)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    *out_memory = memory != NULL ? memory + wasm_address : NULL;
    return OS_STATUS_OK;
}

static OsStatus os_task_get_memory_address(
    OsTaskHandle task,
    const void* memory,
    uint32_t byte_count,
    uint32_t* out_wasm_address
)
{
    uint8_t* wasm_memory = NULL;
    uint32_t memory_size = 0U;
    uintptr_t memory_address = (uintptr_t)memory;
    uintptr_t wasm_memory_address = 0U;
    uintptr_t offset = 0U;

    if (task == NULL || memory == NULL || out_wasm_address == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    *out_wasm_address = 0U;

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    wasm_memory = m3_GetMemory(task->wasm_runtime, &memory_size, 0U);
    if (wasm_memory == NULL)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    wasm_memory_address = (uintptr_t)wasm_memory;
    if (memory_address < wasm_memory_address)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    offset = memory_address - wasm_memory_address;
    if (offset > (uintptr_t)memory_size ||
        byte_count > memory_size - (uint32_t)offset)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    *out_wasm_address = (uint32_t)offset;
    return OS_STATUS_OK;
}

static void os_task_cleanup_wasm(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    task->wasm_started = 0U;
    task->wasm_needs_resume = 0U;
    task->wasm_entry_function = NULL;

    if (task->wasm_runtime != NULL)
    {
        (void)os_task_stack_peak(task);
        m3_FreeRuntime(task->wasm_runtime);
        task->wasm_runtime = NULL;
    }
    task->wasm_stack_base = NULL;

    if (task->wasm_module != NULL && !task->wasm_module_loaded)
    {
        m3_FreeModule(task->wasm_module);
    }
    task->wasm_module = NULL;
    task->wasm_module_loaded = 0U;

    if (task->wasm_environment != NULL)
    {
        m3_FreeEnvironment(task->wasm_environment);
        task->wasm_environment = NULL;
    }
#if d_m3HasDylink
    task->wasm_dylink_task = 0U;
#endif
}

/*
 * Reject a module whose function imports are not all resolved, before any
 * guest code runs. wasm3 binds imports lazily, so without this check a
 * missing import would only trap when the importing call is first compiled.
 */
static OsStatus os_check_module_imports(OsTaskHandle task, IM3Module module)
{
    char* detail = g_os.last_error.detail;
    const uint32_t detail_size = (uint32_t)sizeof(g_os.last_error.detail);
    uint32_t missing_count = 0U;
    uint32_t length = 0U;
    uint32_t import_index = 0U;

    if (module == NULL || module->functions == NULL)
    {
        return OS_STATUS_OK;
    }

    for (import_index = 0U; import_index < module->numFuncImports; ++import_index)
    {
        IM3Function function = &module->functions[import_index];
        const char* parts[3];
        uint32_t part_index = 0U;

        if (function->compiled != NULL)
        {
            continue;
        }

        ++missing_count;
        if (missing_count > 1U)
        {
            continue;
        }

        parts[0] = function->import.moduleUtf8 != NULL ? function->import.moduleUtf8 : "?";
        parts[1] = ".";
        parts[2] = function->import.fieldUtf8 != NULL ? function->import.fieldUtf8 : "?";
        length = 0U;
        for (const char* prefix = "missing import "; *prefix != '\0' && length + 1U < detail_size; ++prefix)
        {
            detail[length++] = *prefix;
        }
        for (part_index = 0U; part_index < 3U; ++part_index)
        {
            for (const char* text = parts[part_index]; *text != '\0' && length + 1U < detail_size; ++text)
            {
                detail[length++] = *text;
            }
        }
        detail[length] = '\0';
    }

    if (missing_count == 0U)
    {
        return OS_STATUS_OK;
    }

    if (missing_count > 1U)
    {
        /* Append " (+N more)" when it fits. */
        char suffix[24];
        uint32_t suffix_length = 0U;
        uint32_t remaining = missing_count - 1U;
        char digits[11];
        uint32_t digit_count = 0U;

        do
        {
            digits[digit_count++] = (char)('0' + (remaining % 10U));
            remaining /= 10U;
        } while (remaining != 0U && digit_count < (uint32_t)sizeof(digits));

        suffix[suffix_length++] = ' ';
        suffix[suffix_length++] = '(';
        suffix[suffix_length++] = '+';
        while (digit_count > 0U)
        {
            suffix[suffix_length++] = digits[--digit_count];
        }
        for (const char* text = " more)"; *text != '\0'; ++text)
        {
            suffix[suffix_length++] = *text;
        }
        if (length + suffix_length < detail_size)
        {
            memcpy(detail + length, suffix, suffix_length);
            detail[length + suffix_length] = '\0';
        }
    }

    os_set_last_error(task, "check_imports", detail, OS_STATUS_WASM_ERROR);
    return OS_STATUS_WASM_ERROR;
}

/*
 * Pick the entry point the way a loader picks an executable's: a WASI command
 * (_start) first, then the wasm-rtos task convention, then a bare main.
 */
static const char* os_detect_entry_function(IM3Module module)
{
    static const char* const candidates[] = { "_start", "app_main", "main" };
    uint32_t candidate_index = 0U;
    uint32_t function_index = 0U;

    if (module == NULL || module->functions == NULL)
    {
        return NULL;
    }

    for (candidate_index = 0U;
         candidate_index < (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
         ++candidate_index)
    {
        for (function_index = 0U; function_index < module->numFunctions; ++function_index)
        {
            const char* export_name = module->functions[function_index].export_name;
            if (export_name != NULL && strcmp(export_name, candidates[candidate_index]) == 0)
            {
                return candidates[candidate_index];
            }
        }
    }

    return NULL;
}

static M3Result os_link_task_host_imports(OsTaskHandle task)
{
    if (task == NULL)
    {
        return m3Err_moduleNotLinked;
    }
    return os_link_module_host_imports(task->wasm_module);
}

static M3Result os_link_module_host_imports(IM3Module module)
{
    M3Result result = m3Err_none;

    if (module == NULL)
    {
        return m3Err_moduleNotLinked;
    }

    result = os_link_registered_host_imports(module);
    if (result != m3Err_none)
    {
        return result;
    }

#if defined(OS_HAS_WASM3_WASI)
    result = m3_LinkWASI(module);
    if (result != m3Err_none)
    {
        return result;
    }
#if OS_HAS_FS
    /* Route WASI file calls to the virtual file system, not the host's. */
    result = os_fs_link_wasi(module);
    if (result != m3Err_none)
    {
        return result;
    }
#endif
#endif
    /* The guest ABI, and the WASI calls the kernel answers itself. */
    result = os_abi_link(module);
    if (result != m3Err_none)
    {
        return result;
    }

    return m3Err_none;
}

static M3Result os_link_registered_host_imports(IM3Module module)
{
    OsHostImportNode* node = NULL;
    M3Result result = m3Err_none;

    if (module == NULL)
    {
        return m3Err_moduleNotLinked;
    }

    for (node = g_os.host_import_list; node != NULL; node = node->next)
    {
        result = m3_LinkRawFunction(
            module,
            node->module_name,
            node->import_name,
            node->signature,
            node->function
        );

        if (result != m3Err_none && result != m3Err_functionLookupFailed)
        {
            return result;
        }
    }

    return m3Err_none;
}




static OsStatus os_wasm_result_to_status(M3Result result)
{
    if (result == m3Err_none)
    {
        return OS_STATUS_OK;
    }

    return OS_STATUS_WASM_ERROR;
}

static OsStatus os_wasm_snapshot_result_to_status(M3Result result)
{
    if (result == m3Err_none)
    {
        return OS_STATUS_OK;
    }

    if (result == m3Err_snapshotBufferTooSmall)
    {
        return OS_STATUS_BUFFER_TOO_SMALL;
    }

    if (result == m3Err_snapshotUnsupported)
    {
        return OS_STATUS_UNSUPPORTED;
    }

    if (result == m3Err_snapshotInvalid)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    return OS_STATUS_WASM_ERROR;
}

static OsTaskHandle os_select_next_ready_task(void)
{
    OsTaskHandle start = NULL;
    OsTaskHandle cursor = NULL;
    OsTaskHandle selected = NULL;

    if (g_os.task_list == NULL)
    {
        return NULL;
    }

    if (os_task_is_in_list(g_os.last_scheduled_task) &&
        g_os.last_scheduled_task->next != NULL)
    {
        start = g_os.last_scheduled_task->next;
    }
    else
    {
        start = g_os.task_list;
    }

    cursor = start;
    do
    {
        if (cursor->state == OS_TASK_READY &&
            (selected == NULL || cursor->priority > selected->priority))
        {
            selected = cursor;
        }

        cursor = cursor->next;
        if (cursor == NULL)
        {
            cursor = g_os.task_list;
        }
    } while (cursor != start);

    return selected;
}

static void os_preempt_for_higher_priority_ready_task(void)
{
    OsTaskHandle current = g_os.current_task;
    OsTaskHandle task = NULL;

    if (current == NULL || current->state != OS_TASK_RUNNING)
    {
        return;
    }

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_READY &&
            task->priority > current->priority)
        {
            os_request_task_stop(current);
            return;
        }
    }
}

static OsTaskHandle os_select_queue_waiter(
    OsQueueHandle queue,
    OsTaskWaitType wait_type
)
{
    OsTaskHandle task = NULL;
    OsTaskHandle selected = NULL;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_WAITING &&
            task->wait_type == wait_type &&
            task->wait_object.queue == queue &&
            (selected == NULL ||
             task->priority > selected->priority ||
             (task->priority == selected->priority &&
              task->wait_sequence < selected->wait_sequence)))
        {
            selected = task;
        }
    }

    return selected;
}

static OsStatus os_queue_deliver_to_waiting_receiver(
    OsTaskHandle task,
    const void* item
)
{
    OsQueueHandle queue = NULL;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || item == NULL ||
        task->wait_type != OS_TASK_WAIT_QUEUE_RECEIVE)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    queue = task->wait_object.queue;
    if (queue == NULL || !os_queue_is_in_list(queue))
    {
        os_complete_task_wait(task, OS_STATUS_QUEUE_NOT_FOUND, 0U);
        return OS_STATUS_QUEUE_NOT_FOUND;
    }

    if (!task->wait_queue_has_wasm_output)
    {
        os_complete_task_wait(task, OS_STATUS_INVALID_ARGUMENT, 0U);
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (task->swapped)
    {
        /* Kept until the task comes back and has its memory again. */
        free(task->swap_item);
        task->swap_item = (uint8_t*)malloc(queue->item_size);
        status = task->swap_item != NULL ? OS_STATUS_OK : OS_STATUS_OUT_OF_MEMORY;
        if (status == OS_STATUS_OK)
        {
            memcpy(task->swap_item, item, queue->item_size);
            task->swap_item_address = task->wait_queue_wasm_output_address;
            task->swap_item_size = queue->item_size;
        }
    }
    else
    {
        status = os_task_write_memory(
            task,
            task->wait_queue_wasm_output_address,
            (const uint8_t*)item,
            queue->item_size
        );
    }
    os_complete_task_wait(task, status, 0U);
    return status;
}

static void os_queue_fill_from_waiting_sender(OsQueueHandle queue)
{
    OsTaskHandle sender = NULL;
    uint8_t* destination = NULL;

    if (queue == NULL || !os_queue_is_in_list(queue) ||
        queue->count >= queue->item_count)
    {
        return;
    }

    sender = os_select_queue_waiter(queue, OS_TASK_WAIT_QUEUE_SEND);
    if (sender == NULL)
    {
        return;
    }

    if (sender->wait_queue_item == NULL)
    {
        os_complete_task_wait(sender, OS_STATUS_INVALID_ARGUMENT, 0U);
        return;
    }

    destination = queue->storage +
        ((size_t)queue->tail * (size_t)queue->item_size);
    memcpy(destination, sender->wait_queue_item, queue->item_size);
    queue->tail = (queue->tail + 1U) % queue->item_count;
    ++queue->count;
    os_complete_task_wait(sender, OS_STATUS_OK, 0U);
}

static OsTaskHandle os_select_mutex_waiter(OsMutexHandle mutex)
{
    OsTaskHandle task = NULL;
    OsTaskHandle selected = NULL;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_WAITING &&
            task->wait_type == OS_TASK_WAIT_MUTEX &&
            task->wait_object.mutex == mutex &&
            (selected == NULL ||
             task->priority > selected->priority ||
             (task->priority == selected->priority &&
              task->wait_sequence < selected->wait_sequence)))
        {
            selected = task;
        }
    }

    return selected;
}

static OsTaskHandle os_select_semaphore_waiter(OsSemaphoreHandle semaphore)
{
    OsTaskHandle task = NULL;
    OsTaskHandle selected = NULL;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_WAITING &&
            task->wait_type == OS_TASK_WAIT_SEMAPHORE &&
            task->wait_object.semaphore == semaphore &&
            (selected == NULL ||
             task->priority > selected->priority ||
             (task->priority == selected->priority &&
              task->wait_sequence < selected->wait_sequence)))
        {
            selected = task;
        }
    }

    return selected;
}

static OsStatus os_begin_task_wait(
    OsTaskHandle task,
    OsTaskWaitType wait_type,
    uint32_t timeout_ms
)
{
    if (task == NULL || task->state != OS_TASK_RUNNING ||
        wait_type == OS_TASK_WAIT_NONE || wait_type == OS_TASK_WAIT_DELAY ||
        (timeout_ms != OS_WAIT_FOREVER && timeout_ms > (uint32_t)INT32_MAX))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    task->wait_type = wait_type;
    task->wait_sequence = g_os.next_wait_sequence++;
    task->wait_has_timeout = timeout_ms == OS_WAIT_FOREVER ? 0U : 1U;
    task->wake_tick_ms = task->wait_has_timeout
        ? g_os.tick_ms + timeout_ms
        : 0U;
    task->last_wait_status = OS_STATUS_OK;
    task->last_wait_value = 0U;
    task->state = OS_TASK_WAITING;
    os_request_task_stop(task);
    os_recalculate_task_counters();
    os_clock_rearm();
    return OS_STATUS_OK;
}

static void os_complete_task_wait(
    OsTaskHandle task,
    OsStatus status,
    uint32_t value
)
{
    uint32_t return_value = 0U;

    if (task == NULL || task->wait_type == OS_TASK_WAIT_NONE)
    {
        return;
    }

    task->last_wait_status = status;
    task->last_wait_value = value;
    if (task->wait_return_kind == OS_WAIT_RETURN_ABI)
    {
        return_value = status == OS_STATUS_OK ? value : (uint32_t)os_abi_error(status);
    }
    else
    {
        return_value = task->wait_return_kind == OS_WAIT_RETURN_VALUE
            ? value
            : (uint32_t)status;
    }

    os_write_wait_return(task, return_value);
    os_clear_task_wait(task);
    os_set_task_ready(task);
}

static void os_cancel_task_wait(OsTaskHandle task, OsStatus status)
{
    if (task == NULL || task->wait_type == OS_TASK_WAIT_NONE)
    {
        return;
    }

    os_complete_task_wait(task, status, 0U);
    os_recompute_task_priorities();
}

static void os_clear_task_wait(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    os_clear_queue_wait(task);
    free(task->wait_targets);
    task->wait_targets = NULL;
    task->wait_target_count = 0U;
    task->wait_type = OS_TASK_WAIT_NONE;
    task->wait_object.mutex = NULL;
    task->wait_sequence = 0U;
    task->wait_bits = 0U;
    task->wait_clear_on_exit = 0U;
    task->wait_for_all = 0U;
    task->wait_has_timeout = 0U;
    task->wake_tick_ms = 0U;
    task->notification_clear_on_exit = 0U;
    task->notification_take_clear_count = 0U;
    os_clear_wait_return(task);
}

static void os_clear_queue_wait(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    free(task->wait_queue_item);
    task->wait_queue_item = NULL;

    task->wait_queue_has_wasm_output = 0U;
    task->wait_queue_wasm_output_address = 0U;
}

static OsStatus os_prepare_wait_return(OsTaskHandle task, uint32_t* raw_return)
{
    uint8_t* stack_start = NULL;
    uint8_t* stack_end = NULL;
    uint8_t* return_address = (uint8_t*)raw_return;

    if (task == NULL || raw_return == NULL || task->wasm_runtime == NULL ||
        task->wasm_stack_base == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    stack_start = task->wasm_stack_base;
    stack_end = stack_start + task->wasm_runtime->stackSize;
    if (return_address < stack_start ||
        return_address + sizeof(uint32_t) > stack_end)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    task->wait_has_return_slot = 1U;
    task->wait_return_offset = (uint32_t)(return_address - stack_start);
    return OS_STATUS_OK;
}

static void os_clear_wait_return(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    task->wait_has_return_slot = 0U;
    task->wait_return_offset = 0U;
}

static void os_write_wait_return(OsTaskHandle task, uint32_t value)
{
    uint8_t* stack_start = NULL;
    uint32_t* return_slot = NULL;

    if (task != NULL && task->swapped && task->wait_has_return_slot)
    {
        /* Written into the stack when the task comes back. */
        task->swap_return_pending = 1U;
        task->swap_return_offset = task->wait_return_offset;
        task->swap_return_value = value;
        return;
    }

    if (task == NULL || !task->wait_has_return_slot ||
        task->wasm_runtime == NULL || task->wasm_stack_base == NULL)
    {
        return;
    }

    if (task->wasm_runtime->stackSize < sizeof(uint32_t) ||
        task->wait_return_offset >
        task->wasm_runtime->stackSize - sizeof(uint32_t))
    {
        return;
    }

    stack_start = task->wasm_stack_base;
    return_slot = (uint32_t*)(stack_start + task->wait_return_offset);
    *return_slot = value;
}

static void os_recompute_task_priorities(void)
{
    OsTaskHandle task = NULL;
    uint32_t pass = 0U;
    uint8_t changed = 0U;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state != OS_TASK_DEAD)
        {
            task->priority = task->base_priority;
        }
    }

    do
    {
        changed = 0U;
        for (task = g_os.task_list; task != NULL; task = task->next)
        {
            OsMutexHandle mutex = NULL;
            OsTaskHandle owner = NULL;

            if (task->state == OS_TASK_DEAD ||
                task->wait_type != OS_TASK_WAIT_MUTEX)
            {
                continue;
            }

            mutex = task->wait_object.mutex;
            owner = mutex != NULL && os_mutex_is_in_list(mutex)
                ? mutex->owner
                : NULL;
            if (owner != NULL && owner->state != OS_TASK_DEAD &&
                task->priority > owner->priority)
            {
                owner->priority = task->priority;
                changed = 1U;
            }
        }
        ++pass;
    } while (changed && pass < g_os.task_count);
}

static void os_release_task_mutexes(OsTaskHandle task)
{
    OsMutexHandle mutex = NULL;

    if (task == NULL)
    {
        return;
    }

    for (mutex = g_os.mutex_list; mutex != NULL; mutex = mutex->next)
    {
        if (mutex->owner == task)
        {
            OsTaskHandle waiter = os_select_mutex_waiter(mutex);
            mutex->owner = waiter;
            if (waiter != NULL)
            {
                os_complete_task_wait(waiter, OS_STATUS_OK, 0U);
            }
        }
    }

    os_recompute_task_priorities();
    os_preempt_for_higher_priority_ready_task();
}

static uint8_t os_event_group_condition_met(
    uint32_t current_bits,
    uint32_t bits_to_wait_for,
    uint8_t wait_for_all
)
{
    if (wait_for_all)
    {
        return (current_bits & bits_to_wait_for) == bits_to_wait_for
            ? 1U
            : 0U;
    }

    return (current_bits & bits_to_wait_for) != 0U ? 1U : 0U;
}

static void os_event_group_wake_waiters(OsEventGroupHandle event_group)
{
    OsTaskHandle task = NULL;
    uint32_t matching_bits = 0U;
    uint32_t bits_to_clear = 0U;

    if (event_group == NULL || !os_event_group_is_in_list(event_group))
    {
        return;
    }

    matching_bits = event_group->bits;
    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state != OS_TASK_WAITING ||
            task->wait_type != OS_TASK_WAIT_EVENT_GROUP ||
            task->wait_object.event_group != event_group ||
            !os_event_group_condition_met(
                matching_bits,
                task->wait_bits,
                task->wait_for_all))
        {
            continue;
        }

        if (task->wait_clear_on_exit)
        {
            bits_to_clear |= task->wait_bits;
        }
        os_complete_task_wait(task, OS_STATUS_OK, matching_bits);
    }

    event_group->bits &= ~bits_to_clear;
}

static void os_complete_notification_wait(OsTaskHandle task)
{
    uint32_t value = 0U;

    if (task == NULL || task->state != OS_TASK_WAITING)
    {
        return;
    }

    if (task->wait_type == OS_TASK_WAIT_NOTIFICATION &&
        task->notification_pending)
    {
        value = task->notification_value;
        task->notification_value &= ~task->notification_clear_on_exit;
        task->notification_pending = 0U;
        os_complete_task_wait(task, OS_STATUS_OK, value);
    }
    else if (task->wait_type == OS_TASK_WAIT_NOTIFICATION_TAKE &&
             task->notification_value > 0U)
    {
        value = task->notification_value;
        if (task->notification_take_clear_count)
        {
            task->notification_value = 0U;
        }
        else
        {
            --task->notification_value;
        }
        task->notification_pending = 0U;
        os_complete_task_wait(task, OS_STATUS_OK, value);
    }
}

static void os_process_timers(void)
{
    OsTimerHandle timer = g_os.timer_list;

    while (timer != NULL)
    {
        OsTimerHandle next = timer->next;

        if (!timer->counting &&
            (!os_task_is_in_list(timer->target_task) || os_task_is_dead(timer->target_task)))
        {
            os_timer_list_remove(timer);
            os_timer_free(timer);
            if (g_os.timer_count > 0U)
            {
                --g_os.timer_count;
            }
            timer = next;
            continue;
        }

        if (timer->active &&
            os_time_reached(g_os.tick_ms, timer->expiry_tick_ms))
        {
            if (timer->auto_reload)
            {
                uint32_t elapsed_ms = g_os.tick_ms - timer->expiry_tick_ms;
                uint32_t elapsed_periods = elapsed_ms / timer->period_ms + 1U;
                timer->expiry_tick_ms += elapsed_periods * timer->period_ms;
            }
            else
            {
                timer->active = 0U;
                timer->expiry_tick_ms = 0U;
            }

            if (timer->counting)
            {
                if (timer->expirations < UINT32_MAX)
                {
                    ++timer->expirations;
                }
                os_wake_any_waiters();
            }
            else
            {
                (void)os_task_notify(
                    timer->target_task,
                    timer->notify_value,
                    timer->notify_action
                );
            }
        }

        timer = next;
    }
}

static void os_update_time_events(void)
{
    g_os.processing_time_events = 1U;
    os_process_timers();
    os_update_waiting_tasks();
    g_os.processing_time_events = 0U;
    os_clock_rearm();
}

static void os_update_waiting_tasks(void)
{
    OsTaskHandle task = NULL;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_WAITING &&
            task->wait_has_timeout &&
            os_time_reached(g_os.tick_ms, task->wake_tick_ms))
        {
            if (task->wait_type == OS_TASK_WAIT_DELAY)
            {
                os_complete_task_wait(task, OS_STATUS_OK, 0U);
            }
            else if (task->wait_type == OS_TASK_WAIT_EVENT_GROUP)
            {
                uint32_t bits = 0U;
                if (task->wait_object.event_group != NULL &&
                    os_event_group_is_in_list(task->wait_object.event_group))
                {
                    bits = task->wait_object.event_group->bits;
                }
                os_complete_task_wait(task, OS_STATUS_TIMEOUT, bits);
            }
            else
            {
                os_complete_task_wait(task, OS_STATUS_TIMEOUT, 0U);
            }
        }
    }

    os_recompute_task_priorities();
    os_recalculate_task_counters();
    os_preempt_for_higher_priority_ready_task();
}

static void os_advance_tick_ms(uint32_t milliseconds)
{
    uint32_t step_ms = 0U;

    do
    {
        step_ms = milliseconds > (uint32_t)INT32_MAX
            ? (uint32_t)INT32_MAX
            : milliseconds;
        g_os.tick_ms += step_ms;
        g_os.tick_ms_extended += (uint64_t)step_ms;
        milliseconds -= step_ms;
        os_update_time_events();
    } while (milliseconds > 0U);
}

static OsStatus os_sync_clock_before_deadline(void)
{
    if (g_os.clock_port.now_ms == NULL || g_os.processing_time_events)
    {
        return OS_STATUS_OK;
    }

    return os_clock_poll();
}

static void os_clock_rearm(void)
{
    uint32_t delay_ms = OS_WAIT_FOREVER;
    uint32_t target_tick_ms = 0U;

    if (g_os.clock_port.arm_wakeup == NULL ||
        g_os.clock_port.cancel_wakeup == NULL)
    {
        return;
    }

    if (g_os.processing_time_events)
    {
        return;
    }

    delay_ms = os_get_next_wakeup_ms();
    if (delay_ms == OS_WAIT_FOREVER)
    {
        if (g_os.clock_wakeup_armed)
        {
            g_os.clock_port.cancel_wakeup(g_os.clock_port.context);
            g_os.clock_wakeup_armed = 0U;
            g_os.clock_armed_tick_ms = 0U;
        }
        return;
    }

    target_tick_ms = g_os.tick_ms + delay_ms;
    if (g_os.clock_wakeup_armed &&
        g_os.clock_armed_tick_ms == target_tick_ms)
    {
        return;
    }

    if (g_os.clock_wakeup_armed)
    {
        g_os.clock_port.cancel_wakeup(g_os.clock_port.context);
    }

    g_os.clock_wakeup_armed = 1U;
    g_os.clock_armed_tick_ms = target_tick_ms;
    g_os.clock_port.arm_wakeup(g_os.clock_port.context, delay_ms);
}

static OsTaskRunResult os_run_task_slice(OsTaskHandle task)
{
    M3Result result = m3Err_none;
    const char* phase = "call";

    if (task != NULL && task->swapped && os_task_swap_in(task) != OS_STATUS_OK)
    {
        return OS_TASK_RUN_ERROR;
    }

    if (task == NULL || task->wasm_runtime == NULL || task->wasm_entry_function == NULL)
    {
        return OS_TASK_RUN_ERROR;
    }

#if OS_HAS_PROCESSES && defined(OS_HAS_WASM3_WASI)
    os_apply_task_wasi_args(task);
#endif


    task->wasm_stack_base = (uint8_t*)task->wasm_runtime->stack;
    m3_SetFuel(task->wasm_runtime, OS_TASK_SLICE_FUEL);
    if (g_os.preempt_requested)
    {
        os_request_task_stop(task);
    }

    if (!task->wasm_started)
    {
        task->wasm_started = 1U;
        if (os_call_entry_function(task, &result) != OS_STATUS_OK)
        {
            os_set_last_error(task, phase, result, OS_STATUS_OUT_OF_MEMORY);
            return OS_TASK_RUN_ERROR;
        }
    }
    else
    {
        phase = "resume";
        result = m3_Resume(task->wasm_runtime);
    }

    if (result == m3Err_none)
    {
        os_clear_last_error();
        g_os.preempt_requested = 0U;
        task->wasm_needs_resume = 0U;
        if (os_capture_entry_return_value(task) != OS_STATUS_OK)
        {
            return OS_TASK_RUN_ERROR;
        }
        return OS_TASK_RUN_FINISHED;
    }

    if (result == m3Err_trapExit)
    {
        os_clear_last_error();
        g_os.preempt_requested = 0U;
        task->wasm_needs_resume = 0U;
#if defined(OS_HAS_WASM3_WASI)
        {
            m3_wasi_context_t* wasi_context = m3_GetWasiContext();
            task->entry_return_code = wasi_context != NULL ? (uint32_t)wasi_context->exit_code : 0U;
        }
#else
        task->entry_return_code = 0U;
#endif
        return OS_TASK_RUN_EXITED;
    }

    if (result == m3Err_fuelExhausted)
    {
        os_clear_last_error();
        g_os.preempt_requested = 0U;
        task->wasm_needs_resume = 1U;
        return OS_TASK_RUN_FUEL_EXHAUSTED;
    }

    if (result == m3Err_runtimeSuspended ||
        (task->wasm_runtime != NULL && m3_IsSuspended(task->wasm_runtime)))
    {
        os_clear_last_error();
        g_os.preempt_requested = 0U;
        task->wasm_needs_resume = 1U;
        return OS_TASK_RUN_SUSPENDED;
    }

    g_os.preempt_requested = 0U;
    os_set_last_error(task, phase, result, OS_STATUS_WASM_ERROR);
    task->wasm_needs_resume = 0U;
    return OS_TASK_RUN_ERROR;
}

static void os_set_task_ready(OsTaskHandle task)
{
    if (task == NULL || task->state == OS_TASK_DEAD)
    {
        return;
    }

    task->state = OS_TASK_READY;
    task->wake_tick_ms = 0U;
    os_clock_rearm();
}

static void os_set_task_waiting(OsTaskHandle task, uint32_t wake_tick_ms)
{
    if (task == NULL || task->state == OS_TASK_DEAD)
    {
        return;
    }

    os_clear_task_wait(task);
    task->state = OS_TASK_WAITING;
    task->wait_type = OS_TASK_WAIT_DELAY;
    task->wait_has_timeout = 1U;
    task->wait_return_kind = OS_WAIT_RETURN_STATUS;
    task->wake_tick_ms = wake_tick_ms;
    os_clock_rearm();
}

static void os_set_task_suspended(OsTaskHandle task)
{
    if (task == NULL || task->state == OS_TASK_DEAD)
    {
        return;
    }

    task->state = OS_TASK_SUSPENDED;
    task->wake_tick_ms = 0U;
    os_clock_rearm();
}

#if OS_HAS_PROCESSES
/* ---------------------------------------------------------------------------
 * Processes: spawn, wait and reclaim WebAssembly programs started at run time
 * ------------------------------------------------------------------------ */

/* Pack argv into one allocation: the pointer table followed by the strings. */
static char** os_pack_argv(const char* const* argv, uint32_t argc)
{
    size_t total = (size_t)(argc + 1U) * sizeof(char*);
    char** packed = NULL;
    char* cursor = NULL;
    uint32_t index = 0U;

    for (index = 0U; index < argc; ++index)
    {
        total += strlen(argv[index]) + 1U;
    }

    packed = (char**)malloc(total);
    if (packed == NULL)
    {
        return NULL;
    }

    cursor = (char*)(packed + argc + 1U);
    for (index = 0U; index < argc; ++index)
    {
        size_t length = strlen(argv[index]) + 1U;
        memcpy(cursor, argv[index], length);
        packed[index] = cursor;
        cursor += length;
    }
    packed[argc] = NULL;
    return packed;
}

static const char* os_process_name_from_argv(char** argv, uint32_t argc)
{
    const char* name = NULL;
    const char* cursor = NULL;

    if (argv == NULL || argc == 0U || argv[0] == NULL || argv[0][0] == '\0')
    {
        return "process";
    }

    name = argv[0];
    for (cursor = argv[0]; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            name = cursor + 1;
        }
    }
    return name[0] != '\0' ? name : "process";
}

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

static void os_process_image_release(OsProcessImage* image)
{
    free(image->owned_bytes);
#if OS_HAS_FS
    os_fs_image_close(&image->file);
#endif
}

/* Takes ownership of image and argv on every path. */
static OsStatus os_task_spawn_owned(
    OsTaskHandle* out_task,
    OsProcessImage* image,
    char** argv,
    uint32_t argc,
    uint32_t priority,
    uint32_t stack_size
)
{
    OsTaskHandle parent = g_os.current_task;
    OsTaskHandle task = NULL;
    OsStatus status = OS_STATUS_OK;

    if (g_os.spawned_task_count >= OS_MAX_SPAWNED_TASKS)
    {
        os_process_image_release(image);
        free(argv);
        os_set_last_error(parent, "spawn", "process limit reached", OS_STATUS_LIMIT_REACHED);
        return OS_STATUS_LIMIT_REACHED;
    }

    if (priority == 0U)
    {
        priority = parent != NULL ? parent->base_priority : (uint32_t)OS_TASK_PRIORITY_NORMAL;
    }

    /* The module is parsed in place and never written. */
    status = os_task_initialize(
        &task,
        (uint8_t*)image->bytes,
        image->size,
        NULL,
        NULL,
        0U,
        os_process_name_from_argv(argv, argc),
        stack_size,
        priority
    );
    if (status != OS_STATUS_OK)
    {
        os_process_image_release(image);
        free(argv);
        return status;
    }

    task->owned_wasm_bytes = image->owned_bytes;
#if OS_HAS_FS
    task->image = image->file;
#endif
    task->argv = argv;
    task->argc = argc;
    task->parent_id = parent != NULL ? parent->id : 0U;
    task->spawned = 1U;
    /* Host-spawned processes stay until the host deletes them, like os_task_create. */
    task->auto_reap = parent != NULL ? 1U : 0U;
    ++g_os.spawned_task_count;

    *out_task = task;
    os_preempt_for_higher_priority_ready_task();
    return OS_STATUS_OK;
}

OsStatus os_task_spawn(
    OsTaskHandle* out_task,
    const uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* const* argv,
    uint32_t argc,
    uint32_t priority,
    uint32_t stack_size
)
{
    OsProcessImage image;
    uint8_t* bytes_copy = NULL;
    char** argv_copy = NULL;
    uint32_t index = 0U;

    if (out_task == NULL || wasm_bytes == NULL || wasm_size == 0U ||
        (argc > 0U && argv == NULL) || argc > OS_PROC_MAX_ARGS)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0U; index < argc; ++index)
    {
        if (argv[index] == NULL)
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
    }

    *out_task = NULL;
    bytes_copy = (uint8_t*)malloc(wasm_size);
    if (bytes_copy == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memcpy(bytes_copy, wasm_bytes, wasm_size);

    if (argc > 0U)
    {
        argv_copy = os_pack_argv(argv, argc);
        if (argv_copy == NULL)
        {
            free(bytes_copy);
            return OS_STATUS_OUT_OF_MEMORY;
        }
    }

    memset(&image, 0, sizeof(image));
    image.bytes = bytes_copy;
    image.size = wasm_size;
    image.owned_bytes = bytes_copy;
    return os_task_spawn_owned(out_task, &image, argv_copy, argc, priority, stack_size);
}

#if OS_HAS_FS
OsStatus os_task_spawn_path(
    OsTaskHandle* out_task,
    const char* path,
    const char* const* argv,
    uint32_t argc,
    uint32_t priority,
    uint32_t stack_size
)
{
    OsProcessImage image;
    char** argv_copy = NULL;
    uint32_t index = 0U;
    OsStatus status = OS_STATUS_OK;

    if (out_task == NULL || path == NULL || path[0] == '\0' ||
        (argc > 0U && argv == NULL) || argc > OS_PROC_MAX_ARGS)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0U; index < argc; ++index)
    {
        if (argv[index] == NULL)
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
    }

    *out_task = NULL;
    /* Without arguments the program still sees its path as argv[0]. */
    argv_copy = argc > 0U ? os_pack_argv(argv, argc) : os_pack_argv(&path, 1U);
    if (argv_copy == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    memset(&image, 0, sizeof(image));
    status = os_fs_image_open(path, &image.file);
    if (status != OS_STATUS_OK)
    {
        free(argv_copy);
        return status;
    }
    image.bytes = image.file.bytes;
    image.size = image.file.size;
    return os_task_spawn_owned(out_task, &image, argv_copy, argc > 0U ? argc : 1U, priority, stack_size);
}
#endif

uint32_t os_task_get_parent_id(OsTaskHandle task)
{
    if (!os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->parent_id;
}

static OsStatus os_process_exit_status(OsTaskHandle task)
{
    if (task->exit_reason == OS_TASK_EXIT_WASM_ERROR)
    {
        return OS_STATUS_WASM_ERROR;
    }
    if (task->exit_reason == OS_TASK_EXIT_DELETED)
    {
        return OS_STATUS_ABORTED;
    }
    return OS_STATUS_OK;
}

/*
 * Called whenever a task becomes dead or is destroyed. Wakes a parent that
 * waits for it, marks unreachable spawned children for reclamation and
 * orphans the task's own children. Never frees a task: the scheduler may
 * still be using the dead one, so reclamation happens in os_reap_processes.
 */
static void os_process_on_task_dead(OsTaskHandle task)
{
    OsTaskHandle parent = NULL;
    OsTaskHandle child = NULL;

    if (task == NULL)
    {
        return;
    }

    for (child = g_os.task_list; child != NULL; child = child->next)
    {
        if (child != task && child->parent_id == task->id && task->id != 0U)
        {
            child->parent_id = 0U;
            if (child->auto_reap && child->state == OS_TASK_DEAD)
            {
                child->reap_pending = 1U;
            }
        }
    }

    if (task->exit_collected)
    {
        return;
    }

    parent = task->parent_id != 0U ? os_task_find_by_id(task->parent_id) : NULL;
    if (parent == NULL || parent->state == OS_TASK_DEAD)
    {
        if (task->auto_reap)
        {
            task->reap_pending = 1U;
        }
        return;
    }

    if (parent->state == OS_TASK_WAITING &&
        parent->wait_type == OS_TASK_WAIT_CHILD &&
        parent->wait_child_id == task->id)
    {
        task->exit_collected = 1U;
        task->reap_pending = task->auto_reap;
        if (parent->wait_return_kind == OS_WAIT_RETURN_ABI)
        {
            os_complete_task_wait(parent, OS_STATUS_OK,
                                  (uint32_t)os_abi_child_result(task->exit_reason, task->exit_code));
        }
        else
        {
            os_complete_task_wait(parent, os_process_exit_status(task), task->exit_code);
        }
        os_recalculate_task_counters();
        os_preempt_for_higher_priority_ready_task();
    }
}

static void os_reap_processes(void)
{
    OsTaskHandle task = g_os.task_list;

    while (task != NULL)
    {
        OsTaskHandle next = task->next;

        if (task->reap_pending && task->state == OS_TASK_DEAD &&
            task != g_os.current_task)
        {
            os_destroy_task(task);
            /* Destroying may orphan and mark later tasks; restart the scan. */
            next = g_os.task_list;
        }

        task = next;
    }
}

#if defined(OS_HAS_WASM3_WASI)
/* WASI keeps one argument context; point it at the task about to run. */
static void os_apply_task_wasi_args(OsTaskHandle task)
{
    m3_wasi_context_t* context = m3_GetWasiContext();

    if (context == NULL)
    {
        return;
    }

    if (task->argv != NULL && task->argc > 0U)
    {
        context->argc = task->argc;
        context->argv = (ccstr_t*)task->argv;
    }
    else
    {
        task->default_argv[0] = task->name;
        context->argc = 1U;
        context->argv = (ccstr_t*)task->default_argv;
    }
}
#endif


#endif /* OS_HAS_PROCESSES */

static void os_set_task_dead(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    if (g_os.current_task == task)
    {
        g_os.current_task = NULL;
    }

    os_cancel_task_wait(task, OS_STATUS_TASK_DEAD);
    os_release_task_mutexes(task);
    os_timer_delete_for_task(task);
    task->state = OS_TASK_DEAD;
    task->wake_tick_ms = 0U;
    os_task_cleanup_wasm(task);
    /* A dead program's handles go now, so the objects it held do too. */
    os_abi_release_task(task);
#if OS_HAS_PROCESSES
    os_process_on_task_dead(task);
#endif
    os_wake_any_waiters();
    os_clock_rearm();
}

static void os_record_task_exit(OsTaskHandle task, OsTaskExitReason reason, uint32_t code)
{
    if (task == NULL || task->exit_reason != OS_TASK_EXIT_NONE)
    {
        return;
    }

    task->exit_reason = reason;
    task->exit_code = code;
}

static void os_request_task_stop(OsTaskHandle task)
{
    if (task == NULL || task->wasm_runtime == NULL)
    {
        return;
    }

    m3_SetFuel(task->wasm_runtime, 0U);
}

static void os_request_task_delete(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    task->delete_requested = 1U;
    os_record_task_exit(task, OS_TASK_EXIT_DELETED, 0U);
    os_timer_delete_for_task(task);
    task->state = OS_TASK_DEAD;
    task->wake_tick_ms = 0U;
    os_request_task_stop(task);
    os_clock_rearm();
}

static void os_destroy_task(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    if (!os_task_is_in_list(task))
    {
        return;
    }

    if (g_os.current_task == task)
    {
        g_os.current_task = NULL;
    }

    if (g_os.last_scheduled_task == task)
    {
        g_os.last_scheduled_task = task->previous;
    }

    os_cancel_task_wait(task, OS_STATUS_TASK_DEAD);
    os_release_task_mutexes(task);
    os_timer_delete_for_task(task);
    os_record_task_exit(task, OS_TASK_EXIT_DELETED, 0U);
    task->state = OS_TASK_DEAD;
#if OS_HAS_PROCESSES
    os_process_on_task_dead(task);
#endif
    os_task_list_remove(task);
    os_task_free(task);
    os_recalculate_task_counters();
    os_clock_rearm();
}

static uint8_t os_time_reached(uint32_t now, uint32_t target)
{
    return ((int32_t)(now - target) >= 0) ? 1U : 0U;
}

static void os_clear_last_error(void)
{
    g_os.last_error.status = OS_STATUS_OK;
    g_os.last_error.result = NULL;
    g_os.last_error.phase = NULL;
    g_os.last_error.task_name[0] = '\0';
    g_os.last_error.has_task_name = 0U;
}

static void os_set_last_error(OsTaskHandle task, const char* phase, M3Result result, OsStatus status)
{
    g_os.last_error.status = status;
    g_os.last_error.result = result;
    g_os.last_error.phase = phase != NULL ? phase : "none";
    g_os.last_error.task_name[0] = '\0';
    g_os.last_error.has_task_name = 0U;

    if (task != NULL && task->name[0] != '\0')
    {
        strncpy(g_os.last_error.task_name, task->name, OS_TASK_NAME_MAX - 1U);
        g_os.last_error.task_name[OS_TASK_NAME_MAX - 1U] = '\0';
        g_os.last_error.has_task_name = 1U;
    }
}

/* ---------------------------------------------------------------------------
 * Support for the guest ABI layer (abi.c): guest calls, object names and
 * references, counting timers and waiting for any of several objects.
 * ------------------------------------------------------------------------ */

/*
 * An import that may block names its result slot first. Some waits clear the
 * previous wait state when they start, so the slot is attached again once
 * the import knows whether the task blocks.
 */
static OsStatus os_import_wait_begin(uint32_t* raw_return)
{
    OsStatus status = os_prepare_wait_return(g_os.current_task, raw_return);

    g_os.import_return_slot = status == OS_STATUS_OK ? raw_return : NULL;
    return status;
}

static M3Result os_import_wait_end(uint8_t abi)
{
    OsTaskHandle task = g_os.current_task;
    uint32_t* slot = g_os.import_return_slot;

    g_os.import_return_slot = NULL;
    if (task != NULL && task->state == OS_TASK_WAITING && task->wait_type != OS_TASK_WAIT_NONE && slot != NULL &&
        os_prepare_wait_return(task, slot) == OS_STATUS_OK)
    {
        if (abi)
        {
            /* The wakeup writes the value or a negative ABI error. */
            task->wait_return_kind = OS_WAIT_RETURN_ABI;
        }
        return m3_Yield();
    }
    os_clear_wait_return(task);
    return m3Err_none;
}

OsStatus os_guest_begin(uint32_t* raw_return)
{
    return os_import_wait_begin(raw_return);
}

M3Result os_guest_end(void)
{
    return os_import_wait_end(1U);
}

OsStatus os_guest_memory(uint32_t address, uint32_t length, uint8_t** out_memory)
{
    return os_task_get_memory_pointer(g_os.current_task, address, length, out_memory);
}

void** os_task_abi_state(OsTaskHandle task)
{
    return task != NULL ? &task->abi_state : NULL;
}

typedef struct OsObjectFields
{
    char** name;
    uint32_t* guest_refs;
    uint8_t* guest_owned;
} OsObjectFields;

#define OS_OBJECT_FIELDS_OF(object, out) \
    do { (out)->name = &(object)->name; (out)->guest_refs = &(object)->guest_refs; \
         (out)->guest_owned = &(object)->guest_owned; } while (0)

static uint8_t os_object_fields(OsObjectType type, uint32_t id, OsObjectFields* out)
{
    switch (type)
    {
        case OS_OBJECT_QUEUE:
        {
            OsQueueHandle object = os_queue_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_MUTEX:
        {
            OsMutexHandle object = os_mutex_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_SEMAPHORE:
        {
            OsSemaphoreHandle object = os_semaphore_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_EVENT_GROUP:
        {
            OsEventGroupHandle object = os_event_group_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_TIMER:
        {
            OsTimerHandle object = os_timer_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        default:
            return 0U;
    }
}

static uint8_t os_object_name_matches(const char* object_name, const char* name, uint32_t name_length)
{
    return object_name != NULL && strlen(object_name) == name_length &&
           memcmp(object_name, name, name_length) == 0;
}

OsStatus os_object_adopt(OsObjectType type, uint32_t id, const char* name, uint32_t name_length)
{
    OsObjectFields fields;

    if (!os_object_fields(type, id, &fields))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (name_length > 0U)
    {
        char* copy = (char*)malloc((size_t)name_length + 1U);
        if (copy == NULL)
        {
            return OS_STATUS_OUT_OF_MEMORY;
        }
        memcpy(copy, name, name_length);
        copy[name_length] = '\0';
        free(*fields.name);
        *fields.name = copy;
    }
    *fields.guest_owned = 1U;
    *fields.guest_refs = 1U;
    return OS_STATUS_OK;
}

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

uint32_t os_object_find_by_name(OsObjectType type, const char* name, uint32_t name_length)
{
    if (name == NULL || name_length == 0U)
    {
        return 0U;
    }
    switch (type)
    {
        case OS_OBJECT_QUEUE: OS_FIND_NAMED(g_os.queue_list, OsQueueHandle);
        case OS_OBJECT_MUTEX: OS_FIND_NAMED(g_os.mutex_list, OsMutexHandle);
        case OS_OBJECT_SEMAPHORE: OS_FIND_NAMED(g_os.semaphore_list, OsSemaphoreHandle);
        case OS_OBJECT_EVENT_GROUP: OS_FIND_NAMED(g_os.event_group_list, OsEventGroupHandle);
        case OS_OBJECT_TIMER: OS_FIND_NAMED(g_os.timer_list, OsTimerHandle);
        default: return 0U;
    }
}

void os_object_retain(OsObjectType type, uint32_t id)
{
    OsObjectFields fields;

    if (os_object_fields(type, id, &fields) && *fields.guest_owned)
    {
        ++*fields.guest_refs;
    }
}

void os_object_release(OsObjectType type, uint32_t id)
{
    OsObjectFields fields;

    if (!os_object_fields(type, id, &fields) || !*fields.guest_owned)
    {
        return;
    }
    if (*fields.guest_refs > 1U)
    {
        --*fields.guest_refs;
        return;
    }
    switch (type)
    {
        case OS_OBJECT_QUEUE: os_queue_delete(os_queue_find_by_id(id)); break;
        case OS_OBJECT_MUTEX: os_mutex_delete(os_mutex_find_by_id(id)); break;
        case OS_OBJECT_SEMAPHORE: os_semaphore_delete(os_semaphore_find_by_id(id)); break;
        case OS_OBJECT_EVENT_GROUP: os_event_group_delete(os_event_group_find_by_id(id)); break;
        case OS_OBJECT_TIMER: os_timer_delete(os_timer_find_by_id(id)); break;
        default: break;
    }
}

OsStatus os_timer_create_counting(OsTimerHandle* out_timer, uint32_t period_ms, uint8_t auto_reload)
{
    OsTimerHandle timer = NULL;

    if (out_timer == NULL || period_ms == 0U || period_ms > (uint32_t)INT32_MAX)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    *out_timer = NULL;

    timer = os_timer_allocate();
    if (timer == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    timer->id = g_os.next_timer_id++;
    timer->period_ms = period_ms;
    timer->auto_reload = auto_reload ? 1U : 0U;
    timer->counting = 1U;
    os_timer_list_insert(timer);
    ++g_os.timer_count;
    *out_timer = timer;
    return OS_STATUS_OK;
}

uint32_t os_timer_take_expirations(OsTimerHandle timer)
{
    uint32_t expirations = 0U;

    if (!os_timer_is_in_list(timer))
    {
        return 0U;
    }
    expirations = timer->expirations;
    timer->expirations = 0U;
    return expirations;
}

uint8_t os_wait_target_ready(const OsWaitTarget* target)
{
    switch (target->type)
    {
        case OS_OBJECT_QUEUE:
        {
            OsQueueHandle queue = os_queue_find_by_id(target->id);
            return queue != NULL && queue->count > 0U;
        }
        case OS_OBJECT_SEMAPHORE:
        {
            OsSemaphoreHandle semaphore = os_semaphore_find_by_id(target->id);
            return semaphore != NULL && semaphore->count > 0U;
        }
        case OS_OBJECT_EVENT_GROUP:
        {
            OsEventGroupHandle event_group = os_event_group_find_by_id(target->id);
            return event_group != NULL && event_group->bits != 0U;
        }
        case OS_OBJECT_TIMER:
        {
            OsTimerHandle timer = os_timer_find_by_id(target->id);
            return timer != NULL && timer->expirations > 0U;
        }
        case OS_OBJECT_TASK:
        {
            OsTaskHandle task = os_task_find_by_id(target->id);
            return task != NULL && task->state == OS_TASK_DEAD;
        }
#if OS_HAS_GPIO
        case OS_OBJECT_GPIO:
            return os_gpio_ready(target->id);
#endif
        default:
            return 0U;
    }
}

OsStatus os_wait_any(const OsWaitTarget* targets, uint32_t count, uint32_t timeout_ms, uint32_t* out_index)
{
    OsTaskHandle task = g_os.current_task;
    OsWaitTarget* copy = NULL;
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;

    if (targets == NULL || count == 0U || out_index == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < count; ++index)
    {
        if (os_wait_target_ready(&targets[index]))
        {
            *out_index = index;
            return OS_STATUS_OK;
        }
    }
    if (timeout_ms == 0U)
    {
        return OS_STATUS_TIMEOUT;
    }
    if (task == NULL || task->state != OS_TASK_RUNNING)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }
    status = os_sync_clock_before_deadline();
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    copy = (OsWaitTarget*)malloc((size_t)count * sizeof(OsWaitTarget));
    if (copy == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memcpy(copy, targets, (size_t)count * sizeof(OsWaitTarget));
    status = os_begin_task_wait(task, OS_TASK_WAIT_ANY, timeout_ms);
    if (status != OS_STATUS_OK)
    {
        free(copy);
        return status;
    }
    task->wait_targets = copy;
    task->wait_target_count = count;
    task->wait_return_kind = OS_WAIT_RETURN_VALUE;
    return OS_STATUS_OK;
}

/* Wake tasks waiting for any of several objects once one of them is ready. */
void os_wait_any_recheck(void)
{
    os_wake_any_waiters();
}

static void os_wake_any_waiters(void)
{
    OsTaskHandle task = NULL;
    uint8_t woke = 0U;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        uint32_t index = 0U;

        if (task->state != OS_TASK_WAITING || task->wait_type != OS_TASK_WAIT_ANY)
        {
            continue;
        }
        for (index = 0U; index < task->wait_target_count; ++index)
        {
            if (os_wait_target_ready(&task->wait_targets[index]))
            {
                os_complete_task_wait(task, OS_STATUS_OK, index);
                woke = 1U;
                break;
            }
        }
    }
    if (woke)
    {
        os_recalculate_task_counters();
    }
}

uint32_t os_guest_last_wait_value(void)
{
    return g_os.current_task != NULL ? g_os.current_task->last_wait_value : 0U;
}

uint64_t os_time_ms64(void)
{
    return g_os.tick_ms_extended;
}

#if OS_HAS_PROCESSES
uint8_t os_child_is_mine(uint32_t child_id)
{
    OsTaskHandle task = g_os.current_task;
    OsTaskHandle child = os_task_find_by_id(child_id);

    return task != NULL && child != NULL && child->parent_id == task->id && !child->exit_collected;
}

OsStatus os_child_wait(uint32_t child_id, uint32_t timeout_ms, int32_t* out_result)
{
    OsTaskHandle task = g_os.current_task;
    OsTaskHandle child = os_task_find_by_id(child_id);

    if (!os_child_is_mine(child_id) || out_result == NULL)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }
    if (child->state == OS_TASK_DEAD)
    {
        *out_result = os_abi_child_result(child->exit_reason, child->exit_code);
        child->exit_collected = 1U;
        child->reap_pending = child->auto_reap;
        return OS_STATUS_OK;
    }
    if (timeout_ms == 0U)
    {
        return OS_STATUS_TIMEOUT;
    }
    task->wait_child_id = child->id;
    task->wait_return_kind = OS_WAIT_RETURN_VALUE;
    return os_begin_task_wait(task, OS_TASK_WAIT_CHILD, timeout_ms);
}

OsStatus os_child_kill(uint32_t child_id)
{
    OsTaskHandle child = os_task_find_by_id(child_id);

    if (!os_child_is_mine(child_id))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }
    if (child->state == OS_TASK_DEAD)
    {
        return OS_STATUS_TASK_DEAD;
    }
    /* Keep the dead child until the parent collects it with wait. */
    os_record_task_exit(child, OS_TASK_EXIT_DELETED, 0U);
    os_set_task_dead(child);
    os_recalculate_task_counters();
    return OS_STATUS_OK;
}

uint32_t os_current_parent_id(void)
{
    return g_os.current_task != NULL ? g_os.current_task->parent_id : 0U;
}
#endif

OsStatus os_host_wait_begin(uint32_t* result_slot)
{
    return os_import_wait_begin(result_slot);
}

M3Result os_host_wait_end(void)
{
    return os_import_wait_end(0U);
}

static OsStatus os_object_set_name(OsObjectType type, uint32_t id, const char* name)
{
    OsObjectFields fields;
    size_t length = name != NULL ? strlen(name) : 0U;
    char* copy = NULL;

    if (id == 0U || !os_object_fields(type, id, &fields))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (length > 0U)
    {
        if (os_object_find_by_name(type, name, (uint32_t)length) != 0U)
        {
            return OS_STATUS_BUSY;
        }
        copy = (char*)malloc(length + 1U);
        if (copy == NULL)
        {
            return OS_STATUS_OUT_OF_MEMORY;
        }
        memcpy(copy, name, length + 1U);
    }
    free(*fields.name);
    *fields.name = copy;
    return OS_STATUS_OK;
}

OsStatus os_queue_set_name(OsQueueHandle queue, const char* name)
{
    return os_object_set_name(OS_OBJECT_QUEUE, os_queue_get_id(queue), name);
}

OsStatus os_mutex_set_name(OsMutexHandle mutex, const char* name)
{
    return os_object_set_name(OS_OBJECT_MUTEX, os_mutex_get_id(mutex), name);
}

OsStatus os_semaphore_set_name(OsSemaphoreHandle semaphore, const char* name)
{
    return os_object_set_name(OS_OBJECT_SEMAPHORE, os_semaphore_get_id(semaphore), name);
}

OsStatus os_event_group_set_name(OsEventGroupHandle event_group, const char* name)
{
    return os_object_set_name(OS_OBJECT_EVENT_GROUP, os_event_group_get_id(event_group), name);
}
