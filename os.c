#include "os.h"

#include "wasm3/source/wasm3.h"
#include "wasm3/source/m3_env.h"
#include "wasm3/source/m3_api_wasi.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define OS_TASK_NAME_MAX 32U
#define OS_DEFAULT_STACK_SIZE (64U * 1024U)
#define OS_TASK_SLICE_FUEL 10000ULL

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
    OS_TASK_WAIT_NOTIFICATION_TAKE
} OsTaskWaitType;

typedef enum OsWaitReturnKind
{
    OS_WAIT_RETURN_STATUS = 0,
    OS_WAIT_RETURN_VALUE
} OsWaitReturnKind;

typedef struct OsLastError
{
    OsStatus status;
    const char* result;
    const char* phase;
    char task_name[OS_TASK_NAME_MAX];
    uint8_t has_task_name;
} OsLastError;

typedef struct OsHostImportNode
{
    char* module_name;
    char* import_name;
    char* signature;
    OsHostImportFunction function;
    struct OsHostImportNode* next;
} OsHostImportNode;

struct OsQueue
{
    uint32_t id;
    uint32_t item_size;
    uint32_t item_count;
    uint32_t count;
    uint32_t head;
    uint32_t tail;
    uint8_t* storage;
    struct OsQueue* previous;
    struct OsQueue* next;
};

struct OsMutex
{
    uint32_t id;
    OsTaskHandle owner;
    struct OsMutex* previous;
    struct OsMutex* next;
};

struct OsSemaphore
{
    uint32_t id;
    uint32_t max_count;
    uint32_t count;
    struct OsSemaphore* previous;
    struct OsSemaphore* next;
};

struct OsEventGroup
{
    uint32_t id;
    uint32_t bits;
    struct OsEventGroup* previous;
    struct OsEventGroup* next;
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
    OsHostImportNode* host_import_list;
    uint32_t tick_ms;
    uint32_t last_time_update_ms;
    uint32_t next_task_id;
    uint32_t next_queue_id;
    uint32_t next_mutex_id;
    uint32_t next_semaphore_id;
    uint32_t next_event_group_id;
    uint64_t next_wait_sequence;
    uint32_t task_count;
    uint32_t ready_task_count;
    uint32_t waiting_task_count;
    uint8_t initialized;
    uint8_t time_update_initialized;
    uint8_t preempt_requested;
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
    void* wait_queue_output;
    uint8_t wait_queue_has_wasm_output;
    uint32_t wait_queue_wasm_output_address;
    uint32_t wait_bits;
    uint8_t wait_clear_on_exit;
    uint8_t wait_for_all;
    uint8_t wait_has_timeout;
    uint8_t wait_has_return_slot;
    uint32_t wait_return_offset;
    OsWaitReturnKind wait_return_kind;
    uint8_t wait_has_wasm_output;
    uint32_t wait_wasm_output_address;
    uint32_t last_wait_value;
    OsStatus last_wait_status;

    uint32_t notification_value;
    uint32_t notification_clear_on_exit;
    uint8_t notification_pending;
    uint8_t notification_take_clear_count;

