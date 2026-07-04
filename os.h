#ifndef MICROWASM_OS_H
#define MICROWASM_OS_H

#include <stdint.h>

#include "wasm3/source/wasm3.h"

typedef struct OsTask* OsTaskHandle;

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
    OS_STATUS_UNSUPPORTED
} OsStatus;

typedef enum OsTaskState
{
    OS_TASK_READY = 0,
    OS_TASK_RUNNING,
    OS_TASK_WAITING,
    OS_TASK_SUSPENDED,
    OS_TASK_SWAPPED,
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

OsStatus os_host_import_register(
    const char* module_name,
    const char* import_name,
    const char* signature,
    OsHostImportFunction function
);

void os_host_import_clear_all(void);

OsStatus os_task_delete(OsTaskHandle task);
OsStatus os_task_delay_ms(uint32_t delay_ms);
OsStatus os_task_yield(void);
void os_request_preempt(void);
OsStatus os_task_suspend(OsTaskHandle task);
OsStatus os_task_resume(OsTaskHandle task);

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

OsStatus os_task_set_priority(
    OsTaskHandle task,
    uint32_t priority
);

uint32_t os_task_get_priority(OsTaskHandle task);
uint32_t os_task_get_id(OsTaskHandle task);
OsTaskHandle os_task_find_by_id(uint32_t task_id);
uint32_t os_task_get_id_list(uint32_t* out_task_ids, uint32_t max_task_ids);
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
const char* os_task_get_name(OsTaskHandle task);
OsTaskHandle os_task_get_current(void);
uint32_t os_task_get_run_count(OsTaskHandle task);

const char* os_get_last_error_phase(void);
const char* os_get_last_error_result(void);
const char* os_get_last_error_task_name(void);
OsStatus os_get_last_error_status(void);

void os_tick(uint32_t milliseconds);
OsStatus os_update_time_ms(uint32_t now_ms);
uint32_t os_get_tick_ms(void);

OsStatus os_schedule(void);

uint32_t os_get_task_count(void);
uint32_t os_get_ready_task_count(void);
uint32_t os_get_waiting_task_count(void);

#endif
