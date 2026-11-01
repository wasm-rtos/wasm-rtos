/*
 * Guest ABI, version 1: the calls a program imports from wasm-rtos.
 *
 * Every call returns an i32: zero or a value on success, a negative WR_E*
 * error otherwise. Strings come as (pointer, length) in UTF-8. Objects are
 * reached through handles that belong to the calling process; an object made
 * by a program goes away with the last handle to it, and every handle closes
 * when its process ends. docs/abi.md and wit/ describe the interfaces.
 */

#include "os_internal.h"

#if defined(d_m3HasWASI) || defined(d_m3HasMetaWASI) || defined(d_m3HasUVWASI)
#include "wasm3/source/extra/wasi_core.h"
#define OS_ABI_HAS_WASI 1
#endif

#include <stdlib.h>
#include <string.h>

/* Handles a process may hold at once. */
#ifndef OS_ABI_MAX_HANDLES
#define OS_ABI_MAX_HANDLES 32U
#endif
/* Longest object name, in bytes. */
#define OS_ABI_NAME_MAX 32U
/* Longest path and argument a program may pass to spawn. */
#define OS_ABI_PATH_MAX 256U
#define OS_ABI_ARG_MAX 256U
#define OS_ABI_ARGS_MAX 32U
/* Event groups keep 24 bits so every value fits a non-negative i32. */
#define OS_ABI_EVENT_BITS 0x00FFFFFFU

/* ABI error codes; the calls return their negatives. Keep in sync with wasm-rtos.h. */
enum
{
    WR_EFAULT = 1,
    WR_EINVAL = 2,
    WR_EBADH = 3,
    WR_ENOMEM = 4,
    WR_ELIMIT = 5,
    WR_ENOTFOUND = 6,
    WR_ETIMEOUT = 7,
    WR_EABORTED = 8,
    WR_EBUSY = 9,
    WR_ENOTOWNER = 10,
    WR_EFULL = 11,
    WR_EEMPTY = 12,
    WR_EUNSUPPORTED = 13,
    WR_ETOOLONG = 14,
    WR_EBADPROGRAM = 15,
    WR_EISDIR = 16,
    WR_ENOTDIR = 17,
    WR_EDEAD = 18,
    WR_EMISMATCH = 19,
    WR_ETRAPPED = 20,
    WR_EKILLED = 21
};

#define WR_SYNC_EVENT_ALL 1U
#define WR_SYNC_EVENT_CLEAR 2U
#define WR_TIMER_PERIODIC 1U

int32_t os_abi_error(OsStatus status)
{
    switch (status)
    {
        case OS_STATUS_OK: return 0;
        case OS_STATUS_OUT_OF_MEMORY: return -WR_ENOMEM;
        case OS_STATUS_WASM_ERROR: return -WR_EBADPROGRAM;
        case OS_STATUS_TASK_DEAD: return -WR_EDEAD;
        case OS_STATUS_TASK_NOT_FOUND:
        case OS_STATUS_QUEUE_NOT_FOUND:
        case OS_STATUS_MUTEX_NOT_FOUND:
        case OS_STATUS_SEMAPHORE_NOT_FOUND:
        case OS_STATUS_EVENT_GROUP_NOT_FOUND:
        case OS_STATUS_TIMER_NOT_FOUND: return -WR_EBADH;
        case OS_STATUS_BUFFER_TOO_SMALL: return -WR_ETOOLONG;
        case OS_STATUS_UNSUPPORTED:
        case OS_STATUS_READ_ONLY: return -WR_EUNSUPPORTED;
        case OS_STATUS_OUT_OF_BOUNDS: return -WR_EFAULT;
        case OS_STATUS_QUEUE_FULL:
        case OS_STATUS_SEMAPHORE_FULL: return -WR_EFULL;
        case OS_STATUS_QUEUE_EMPTY: return -WR_EEMPTY;
        case OS_STATUS_TIMEOUT: return -WR_ETIMEOUT;
        case OS_STATUS_BUSY: return -WR_EBUSY;
        case OS_STATUS_NOT_OWNER: return -WR_ENOTOWNER;
        case OS_STATUS_ABORTED: return -WR_EABORTED;
        case OS_STATUS_LIMIT_REACHED: return -WR_ELIMIT;
        case OS_STATUS_FILE_NOT_FOUND:
        case OS_STATUS_END_OF_DIRECTORY: return -WR_ENOTFOUND;
        case OS_STATUS_NOT_A_DIRECTORY: return -WR_ENOTDIR;
        case OS_STATUS_IS_A_DIRECTORY: return -WR_EISDIR;
        default: return -WR_EINVAL;
    }
}

int32_t os_abi_child_result(OsTaskExitReason reason, uint32_t code)
{
    if (reason == OS_TASK_EXIT_WASM_ERROR)
    {
        return -WR_ETRAPPED;
    }
    if (reason == OS_TASK_EXIT_DELETED)
    {
        return -WR_EKILLED;
    }
    return (int32_t)(code & 0x7FFFFFFFU);
}

