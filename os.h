#ifndef WASM_RTOS_H
#define WASM_RTOS_H

#include <stdint.h>

#include "wasm3/source/wasm3.h"

#define OS_WAIT_FOREVER UINT32_MAX

typedef struct OsTask* OsTaskHandle;
typedef struct OsQueue* OsQueueHandle;
typedef struct OsMutex* OsMutexHandle;
typedef struct OsSemaphore* OsSemaphoreHandle;
typedef struct OsEventGroup* OsEventGroupHandle;

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
    OS_STATUS_EVENT_GROUP_NOT_FOUND
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

typedef enum OsNotifyAction
{
    OS_NOTIFY_NO_ACTION = 0,
    OS_NOTIFY_SET_BITS,
    OS_NOTIFY_INCREMENT,
    OS_NOTIFY_SET_VALUE_WITH_OVERWRITE,
    OS_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE
} OsNotifyAction;

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
uint32_t os_queue_get_count(OsQueueHandle queue);
uint32_t os_queue_get_space(OsQueueHandle queue);

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
uint32_t os_task_get_base_priority(OsTaskHandle task);
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
