#include "os_kernel.h"

/* Task snapshots and swapping a task out of RAM. */

static const char os_snapshot_stream_failed[] = "snapshot stream failed";

static WmResult os_snapshot_stream_write(void* context, const void* data, uint32_t size)
{
    OsSnapshotStream* stream = (OsSnapshotStream*)context;
    stream->status = stream->write(stream->context, data, size);
    return stream->status == OS_STATUS_OK ? NULL : os_snapshot_stream_failed;
}

static WmResult os_snapshot_stream_read(void* context, void* data, uint32_t size)
{
    OsSnapshotStream* stream = (OsSnapshotStream*)context;
    stream->status = stream->read(stream->context, data, size);
    return stream->status == OS_STATUS_OK ? NULL : os_snapshot_stream_failed;
}

static OsStatus os_snapshot_stream_status(const OsSnapshotStream* stream, WmResult result)
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
#if OS_HAS_DYLINK
    if (task->wasm_dylink != NULL)
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
    WmResult result = NULL;

    if (task == NULL || write == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_task_check_snapshot(task);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    result = wm_snapshot_write(task->wasm_runtime, task->wasm_instance, os_snapshot_stream_write, &stream);
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
    WmResult result = NULL;

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
        result = wm_snapshot_read(task->wasm_runtime, task->wasm_instance, os_snapshot_stream_read, &stream);
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

/* What a swapped-out task keeps outside RAM goes: its state image and its memory. */
void os_swap_discard(OsTaskHandle task)
{
    os_page_image_drop(&task->swap_image);
    wm_memory_free(task->swapped_memory);
    task->swapped_memory = NULL;
}

void os_swap_forget_deliveries(OsTaskHandle task)
{
    task->swap_return_pending = 0U;
}

OsStatus os_output_port_set(const OsOutputPort* port)
{
    if (port == NULL)
    {
        memset(&g_os.output_port, 0, sizeof(g_os.output_port));
        return OS_STATUS_OK;
    }
    if (port->write == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_os.output_port = *port;
    return OS_STATUS_OK;
}

void os_output_write(uint32_t fd, const void* data, uint32_t size)
{
    if (size == 0U)
    {
        return;
    }
    if (g_os.output_port.write != NULL)
    {
        uint32_t task_id = g_os.current_task != NULL ? g_os.current_task->id : 0U;
        g_os.output_port.write(g_os.output_port.context, task_id, fd, data, size);
        return;
    }
    {
        FILE* stream = fd == 2U ? stderr : stdout;
        fwrite(data, 1U, size, stream);
        fflush(stream);
    }
}

uint8_t os_task_is_swapped(OsTaskHandle task)
{
    return task != NULL && os_task_is_in_list(task) && task->swapped ? 1U : 0U;
}

OsStatus os_task_swap_out(OsTaskHandle task)
{
    WmResult result = NULL;
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
    if (!os_page_store_on())
    {
        return OS_STATUS_UNSUPPORTED;
    }
#if OS_HAS_DYLINK
    if (task->wasm_dylink != NULL)
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
    /* A bus transfer is using its memory right now. */
    if (task->wait_type == OS_TASK_WAIT_TRANSFER)
    {
        return OS_STATUS_BUSY;
    }

    /*
     * A started task leaves its state (stack, calls, globals, tables) in
     * the page store and its memory paged out, kept apart from the rest,
     * which is freed: it comes back from that state with the same memory.
     * One that has not started has nothing to keep and starts afresh.
     */
    if (task->wasm_started)
    {
        WmMemory* memory = wm_instance_memory(task->wasm_instance);
        status = os_page_image_save(&task->swap_image, task->wasm_runtime, task->wasm_instance, &result);
        if (status != OS_STATUS_OK)
        {
            os_set_last_error(task, "swap_out", result, status);
            return status;
        }
        if (memory != NULL)
        {
            uint32_t index = 0U;
            for (index = 0U; index < wm_memory_frames(memory); ++index)
            {
                if (wm_memory_frame_look(memory, index) >= 0)
                {
                    (void)wm_memory_frame_take(memory, index);
                }
            }
            task->swapped_memory = wm_instance_detach_memory(task->wasm_instance);
        }
    }

    os_task_cleanup_wasm(task);
    task->swapped = 1U;
    os_clear_last_error();
    return OS_STATUS_OK;
}

OsStatus os_task_swap_in(OsTaskHandle task)
{
    WmResult result = NULL;
    OsStatus status = OS_STATUS_OK;
    uint8_t has_state = 0U;

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
    has_state = task->swap_image.count > 0U;

    {
        WmMemory* kept = task->swapped_memory;
        OS_HEAP_ENTER(task);
        /* With a kept memory the new instance takes it (os_task_create_wasm). */
        status = os_task_create_wasm(task, task->wasm_bytes, task->wasm_size, task->entry_function_name,
                                     task->stack_size);
        if (status == OS_STATUS_OK && has_state)
        {
            status = os_page_image_load(&task->swap_image, task->wasm_runtime, task->wasm_instance, &result);
        }
        OS_HEAP_LEAVE();
        if (status != OS_STATUS_OK && kept != NULL && task->swapped_memory == NULL && task->wasm_instance != NULL)
        {
            /* The memory goes back apart, so the task can be brought back later. */
            task->swapped_memory = wm_instance_detach_memory(task->wasm_instance);
        }
    }
    if (status != OS_STATUS_OK)
    {
        /* The state stays; the task can be brought back later. */
        os_task_cleanup_wasm(task);
        os_set_last_error(task, "swap_in", result != NULL ? result : "cannot recreate the runtime", status);
        return status;
    }

    if (has_state)
    {
        os_task_continue_from_snapshot(task);
    }
    task->swapped = 0U;
    task->wasm_stack_base = wm_runtime_stack(task->wasm_runtime, NULL);
    if (task->swap_return_pending && task->swap_return_offset <= os_task_stack_bytes(task) - sizeof(uint32_t))
    {
        memcpy(task->wasm_stack_base + task->swap_return_offset, &task->swap_return_value, sizeof(uint32_t));
    }
    os_swap_forget_deliveries(task);
    os_swap_discard(task);
    os_clear_last_error();
    return OS_STATUS_OK;
}