/* ---------------------------------------------------------------------------
 * Handles
 * ------------------------------------------------------------------------ */

typedef struct OsAbiHandle
{
    OsObjectType type;
    uint32_t id;
} OsAbiHandle;

typedef struct OsAbiState
{
    OsAbiHandle handles[OS_ABI_MAX_HANDLES];
} OsAbiState;

static OsAbiState* os_abi_state(uint8_t create)
{
    void** slot = os_task_abi_state(os_task_get_current());

    if (slot == NULL)
    {
        return NULL;
    }
    if (*slot == NULL && create)
    {
        *slot = calloc(1U, sizeof(OsAbiState));
    }
    return (OsAbiState*)*slot;
}

static void os_abi_drop(OsAbiHandle* handle)
{
    if (handle->type != OS_OBJECT_TASK)
    {
        os_object_release(handle->type, handle->id);
    }
    handle->type = OS_OBJECT_NONE;
    handle->id = 0U;
}

void os_abi_release_task(OsTaskHandle task)
{
    void** slot = os_task_abi_state(task);
    OsAbiState* state = slot != NULL ? (OsAbiState*)*slot : NULL;
    uint32_t index = 0U;

    if (state == NULL)
    {
        return;
    }
    *slot = NULL;
    for (index = 0U; index < OS_ABI_MAX_HANDLES; ++index)
    {
        if (state->handles[index].type != OS_OBJECT_NONE)
        {
            os_abi_drop(&state->handles[index]);
        }
    }
    free(state);
}

/* A new handle for an object the caller already holds a reference to. */
static int32_t os_abi_handle_new(OsObjectType type, uint32_t id)
{
    OsAbiState* state = os_abi_state(1U);
    uint32_t index = 0U;

    if (state == NULL)
    {
        return -WR_ENOMEM;
    }
    for (index = 0U; index < OS_ABI_MAX_HANDLES; ++index)
    {
        if (state->handles[index].type == OS_OBJECT_NONE)
        {
            state->handles[index].type = type;
            state->handles[index].id = id;
            return (int32_t)(index + 1U);
        }
    }
    return -WR_ELIMIT;
}

static OsAbiHandle* os_abi_handle_get(int32_t handle, OsObjectType type)
{
    OsAbiState* state = os_abi_state(0U);

    if (state == NULL || handle < 1 || (uint32_t)handle > OS_ABI_MAX_HANDLES)
    {
        return NULL;
    }
    if (state->handles[handle - 1].type == OS_OBJECT_NONE ||
        (type != OS_OBJECT_NONE && state->handles[handle - 1].type != type))
    {
        return NULL;
    }
    return &state->handles[handle - 1];
}

/* Give a freshly made object (already adopted) a handle, or delete it. */
static int32_t os_abi_handle_for_new(OsObjectType type, uint32_t id)
{
    int32_t handle = os_abi_handle_new(type, id);
    if (handle < 0)
    {
        os_object_release(type, id);
    }
    return handle;
}

/* ---------------------------------------------------------------------------
 * Guest memory and strings
 * ------------------------------------------------------------------------ */

static uint8_t os_abi_utf8_valid(const uint8_t* text, uint32_t length)
{
    uint32_t index = 0U;

    while (index < length)
    {
        uint8_t byte = text[index];
        uint32_t extra = byte < 0x80U ? 0U : (byte & 0xE0U) == 0xC0U ? 1U
                       : (byte & 0xF0U) == 0xE0U ? 2U : (byte & 0xF8U) == 0xF0U ? 3U : 4U;
        uint32_t next = 0U;

        if (extra == 4U || (extra == 1U && byte < 0xC2U) || length - index <= extra)
        {
            return 0U;
        }
        for (next = 1U; next <= extra; ++next)
        {
            if ((text[index + next] & 0xC0U) != 0x80U)
            {
                return 0U;
            }
        }
        index += extra + 1U;
    }
    return 1U;
}

/* An optional object name: (0, 0) means none. */
static int32_t os_abi_read_name(uint32_t address, uint32_t length, const char** out_name)
{
    uint8_t* memory = NULL;

    *out_name = NULL;
    if (length == 0U)
    {
        return 0;
    }
    if (length > OS_ABI_NAME_MAX)
    {
        return -WR_ETOOLONG;
    }
    if (os_guest_memory(address, length, &memory) != OS_STATUS_OK)
    {
        return -WR_EFAULT;
    }
    if (!os_abi_utf8_valid(memory, length))
    {
        return -WR_EINVAL;
    }
    *out_name = (const char*)memory;
    return 0;
}

static uint32_t os_abi_read_le32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

#define OS_ABI_UNUSED() (void)runtime; (void)_ctx; (void)_mem

