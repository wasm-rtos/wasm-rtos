#include "os_kernel.h"

/*
 * The whole system saved to a stream and restored into a kernel started
 * afresh, as around a deep sleep that loses RAM (docs/decisions.md, 6).
 * Everything is numbers and names, little-endian, with no host address:
 *
 *   header   magic "WRS1", version, features, the clock, the next ids
 *   objects  queues, mutexes, semaphores, event groups and timers, each
 *            with its id, name, references and contents
 *   tasks    in the scheduler's order: the kernel's record (state, wait,
 *            notifications, process), where its module comes from, its
 *            handles and open files, and the engine's snapshot in chunks
 *
 * Modules are not saved but found again: the host's by task name, a
 * process's by its path, or the copy the kernel owned. A stream that does
 * not agree with itself restores nothing; a task whose module, files or
 * memory cannot be had ends as a task that traps, and the others go on.
 */

#define OS_SYSTEM_MAGIC 0x31535257U /* "WRS1" */
#define OS_SYSTEM_VERSION 1U
#define OS_SYSTEM_FEATURE_PROCESSES 1U
#define OS_SYSTEM_FEATURE_FS 2U
#define OS_SYSTEM_FEATURES ((OS_HAS_PROCESSES ? OS_SYSTEM_FEATURE_PROCESSES : 0U) | \
                            (OS_HAS_FS ? OS_SYSTEM_FEATURE_FS : 0U))
/* Longest text a stream may hold: names, paths, error texts, arguments. */
#define OS_SYSTEM_TEXT_MAX 4096U
#define OS_SYSTEM_NO_TEXT 0xFFFFFFFFU
/* Largest queue storage and module a stream may hold, against a broken length. */
#define OS_SYSTEM_BYTES_MAX (16U * 1024U * 1024U)
/* The engine's snapshot goes in chunks of at most this, so a task can be skipped. */
#define OS_SYSTEM_CHUNK 256U

/* Where a task's module comes from when the system is restored. */
enum
{
    OS_SYSTEM_MODULE_NONE = 0,
    OS_SYSTEM_MODULE_HOST,
    OS_SYSTEM_MODULE_PATH,
    OS_SYSTEM_MODULE_BYTES
};

struct OsSystemStream
{
    OsSnapshotWrite write;
    OsSnapshotRead read;
    void* context;
    OsStatus status;
    uint8_t chunk[OS_SYSTEM_CHUNK];
    uint32_t chunk_used;
    uint32_t chunk_left;
    uint8_t chunk_end;
};

static const char os_system_stream_failed[] = "system stream failed";

/* ---- The stream ----------------------------------------------------------- */

void os_system_fail(OsSystemStream* stream, OsStatus status)
{
    if (stream->status == OS_STATUS_OK)
    {
        stream->status = status;
    }
}

uint8_t os_system_ok(const OsSystemStream* stream)
{
    return stream->status == OS_STATUS_OK ? 1U : 0U;
}

static void os_system_put(OsSystemStream* stream, const void* data, uint32_t size)
{
    if (stream->status == OS_STATUS_OK && size > 0U)
    {
        stream->status = stream->write(stream->context, data, size);
    }
}

void os_system_put32(OsSystemStream* stream, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
    os_system_put(stream, bytes, 4U);
}

void os_system_put64(OsSystemStream* stream, uint64_t value)
{
    os_system_put32(stream, (uint32_t)value);
    os_system_put32(stream, (uint32_t)(value >> 32U));
}

void os_system_put_text(OsSystemStream* stream, const char* text)
{
    if (text == NULL)
    {
        os_system_put32(stream, OS_SYSTEM_NO_TEXT);
        return;
    }
    os_system_put32(stream, (uint32_t)strlen(text));
    os_system_put(stream, text, (uint32_t)strlen(text));
}

static void os_system_get(OsSystemStream* stream, void* data, uint32_t size)
{
    if (stream->status == OS_STATUS_OK && size > 0U)
    {
        stream->status = stream->read(stream->context, data, size);
    }
    if (stream->status != OS_STATUS_OK)
    {
        memset(data, 0, size);
    }
}

uint32_t os_system_get32(OsSystemStream* stream)
{
    uint8_t bytes[4];
    os_system_get(stream, bytes, 4U);
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

uint64_t os_system_get64(OsSystemStream* stream)
{
    uint64_t low = os_system_get32(stream);
    return low | ((uint64_t)os_system_get32(stream) << 32U);
}

char* os_system_get_text(OsSystemStream* stream)
{
    uint32_t length = os_system_get32(stream);
    char* text = NULL;

    if (!os_system_ok(stream) || length == OS_SYSTEM_NO_TEXT)
    {
        return NULL;
    }
    if (length > OS_SYSTEM_TEXT_MAX)
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        return NULL;
    }
    text = (char*)malloc((size_t)length + 1U);
    if (text == NULL)
    {
        os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
        return NULL;
    }
    os_system_get(stream, text, length);
    text[length] = '\0';
    if (!os_system_ok(stream))
    {
        free(text);
        return NULL;
    }
    return text;
}

/* A number that has to be at most max, or the stream is broken. */
static uint32_t os_system_get_max(OsSystemStream* stream, uint32_t max)
{
    uint32_t value = os_system_get32(stream);
    if (value > max)
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        return 0U;
    }
    return value;
}

/* Bytes the stream holds that nothing needs: a task not restored. */
static void os_system_skip(OsSystemStream* stream, uint32_t size)
{
    uint8_t bytes[32];
    while (size > 0U && os_system_ok(stream))
    {
        uint32_t take = size < sizeof(bytes) ? size : (uint32_t)sizeof(bytes);
        os_system_get(stream, bytes, take);
        size -= take;
    }
}

/* ---- The engine's snapshot in chunks: a length, then that many bytes; 0 ends it. */

static void os_system_chunk_flush(OsSystemStream* stream)
{
    if (stream->chunk_used > 0U)
    {
        os_system_put32(stream, stream->chunk_used);
        os_system_put(stream, stream->chunk, stream->chunk_used);
        stream->chunk_used = 0U;
    }
}

static WmResult os_system_chunk_write(void* context, const void* data, uint32_t size)
{
    OsSystemStream* stream = (OsSystemStream*)context;
    const uint8_t* bytes = (const uint8_t*)data;

    while (size > 0U && os_system_ok(stream))
    {
        uint32_t take = OS_SYSTEM_CHUNK - stream->chunk_used;
        take = take < size ? take : size;
        memcpy(stream->chunk + stream->chunk_used, bytes, take);
        stream->chunk_used += take;
        bytes += take;
        size -= take;
        if (stream->chunk_used == OS_SYSTEM_CHUNK)
        {
            os_system_chunk_flush(stream);
        }
    }
    return os_system_ok(stream) ? NULL : os_system_stream_failed;
}