    uint8_t* wasm_bytes;
    uint32_t wasm_size;
    const char* entry_function_name;
    uint32_t stack_size;
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
static OsTaskHandle os_task_allocate(void);
static void os_task_free(OsTaskHandle task);
static void os_task_list_insert(OsTaskHandle task);
static void os_task_list_remove(OsTaskHandle task);
static int os_task_is_in_list(OsTaskHandle task);
static uint8_t os_task_is_dead(OsTaskHandle task);
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
static M3Result os_link_registered_host_imports(OsTaskHandle task);
static m3ApiRawFunction(os_wasm_import_os_yield);
static m3ApiRawFunction(os_wasm_import_os_delay_ms);
static m3ApiRawFunction(os_wasm_import_os_get_time_ms);
static m3ApiRawFunction(os_wasm_import_os_queue_send);
static m3ApiRawFunction(os_wasm_import_os_queue_receive);
static m3ApiRawFunction(os_wasm_import_os_queue_send_wait);
static m3ApiRawFunction(os_wasm_import_os_queue_receive_wait);
static m3ApiRawFunction(os_wasm_import_os_mutex_create);
static m3ApiRawFunction(os_wasm_import_os_mutex_delete);
static m3ApiRawFunction(os_wasm_import_os_mutex_lock);
static m3ApiRawFunction(os_wasm_import_os_mutex_unlock);
static m3ApiRawFunction(os_wasm_import_os_semaphore_create);
static m3ApiRawFunction(os_wasm_import_os_semaphore_delete);
static m3ApiRawFunction(os_wasm_import_os_semaphore_take);
static m3ApiRawFunction(os_wasm_import_os_semaphore_give);
static m3ApiRawFunction(os_wasm_import_os_event_group_create);
static m3ApiRawFunction(os_wasm_import_os_event_group_delete);
static m3ApiRawFunction(os_wasm_import_os_event_group_get_bits);
static m3ApiRawFunction(os_wasm_import_os_event_group_set_bits);
static m3ApiRawFunction(os_wasm_import_os_event_group_clear_bits);
static m3ApiRawFunction(os_wasm_import_os_event_group_wait_bits);
static m3ApiRawFunction(os_wasm_import_os_task_notify);
static m3ApiRawFunction(os_wasm_import_os_task_notify_wait);
static m3ApiRawFunction(os_wasm_import_os_task_notify_take);
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
static void os_update_waiting_tasks(void);
static void os_advance_tick_ms(uint32_t milliseconds);
static OsTaskRunResult os_run_task_slice(OsTaskHandle task);
static OsStatus os_validate_entry_function(OsTaskHandle task);
static OsStatus os_capture_entry_return_value(OsTaskHandle task);
static OsStatus os_call_entry_function(OsTaskHandle task, M3Result* out_result);
static OsStatus os_task_get_memory_pointer(
    OsTaskHandle task,
    uint32_t wasm_address,
    uint32_t byte_count,
    uint8_t** out_memory
);
static void os_set_task_ready(OsTaskHandle task);
static void os_set_task_waiting(OsTaskHandle task, uint32_t wake_tick_ms);
static void os_set_task_suspended(OsTaskHandle task);
static void os_set_task_dead(OsTaskHandle task);
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
static void os_write_wait_output(OsTaskHandle task, uint32_t value);
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
    g_os.next_wait_sequence = 1U;
    g_os.initialized = 1U;
    os_clear_last_error();

    return OS_STATUS_OK;
}

void os_shutdown(void)
{
    OsTaskHandle task = g_os.task_list;

    while (task != NULL)
    {
        OsTaskHandle next = task->next;
        os_task_free(task);
        task = next;
    }

    os_queue_free_all();
    os_mutex_free_all();
    os_semaphore_free_all();
    os_event_group_free_all();
    os_host_import_free_list();

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

    task->wait_object.queue = queue;
    task->wait_queue_output = out_item;
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
        entry_function_name == NULL || entry_function_name[0] == '\0' ||
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
    task->stack_size = (stack_size == 0U) ? OS_DEFAULT_STACK_SIZE : stack_size;
    os_copy_task_name(task, task_name);

    status = os_task_copy_entry_args(task, entry_args, entry_arg_count);
    if (status != OS_STATUS_OK)
    {
        os_task_free(task);
        return status;
    }

    status = os_task_create_wasm(
        task,
        wasm_bytes,
        wasm_size,
        entry_function_name,
        task->stack_size
    );

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
    if (g_os.current_task == NULL)
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (delay_ms == 0U)
    {
        return os_task_yield();
    }

    os_set_task_waiting(g_os.current_task, g_os.tick_ms + delay_ms);
    os_request_task_stop(g_os.current_task);
    os_recalculate_task_counters();

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

OsStatus os_task_get_snapshot_size(OsTaskHandle task, uint32_t* out_size)
{
    M3Result result = m3Err_none;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || out_size == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task) || task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (g_os.current_task == task)
    {
        return OS_STATUS_ERROR;
    }

    result = m3_GetRuntimeSnapshotSize(task->wasm_runtime, out_size);
    status = os_wasm_snapshot_result_to_status(result);
    if (status == OS_STATUS_OK)
    {
        os_clear_last_error();
    }
    else
    {
        os_set_last_error(task, "snapshot_size", result, status);
    }

    return status;
}

OsStatus os_task_save_snapshot(
    OsTaskHandle task,
    uint8_t* buffer,
    uint32_t buffer_size,
    uint32_t* out_size
)
{
    M3Result result = m3Err_none;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || buffer == NULL || buffer_size == 0U || out_size == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task) || task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (g_os.current_task == task)
    {
        return OS_STATUS_ERROR;
    }

    result = m3_SaveRuntimeSnapshot(task->wasm_runtime, buffer, buffer_size, out_size);
    status = os_wasm_snapshot_result_to_status(result);
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

