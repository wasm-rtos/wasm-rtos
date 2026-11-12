#include "os_kernel.h"

/* The clock, waits and the scheduler that runs task slices. */

static OsTaskHandle os_select_next_ready_task(void);
static void os_update_time_events(void);
static void os_update_waiting_tasks(void);
static void os_advance_tick_ms(uint32_t milliseconds);
static OsTaskRunResult os_run_task_slice(OsTaskHandle task);
static void os_task_count_fuel(OsTaskHandle task);
static void os_write_wait_return(OsTaskHandle task, uint32_t value);

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

    {
        /* What drivers poll for, such as counters whose hardware cannot watch. */
        uint32_t driver_ms = os_drivers_next_deadline_ms();
        if (driver_ms < next_wakeup_ms)
        {
            next_wakeup_ms = driver_ms;
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

    /* What drivers saw in the background, such as edges from interrupt handlers. */
    os_drivers_poll();
    os_page_poll();

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
    os_task_count_fuel(task);

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
            os_task_record_error(task);
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

void os_preempt_for_higher_priority_ready_task(void)
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

OsStatus os_begin_task_wait(
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

void os_complete_task_wait(
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

void os_cancel_task_wait(OsTaskHandle task, OsStatus status)
{
    if (task == NULL || task->wait_type == OS_TASK_WAIT_NONE)
    {
        return;
    }

    os_complete_task_wait(task, status, 0U);
    os_recompute_task_priorities();
}

void os_clear_task_wait(OsTaskHandle task)
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

void os_clear_queue_wait(OsTaskHandle task)
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

OsStatus os_prepare_wait_return(OsTaskHandle task, uint32_t* raw_return)
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
    stack_end = stack_start + os_task_stack_bytes(task);
    if (return_address < stack_start ||
        return_address + sizeof(uint32_t) > stack_end)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    task->wait_has_return_slot = 1U;
    task->wait_return_offset = (uint32_t)(return_address - stack_start);
    return OS_STATUS_OK;
}

void os_clear_wait_return(OsTaskHandle task)
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

    if (os_task_stack_bytes(task) < sizeof(uint32_t) ||
        task->wait_return_offset >
        os_task_stack_bytes(task) - sizeof(uint32_t))
    {
        return;
    }

    stack_start = task->wasm_stack_base;
    return_slot = (uint32_t*)(stack_start + task->wait_return_offset);
    *return_slot = value;
}

static void os_update_time_events(void)
{
    g_os.processing_time_events = 1U;
    os_process_timers();
    os_drivers_time();
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

OsStatus os_sync_clock_before_deadline(void)
{
    if (g_os.clock_port.now_ms == NULL || g_os.processing_time_events)
    {
        return OS_STATUS_OK;
    }

    return os_clock_poll();
}

void os_clock_rearm(void)
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

/* Fuel the slice that just ended spent: what it got, less what is left or was taken back. */
static void os_task_count_fuel(OsTaskHandle task)
{
    uint64_t unspent = 0U;

    if (task->wasm_runtime == NULL || !wm_is_fuel_enabled(task->wasm_runtime))
    {
        return;
    }
    unspent = wm_get_fuel(task->wasm_runtime) + task->fuel_taken_back;
    if (unspent < OS_TASK_SLICE_FUEL)
    {
        task->fuel_used += OS_TASK_SLICE_FUEL - unspent;
    }
    task->fuel_taken_back = 0U;
}

static OsTaskRunResult os_run_task_slice(OsTaskHandle task)
{
    WmResult result = NULL;
    const char* phase = "call";

    if (task != NULL && task->swapped && os_task_swap_in(task) != OS_STATUS_OK)
    {
        return OS_TASK_RUN_ERROR;
    }

    if (task == NULL || task->wasm_runtime == NULL || task->wasm_entry_function == NULL)
    {
        return OS_TASK_RUN_ERROR;
    }

    task->wasm_stack_base = wm_runtime_stack(task->wasm_runtime, NULL);
    wm_set_fuel(task->wasm_runtime,
                OS_TASK_SLICE_FUEL > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)OS_TASK_SLICE_FUEL);
    task->fuel_taken_back = 0U;
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
        result = wm_resume(task->wasm_runtime, task->wasm_results);
    }

    if (result == NULL)
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

    if (result == os_exit_trap)
    {
        os_clear_last_error();
        g_os.preempt_requested = 0U;
        task->wasm_needs_resume = 0U;
        task->entry_return_code = task->wasi_exit_code;
        return OS_TASK_RUN_EXITED;
    }

    if (result == wm_fuel_exhausted)
    {
        os_clear_last_error();
        g_os.preempt_requested = 0U;
        task->wasm_needs_resume = 1U;
        return OS_TASK_RUN_FUEL_EXHAUSTED;
    }

    /* The program touched a frame the page store reads in the background: it waits, the access runs again. */
    if (result == wm_page_wait && os_page_wait_begin(task) == OS_STATUS_OK)
    {
        os_clear_last_error();
        g_os.preempt_requested = 0U;
        task->wasm_needs_resume = 1U;
        return OS_TASK_RUN_SUSPENDED;
    }

    if (result == wm_yielded || (task->wasm_runtime != NULL && wm_is_suspended(task->wasm_runtime)))
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

void os_set_task_ready(OsTaskHandle task)
{
    if (task == NULL || task->state == OS_TASK_DEAD)
    {
        return;
    }

    task->state = OS_TASK_READY;
    task->wake_tick_ms = 0U;
    os_clock_rearm();
}

void os_set_task_waiting(OsTaskHandle task, uint32_t wake_tick_ms)
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

void os_set_task_suspended(OsTaskHandle task)
{
    if (task == NULL || task->state == OS_TASK_DEAD)
    {
        return;
    }

    task->state = OS_TASK_SUSPENDED;
    task->wake_tick_ms = 0U;
    os_clock_rearm();
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
        case OS_OBJECT_DRIVER:
            return target->driver->ready != NULL && target->driver->ready(target->id);
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

void os_wake_any_waiters(void)
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

OsStatus os_transfer_wait_begin(void)
{
    return os_begin_task_wait(g_os.current_task, OS_TASK_WAIT_TRANSFER, OS_WAIT_FOREVER);
}

void os_transfer_wait_end(uint32_t task_id, OsStatus status, uint32_t value)
{
    OsTaskHandle task = os_task_find_by_id(task_id);

    if (task == NULL || task->wait_type != OS_TASK_WAIT_TRANSFER)
    {
        return;
    }
    os_complete_task_wait(task, status, value);
    os_recalculate_task_counters();
}

OsStatus os_page_wait_begin(OsTaskHandle task)
{
    if (task == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    /* Outside any host call: nothing to return into. */
    task->wait_has_return_slot = 0U;
    task->wait_return_kind = OS_WAIT_RETURN_STATUS;
    return os_begin_task_wait(task, OS_TASK_WAIT_PAGE, OS_WAIT_FOREVER);
}

void os_page_wait_end(uint32_t task_id)
{
    OsTaskHandle task = os_task_find_by_id(task_id);

    if (task == NULL || task->wait_type != OS_TASK_WAIT_PAGE)
    {
        return;
    }
    os_complete_task_wait(task, OS_STATUS_OK, 0U);
    os_recalculate_task_counters();
}

void os_transfer_finish_task(uint32_t task_id)
{
    os_drivers_finish_task(task_id);
}

OsStatus os_signal_wait(uint32_t key, uint32_t timeout_ms)
{
    OsTaskHandle task = g_os.current_task;
    OsStatus status = os_begin_task_wait(task, OS_TASK_WAIT_SIGNAL, timeout_ms);

    if (status == OS_STATUS_OK)
    {
        task->wait_bits = key;
    }
    return status;
}

void os_signal_wake(uint32_t key)
{
    OsTaskHandle task = NULL;
    uint8_t woke = 0U;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->state == OS_TASK_WAITING && task->wait_type == OS_TASK_WAIT_SIGNAL && task->wait_bits == key)
        {
            os_complete_task_wait(task, OS_STATUS_OK, 0U);
            woke = 1U;
        }
    }
    if (woke)
    {
        os_recalculate_task_counters();
    }
}

OsStatus os_host_wait_begin(uint32_t* result_slot)
{
    return os_import_wait_begin(result_slot);
}

WmResult os_host_wait_end(void)
{
    return os_import_wait_end(0U);
}