static void os_system_chunk_end(OsSystemStream* stream)
{
    os_system_chunk_flush(stream);
    os_system_put32(stream, 0U);
}

/* The next chunk's length into chunk_left; at the end, chunk_end. */
static void os_system_chunk_next(OsSystemStream* stream)
{
    uint32_t length = os_system_get_max(stream, OS_SYSTEM_CHUNK);
    stream->chunk_left = length;
    stream->chunk_end = length == 0U ? 1U : 0U;
}

static WmResult os_system_chunk_read(void* context, void* data, uint32_t size)
{
    OsSystemStream* stream = (OsSystemStream*)context;
    uint8_t* bytes = (uint8_t*)data;

    while (size > 0U && os_system_ok(stream))
    {
        uint32_t take = 0U;
        if (stream->chunk_left == 0U)
        {
            if (!stream->chunk_end)
            {
                os_system_chunk_next(stream);
            }
            if (stream->chunk_end)
            {
                /* The engine wants more than was written: the stream is broken. */
                os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
                break;
            }
            continue;
        }
        take = stream->chunk_left < size ? stream->chunk_left : size;
        os_system_get(stream, bytes, take);
        stream->chunk_left -= take;
        bytes += take;
        size -= take;
    }
    return os_system_ok(stream) ? NULL : os_system_stream_failed;
}

/* Past the end of the engine's snapshot; whole means it was all read, as it must be after a load. */
static void os_system_chunk_finish(OsSystemStream* stream, uint8_t whole)
{
    while (os_system_ok(stream) && !stream->chunk_end)
    {
        if (stream->chunk_left > 0U)
        {
            if (whole)
            {
                os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
                return;
            }
            os_system_skip(stream, stream->chunk_left);
            stream->chunk_left = 0U;
        }
        os_system_chunk_next(stream);
        if (whole && !stream->chunk_end)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
    }
}

/* ---- Saving ----------------------------------------------------------------- */

/* FNV-1a of a module's bytes, as the engine's snapshots check theirs. */
static uint32_t os_system_module_hash(const uint8_t* bytes, uint32_t size)
{
    uint32_t hash = 2166136261U;
    uint32_t index = 0U;
    for (index = 0U; index < size; ++index)
    {
        hash = (hash ^ bytes[index]) * 16777619U;
    }
    return hash;
}

static void os_system_put_fields(OsSystemStream* stream, const char* name, uint32_t guest_refs, uint8_t guest_owned)
{
    os_system_put_text(stream, name);
    os_system_put32(stream, guest_refs);
    os_system_put32(stream, guest_owned);
}

static uint32_t os_system_task_id(OsTaskHandle task)
{
    return task != NULL && os_task_is_in_list(task) ? task->id : 0U;
}

static void os_system_save_objects(OsSystemStream* stream)
{
    uint32_t count = 0U;
    OsQueueHandle queue = NULL;
    OsMutexHandle mutex = NULL;
    OsSemaphoreHandle semaphore = NULL;
    OsEventGroupHandle event_group = NULL;
    OsTimerHandle timer = NULL;

    for (count = 0U, queue = g_os.queue_list; queue != NULL; queue = queue->next, ++count)
    {
    }
    os_system_put32(stream, count);
    for (queue = g_os.queue_list; queue != NULL; queue = queue->next)
    {
        os_system_put32(stream, queue->id);
        os_system_put32(stream, queue->item_size);
        os_system_put32(stream, queue->item_count);
        os_system_put32(stream, queue->count);
        os_system_put32(stream, queue->head);
        os_system_put32(stream, queue->tail);
        os_system_put_fields(stream, queue->name, queue->guest_refs, queue->guest_owned);
        os_system_put(stream, queue->storage, queue->item_size * queue->item_count);
    }

    for (count = 0U, mutex = g_os.mutex_list; mutex != NULL; mutex = mutex->next, ++count)
    {
    }
    os_system_put32(stream, count);
    for (mutex = g_os.mutex_list; mutex != NULL; mutex = mutex->next)
    {
        os_system_put32(stream, mutex->id);
        os_system_put32(stream, os_system_task_id(mutex->owner));
        os_system_put_fields(stream, mutex->name, mutex->guest_refs, mutex->guest_owned);
    }

    for (count = 0U, semaphore = g_os.semaphore_list; semaphore != NULL; semaphore = semaphore->next, ++count)
    {
    }
    os_system_put32(stream, count);
    for (semaphore = g_os.semaphore_list; semaphore != NULL; semaphore = semaphore->next)
    {
        os_system_put32(stream, semaphore->id);
        os_system_put32(stream, semaphore->max_count);
        os_system_put32(stream, semaphore->count);
        os_system_put_fields(stream, semaphore->name, semaphore->guest_refs, semaphore->guest_owned);
    }

    for (count = 0U, event_group = g_os.event_group_list; event_group != NULL; event_group = event_group->next, ++count)
    {
    }
    os_system_put32(stream, count);
    for (event_group = g_os.event_group_list; event_group != NULL; event_group = event_group->next)
    {
        os_system_put32(stream, event_group->id);
        os_system_put32(stream, event_group->bits);
        os_system_put_fields(stream, event_group->name, event_group->guest_refs, event_group->guest_owned);
    }

    for (count = 0U, timer = g_os.timer_list; timer != NULL; timer = timer->next, ++count)
    {
    }
    os_system_put32(stream, count);
    for (timer = g_os.timer_list; timer != NULL; timer = timer->next)
    {
        os_system_put32(stream, timer->id);
        os_system_put32(stream, timer->period_ms);
        os_system_put32(stream, timer->expiry_tick_ms);
        /* A timer whose task is gone is deleted at the next time event: it saves with none. */
        os_system_put32(stream, timer->counting ? 0U : os_system_task_id(timer->target_task));
        os_system_put32(stream, timer->notify_value);
        os_system_put32(stream, (uint32_t)timer->notify_action);
        os_system_put32(stream, timer->auto_reload);
        os_system_put32(stream, timer->active);
        os_system_put32(stream, timer->counting);
        os_system_put32(stream, timer->expirations);
        os_system_put_fields(stream, timer->name, timer->guest_refs, timer->guest_owned);
    }
}

static void os_system_put_value(OsSystemStream* stream, const OsValue* value)
{
    uint64_t bits = 0U;
    memcpy(&bits, &value->value, sizeof(value->value) < sizeof(bits) ? sizeof(value->value) : sizeof(bits));
    os_system_put32(stream, (uint32_t)value->type);
    os_system_put64(stream, bits);
}