OsStatus os_task_load_snapshot(OsTaskHandle task, const uint8_t* buffer, uint32_t buffer_size)
{
    M3Result result = m3Err_none;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || buffer == NULL || buffer_size == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    if (os_task_is_dead(task) || task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (g_os.current_task == task)
    {
        return OS_STATUS_ERROR;
    }

    result = m3_LoadRuntimeSnapshot(task->wasm_runtime, buffer, buffer_size);
    status = os_wasm_snapshot_result_to_status(result);
    if (status == OS_STATUS_OK)
    {
        os_clear_last_error();
        task->wasm_started = 1U;
        task->wasm_needs_resume = 1U;
        task->entry_return_code = 0U;
        os_set_task_ready(task);
        os_recalculate_task_counters();
    }
    else
    {
        os_set_last_error(task, "snapshot_load", result, status);
    }

    return status;
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
        os_update_waiting_tasks();
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
            os_update_waiting_tasks();
        }
    }

    return OS_STATUS_OK;
}

uint32_t os_get_tick_ms(void)
{
    return g_os.tick_ms;
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

    os_update_waiting_tasks();

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

    run_result = os_run_task_slice(task);
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
    os_clear_queue_wait(task);
    os_task_free_entry_args(task);
    os_task_free_return_values(task);
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

    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }

    if (task->wasm_runtime->memory.mallocated == NULL)
    {
        return byte_count == 0U ? OS_STATUS_OK : OS_STATUS_OUT_OF_BOUNDS;
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
}

static M3Result os_link_task_host_imports(OsTaskHandle task)
{
    static const struct OsBuiltinImport
    {
        const char* name;
        const char* signature;
        M3RawCall function;
    } imports[] =
    {
        { "os_yield", "v()", os_wasm_import_os_yield },
        { "os_delay_ms", "v(i)", os_wasm_import_os_delay_ms },
        { "os_get_time_ms", "i()", os_wasm_import_os_get_time_ms },
        { "os_queue_send", "i(ii)", os_wasm_import_os_queue_send },
        { "os_queue_receive", "i(ii)", os_wasm_import_os_queue_receive },
        { "os_queue_send_wait", "i(iii)", os_wasm_import_os_queue_send_wait },
        { "os_queue_receive_wait", "i(iii)", os_wasm_import_os_queue_receive_wait },
        { "os_mutex_create", "i()", os_wasm_import_os_mutex_create },
        { "os_mutex_delete", "i(i)", os_wasm_import_os_mutex_delete },
        { "os_mutex_lock", "i(ii)", os_wasm_import_os_mutex_lock },
        { "os_mutex_unlock", "i(i)", os_wasm_import_os_mutex_unlock },
        { "os_semaphore_create", "i(ii)", os_wasm_import_os_semaphore_create },
        { "os_semaphore_delete", "i(i)", os_wasm_import_os_semaphore_delete },
        { "os_semaphore_take", "i(ii)", os_wasm_import_os_semaphore_take },
        { "os_semaphore_give", "i(i)", os_wasm_import_os_semaphore_give },
        { "os_event_group_create", "i()", os_wasm_import_os_event_group_create },
        { "os_event_group_delete", "i(i)", os_wasm_import_os_event_group_delete },
        { "os_event_group_get_bits", "i(i)", os_wasm_import_os_event_group_get_bits },
        { "os_event_group_set_bits", "i(ii)", os_wasm_import_os_event_group_set_bits },
        { "os_event_group_clear_bits", "i(ii)", os_wasm_import_os_event_group_clear_bits },
        { "os_event_group_wait_bits", "i(iiiii)", os_wasm_import_os_event_group_wait_bits },
        { "os_task_notify", "i(iii)", os_wasm_import_os_task_notify },
        { "os_task_notify_wait", "i(iiii)", os_wasm_import_os_task_notify_wait },
        { "os_task_notify_take", "i(ii)", os_wasm_import_os_task_notify_take }
    };
    uint32_t import_index = 0U;
    M3Result result = m3Err_none;

    if (task == NULL || task->wasm_module == NULL)
    {
        return m3Err_moduleNotLinked;
    }

    for (import_index = 0U;
         import_index < (uint32_t)(sizeof(imports) / sizeof(imports[0]));
         ++import_index)
    {
        result = m3_LinkRawFunction(
            task->wasm_module,
            "env",
            imports[import_index].name,
            imports[import_index].signature,
            imports[import_index].function
        );

        if (result != m3Err_none && result != m3Err_functionLookupFailed)
        {
            return result;
        }
    }

    result = os_link_registered_host_imports(task);
    if (result != m3Err_none)
    {
        return result;
    }

#if defined(OS_HAS_WASM3_WASI)
    result = m3_LinkWASI(task->wasm_module);
    if (result != m3Err_none)
    {
        return result;
    }
#endif

    return m3Err_none;
}