/* ---------------------------------------------------------------------------
 * handle
 * ------------------------------------------------------------------------ */

static m3ApiRawFunction(os_abi_handle_close)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_NONE);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    os_abi_drop(entry);
    m3ApiReturn(0);
}

/* ---------------------------------------------------------------------------
 * sync
 * ------------------------------------------------------------------------ */

/* Open an existing named object, or report that there is none (0). */
static int32_t os_abi_open_named(OsObjectType type, const char* name, uint32_t length, uint32_t* out_id)
{
    *out_id = name != NULL ? os_object_find_by_name(type, name, length) : 0U;
    if (*out_id == 0U)
    {
        return 0;
    }
    os_object_retain(type, *out_id);
    return 1;
}

static int32_t os_abi_adopt(OsObjectType type, uint32_t id, const char* name, uint32_t length)
{
    OsStatus status = os_object_adopt(type, id, name, length);
    if (status != OS_STATUS_OK)
    {
        os_object_release(type, id);
        return os_abi_error(status);
    }
    return os_abi_handle_for_new(type, id);
}

static m3ApiRawFunction(os_abi_mutex_create)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, name_address);
    m3ApiGetArg(uint32_t, name_length);
    const char* name = NULL;
    OsMutexHandle mutex = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        m3ApiReturn(result);
    }
    if (os_abi_open_named(OS_OBJECT_MUTEX, name, name_length, &id))
    {
        result = os_abi_handle_new(OS_OBJECT_MUTEX, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_MUTEX, id);
        }
        m3ApiReturn(result);
    }
    status = os_mutex_create(&mutex);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(os_abi_error(status));
    }
    m3ApiReturn(os_abi_adopt(OS_OBJECT_MUTEX, os_mutex_get_id(mutex), name, name_length));
}

static m3ApiRawFunction(os_abi_mutex_lock)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_MUTEX);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    status = os_mutex_lock(os_mutex_find_by_id(entry->id), timeout_ms);
    *raw_return = os_abi_error(status);
    return os_guest_end();
}

static m3ApiRawFunction(os_abi_mutex_unlock)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_MUTEX);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    m3ApiReturn(os_abi_error(os_mutex_unlock(os_mutex_find_by_id(entry->id))));
}

static m3ApiRawFunction(os_abi_semaphore_create)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, name_address);
    m3ApiGetArg(uint32_t, name_length);
    m3ApiGetArg(uint32_t, max_count);
    m3ApiGetArg(uint32_t, initial_count);
    const char* name = NULL;
    OsSemaphoreHandle semaphore = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        m3ApiReturn(result);
    }
    if (max_count == 0U || initial_count > max_count || max_count > 0x7FFFFFFFU)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    if (os_abi_open_named(OS_OBJECT_SEMAPHORE, name, name_length, &id))
    {
        if (os_semaphore_get_max_count(os_semaphore_find_by_id(id)) != max_count)
        {
            os_object_release(OS_OBJECT_SEMAPHORE, id);
            m3ApiReturn(-WR_EMISMATCH);
        }
        result = os_abi_handle_new(OS_OBJECT_SEMAPHORE, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_SEMAPHORE, id);
        }
        m3ApiReturn(result);
    }
    status = os_semaphore_create(&semaphore, max_count, initial_count);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(os_abi_error(status));
    }
    m3ApiReturn(os_abi_adopt(OS_OBJECT_SEMAPHORE, os_semaphore_get_id(semaphore), name, name_length));
}

static m3ApiRawFunction(os_abi_semaphore_take)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_SEMAPHORE);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    status = os_semaphore_take(os_semaphore_find_by_id(entry->id), timeout_ms);
    *raw_return = os_abi_error(status);
    return os_guest_end();
}

static m3ApiRawFunction(os_abi_semaphore_give)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_SEMAPHORE);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    m3ApiReturn(os_abi_error(os_semaphore_give(os_semaphore_find_by_id(entry->id))));
}

static m3ApiRawFunction(os_abi_event_create)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, name_address);
    m3ApiGetArg(uint32_t, name_length);
    const char* name = NULL;
    OsEventGroupHandle event_group = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        m3ApiReturn(result);
    }
    if (os_abi_open_named(OS_OBJECT_EVENT_GROUP, name, name_length, &id))
    {
        result = os_abi_handle_new(OS_OBJECT_EVENT_GROUP, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_EVENT_GROUP, id);
        }
        m3ApiReturn(result);
    }
    status = os_event_group_create(&event_group);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(os_abi_error(status));
    }
    m3ApiReturn(os_abi_adopt(OS_OBJECT_EVENT_GROUP, os_event_group_get_id(event_group), name, name_length));
}

static OsEventGroupHandle os_abi_event(int32_t handle)
{
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_EVENT_GROUP);
    return entry != NULL ? os_event_group_find_by_id(entry->id) : NULL;
}