/* The id of the object a task waits on, by the wait's type. */
static uint32_t os_system_wait_object_id(OsTaskHandle task)
{
    switch (task->wait_type)
    {
        case OS_TASK_WAIT_QUEUE_SEND:
        case OS_TASK_WAIT_QUEUE_RECEIVE:
            return task->wait_object.queue != NULL ? task->wait_object.queue->id : 0U;
        case OS_TASK_WAIT_MUTEX:
            return task->wait_object.mutex != NULL ? task->wait_object.mutex->id : 0U;
        case OS_TASK_WAIT_SEMAPHORE:
            return task->wait_object.semaphore != NULL ? task->wait_object.semaphore->id : 0U;
        case OS_TASK_WAIT_EVENT_GROUP:
            return task->wait_object.event_group != NULL ? task->wait_object.event_group->id : 0U;
        default:
            return 0U;
    }
}

static void os_system_save_module(OsSystemStream* stream, OsTaskHandle task)
{
    uint32_t source = OS_SYSTEM_MODULE_HOST;

    if (task->state == OS_TASK_DEAD)
    {
        os_system_put32(stream, OS_SYSTEM_MODULE_NONE);
        return;
    }
#if OS_HAS_PROCESSES
    if (task->spawned)
    {
        source = OS_SYSTEM_MODULE_BYTES;
#if OS_HAS_FS
        if (task->image_path != NULL)
        {
            source = OS_SYSTEM_MODULE_PATH;
        }
#endif
    }
#endif
    os_system_put32(stream, source);
#if OS_HAS_PROCESSES && OS_HAS_FS
    if (source == OS_SYSTEM_MODULE_PATH)
    {
        os_system_put_text(stream, task->image_path);
    }
#endif
    os_system_put32(stream, task->wasm_size);
    os_system_put32(stream, os_system_module_hash(task->wasm_bytes, task->wasm_size));
    if (source == OS_SYSTEM_MODULE_BYTES)
    {
        os_system_put(stream, task->wasm_bytes, task->wasm_size);
    }
}

static void os_system_save_task(OsSystemStream* stream, OsTaskHandle task)
{
    uint8_t dead = task->state == OS_TASK_DEAD ? 1U : 0U;
    uint32_t index = 0U;

    os_system_put32(stream, task->id);
    os_system_put_text(stream, task->name);
    os_system_put32(stream, (uint32_t)task->state);
    os_system_put32(stream, (uint32_t)task->exit_reason);
    os_system_put32(stream, task->exit_code);
    os_system_put32(stream, task->base_priority);
    os_system_put32(stream, task->priority);
    os_system_put32(stream, task->wake_tick_ms);

    os_system_put32(stream, (uint32_t)task->wait_type);
    os_system_put32(stream, os_system_wait_object_id(task));
    os_system_put64(stream, task->wait_sequence);
    if (task->wait_queue_item != NULL && task->wait_object.queue != NULL)
    {
        os_system_put32(stream, task->wait_object.queue->item_size);
        os_system_put(stream, task->wait_queue_item, task->wait_object.queue->item_size);
    }
    else
    {
        os_system_put32(stream, OS_SYSTEM_NO_TEXT);
    }
    os_system_put32(stream, task->wait_queue_has_wasm_output);
    os_system_put32(stream, task->wait_queue_wasm_output_address);
    os_system_put32(stream, task->wait_bits);
    os_system_put32(stream, task->wait_clear_on_exit);
    os_system_put32(stream, task->wait_for_all);
    os_system_put32(stream, task->wait_has_timeout);
    os_system_put32(stream, task->wait_has_return_slot);
    os_system_put32(stream, task->wait_return_offset);
    os_system_put32(stream, (uint32_t)task->wait_return_kind);
    os_system_put32(stream, task->last_wait_value);
    os_system_put32(stream, (uint32_t)task->last_wait_status);
    os_system_put32(stream, task->wait_target_count);
    for (index = 0U; index < task->wait_target_count; ++index)
    {
        os_system_put32(stream, (uint32_t)task->wait_targets[index].type);
        os_system_put32(stream, task->wait_targets[index].id);
    }

    os_system_put32(stream, task->notification_value);
    os_system_put32(stream, task->notification_clear_on_exit);
    os_system_put32(stream, task->notification_pending);
    os_system_put32(stream, task->notification_take_clear_count);

    os_system_put_text(stream, task->entry_function_name);
    os_system_put32(stream, task->stack_size);
    os_system_put32(stream, os_task_stack_peak(task));
    os_system_put_text(stream, task->app_name);
    os_system_put_text(stream, task->error_text);
    os_system_put32(stream, task->entry_arg_count);
    for (index = 0U; index < task->entry_arg_count; ++index)
    {
        os_system_put_value(stream, &task->entry_args[index]);
    }
    os_system_put32(stream, task->wasi_exit_code);
    os_system_put32(stream, task->wasm_started);
    os_system_put32(stream, task->entry_return_code);
    os_system_put32(stream, task->wasm_needs_resume);
    os_system_put32(stream, task->run_count);
    os_system_put64(stream, task->fuel_used);
    /* What a dead task returned; a living one has not returned yet. */
    os_system_put32(stream, dead ? task->entry_return_count : 0U);
    for (index = 0U; dead && index < task->entry_return_count; ++index)
    {
        os_system_put_value(stream, &task->entry_return_values[index]);
    }

#if OS_HAS_PROCESSES
    os_system_put32(stream, task->parent_id);
    os_system_put32(stream, task->wait_child_id);
    os_system_put32(stream, task->spawned);
    os_system_put32(stream, task->auto_reap);
    os_system_put32(stream, task->exit_collected);
    os_system_put32(stream, task->reap_pending);
    os_system_put32(stream, task->argc);
    for (index = 0U; index < task->argc; ++index)
    {
        os_system_put_text(stream, task->argv[index]);
    }
#endif

    os_system_save_module(stream, task);
    /* A dead task's handles are closed; its files go with it. */
    if (dead)
    {
        os_system_put32(stream, 0U);
#if OS_HAS_FS
        os_system_put32(stream, 0U);
#endif
        os_system_put32(stream, 0U);
        return;
    }
    os_abi_save_task(task, stream);
#if OS_HAS_FS
    os_fs_save_task(task->id, stream);
#endif

    os_system_put32(stream, task->wasm_started);
    if (task->wasm_started && os_system_ok(stream))
    {
        WmResult result = wm_snapshot_write(task->wasm_runtime, task->wasm_instance, os_system_chunk_write, stream);
        if (result != NULL && os_system_ok(stream))
        {
            os_set_last_error(task, "system_save", result, os_wasm_snapshot_result_to_status(result));
            os_system_fail(stream, os_wasm_snapshot_result_to_status(result));
        }
        os_system_chunk_end(stream);
    }
}