static M3Result os_link_registered_host_imports(OsTaskHandle task)
{
    OsHostImportNode* node = NULL;
    M3Result result = m3Err_none;

    if (task == NULL || task->wasm_module == NULL)
    {
        return m3Err_moduleNotLinked;
    }

    for (node = g_os.host_import_list; node != NULL; node = node->next)
    {
        result = m3_LinkRawFunction(
            task->wasm_module,
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

static m3ApiRawFunction(os_wasm_import_os_yield)
{
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_sp;
    (void)_mem;

    status = os_task_yield();
    if (status != OS_STATUS_OK)
    {
        m3ApiTrap(m3Err_trapAbort);
    }

    return m3_Yield();
}

static m3ApiRawFunction(os_wasm_import_os_delay_ms)
{
    OsStatus status = OS_STATUS_OK;
    m3ApiGetArg(uint32_t, delay_ms);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    status = os_task_delay_ms(delay_ms);
    if (status != OS_STATUS_OK)
    {
        m3ApiTrap(m3Err_trapAbort);
    }

    return m3_Yield();
}

static m3ApiRawFunction(os_wasm_import_os_get_time_ms)
{
    m3ApiReturnType(uint32_t);
    (void)runtime;
    (void)_ctx;
    (void)_sp;
    (void)_mem;

    m3ApiReturn(os_get_tick_ms());
}




static m3ApiRawFunction(os_wasm_import_os_queue_send)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, queue_id);
    m3ApiGetArg(uint32_t, item_ptr);
    OsQueueHandle queue = os_queue_find_by_id(queue_id);
    OsTaskHandle task = os_task_get_current();
    uint8_t* item = NULL;
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (queue == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_QUEUE_NOT_FOUND);
    }

    item = (uint8_t*)malloc(queue->item_size);
    if (item == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_OUT_OF_MEMORY);
    }

    status = os_task_read_memory(task, item_ptr, item, queue->item_size);
    if (status == OS_STATUS_OK)
    {
        status = os_queue_send(queue, item);
    }

    free(item);
    m3ApiReturn((uint32_t)status);
}

static m3ApiRawFunction(os_wasm_import_os_queue_receive)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, queue_id);
    m3ApiGetArg(uint32_t, item_ptr);
    OsQueueHandle queue = os_queue_find_by_id(queue_id);
    OsTaskHandle task = os_task_get_current();
    uint8_t* item = NULL;
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (queue == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_QUEUE_NOT_FOUND);
    }

    item = (uint8_t*)malloc(queue->item_size);
    if (item == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_OUT_OF_MEMORY);
    }

    status = os_queue_receive(queue, item);
    if (status == OS_STATUS_OK)
    {
        status = os_task_write_memory(task, item_ptr, item, queue->item_size);
    }

    free(item);
    m3ApiReturn((uint32_t)status);
}

static m3ApiRawFunction(os_wasm_import_os_queue_send_wait)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, queue_id);
    m3ApiGetArg(uint32_t, item_ptr);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsQueueHandle queue = os_queue_find_by_id(queue_id);
    OsTaskHandle task = os_task_get_current();
    uint8_t* item = NULL;
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (queue == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_QUEUE_NOT_FOUND);
    }

    status = os_prepare_wait_return(task, raw_return);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn((uint32_t)status);
    }

    item = (uint8_t*)malloc(queue->item_size);
    if (item == NULL)
    {
        os_clear_wait_return(task);
        m3ApiReturn((uint32_t)OS_STATUS_OUT_OF_MEMORY);
    }

    status = os_task_read_memory(task, item_ptr, item, queue->item_size);
    if (status == OS_STATUS_OK)
    {
        status = os_queue_send_wait(queue, item, timeout_ms);
    }
    free(item);

    *raw_return = (uint32_t)status;
    if (task != NULL && task->wait_type == OS_TASK_WAIT_QUEUE_SEND)
    {
        return m3_Yield();
    }

    os_clear_wait_return(task);
    return m3Err_none;
}