static m3ApiRawFunction(os_abi_event_set)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, bits);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    if ((bits & ~OS_ABI_EVENT_BITS) != 0U)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    m3ApiReturn((int32_t)os_event_group_set_bits(event_group, bits));
}

static m3ApiRawFunction(os_abi_event_clear)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, bits);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    if ((bits & ~OS_ABI_EVENT_BITS) != 0U)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    m3ApiReturn((int32_t)os_event_group_clear_bits(event_group, bits));
}

static m3ApiRawFunction(os_abi_event_get)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    m3ApiReturn((int32_t)os_event_group_get_bits(event_group));
}

static m3ApiRawFunction(os_abi_event_wait)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, bits);
    m3ApiGetArg(uint32_t, flags);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    if (bits == 0U || (bits & ~OS_ABI_EVENT_BITS) != 0U ||
        (flags & ~(WR_SYNC_EVENT_ALL | WR_SYNC_EVENT_CLEAR)) != 0U)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    status = os_event_group_wait_bits(event_group, bits, (flags & WR_SYNC_EVENT_CLEAR) != 0U,
                                      (flags & WR_SYNC_EVENT_ALL) != 0U, timeout_ms);
    *raw_return = status == OS_STATUS_OK ? (int32_t)os_guest_last_wait_value() : os_abi_error(status);
    return os_guest_end();
}

static m3ApiRawFunction(os_abi_queue_create)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, name_address);
    m3ApiGetArg(uint32_t, name_length);
    m3ApiGetArg(uint32_t, item_size);
    m3ApiGetArg(uint32_t, capacity);
    const char* name = NULL;
    OsQueueHandle queue = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        m3ApiReturn(result);
    }
    if (item_size == 0U || capacity == 0U)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    if (os_abi_open_named(OS_OBJECT_QUEUE, name, name_length, &id))
    {
        queue = os_queue_find_by_id(id);
        if (os_queue_get_space(queue) + os_queue_get_count(queue) != capacity ||
            os_queue_get_item_size(queue) != item_size)
        {
            os_object_release(OS_OBJECT_QUEUE, id);
            m3ApiReturn(-WR_EMISMATCH);
        }
        result = os_abi_handle_new(OS_OBJECT_QUEUE, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_QUEUE, id);
        }
        m3ApiReturn(result);
    }
    status = os_queue_create(&queue, item_size, capacity);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(os_abi_error(status));
    }
    m3ApiReturn(os_abi_adopt(OS_OBJECT_QUEUE, os_queue_get_id(queue), name, name_length));
}

/* Send or receive one item through guest memory at item_address. */
static int32_t os_abi_queue_transfer(int32_t handle, uint32_t item_address, uint32_t timeout_ms,
                                     uint32_t* raw_return, uint8_t receive)
{
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_QUEUE);
    OsQueueHandle queue = entry != NULL ? os_queue_find_by_id(entry->id) : NULL;
    uint8_t* item = NULL;
    OsStatus status = OS_STATUS_OK;

    if (queue == NULL)
    {
        return -WR_EBADH;
    }
    if (os_guest_memory(item_address, os_queue_get_item_size(queue), &item) != OS_STATUS_OK)
    {
        return -WR_EFAULT;
    }
    if (os_guest_begin(raw_return) != OS_STATUS_OK)
    {
        return -WR_EFAULT;
    }
    status = receive ? os_queue_receive_wait(queue, item, timeout_ms)
                     : os_queue_send_wait(queue, item, timeout_ms);
    /* Without a timeout the caller only tries: say why it could not. */
    if (timeout_ms == 0U && status == OS_STATUS_TIMEOUT)
    {
        return receive ? -WR_EEMPTY : -WR_EFULL;
    }
    return os_abi_error(status);
}

static m3ApiRawFunction(os_abi_queue_send)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, item_address);
    m3ApiGetArg(uint32_t, timeout_ms);
    int32_t result = 0;
    OS_ABI_UNUSED();

    result = os_abi_queue_transfer(handle, item_address, timeout_ms, (uint32_t*)raw_return, 0U);
    *raw_return = result;
    return os_guest_end();
}

static m3ApiRawFunction(os_abi_queue_receive)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, item_address);
    m3ApiGetArg(uint32_t, timeout_ms);
    int32_t result = 0;
    OS_ABI_UNUSED();

    result = os_abi_queue_transfer(handle, item_address, timeout_ms, (uint32_t*)raw_return, 1U);
    *raw_return = result;
    return os_guest_end();
}

/* ---------------------------------------------------------------------------
 * timer
 * ------------------------------------------------------------------------ */

static OsTimerHandle os_abi_timer(int32_t handle)
{
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_TIMER);
    return entry != NULL ? os_timer_find_by_id(entry->id) : NULL;
}