/* Whether every task can be saved now; the reason in the last error when not. */
static OsStatus os_system_check(void)
{
    OsTaskHandle task = NULL;

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        uint32_t index = 0U;

        if (task->state == OS_TASK_DEAD)
        {
            continue;
        }
#if OS_HAS_DYLINK
        if (task->wasm_dylink != NULL)
        {
            os_set_last_error(task, "system_save", "unsupported: a dylink.0 program cannot be saved yet",
                              OS_STATUS_UNSUPPORTED);
            return OS_STATUS_UNSUPPORTED;
        }
#endif
        if (task->wait_type == OS_TASK_WAIT_TRANSFER || task->wait_type == OS_TASK_WAIT_PAGE)
        {
            os_set_last_error(task, "system_save", "a transfer or page is on its way: try after the next slice",
                              OS_STATUS_BUSY);
            return OS_STATUS_BUSY;
        }
        for (index = 0U; index < task->wait_target_count; ++index)
        {
            if (task->wait_targets[index].type == OS_OBJECT_DRIVER)
            {
                os_set_last_error(task, "system_save", "unsupported: it waits for a device", OS_STATUS_UNSUPPORTED);
                return OS_STATUS_UNSUPPORTED;
            }
        }
        if (os_abi_task_device(task) != NULL)
        {
            os_set_last_error(task, "system_save", "unsupported: it holds a device, which cannot be saved yet",
                              OS_STATUS_UNSUPPORTED);
            return OS_STATUS_UNSUPPORTED;
        }
    }
    return OS_STATUS_OK;
}

OsStatus os_system_save(OsSnapshotWrite write, void* context)
{
    OsSystemStream stream;
    OsTaskHandle task = NULL;
    OsStatus status = OS_STATUS_OK;
    uint32_t count = 0U;

    if (write == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_os.initialized)
    {
        return OS_STATUS_ERROR;
    }
    if (g_os.current_task != NULL || g_os.processing_time_events)
    {
        return OS_STATUS_BUSY;
    }
    status = os_system_check();
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    /* A swapped-out task comes back first; its memory stays paged. */
    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (task->swapped && task->state != OS_TASK_DEAD)
        {
            status = os_task_swap_in(task);
            if (status != OS_STATUS_OK)
            {
                return status;
            }
        }
    }

    memset(&stream, 0, sizeof(stream));
    stream.write = write;
    stream.context = context;
    os_system_put32(&stream, OS_SYSTEM_MAGIC);
    os_system_put32(&stream, OS_SYSTEM_VERSION);
    os_system_put32(&stream, OS_SYSTEM_FEATURES);
    os_system_put32(&stream, g_os.tick_ms);
    os_system_put64(&stream, g_os.tick_ms_extended);
    os_system_put32(&stream, g_os.next_task_id);
    os_system_put32(&stream, g_os.next_queue_id);
    os_system_put32(&stream, g_os.next_mutex_id);
    os_system_put32(&stream, g_os.next_semaphore_id);
    os_system_put32(&stream, g_os.next_event_group_id);
    os_system_put32(&stream, g_os.next_timer_id);
    os_system_put64(&stream, g_os.next_wait_sequence);
    os_system_put32(&stream, os_system_task_id(g_os.last_scheduled_task));

    os_system_save_objects(&stream);

    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        ++count;
    }
    os_system_put32(&stream, count);
    for (task = g_os.task_list; task != NULL && os_system_ok(&stream); task = task->next)
    {
        os_system_save_task(&stream, task);
    }

    if (stream.status == OS_STATUS_OK)
    {
        os_clear_last_error();
    }
    else if (g_os.last_error.phase == NULL || strcmp(g_os.last_error.phase, "system_save") != 0)
    {
        os_set_last_error(NULL, "system_save", os_system_stream_failed, stream.status);
    }
    return stream.status;
}

/* ---- Restoring -------------------------------------------------------------- */

/* What a restore keeps until objects and tasks are linked by id. */
typedef struct OsSystemLinks
{
    uint32_t* mutex_owners;
    uint32_t mutex_count;
    uint32_t* timer_targets;
    uint32_t timer_count;
    uint32_t last_scheduled_id;
    /* Why each task is not restored, in list order; NULL for one that is. */
    const char** lost;
    OsStatus* lost_status;
    uint32_t task_count;
} OsSystemLinks;

/* Ids of one kind: new, not 0, below the next id. */
static uint8_t os_system_id_ok(uint32_t id, uint32_t next_id)
{
    return id != 0U && id < next_id ? 1U : 0U;
}

static void os_system_get_fields(OsSystemStream* stream, char** name, uint32_t* guest_refs, uint8_t* guest_owned)
{
    *name = os_system_get_text(stream);
    *guest_refs = os_system_get32(stream);
    *guest_owned = (uint8_t)os_system_get_max(stream, 1U);
}

/* Append to the end of a list kept by its head, as the kernel's own inserts do. */
#define OS_SYSTEM_APPEND(head, tail, object) \
    do { (object)->previous = (tail); (object)->next = NULL; \
         if ((tail) != NULL) { (tail)->next = (object); } else { (head) = (object); } \
         (tail) = (object); } while (0)