static m3ApiRawFunction(os_wasm_import_os_queue_receive_wait)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, queue_id);
    m3ApiGetArg(uint32_t, item_ptr);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsQueueHandle queue = os_queue_find_by_id(queue_id);
    OsTaskHandle task = os_task_get_current();
    uint8_t* item = NULL;
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (queue == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_QUEUE_NOT_FOUND);
    }

    status = os_prepare_wait_return(task, raw_return);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn((uint32_t)status);
    }

    status = os_task_get_memory_pointer(
        task,
        item_ptr,
        queue->item_size,
        &item
    );
    if (status == OS_STATUS_OK)
    {
        status = os_queue_receive_wait(queue, item, timeout_ms);
    }

    *raw_return = (uint32_t)status;
    if (task != NULL && task->wait_type == OS_TASK_WAIT_QUEUE_RECEIVE)
    {
        task->wait_queue_has_wasm_output = 1U;
        task->wait_queue_wasm_output_address = item_ptr;
        return m3_Yield();
    }

    os_clear_wait_return(task);
    return m3Err_none;
}

static m3ApiRawFunction(os_wasm_import_os_mutex_create)
{
    m3ApiReturnType(uint32_t);
    OsMutexHandle mutex = NULL;
    OsStatus status = os_mutex_create(&mutex);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn(status == OS_STATUS_OK ? os_mutex_get_id(mutex) : 0U);
}

static m3ApiRawFunction(os_wasm_import_os_mutex_delete)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, mutex_id);
    OsMutexHandle mutex = os_mutex_find_by_id(mutex_id);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (mutex == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_MUTEX_NOT_FOUND);
    }

    os_mutex_delete(mutex);
    m3ApiReturn((uint32_t)OS_STATUS_OK);
}

static m3ApiRawFunction(os_wasm_import_os_mutex_lock)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, mutex_id);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsTaskHandle task = os_task_get_current();
    OsMutexHandle mutex = os_mutex_find_by_id(mutex_id);
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (mutex == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_MUTEX_NOT_FOUND);
    }

    status = os_prepare_wait_return(task, raw_return);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn((uint32_t)status);
    }

    status = os_mutex_lock(mutex, timeout_ms);
    *raw_return = (uint32_t)status;
    if (task != NULL && task->wait_type == OS_TASK_WAIT_MUTEX)
    {
        return m3_Yield();
    }

    os_clear_wait_return(task);
    return m3Err_none;
}

static m3ApiRawFunction(os_wasm_import_os_mutex_unlock)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, mutex_id);
    OsMutexHandle mutex = os_mutex_find_by_id(mutex_id);
    OsStatus status = mutex != NULL
        ? os_mutex_unlock(mutex)
        : OS_STATUS_MUTEX_NOT_FOUND;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn((uint32_t)status);
}

static m3ApiRawFunction(os_wasm_import_os_semaphore_create)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, max_count);
    m3ApiGetArg(uint32_t, initial_count);
    OsSemaphoreHandle semaphore = NULL;
    OsStatus status = os_semaphore_create(
        &semaphore,
        max_count,
        initial_count
    );
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn(status == OS_STATUS_OK
        ? os_semaphore_get_id(semaphore)
        : 0U);
}

static m3ApiRawFunction(os_wasm_import_os_semaphore_delete)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, semaphore_id);
    OsSemaphoreHandle semaphore = os_semaphore_find_by_id(semaphore_id);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (semaphore == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_SEMAPHORE_NOT_FOUND);
    }

    os_semaphore_delete(semaphore);
    m3ApiReturn((uint32_t)OS_STATUS_OK);
}

static m3ApiRawFunction(os_wasm_import_os_semaphore_take)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, semaphore_id);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsTaskHandle task = os_task_get_current();
    OsSemaphoreHandle semaphore = os_semaphore_find_by_id(semaphore_id);
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (semaphore == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_SEMAPHORE_NOT_FOUND);
    }

    status = os_prepare_wait_return(task, raw_return);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn((uint32_t)status);
    }

    status = os_semaphore_take(semaphore, timeout_ms);
    *raw_return = (uint32_t)status;
    if (task != NULL && task->wait_type == OS_TASK_WAIT_SEMAPHORE)
    {
        return m3_Yield();
    }

    os_clear_wait_return(task);
    return m3Err_none;
}