static m3ApiRawFunction(os_abi_timer_create)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, period_ms);
    m3ApiGetArg(uint32_t, flags);
    OsTimerHandle timer = NULL;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if ((flags & ~WR_TIMER_PERIODIC) != 0U)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    status = os_timer_create_counting(&timer, period_ms, (flags & WR_TIMER_PERIODIC) != 0U);
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(os_abi_error(status));
    }
    m3ApiReturn(os_abi_adopt(OS_OBJECT_TIMER, os_timer_get_id(timer), NULL, 0U));
}

static m3ApiRawFunction(os_abi_timer_start)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    m3ApiReturn(timer != NULL ? os_abi_error(os_timer_start(timer)) : -WR_EBADH);
}

static m3ApiRawFunction(os_abi_timer_stop)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    m3ApiReturn(timer != NULL ? os_abi_error(os_timer_stop(timer)) : -WR_EBADH);
}

static m3ApiRawFunction(os_abi_timer_set_period)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, period_ms);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    m3ApiReturn(timer != NULL ? os_abi_error(os_timer_change_period(timer, period_ms)) : -WR_EBADH);
}

static m3ApiRawFunction(os_abi_timer_read)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    uint32_t expirations = 0U;
    OS_ABI_UNUSED();

    if (timer == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    expirations = os_timer_take_expirations(timer);
    m3ApiReturn((int32_t)(expirations > 0x7FFFFFFFU ? 0x7FFFFFFFU : expirations));
}

/* ---------------------------------------------------------------------------
 * wait
 * ------------------------------------------------------------------------ */

static m3ApiRawFunction(os_abi_wait_any)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, handles_address);
    m3ApiGetArg(uint32_t, count);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsWaitTarget targets[OS_ABI_MAX_HANDLES];
    uint8_t* handles = NULL;
    uint32_t index = 0U;
    uint32_t ready = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (count == 0U || count > OS_ABI_MAX_HANDLES)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    if (os_guest_memory(handles_address, count * 4U, &handles) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        OsAbiHandle* entry = os_abi_handle_get((int32_t)os_abi_read_le32(handles + index * 4U), OS_OBJECT_NONE);
        if (entry == NULL || entry->type == OS_OBJECT_MUTEX)
        {
            m3ApiReturn(-WR_EBADH);
        }
        targets[index].type = entry->type;
        targets[index].id = entry->id;
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    status = os_wait_any(targets, count, timeout_ms, &ready);
    *raw_return = status == OS_STATUS_OK ? (int32_t)ready : os_abi_error(status);
    return os_guest_end();
}

/* ---------------------------------------------------------------------------
 * proc
 * ------------------------------------------------------------------------ */

#if OS_HAS_PROCESSES
/* Copy argc (pointer, length) string pairs out of guest memory. */
static int32_t os_abi_read_args(uint32_t address, uint32_t argc, char** out_storage, const char** out_args)
{
    uint8_t* pairs = NULL;
    char* storage = NULL;
    uint32_t index = 0U;
    size_t used = 0U;

    *out_storage = NULL;
    if (argc == 0U)
    {
        return 0;
    }
    if (argc > OS_ABI_ARGS_MAX)
    {
        return -WR_ELIMIT;
    }
    if (os_guest_memory(address, argc * 8U, &pairs) != OS_STATUS_OK)
    {
        return -WR_EFAULT;
    }
    storage = (char*)malloc((size_t)argc * (OS_ABI_ARG_MAX + 1U));
    if (storage == NULL)
    {
        return -WR_ENOMEM;
    }
    for (index = 0U; index < argc; ++index)
    {
        uint32_t text_address = os_abi_read_le32(pairs + index * 8U);
        uint32_t length = os_abi_read_le32(pairs + index * 8U + 4U);
        uint8_t* text = NULL;

        if (length > OS_ABI_ARG_MAX)
        {
            free(storage);
            return -WR_ETOOLONG;
        }
        if (os_guest_memory(text_address, length, &text) != OS_STATUS_OK)
        {
            free(storage);
            return -WR_EFAULT;
        }
        memcpy(storage + used, text, length);
        storage[used + length] = '\0';
        out_args[index] = storage + used;
        used += (size_t)length + 1U;
    }
    *out_storage = storage;
    return 0;
}

static int32_t os_abi_child_handle(OsStatus status, OsTaskHandle child)
{
    int32_t handle = 0;

    if (status != OS_STATUS_OK)
    {
        return os_abi_error(status);
    }
    handle = os_abi_handle_new(OS_OBJECT_TASK, os_task_get_id(child));
    if (handle < 0)
    {
        (void)os_child_kill(os_task_get_id(child));
    }
    return handle;
}

