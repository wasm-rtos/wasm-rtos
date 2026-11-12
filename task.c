#include "os_kernel.h"

/* Tasks: create, delay, suspend, priorities and what is known about them; processes. */

static OsTaskHandle os_task_allocate(void);
static void os_task_list_insert(OsTaskHandle task);
static void os_task_list_remove(OsTaskHandle task);
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
#if OS_HAS_PROCESSES
static void os_process_on_task_dead(OsTaskHandle task);
#endif
static void os_request_task_delete(OsTaskHandle task);

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

/* Bytes in the task's interpreter stack. */
uint32_t os_task_stack_bytes(OsTaskHandle task)
{
    uint32_t bytes = 0U;
    (void)wm_runtime_stack(task->wasm_runtime, &bytes);
    return bytes;
}

/* Highest byte of the interpreter stack the task has written. */
uint32_t os_task_stack_peak(OsTaskHandle task)
{
    const uint8_t* stack = NULL;
    uint32_t top = 0U;

    if (task->wasm_runtime == NULL)
    {
        return task->stack_peak_bytes;
    }
    stack = wm_runtime_stack(task->wasm_runtime, &top);
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

    /* Bytes on the wire cannot be called back: the transfer ends and the program gets its result. */
    if (task->wait_type == OS_TASK_WAIT_TRANSFER)
    {
        os_transfer_finish_task(task->id);
        os_recalculate_task_counters();
        return OS_STATUS_OK;
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

    /* A transfer in progress ends first; its result reaches the program when it resumes. */
    if (task->wait_type == OS_TASK_WAIT_TRANSFER)
    {
        os_transfer_finish_task(task->id);
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

uint64_t os_task_get_fuel_used(OsTaskHandle task)
{
    if (task == NULL || !os_task_is_in_list(task))
    {
        return 0U;
    }

    return task->fuel_used;
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

static OsTaskHandle os_task_allocate(void)
{
    return (OsTaskHandle)calloc(1U, sizeof(struct OsTask));
}

void os_task_free(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    os_task_cleanup_wasm(task);
    os_swap_discard(task);
    os_swap_forget_deliveries(task);
    os_abi_release_task(task);
    os_clear_task_wait(task);
    os_task_free_entry_args(task);
    os_task_free_return_values(task);
    free(task->app_name);
    free(task->error_text);
    free(task->restored_entry_name);
#if OS_HAS_PROCESSES
    /* The module was parsed in place, so its bytes outlive the runtime. */
    free(task->owned_wasm_bytes);
#if OS_HAS_FS
    os_fs_image_close(&task->image);
    free(task->image_path);
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

int os_task_is_in_list(OsTaskHandle task)
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

uint8_t os_task_is_dead(OsTaskHandle task)
{
    return (task != NULL && task->state == OS_TASK_DEAD) ? 1U : 0U;
}

uint8_t os_task_snapshot_is_busy(OsTaskHandle task)
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

void os_recalculate_task_counters(void)
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

void os_task_free_return_values(OsTaskHandle task)
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
    char* path_copy = NULL;
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

    path_copy = (char*)malloc(strlen(path) + 1U);
    if (path_copy == NULL)
    {
        free(argv_copy);
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memcpy(path_copy, path, strlen(path) + 1U);

    memset(&image, 0, sizeof(image));
    status = os_fs_image_open(path, &image.file);
    if (status == OS_STATUS_OK)
    {
        image.bytes = image.file.bytes;
        image.size = image.file.size;
        status = os_task_spawn_owned(out_task, &image, argv_copy, argc > 0U ? argc : 1U, priority, stack_size);
        argv_copy = NULL;
    }
    if (status != OS_STATUS_OK)
    {
        free(argv_copy);
        free(path_copy);
        return status;
    }
    (*out_task)->image_path = path_copy;
    return OS_STATUS_OK;
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

void os_reap_processes(void)
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
#endif

void os_set_task_dead(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    if (g_os.current_task == task)
    {
        g_os.current_task = NULL;
    }

    if (task->wait_type == OS_TASK_WAIT_TRANSFER)
    {
        os_transfer_finish_task(task->id);
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

void os_record_task_exit(OsTaskHandle task, OsTaskExitReason reason, uint32_t code)
{
    if (task == NULL || task->exit_reason != OS_TASK_EXIT_NONE)
    {
        return;
    }

    task->exit_reason = reason;
    task->exit_code = code;
}

void os_request_task_stop(OsTaskHandle task)
{
    if (task == NULL || task->wasm_runtime == NULL)
    {
        return;
    }

    if (task == g_os.current_task)
    {
        task->fuel_taken_back += wm_get_fuel(task->wasm_runtime);
    }
    wm_set_fuel(task->wasm_runtime, 0U);
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

void os_destroy_task(OsTaskHandle task)
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

    /* The port must be done with the task's memory before it is freed. */
    if (task->wait_type == OS_TASK_WAIT_TRANSFER)
    {
        os_transfer_finish_task(task->id);
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