static m3ApiRawFunction(os_wasm_import_os_semaphore_give)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, semaphore_id);
    OsSemaphoreHandle semaphore = os_semaphore_find_by_id(semaphore_id);
    OsStatus status = semaphore != NULL
        ? os_semaphore_give(semaphore)
        : OS_STATUS_SEMAPHORE_NOT_FOUND;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn((uint32_t)status);
}

static m3ApiRawFunction(os_wasm_import_os_event_group_create)
{
    m3ApiReturnType(uint32_t);
    OsEventGroupHandle event_group = NULL;
    OsStatus status = os_event_group_create(&event_group);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn(status == OS_STATUS_OK
        ? os_event_group_get_id(event_group)
        : 0U);
}

static m3ApiRawFunction(os_wasm_import_os_event_group_delete)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, event_group_id);
    OsEventGroupHandle event_group =
        os_event_group_find_by_id(event_group_id);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (event_group == NULL)
    {
        m3ApiReturn((uint32_t)OS_STATUS_EVENT_GROUP_NOT_FOUND);
    }

    os_event_group_delete(event_group);
    m3ApiReturn((uint32_t)OS_STATUS_OK);
}

static m3ApiRawFunction(os_wasm_import_os_event_group_get_bits)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, event_group_id);
    OsEventGroupHandle event_group =
        os_event_group_find_by_id(event_group_id);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn(os_event_group_get_bits(event_group));
}

static m3ApiRawFunction(os_wasm_import_os_event_group_set_bits)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, event_group_id);
    m3ApiGetArg(uint32_t, bits);
    OsEventGroupHandle event_group =
        os_event_group_find_by_id(event_group_id);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn(os_event_group_set_bits(event_group, bits));
}

static m3ApiRawFunction(os_wasm_import_os_event_group_clear_bits)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, event_group_id);
    m3ApiGetArg(uint32_t, bits);
    OsEventGroupHandle event_group =
        os_event_group_find_by_id(event_group_id);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn(os_event_group_clear_bits(event_group, bits));
}

static m3ApiRawFunction(os_wasm_import_os_event_group_wait_bits)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, event_group_id);
    m3ApiGetArg(uint32_t, bits_to_wait_for);
    m3ApiGetArg(uint32_t, clear_on_exit);
    m3ApiGetArg(uint32_t, wait_for_all);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsTaskHandle task = os_task_get_current();
    OsEventGroupHandle event_group =
        os_event_group_find_by_id(event_group_id);
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (event_group == NULL)
    {
        m3ApiReturn(0U);
    }

    status = os_prepare_wait_return(task, raw_return);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(0U);
    }

    status = os_event_group_wait_bits(
        event_group,
        bits_to_wait_for,
        clear_on_exit ? 1U : 0U,
        wait_for_all ? 1U : 0U,
        timeout_ms
    );
    if (task != NULL && task->wait_type == OS_TASK_WAIT_EVENT_GROUP)
    {
        *raw_return = 0U;
        return m3_Yield();
    }

    *raw_return = task != NULL ? task->last_wait_value : 0U;
    os_clear_wait_return(task);
    (void)status;
    return m3Err_none;
}

static m3ApiRawFunction(os_wasm_import_os_task_notify)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, task_id);
    m3ApiGetArg(uint32_t, value);
    m3ApiGetArg(uint32_t, action);
    OsTaskHandle task = os_task_find_by_id(task_id);
    OsStatus status = task != NULL
        ? os_task_notify(task, value, (OsNotifyAction)action)
        : OS_STATUS_TASK_NOT_FOUND;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    m3ApiReturn((uint32_t)status);
}

static m3ApiRawFunction(os_wasm_import_os_task_notify_wait)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, clear_on_entry);
    m3ApiGetArg(uint32_t, clear_on_exit);
    m3ApiGetArg(uint32_t, out_value_address);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsTaskHandle task = os_task_get_current();
    uint8_t zero[sizeof(uint32_t)] = { 0U, 0U, 0U, 0U };
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    status = os_task_write_memory(
        task,
        out_value_address,
        zero,
        (uint32_t)sizeof(zero)
    );
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn((uint32_t)status);
    }

    status = os_prepare_wait_return(task, raw_return);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn((uint32_t)status);
    }

    task->wait_has_wasm_output = 1U;
    task->wait_wasm_output_address = out_value_address;
    status = os_task_notify_wait(clear_on_entry, clear_on_exit, timeout_ms);
    *raw_return = (uint32_t)status;
    if (task->wait_type == OS_TASK_WAIT_NOTIFICATION)
    {
        return m3_Yield();
    }

    os_write_wait_output(task, task->last_wait_value);
    os_clear_wait_return(task);
    return m3Err_none;
}