static m3ApiRawFunction(os_abi_proc_spawn)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, bytes_address);
    m3ApiGetArg(uint32_t, bytes_length);
    m3ApiGetArg(uint32_t, args_address);
    m3ApiGetArg(uint32_t, argc);
    m3ApiGetArg(uint32_t, priority);
    m3ApiGetArg(uint32_t, stack_size);
    const char* args[OS_ABI_ARGS_MAX];
    char* storage = NULL;
    uint8_t* bytes = NULL;
    OsTaskHandle child = NULL;
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (bytes_length == 0U)
    {
        m3ApiReturn(-WR_EINVAL);
    }
    if (os_guest_memory(bytes_address, bytes_length, &bytes) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    result = os_abi_read_args(args_address, argc, &storage, args);
    if (result < 0)
    {
        m3ApiReturn(result);
    }
    status = os_task_spawn(&child, bytes, bytes_length, argc > 0U ? args : NULL, argc, priority, stack_size);
    free(storage);
    m3ApiReturn(os_abi_child_handle(status, child));
}

#if OS_HAS_FS
static m3ApiRawFunction(os_abi_proc_spawn_path)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, path_address);
    m3ApiGetArg(uint32_t, path_length);
    m3ApiGetArg(uint32_t, args_address);
    m3ApiGetArg(uint32_t, argc);
    m3ApiGetArg(uint32_t, priority);
    m3ApiGetArg(uint32_t, stack_size);
    const char* args[OS_ABI_ARGS_MAX];
    char path[OS_ABI_PATH_MAX + 1U];
    char* storage = NULL;
    uint8_t* text = NULL;
    OsTaskHandle child = NULL;
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (path_length == 0U || path_length > OS_ABI_PATH_MAX)
    {
        m3ApiReturn(path_length == 0U ? -WR_EINVAL : -WR_ETOOLONG);
    }
    if (os_guest_memory(path_address, path_length, &text) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    memcpy(path, text, path_length);
    path[path_length] = '\0';
    result = os_abi_read_args(args_address, argc, &storage, args);
    if (result < 0)
    {
        m3ApiReturn(result);
    }
    status = os_task_spawn_path(&child, path, argc > 0U ? args : NULL, argc, priority, stack_size);
    free(storage);
    m3ApiReturn(os_abi_child_handle(status, child));
}
#endif

static m3ApiRawFunction(os_abi_proc_wait)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    m3ApiGetArg(uint32_t, timeout_ms);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_TASK);
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (entry == NULL || !os_child_is_mine(entry->id))
    {
        m3ApiReturn(-WR_EBADH);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        m3ApiReturn(-WR_EFAULT);
    }
    status = os_child_wait(entry->id, timeout_ms, &result);
    *raw_return = status == OS_STATUS_OK ? result : os_abi_error(status);
    return os_guest_end();
}

static m3ApiRawFunction(os_abi_proc_kill)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_TASK);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        m3ApiReturn(-WR_EBADH);
    }
    m3ApiReturn(os_abi_error(os_child_kill(entry->id)));
}

#endif

static m3ApiRawFunction(os_abi_proc_self)
{
    m3ApiReturnType(int32_t);
    (void)_sp;
    OS_ABI_UNUSED();
    m3ApiReturn((int32_t)os_task_get_id(os_task_get_current()));
}

static m3ApiRawFunction(os_abi_proc_parent)
{
    m3ApiReturnType(int32_t);
    (void)_sp;
    OS_ABI_UNUSED();
#if OS_HAS_PROCESSES
    m3ApiReturn((int32_t)os_current_parent_id());
#else
    m3ApiReturn(0);
#endif
}

#if !OS_HAS_PROCESSES || !OS_HAS_FS
/* Calls this build leaves out; the result slot is first whatever the parameters. */
static m3ApiRawFunction(os_abi_unsupported)
{
    m3ApiReturnType(int32_t);
    OS_ABI_UNUSED();
    m3ApiReturn(-WR_EUNSUPPORTED);
}
#endif

/* ---------------------------------------------------------------------------
 * WASI calls the kernel answers itself: time is kernel time, and sleeping or
 * yielding blocks only the calling task.
 * ------------------------------------------------------------------------ */

#if defined(OS_ABI_HAS_WASI)
static void os_abi_write_le64(uint8_t* out, uint64_t value)
{
    uint32_t index = 0U;
    for (index = 0U; index < 8U; ++index)
    {
        out[index] = (uint8_t)(value >> (8U * index));
    }
}

static uint64_t os_abi_read_le64(const uint8_t* bytes)
{
    return (uint64_t)os_abi_read_le32(bytes) | ((uint64_t)os_abi_read_le32(bytes + 4U) << 32U);
}

