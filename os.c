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

typedef struct OsState
{
    OsTaskHandle task_list;
    OsTaskHandle current_task;
    OsTaskHandle last_scheduled_task;
    OsHostImportNode* host_import_list;
    uint32_t tick_ms;
    uint32_t last_time_update_ms;
    uint32_t next_task_id;
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
    uint32_t priority;
    uint32_t wake_tick_ms;

    uint8_t* wasm_bytes;
    uint32_t wasm_size;
    const char* entry_function_name;
    uint32_t stack_size;
    OsValue* entry_args;
    uint32_t entry_arg_count;

    IM3Environment wasm_environment;
    IM3Runtime wasm_runtime;
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
static OsTaskHandle os_select_next_ready_task(void);
static void os_update_waiting_tasks(void);
static void os_advance_tick_ms(uint32_t milliseconds);
static OsTaskRunResult os_run_task_slice(OsTaskHandle task);
static OsStatus os_validate_entry_function(OsTaskHandle task);
static OsStatus os_capture_entry_return_value(OsTaskHandle task);
static OsStatus os_call_entry_function(OsTaskHandle task, M3Result* out_result);
static void os_set_task_ready(OsTaskHandle task);
static void os_set_task_waiting(OsTaskHandle task, uint32_t wake_tick_ms);
static void os_set_task_suspended(OsTaskHandle task);
static void os_set_task_dead(OsTaskHandle task);
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
    task->priority = priority;
    task->state = OS_TASK_READY;
    task->exit_reason = OS_TASK_EXIT_NONE;
    task->exit_code = 0U;
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

    os_set_task_suspended(task);
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

    task->priority = priority;
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
    M3Result result = m3Err_none;

    if (task == NULL || task->wasm_module == NULL)
    {
        return m3Err_moduleNotLinked;
    }

    result = m3_LinkRawFunction(
        task->wasm_module,
        "env",
        "os_yield",
        "v()",
        os_wasm_import_os_yield
    );

    if (result != m3Err_none && result != m3Err_functionLookupFailed)
    {
        return result;
    }

    result = m3_LinkRawFunction(
        task->wasm_module,
        "env",
        "os_delay_ms",
        "v(i)",
        os_wasm_import_os_delay_ms
    );

    if (result != m3Err_none && result != m3Err_functionLookupFailed)
    {
        return result;
    }

    result = m3_LinkRawFunction(
        task->wasm_module,
        "env",
        "os_get_time_ms",
        "i()",
        os_wasm_import_os_get_time_ms
    );

    if (result != m3Err_none && result != m3Err_functionLookupFailed)
    {
        return result;
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

static void os_update_waiting_tasks(void)
{
    OsTaskHandle task = NULL;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_WAITING &&
            os_time_reached(g_os.tick_ms, task->wake_tick_ms))
        {
            os_set_task_ready(task);
        }
    }

    os_recalculate_task_counters();
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

    task->state = OS_TASK_WAITING;
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