static m3ApiRawFunction(os_wasm_import_os_task_notify_take)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, clear_count_on_exit);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsTaskHandle task = os_task_get_current();
    OsStatus status = OS_STATUS_OK;
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    status = os_prepare_wait_return(task, raw_return);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(0U);
    }

    status = os_task_notify_take(
        clear_count_on_exit ? 1U : 0U,
        timeout_ms
    );
    if (task->wait_type == OS_TASK_WAIT_NOTIFICATION_TAKE)
    {
        *raw_return = 0U;
        return m3_Yield();
    }

    *raw_return = status == OS_STATUS_OK ? task->last_wait_value : 0U;
    os_clear_wait_return(task);
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

    if (task->wait_queue_output == NULL &&
        !task->wait_queue_has_wasm_output)
    {
        os_complete_task_wait(task, OS_STATUS_INVALID_ARGUMENT, 0U);
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (task->wait_queue_has_wasm_output)
    {
        status = os_task_write_memory(
            task,
            task->wait_queue_wasm_output_address,
            (const uint8_t*)item,
            queue->item_size
        );
    }
    else
    {
        memcpy(task->wait_queue_output, item, queue->item_size);
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
        wait_type == OS_TASK_WAIT_NONE || wait_type == OS_TASK_WAIT_DELAY)
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
    return_value = task->wait_return_kind == OS_WAIT_RETURN_VALUE
        ? value
        : (uint32_t)status;

    os_write_wait_output(task, value);
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

    task->wait_queue_output = NULL;
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
    task->wait_has_wasm_output = 0U;
    task->wait_wasm_output_address = 0U;
}

static void os_write_wait_return(OsTaskHandle task, uint32_t value)
{
    uint8_t* stack_start = NULL;
    uint32_t* return_slot = NULL;

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

static void os_write_wait_output(OsTaskHandle task, uint32_t value)
{
    uint8_t bytes[sizeof(uint32_t)];

    if (task == NULL || !task->wait_has_wasm_output)
    {
        return;
    }

    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xffU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xffU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xffU);
    (void)os_task_write_memory(
        task,
        task->wait_wasm_output_address,
        bytes,
        (uint32_t)sizeof(bytes)
    );
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
    g_os.tick_ms += milliseconds;
    os_update_waiting_tasks();
}

static OsTaskRunResult os_run_task_slice(OsTaskHandle task)
{
    M3Result result = m3Err_none;
    const char* phase = "call";

    if (task == NULL || task->wasm_runtime == NULL || task->wasm_entry_function == NULL)
    {
        return OS_TASK_RUN_ERROR;
    }

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
}

static void os_set_task_suspended(OsTaskHandle task)
{
    if (task == NULL || task->state == OS_TASK_DEAD)
    {
        return;
    }

    task->state = OS_TASK_SUSPENDED;
    task->wake_tick_ms = 0U;
}

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
    task->state = OS_TASK_DEAD;
    task->wake_tick_ms = 0U;
    os_task_cleanup_wasm(task);
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

static void os_mark_current_task_exiting(uint32_t exit_code)
{
    OsTaskHandle task = g_os.current_task;

    if (task == NULL)
    {
        return;
    }

    os_record_task_exit(task, OS_TASK_EXIT_EXPLICIT, exit_code);
    os_cancel_task_wait(task, OS_STATUS_TASK_DEAD);
    os_release_task_mutexes(task);
    task->state = OS_TASK_DEAD;
    task->wake_tick_ms = 0U;
    os_request_task_stop(task);
    os_recalculate_task_counters();
}

static void os_request_task_delete(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    task->delete_requested = 1U;
    os_record_task_exit(task, OS_TASK_EXIT_DELETED, 0U);
    task->state = OS_TASK_DEAD;
    task->wake_tick_ms = 0U;
    os_request_task_stop(task);
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
    os_record_task_exit(task, OS_TASK_EXIT_DELETED, 0U);
    task->state = OS_TASK_DEAD;
    os_task_list_remove(task);
    os_task_free(task);
    os_recalculate_task_counters();
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