static m3ApiRawFunction(os_abi_wasi_clock_res_get)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, clock_id);
    m3ApiGetArg(uint32_t, resolution_address);
    uint8_t* out = NULL;
    OS_ABI_UNUSED();

    if (clock_id > __WASI_CLOCKID_THREAD_CPUTIME_ID)
    {
        m3ApiReturn(__WASI_ERRNO_INVAL);
    }
    if (os_guest_memory(resolution_address, 8U, &out) != OS_STATUS_OK)
    {
        m3ApiReturn(__WASI_ERRNO_FAULT);
    }
    os_abi_write_le64(out, 1000000U);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_abi_wasi_clock_time_get)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, clock_id);
    m3ApiGetArg(uint64_t, precision);
    m3ApiGetArg(uint32_t, time_address);
    uint8_t* out = NULL;
    (void)precision;
    OS_ABI_UNUSED();

    /* Every clock reads kernel time until a platform supplies wall time. */
    if (clock_id > __WASI_CLOCKID_THREAD_CPUTIME_ID)
    {
        m3ApiReturn(__WASI_ERRNO_INVAL);
    }
    if (os_guest_memory(time_address, 8U, &out) != OS_STATUS_OK)
    {
        m3ApiReturn(__WASI_ERRNO_FAULT);
    }
    os_abi_write_le64(out, os_time_ms64() * 1000000U);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_abi_wasi_sched_yield)
{
    m3ApiReturnType(uint32_t);
    (void)_sp;
    OS_ABI_UNUSED();

    *raw_return = __WASI_ERRNO_SUCCESS;
    if (os_task_yield() != OS_STATUS_OK)
    {
        return m3Err_none;
    }
    return m3_Yield();
}

/*
 * poll_oneoff: file subscriptions are always ready (files and stdio never
 * block); otherwise the task sleeps until the earliest clock subscription.
 */
static m3ApiRawFunction(os_abi_wasi_poll_oneoff)
{
    m3ApiReturnType(uint32_t);
    m3ApiGetArg(uint32_t, in_address);
    m3ApiGetArg(uint32_t, out_address);
    m3ApiGetArg(uint32_t, count);
    m3ApiGetArg(uint32_t, events_address);
    uint8_t* in = NULL;
    uint8_t* out = NULL;
    uint8_t* events = NULL;
    uint64_t now_ns = os_time_ms64() * 1000000U;
    uint64_t soonest_ns = UINT64_MAX;
    uint32_t soonest = 0U;
    uint32_t written = 0U;
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    if (count == 0U)
    {
        m3ApiReturn(__WASI_ERRNO_INVAL);
    }
    if (count > 0xFFFFFFFFU / 48U ||
        os_guest_memory(in_address, count * 48U, &in) != OS_STATUS_OK ||
        os_guest_memory(out_address, count * 32U, &out) != OS_STATUS_OK ||
        os_guest_memory(events_address, 4U, &events) != OS_STATUS_OK)
    {
        m3ApiReturn(__WASI_ERRNO_FAULT);
    }

    for (index = 0U; index < count; ++index)
    {
        const uint8_t* subscription = in + index * 48U;
        uint8_t tag = subscription[8];

        if (tag == __WASI_EVENTTYPE_CLOCK)
        {
            uint64_t timeout = os_abi_read_le64(subscription + 24U);
            uint16_t flags = (uint16_t)(subscription[40] | (subscription[41] << 8U));
            uint64_t due = (flags & __WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME) != 0U
                ? timeout : now_ns + timeout;
            if (due < soonest_ns)
            {
                soonest_ns = due;
                soonest = index;
            }
        }
        else
        {
            uint8_t* event = out + written * 32U;
            memset(event, 0, 32U);
            memcpy(event, subscription, 8U);
            event[10] = tag;
            ++written;
        }
    }

    if (written == 0U)
    {
        uint8_t* event = out;
        uint64_t wait_ns = soonest_ns > now_ns ? soonest_ns - now_ns : 0U;
        uint64_t wait_ms = (wait_ns + 999999U) / 1000000U;

        memset(event, 0, 32U);
        memcpy(event, in + soonest * 48U, 8U);
        event[10] = __WASI_EVENTTYPE_CLOCK;
        written = 1U;
        events[0] = 1U;
        events[1] = events[2] = events[3] = 0U;
        *raw_return = __WASI_ERRNO_SUCCESS;
        if (wait_ms > 0U && os_task_delay_ms(wait_ms > 0x7FFFFFFFU ? 0x7FFFFFFFU : (uint32_t)wait_ms) == OS_STATUS_OK)
        {
            return m3_Yield();
        }
        return m3Err_none;
    }

    events[0] = (uint8_t)written;
    events[1] = (uint8_t)(written >> 8U);
    events[2] = (uint8_t)(written >> 16U);
    events[3] = (uint8_t)(written >> 24U);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}
#endif

/* ---------------------------------------------------------------------------
 * Linking
 * ------------------------------------------------------------------------ */

typedef struct OsAbiImport
{
    const char* module;
    const char* name;
    const char* signature;
    M3RawCall function;
} OsAbiImport;

#define WR_MODULE(interface) "wasm-rtos:kernel/" interface "@1.0.0"