static void os_system_restore_objects(OsSystemStream* stream, OsSystemLinks* links)
{
    uint32_t count = 0U;
    uint32_t index = 0U;
    OsQueueHandle queue_tail = NULL;
    OsMutexHandle mutex_tail = NULL;
    OsSemaphoreHandle semaphore_tail = NULL;
    OsEventGroupHandle event_group_tail = NULL;
    OsTimerHandle timer_tail = NULL;

    count = os_system_get32(stream);
    for (index = 0U; index < count && os_system_ok(stream); ++index)
    {
        OsQueueHandle queue = (OsQueueHandle)calloc(1U, sizeof(struct OsQueue));
        uint32_t bytes = 0U;
        if (queue == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        OS_SYSTEM_APPEND(g_os.queue_list, queue_tail, queue);
        queue->id = os_system_get32(stream);
        queue->item_size = os_system_get32(stream);
        queue->item_count = os_system_get32(stream);
        queue->count = os_system_get32(stream);
        queue->head = os_system_get32(stream);
        queue->tail = os_system_get32(stream);
        os_system_get_fields(stream, &queue->name, &queue->guest_refs, &queue->guest_owned);
        if (!os_system_ok(stream))
        {
            return;
        }
        if (!os_system_id_ok(queue->id, g_os.next_queue_id) || os_queue_find_by_id(queue->id) != queue ||
            queue->item_size == 0U || queue->item_count == 0U ||
            queue->item_count > OS_SYSTEM_BYTES_MAX / queue->item_size || queue->count > queue->item_count ||
            queue->head >= queue->item_count || queue->tail >= queue->item_count)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
            return;
        }
        bytes = queue->item_size * queue->item_count;
        queue->storage = (uint8_t*)malloc(bytes);
        if (queue->storage == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        os_system_get(stream, queue->storage, bytes);
    }

    count = os_system_get32(stream);
    if (os_system_ok(stream) && count > 0U)
    {
        links->mutex_owners = (uint32_t*)calloc(count, sizeof(uint32_t));
        if (links->mutex_owners == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        links->mutex_count = count;
    }
    for (index = 0U; index < count && os_system_ok(stream); ++index)
    {
        OsMutexHandle mutex = (OsMutexHandle)calloc(1U, sizeof(struct OsMutex));
        if (mutex == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        OS_SYSTEM_APPEND(g_os.mutex_list, mutex_tail, mutex);
        mutex->id = os_system_get32(stream);
        links->mutex_owners[index] = os_system_get32(stream);
        os_system_get_fields(stream, &mutex->name, &mutex->guest_refs, &mutex->guest_owned);
        if (os_system_ok(stream) &&
            (!os_system_id_ok(mutex->id, g_os.next_mutex_id) || os_mutex_find_by_id(mutex->id) != mutex))
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
    }

    count = os_system_get32(stream);
    for (index = 0U; index < count && os_system_ok(stream); ++index)
    {
        OsSemaphoreHandle semaphore = (OsSemaphoreHandle)calloc(1U, sizeof(struct OsSemaphore));
        if (semaphore == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        OS_SYSTEM_APPEND(g_os.semaphore_list, semaphore_tail, semaphore);
        semaphore->id = os_system_get32(stream);
        semaphore->max_count = os_system_get32(stream);
        semaphore->count = os_system_get32(stream);
        os_system_get_fields(stream, &semaphore->name, &semaphore->guest_refs, &semaphore->guest_owned);
        if (os_system_ok(stream) &&
            (!os_system_id_ok(semaphore->id, g_os.next_semaphore_id) ||
             os_semaphore_find_by_id(semaphore->id) != semaphore || semaphore->max_count == 0U ||
             semaphore->count > semaphore->max_count))
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
    }

    count = os_system_get32(stream);
    for (index = 0U; index < count && os_system_ok(stream); ++index)
    {
        OsEventGroupHandle event_group = (OsEventGroupHandle)calloc(1U, sizeof(struct OsEventGroup));
        if (event_group == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        OS_SYSTEM_APPEND(g_os.event_group_list, event_group_tail, event_group);
        event_group->id = os_system_get32(stream);
        event_group->bits = os_system_get32(stream);
        os_system_get_fields(stream, &event_group->name, &event_group->guest_refs, &event_group->guest_owned);
        if (os_system_ok(stream) &&
            (!os_system_id_ok(event_group->id, g_os.next_event_group_id) ||
             os_event_group_find_by_id(event_group->id) != event_group))
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
    }

    count = os_system_get32(stream);
    if (os_system_ok(stream) && count > 0U)
    {
        links->timer_targets = (uint32_t*)calloc(count, sizeof(uint32_t));
        if (links->timer_targets == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        links->timer_count = count;
    }
    for (index = 0U; index < count && os_system_ok(stream); ++index)
    {
        OsTimerHandle timer = (OsTimerHandle)calloc(1U, sizeof(struct OsTimer));
        if (timer == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        OS_SYSTEM_APPEND(g_os.timer_list, timer_tail, timer);
        ++g_os.timer_count;
        timer->id = os_system_get32(stream);
        timer->period_ms = os_system_get32(stream);
        timer->expiry_tick_ms = os_system_get32(stream);
        links->timer_targets[index] = os_system_get32(stream);
        timer->notify_value = os_system_get32(stream);
        timer->notify_action = (OsNotifyAction)os_system_get_max(stream, (uint32_t)OS_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE);
        timer->auto_reload = (uint8_t)os_system_get_max(stream, 1U);
        timer->active = (uint8_t)os_system_get_max(stream, 1U);
        timer->counting = (uint8_t)os_system_get_max(stream, 1U);
        timer->expirations = os_system_get32(stream);
        os_system_get_fields(stream, &timer->name, &timer->guest_refs, &timer->guest_owned);
        if (os_system_ok(stream) &&
            (!os_system_id_ok(timer->id, g_os.next_timer_id) || os_timer_find_by_id(timer->id) != timer ||
             timer->period_ms == 0U || timer->period_ms > (uint32_t)INT32_MAX))
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
    }
}

static void os_system_get_value(OsSystemStream* stream, OsValue* value)
{
    uint32_t type = os_system_get_max(stream, (uint32_t)OS_VALUE_TYPE_F64);
    uint64_t bits = os_system_get64(stream);
    value->type = (OsValueType)type;
    memcpy(&value->value, &bits, sizeof(value->value) < sizeof(bits) ? sizeof(value->value) : sizeof(bits));
}

/* The object a wait names, found by its id; 0 when it names none; the stream breaks when it is missing. */
static void os_system_link_wait(OsSystemStream* stream, OsTaskHandle task, uint32_t id)
{
    uint8_t found = 1U;

    switch (task->wait_type)
    {
        case OS_TASK_WAIT_QUEUE_SEND:
        case OS_TASK_WAIT_QUEUE_RECEIVE:
            task->wait_object.queue = id != 0U ? os_queue_find_by_id(id) : NULL;
            found = id == 0U || task->wait_object.queue != NULL;
            break;
        case OS_TASK_WAIT_MUTEX:
            task->wait_object.mutex = id != 0U ? os_mutex_find_by_id(id) : NULL;
            found = id == 0U || task->wait_object.mutex != NULL;
            break;
        case OS_TASK_WAIT_SEMAPHORE:
            task->wait_object.semaphore = id != 0U ? os_semaphore_find_by_id(id) : NULL;
            found = id == 0U || task->wait_object.semaphore != NULL;
            break;
        case OS_TASK_WAIT_EVENT_GROUP:
            task->wait_object.event_group = id != 0U ? os_event_group_find_by_id(id) : NULL;
            found = id == 0U || task->wait_object.event_group != NULL;
            break;
        default:
            found = id == 0U;
            break;
    }
    if (!found)
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
    }
}

/* The module of a task that lives, found again; why it cannot be, or NULL. */
static const char* os_system_restore_module(OsSystemStream* stream, OsTaskHandle task, OsModuleFind find,
                                            void* find_context)
{
    uint32_t source = os_system_get_max(stream, OS_SYSTEM_MODULE_BYTES);
    char* path = NULL;
    uint32_t size = 0U;
    uint32_t hash = 0U;
    const uint8_t* bytes = NULL;
    uint32_t found_size = 0U;
    const char* reason = NULL;

    if (source == OS_SYSTEM_MODULE_PATH)
    {
        path = os_system_get_text(stream);
    }
    if ((source == OS_SYSTEM_MODULE_NONE) != (task->state == OS_TASK_DEAD))
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
    }
    if (source == OS_SYSTEM_MODULE_NONE || !os_system_ok(stream))
    {
        free(path);
        return NULL;
    }
    size = os_system_get_max(stream, OS_SYSTEM_BYTES_MAX);
    hash = os_system_get32(stream);
    if (!os_system_ok(stream) || size == 0U || (source == OS_SYSTEM_MODULE_PATH && path == NULL))
    {
        free(path);
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        return NULL;
    }

    if (source == OS_SYSTEM_MODULE_HOST)
    {
        if (find == NULL || find(find_context, task->name, &bytes, &found_size) != OS_STATUS_OK || bytes == NULL)
        {
            reason = "its module was not found";
        }
    }
    else if (source == OS_SYSTEM_MODULE_PATH)
    {
#if OS_HAS_PROCESSES && OS_HAS_FS
        task->image_path = path;
        path = NULL;
        if (os_fs_image_open(task->image_path, &task->image) == OS_STATUS_OK)
        {
            bytes = task->image.bytes;
            found_size = task->image.size;
        }
        else
        {
            reason = "its program file is gone";
        }
#else
        os_system_fail(stream, OS_STATUS_UNSUPPORTED);
#endif
    }
    else
    {
#if OS_HAS_PROCESSES
        task->owned_wasm_bytes = (uint8_t*)malloc(size);
        if (task->owned_wasm_bytes == NULL)
        {
            os_system_skip(stream, size);
            reason = "out of memory for its module";
        }
        else
        {
            os_system_get(stream, task->owned_wasm_bytes, size);
            bytes = task->owned_wasm_bytes;
            found_size = size;
        }
#else
        os_system_fail(stream, OS_STATUS_UNSUPPORTED);
#endif
    }
    free(path);
    if (!os_system_ok(stream) || reason != NULL)
    {
        return reason;
    }
    if (found_size != size || os_system_module_hash(bytes, found_size) != hash)
    {
        return "its module changed";
    }
    task->wasm_bytes = (uint8_t*)bytes;
    task->wasm_size = size;
    return NULL;
}

/* The engine's side of a task that lives: its runtime made again and its snapshot loaded. */
static const char* os_system_restore_engine(OsSystemStream* stream, OsTaskHandle task, uint8_t lost,
                                            OsStatus* out_status)
{
    uint32_t started = os_system_get_max(stream, 1U);
    WmResult result = NULL;
    OsStatus status = OS_STATUS_OK;

    stream->chunk_left = 0U;
    stream->chunk_end = 0U;
    if (!os_system_ok(stream))
    {
        return NULL;
    }
    if (started != task->wasm_started)
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        return NULL;
    }
    if (!lost)
    {
        OS_HEAP_ENTER(task);
        status = os_task_create_wasm(task, task->wasm_bytes, task->wasm_size, task->entry_function_name,
                                     task->stack_size);
        if (status == OS_STATUS_OK && started)
        {
            result = wm_snapshot_read(task->wasm_runtime, task->wasm_instance, os_system_chunk_read, stream);
            status = result == os_system_stream_failed ? stream->status : os_wasm_snapshot_result_to_status(result);
        }
        OS_HEAP_LEAVE();
    }
    if (started)
    {
        os_system_chunk_finish(stream, (uint8_t)(!lost && status == OS_STATUS_OK));
    }
    if (lost || !os_system_ok(stream) || status == OS_STATUS_OK)
    {
        return NULL;
    }
    *out_status = status;
    return result != NULL ? result
                          : g_os.last_error.result != NULL ? g_os.last_error.result : "its program cannot be loaded";
}

static void os_system_restore_task(OsSystemStream* stream, OsTaskHandle task, OsModuleFind find, void* find_context,
                                   const char** out_lost, OsStatus* out_lost_status)
{
    uint32_t index = 0U;
    uint32_t wait_object_id = 0U;
    uint32_t item_size = 0U;
    char* name = NULL;
    const char* lost = NULL;

    task->id = os_system_get32(stream);
    name = os_system_get_text(stream);
    if (name != NULL)
    {
        strncpy(task->name, name, OS_TASK_NAME_MAX - 1U);
        task->name[OS_TASK_NAME_MAX - 1U] = '\0';
        free(name);
    }
    task->state = (OsTaskState)os_system_get_max(stream, (uint32_t)OS_TASK_DEAD);
    task->exit_reason = (OsTaskExitReason)os_system_get_max(stream, (uint32_t)OS_TASK_EXIT_WASM_ERROR);
    task->exit_code = os_system_get32(stream);
    task->base_priority = os_system_get32(stream);
    task->priority = os_system_get32(stream);
    task->wake_tick_ms = os_system_get32(stream);
    if (os_system_ok(stream) &&
        (!os_system_id_ok(task->id, g_os.next_task_id) || os_task_find_by_id(task->id) != task ||
         task->state == OS_TASK_RUNNING))
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
    }

    task->wait_type = (OsTaskWaitType)os_system_get_max(stream, (uint32_t)OS_TASK_WAIT_SIGNAL);
    wait_object_id = os_system_get32(stream);
    os_system_link_wait(stream, task, wait_object_id);
    task->wait_sequence = os_system_get64(stream);
    item_size = os_system_get32(stream);
    if (item_size != OS_SYSTEM_NO_TEXT && os_system_ok(stream))
    {
        if (task->wait_type != OS_TASK_WAIT_QUEUE_SEND || task->wait_object.queue == NULL ||
            item_size != task->wait_object.queue->item_size)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
            return;
        }
        task->wait_queue_item = (uint8_t*)malloc(item_size);
        if (task->wait_queue_item == NULL)
        {
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
        os_system_get(stream, task->wait_queue_item, item_size);
    }
    task->wait_queue_has_wasm_output = (uint8_t)os_system_get_max(stream, 1U);
    task->wait_queue_wasm_output_address = os_system_get32(stream);
    task->wait_bits = os_system_get32(stream);
    task->wait_clear_on_exit = (uint8_t)os_system_get_max(stream, 1U);
    task->wait_for_all = (uint8_t)os_system_get_max(stream, 1U);
    task->wait_has_timeout = (uint8_t)os_system_get_max(stream, 1U);
    task->wait_has_return_slot = (uint8_t)os_system_get_max(stream, 1U);
    task->wait_return_offset = os_system_get32(stream);
    task->wait_return_kind = (OsWaitReturnKind)os_system_get_max(stream, (uint32_t)OS_WAIT_RETURN_ABI);
    task->last_wait_value = os_system_get32(stream);
    task->last_wait_status = (OsStatus)os_system_get32(stream);
    task->wait_target_count = os_system_get_max(stream, OS_SYSTEM_TEXT_MAX);
    if (task->wait_target_count > 0U && os_system_ok(stream))
    {
        task->wait_targets = (OsWaitTarget*)calloc(task->wait_target_count, sizeof(OsWaitTarget));
        if (task->wait_targets == NULL)
        {
            task->wait_target_count = 0U;
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
    }
    for (index = 0U; index < task->wait_target_count && os_system_ok(stream); ++index)
    {
        uint32_t type = os_system_get32(stream);
        task->wait_targets[index].id = os_system_get32(stream);
        /* Waits for devices are never saved. */
        if (type < (uint32_t)OS_OBJECT_QUEUE || type > (uint32_t)OS_OBJECT_TASK)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
        task->wait_targets[index].type = (OsObjectType)type;
    }

    task->notification_value = os_system_get32(stream);
    task->notification_clear_on_exit = os_system_get32(stream);
    task->notification_pending = (uint8_t)os_system_get_max(stream, 1U);
    task->notification_take_clear_count = (uint8_t)os_system_get_max(stream, 1U);

    task->restored_entry_name = os_system_get_text(stream);
    task->entry_function_name = task->restored_entry_name;
    task->stack_size = os_system_get32(stream);
    task->stack_peak_bytes = os_system_get32(stream);
    task->app_name = os_system_get_text(stream);
    task->error_text = os_system_get_text(stream);
    task->entry_arg_count = os_system_get_max(stream, OS_SYSTEM_TEXT_MAX);
    if (task->entry_arg_count > 0U && os_system_ok(stream))
    {
        task->entry_args = (OsValue*)calloc(task->entry_arg_count, sizeof(OsValue));
        if (task->entry_args == NULL)
        {
            task->entry_arg_count = 0U;
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
    }
    for (index = 0U; index < task->entry_arg_count && os_system_ok(stream); ++index)
    {
        os_system_get_value(stream, &task->entry_args[index]);
    }
    task->wasi_exit_code = os_system_get32(stream);
    task->wasm_started = (uint8_t)os_system_get_max(stream, 1U);
    task->entry_return_code = os_system_get32(stream);
    task->wasm_needs_resume = (uint8_t)os_system_get_max(stream, 1U);
    task->run_count = os_system_get32(stream);
    task->fuel_used = os_system_get64(stream);
    task->entry_return_count = os_system_get_max(stream, OS_SYSTEM_TEXT_MAX);
    if (task->entry_return_count > 0U && os_system_ok(stream))
    {
        task->entry_return_values = (OsValue*)calloc(task->entry_return_count, sizeof(OsValue));
        if (task->entry_return_values == NULL)
        {
            task->entry_return_count = 0U;
            os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            return;
        }
    }
    for (index = 0U; index < task->entry_return_count && os_system_ok(stream); ++index)
    {
        os_system_get_value(stream, &task->entry_return_values[index]);
    }

#if OS_HAS_PROCESSES
    task->parent_id = os_system_get32(stream);
    task->wait_child_id = os_system_get32(stream);
    task->spawned = (uint8_t)os_system_get_max(stream, 1U);
    task->auto_reap = (uint8_t)os_system_get_max(stream, 1U);
    task->exit_collected = (uint8_t)os_system_get_max(stream, 1U);
    task->reap_pending = (uint8_t)os_system_get_max(stream, 1U);
    g_os.spawned_task_count += task->spawned;
    task->argc = os_system_get_max(stream, OS_PROC_MAX_ARGS);
    if (os_system_ok(stream))
    {
        char* arguments[OS_PROC_MAX_ARGS];
        uint32_t read = 0U;
        for (read = 0U; read < task->argc && os_system_ok(stream); ++read)
        {
            arguments[read] = os_system_get_text(stream);
            if (arguments[read] == NULL)
            {
                os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
            }
        }
        if (os_system_ok(stream) && task->argc > 0U)
        {
            /* One block, as os_task_spawn packs it: pointers, then the texts. */
            size_t total = (size_t)(task->argc + 1U) * sizeof(char*);
            char* cursor = NULL;
            for (index = 0U; index < task->argc; ++index)
            {
                total += strlen(arguments[index]) + 1U;
            }
            task->argv = (char**)malloc(total);
            if (task->argv == NULL)
            {
                os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
            }
            else
            {
                cursor = (char*)(task->argv + task->argc + 1U);
                for (index = 0U; index < task->argc; ++index)
                {
                    size_t length = strlen(arguments[index]) + 1U;
                    memcpy(cursor, arguments[index], length);
                    task->argv[index] = cursor;
                    cursor += length;
                }
                task->argv[task->argc] = NULL;
            }
        }
        for (index = 0U; index < read; ++index)
        {
            free(arguments[index]);
        }
        if (task->argv == NULL)
        {
            task->argc = 0U;
        }
    }
#endif
    if (!os_system_ok(stream))
    {
        return;
    }

    lost = os_system_restore_module(stream, task, find, find_context);
    *out_lost_status = OS_STATUS_WASM_ERROR;
    os_abi_restore_task(task, stream);
#if OS_HAS_FS
    {
        const char* files = os_fs_restore_task(task->id, stream, (uint8_t)(lost == NULL && task->state != OS_TASK_DEAD));
        lost = lost != NULL ? lost : files;
    }
#endif
    if (task->state == OS_TASK_DEAD)
    {
        if (os_system_get32(stream) != 0U)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
        return;
    }
    {
        const char* engine = os_system_restore_engine(stream, task, (uint8_t)(lost != NULL), out_lost_status);
        lost = lost != NULL ? lost : engine;
    }
    *out_lost = lost;
}

/* Everything a restore made goes; the kernel is as the host had it before. */
static void os_system_clear(uint32_t tick_ms, uint64_t tick_ms_extended)
{
    OsTaskHandle task = g_os.task_list;

    g_os.last_scheduled_task = NULL;
    /* Out of the list before it goes, as os_shutdown does. */
    while (task != NULL)
    {
        OsTaskHandle next = task->next;
        g_os.task_list = next;
        if (next != NULL)
        {
            next->previous = NULL;
        }
        os_task_free(task);
        task = next;
    }
    os_queue_free_all();
    os_mutex_free_all();
    os_semaphore_free_all();
    os_event_group_free_all();
    os_timer_free_all();
    g_os.next_task_id = 1U;
    g_os.next_queue_id = 1U;
    g_os.next_mutex_id = 1U;
    g_os.next_semaphore_id = 1U;
    g_os.next_event_group_id = 1U;
    g_os.next_timer_id = 1U;
    g_os.next_wait_sequence = 1U;
    g_os.timer_count = 0U;
#if OS_HAS_PROCESSES
    g_os.spawned_task_count = 0U;
#endif
    g_os.tick_ms = tick_ms;
    g_os.tick_ms_extended = tick_ms_extended;
    os_recalculate_task_counters();
}

/* Mutex owners, timer targets and the task scheduled last, by id now that every task is there. */
static void os_system_link(OsSystemStream* stream, const OsSystemLinks* links)
{
    OsMutexHandle mutex = g_os.mutex_list;
    OsTimerHandle timer = g_os.timer_list;
    uint32_t index = 0U;

    for (index = 0U; mutex != NULL && index < links->mutex_count; ++index, mutex = mutex->next)
    {
        mutex->owner = links->mutex_owners[index] != 0U ? os_task_find_by_id(links->mutex_owners[index]) : NULL;
        if (links->mutex_owners[index] != 0U && mutex->owner == NULL)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
    }
    for (index = 0U; timer != NULL && index < links->timer_count; ++index, timer = timer->next)
    {
        timer->target_task = links->timer_targets[index] != 0U ? os_task_find_by_id(links->timer_targets[index]) : NULL;
        if (links->timer_targets[index] != 0U && timer->target_task == NULL)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        }
    }
    g_os.last_scheduled_task = links->last_scheduled_id != 0U ? os_task_find_by_id(links->last_scheduled_id) : NULL;
    if (links->last_scheduled_id != 0U && g_os.last_scheduled_task == NULL)
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
    }
}

/* A task that could not be restored ends as one that traps: its objects and its parent see it end. */
static void os_system_end_lost(OsTaskHandle task, const char* reason, OsStatus status)
{
    os_task_cleanup_wasm(task);
    os_set_last_error(task, "restore", reason, status);
    os_task_record_error(task);
    os_record_task_exit(task, OS_TASK_EXIT_WASM_ERROR, 1U);
    os_set_task_dead(task);
}

OsStatus os_system_restore(OsSnapshotRead read, void* context, OsModuleFind find, void* find_context,
                           uint32_t slept_ms)
{
    OsSystemStream stream;
    OsSystemLinks links;
    OsTaskHandle task = NULL;
    OsTaskHandle tail = NULL;
    uint32_t tick_ms = g_os.tick_ms;
    uint64_t tick_ms_extended = g_os.tick_ms_extended;
    uint32_t count = 0U;
    uint32_t index = 0U;

    if (read == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_os.initialized)
    {
        return OS_STATUS_ERROR;
    }
    /* Into a kernel started afresh: the stream's ids would meet the ones made since. */
    if (g_os.task_list != NULL || g_os.queue_list != NULL || g_os.mutex_list != NULL ||
        g_os.semaphore_list != NULL || g_os.event_group_list != NULL || g_os.timer_list != NULL ||
        g_os.current_task != NULL)
    {
        return OS_STATUS_BUSY;
    }

    memset(&stream, 0, sizeof(stream));
    memset(&links, 0, sizeof(links));
    stream.read = read;
    stream.context = context;
    if (os_system_get32(&stream) != OS_SYSTEM_MAGIC || os_system_get32(&stream) != OS_SYSTEM_VERSION)
    {
        os_system_fail(&stream, OS_STATUS_INVALID_ARGUMENT);
    }
    if (os_system_get32(&stream) != OS_SYSTEM_FEATURES)
    {
        /* Saved by a kernel built with other features. */
        os_system_fail(&stream, OS_STATUS_UNSUPPORTED);
    }
    g_os.tick_ms = os_system_get32(&stream);
    g_os.tick_ms_extended = os_system_get64(&stream);
    g_os.next_task_id = os_system_get32(&stream);
    g_os.next_queue_id = os_system_get32(&stream);
    g_os.next_mutex_id = os_system_get32(&stream);
    g_os.next_semaphore_id = os_system_get32(&stream);
    g_os.next_event_group_id = os_system_get32(&stream);
    g_os.next_timer_id = os_system_get32(&stream);
    g_os.next_wait_sequence = os_system_get64(&stream);
    links.last_scheduled_id = os_system_get32(&stream);

    if (os_system_ok(&stream))
    {
        os_system_restore_objects(&stream, &links);
    }

    count = os_system_get32(&stream);
    if (os_system_ok(&stream) && count > 0U)
    {
        links.lost = (const char**)calloc(count, sizeof(const char*));
        links.lost_status = (OsStatus*)calloc(count, sizeof(OsStatus));
        if (links.lost == NULL || links.lost_status == NULL)
        {
            os_system_fail(&stream, OS_STATUS_OUT_OF_MEMORY);
        }
        links.task_count = count;
    }
    for (index = 0U; index < count && os_system_ok(&stream); ++index)
    {
        task = (OsTaskHandle)calloc(1U, sizeof(struct OsTask));
        if (task == NULL)
        {
            os_system_fail(&stream, OS_STATUS_OUT_OF_MEMORY);
            break;
        }
        OS_SYSTEM_APPEND(g_os.task_list, tail, task);
        os_system_restore_task(&stream, task, find, find_context, &links.lost[index], &links.lost_status[index]);
    }
    if (os_system_ok(&stream))
    {
        os_system_link(&stream, &links);
    }

    if (!os_system_ok(&stream))
    {
        OsStatus status = stream.status;
        os_system_clear(tick_ms, tick_ms_extended);
        os_set_last_error(NULL, "system_restore", "the stream does not fit this system", status);
        free(links.mutex_owners);
        free(links.timer_targets);
        free(links.lost);
        free(links.lost_status);
        return status;
    }

    os_clear_last_error();
    os_recalculate_task_counters();
    for (index = 0U, task = g_os.task_list; task != NULL && index < links.task_count; ++index)
    {
        OsTaskHandle next = task->next;
        if (links.lost[index] != NULL)
        {
            os_system_end_lost(task, links.lost[index], links.lost_status[index]);
        }
        task = next;
    }
    free(links.mutex_owners);
    free(links.timer_targets);
    free(links.lost);
    free(links.lost_status);
    os_recompute_task_priorities();
    os_recalculate_task_counters();

    /* The clock goes on from where it was, plus the sleep: what fell due is delivered now. */
    g_os.time_update_initialized = 0U;
    if (slept_ms > 0U)
    {
        os_tick(slept_ms);
    }
    else
    {
        os_clock_rearm();
    }
    return OS_STATUS_OK;
}
