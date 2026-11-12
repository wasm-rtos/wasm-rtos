#include "os_kernel.h"

/* Queues, mutexes, semaphores, event groups, task notifications and software timers. */

static OsQueueHandle os_queue_allocate(void);
static void os_queue_free(OsQueueHandle queue);
static void os_queue_list_insert(OsQueueHandle queue);
static void os_queue_list_remove(OsQueueHandle queue);
static int os_queue_is_in_list(OsQueueHandle queue);
static OsMutexHandle os_mutex_allocate(void);
static void os_mutex_free(OsMutexHandle mutex);
static void os_mutex_list_insert(OsMutexHandle mutex);
static void os_mutex_list_remove(OsMutexHandle mutex);
static int os_mutex_is_in_list(OsMutexHandle mutex);
static OsSemaphoreHandle os_semaphore_allocate(void);
static void os_semaphore_free(OsSemaphoreHandle semaphore);
static void os_semaphore_list_insert(OsSemaphoreHandle semaphore);
static void os_semaphore_list_remove(OsSemaphoreHandle semaphore);
static int os_semaphore_is_in_list(OsSemaphoreHandle semaphore);
static OsEventGroupHandle os_event_group_allocate(void);
static void os_event_group_free(OsEventGroupHandle event_group);
static void os_event_group_list_insert(OsEventGroupHandle event_group);
static void os_event_group_list_remove(OsEventGroupHandle event_group);
static OsTimerHandle os_timer_allocate(void);
static void os_timer_free(OsTimerHandle timer);
static void os_timer_list_insert(OsTimerHandle timer);
static void os_timer_list_remove(OsTimerHandle timer);
static int os_timer_is_in_list(OsTimerHandle timer);
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
static uint8_t os_event_group_condition_met(
    uint32_t current_bits,
    uint32_t bits_to_wait_for,
    uint8_t wait_for_all
);
static void os_event_group_wake_waiters(OsEventGroupHandle event_group);
static void os_complete_notification_wait(OsTaskHandle task);


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

/*
 * An empty queue: the current task waits for an item, which the sender's
 * wakeup writes to wasm_output_address in its memory.
 */
static OsStatus os_queue_receive_begin_wait(OsQueueHandle queue, uint32_t wasm_output_address, uint32_t timeout_ms)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = OS_STATUS_OK;

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
    if (timeout_ms == 0U || task == NULL || task->state != OS_TASK_RUNNING)
    {
        return os_queue_receive_begin_wait(queue, 0U, timeout_ms);
    }

    /* Only a pointer into the task's own one-block memory can be written to later. */
    status = os_task_get_memory_address(task, out_item, queue->item_size, &wasm_output_address);
    if (status != OS_STATUS_OK)
    {
        task->last_wait_status = OS_STATUS_UNSUPPORTED;
        task->last_wait_value = 0U;
        return OS_STATUS_UNSUPPORTED;
    }
    return os_queue_receive_begin_wait(queue, wasm_output_address, timeout_ms);
}

OsStatus os_queue_receive_wait_guest(OsQueueHandle queue, uint32_t address, uint32_t timeout_ms)
{
    OsTaskHandle task = g_os.current_task;
    OsGuestBuffer item;
    OsStatus status = OS_STATUS_OK;

    if (queue == NULL || !os_queue_is_in_list(queue))
    {
        return OS_STATUS_QUEUE_NOT_FOUND;
    }
    status = os_guest_buffer_open(&item, address, queue->item_size, OS_GUEST_OUT);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    status = os_queue_receive(queue, item.data);
    if (status == OS_STATUS_OK)
    {
        status = os_guest_buffer_close(&item);
    }
    else
    {
        os_guest_buffer_drop(&item);
    }
    if (status != OS_STATUS_QUEUE_EMPTY)
    {
        if (task != NULL)
        {
            task->last_wait_status = status;
            task->last_wait_value = 0U;
        }
        return status;
    }
    return os_queue_receive_begin_wait(queue, address, timeout_ms);
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

void os_queue_free_all(void)
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

void os_mutex_free_all(void)
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

void os_semaphore_free_all(void)
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

int os_event_group_is_in_list(OsEventGroupHandle event_group)
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

void os_event_group_free_all(void)
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

void os_timer_free_all(void)
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

void os_timer_delete_for_task(OsTaskHandle task)
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

    /* Also into a swapped-out task's memory: it is kept, paged, and its frame comes back. */
    status = os_task_write_memory(
        task,
        task->wait_queue_wasm_output_address,
        (const uint8_t*)item,
        queue->item_size
    );
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

void os_recompute_task_priorities(void)
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

void os_release_task_mutexes(OsTaskHandle task)
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

void os_process_timers(void)
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