static const OsAbiImport g_abi_imports[] =
{
    { WR_MODULE("handle"), "close", "i(i)", os_abi_handle_close },

    { WR_MODULE("sync"), "mutex-create", "i(ii)", os_abi_mutex_create },
    { WR_MODULE("sync"), "mutex-lock", "i(ii)", os_abi_mutex_lock },
    { WR_MODULE("sync"), "mutex-unlock", "i(i)", os_abi_mutex_unlock },
    { WR_MODULE("sync"), "semaphore-create", "i(iiii)", os_abi_semaphore_create },
    { WR_MODULE("sync"), "semaphore-take", "i(ii)", os_abi_semaphore_take },
    { WR_MODULE("sync"), "semaphore-give", "i(i)", os_abi_semaphore_give },
    { WR_MODULE("sync"), "event-create", "i(ii)", os_abi_event_create },
    { WR_MODULE("sync"), "event-set", "i(ii)", os_abi_event_set },
    { WR_MODULE("sync"), "event-clear", "i(ii)", os_abi_event_clear },
    { WR_MODULE("sync"), "event-get", "i(i)", os_abi_event_get },
    { WR_MODULE("sync"), "event-wait", "i(iiii)", os_abi_event_wait },
    { WR_MODULE("sync"), "queue-create", "i(iiii)", os_abi_queue_create },
    { WR_MODULE("sync"), "queue-send", "i(iii)", os_abi_queue_send },
    { WR_MODULE("sync"), "queue-receive", "i(iii)", os_abi_queue_receive },

    { WR_MODULE("timer"), "create", "i(ii)", os_abi_timer_create },
    { WR_MODULE("timer"), "start", "i(i)", os_abi_timer_start },
    { WR_MODULE("timer"), "stop", "i(i)", os_abi_timer_stop },
    { WR_MODULE("timer"), "set-period", "i(ii)", os_abi_timer_set_period },
    { WR_MODULE("timer"), "read", "i(i)", os_abi_timer_read },

    { WR_MODULE("wait"), "any", "i(iii)", os_abi_wait_any },

    /* Every call is linked; what the build leaves out returns -WR_EUNSUPPORTED. */
#if OS_HAS_PROCESSES
    { WR_MODULE("proc"), "spawn", "i(iiiiii)", os_abi_proc_spawn },
#if OS_HAS_FS
    { WR_MODULE("proc"), "spawn-path", "i(iiiiii)", os_abi_proc_spawn_path },
#else
    { WR_MODULE("proc"), "spawn-path", "i(iiiiii)", os_abi_unsupported },
#endif
    { WR_MODULE("proc"), "wait", "i(ii)", os_abi_proc_wait },
    { WR_MODULE("proc"), "kill", "i(i)", os_abi_proc_kill },
#else
    { WR_MODULE("proc"), "spawn", "i(iiiiii)", os_abi_unsupported },
    { WR_MODULE("proc"), "spawn-path", "i(iiiiii)", os_abi_unsupported },
    { WR_MODULE("proc"), "wait", "i(ii)", os_abi_unsupported },
    { WR_MODULE("proc"), "kill", "i(i)", os_abi_unsupported },
#endif
    { WR_MODULE("proc"), "self", "i()", os_abi_proc_self },
    { WR_MODULE("proc"), "parent", "i()", os_abi_proc_parent },

#if defined(OS_ABI_HAS_WASI)
    { "wasi_snapshot_preview1", "clock_res_get", "i(ii)", os_abi_wasi_clock_res_get },
    { "wasi_snapshot_preview1", "clock_time_get", "i(iIi)", os_abi_wasi_clock_time_get },
    { "wasi_snapshot_preview1", "sched_yield", "i()", os_abi_wasi_sched_yield },
    { "wasi_snapshot_preview1", "poll_oneoff", "i(iiii)", os_abi_wasi_poll_oneoff },
    { "wasi_unstable", "clock_res_get", "i(ii)", os_abi_wasi_clock_res_get },
    { "wasi_unstable", "clock_time_get", "i(iIi)", os_abi_wasi_clock_time_get },
    { "wasi_unstable", "sched_yield", "i()", os_abi_wasi_sched_yield },
    { "wasi_unstable", "poll_oneoff", "i(iiii)", os_abi_wasi_poll_oneoff },
#endif
};

/* Bind every ABI import the module uses; call after m3_LinkWASI to override it. */
M3Result os_abi_link(IM3Module module)
{
    uint32_t index = 0U;

    for (index = 0U; index < (uint32_t)(sizeof(g_abi_imports) / sizeof(g_abi_imports[0])); ++index)
    {
        M3Result result = m3_LinkRawFunction(module, g_abi_imports[index].module, g_abi_imports[index].name,
                                             g_abi_imports[index].signature, g_abi_imports[index].function);
        if (result != m3Err_none && result != m3Err_functionLookupFailed)
        {
            return result;
        }
    }
    return m3Err_none;
}
