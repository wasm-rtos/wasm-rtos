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
#include "fs.h"

#if OS_HAS_WASI
#include "wasi.h"
#define OS_ABI_HAS_WASI 1
#endif

#include <stdlib.h>
#include <string.h>

/* Handles a process may hold at once. */
#ifndef OS_ABI_MAX_HANDLES
#define OS_ABI_MAX_HANDLES 32U
#endif
/* Longest path and argument a program may pass to spawn. */
#define OS_ABI_PATH_MAX 256U
#define OS_ABI_ARG_MAX 256U
#define OS_ABI_ARGS_MAX 32U
/* Event groups keep 24 bits so every value fits a non-negative i32. */
#define OS_ABI_EVENT_BITS 0x00FFFFFFU

#define WR_SYNC_EVENT_ALL 1U
#define WR_SYNC_EVENT_CLEAR 2U
#define WR_TIMER_PERIODIC 1U


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
    /* For OS_OBJECT_DRIVER. */
    const HwDriver* driver;
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
    if (handle->type == OS_OBJECT_DRIVER)
    {
        if (handle->driver->close != NULL)
        {
            handle->driver->close(handle->id);
        }
    }
    else
    if (handle->type != OS_OBJECT_TASK)
    {
        os_object_release(handle->type, handle->id);
    }
    handle->type = OS_OBJECT_NONE;
    handle->id = 0U;
    handle->driver = NULL;
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

const char* os_abi_task_device(OsTaskHandle task)
{
    void** slot = os_task_abi_state(task);
    OsAbiState* state = slot != NULL ? (OsAbiState*)*slot : NULL;
    uint32_t index = 0U;

    for (index = 0U; state != NULL && index < OS_ABI_MAX_HANDLES; ++index)
    {
        if (state->handles[index].type == OS_OBJECT_DRIVER && state->handles[index].driver->save == NULL)
        {
            return state->handles[index].driver->interface;
        }
    }
    return NULL;
}

/* The handles in use: number, type and object id of each; a device's also its driver's interface. */
void os_abi_save_task(OsTaskHandle task, OsSystemStream* stream)
{
    void** slot = os_task_abi_state(task);
    OsAbiState* state = slot != NULL ? (OsAbiState*)*slot : NULL;
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; state != NULL && index < OS_ABI_MAX_HANDLES; ++index)
    {
        count += state->handles[index].type != OS_OBJECT_NONE ? 1U : 0U;
    }
    os_system_put32(stream, count);
    for (index = 0U; count > 0U && index < OS_ABI_MAX_HANDLES; ++index)
    {
        const OsAbiHandle* handle = &state->handles[index];
        if (handle->type != OS_OBJECT_NONE)
        {
            os_system_put32(stream, index);
            os_system_put32(stream, (uint32_t)handle->type);
            os_system_put32(stream, handle->id);
            if (handle->type == OS_OBJECT_DRIVER)
            {
                os_system_put_text(stream, handle->driver->interface);
            }
        }
    }
}

const char* os_abi_restore_task(OsTaskHandle task, OsSystemStream* stream)
{
    uint32_t count = os_system_get32(stream);
    OsAbiState* state = NULL;
    const char* lost = NULL;
    uint32_t read = 0U;

    if (!os_system_ok(stream) || count == 0U)
    {
        return NULL;
    }
    if (count > OS_ABI_MAX_HANDLES)
    {
        os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
        return NULL;
    }
    state = (OsAbiState*)calloc(1U, sizeof(OsAbiState));
    if (state == NULL)
    {
        os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
        return NULL;
    }
    *os_task_abi_state(task) = state;
    for (read = 0U; read < count && os_system_ok(stream); ++read)
    {
        uint32_t index = os_system_get32(stream);
        uint32_t type = os_system_get32(stream);
        uint32_t id = os_system_get32(stream);
        const HwDriver* driver = NULL;

        if (index >= OS_ABI_MAX_HANDLES || state->handles[index].type != OS_OBJECT_NONE ||
            type < (uint32_t)OS_OBJECT_QUEUE || type > (uint32_t)OS_OBJECT_DRIVER || id == 0U)
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
            return NULL;
        }
        if (type == (uint32_t)OS_OBJECT_DRIVER)
        {
            /* The device as its driver restored it, under the id it has now. */
            char* interface = os_system_get_text(stream);
            const char* gone = NULL;
            driver = interface != NULL ? hw_driver_find(interface) : NULL;
            free(interface);
            id = os_system_ok(stream) ? os_system_device_id(stream, driver, id, &gone) : 0U;
            if (id == 0U)
            {
                lost = lost != NULL ? lost : gone;
                continue;
            }
        }
        state->handles[index].type = (OsObjectType)type;
        state->handles[index].id = id;
        state->handles[index].driver = driver;
    }
    return lost;
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

int32_t hw_driver_handle_new(const HwDriver* driver, uint32_t id)
{
    int32_t handle = os_abi_handle_new(OS_OBJECT_DRIVER, id);
    if (handle > 0)
    {
        os_abi_state(0U)->handles[handle - 1].driver = driver;
    }
    return handle;
}

int hw_driver_handle_get(const HwDriver* driver, int32_t handle, uint32_t* out_id)
{
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_DRIVER);
    if (entry == NULL || entry->driver != driver)
    {
        return 0;
    }
    *out_id = entry->id;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Drivers
 * ------------------------------------------------------------------------ */














/* ---------------------------------------------------------------------------
 * Guest memory and strings
 * ------------------------------------------------------------------------ */



/* ---------------------------------------------------------------------------
 * handle
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_abi_handle_close)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_NONE);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    os_abi_drop(entry);
    OS_HOST_RETURN(0);
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
        return hw_abi_error(status);
    }
    return os_abi_handle_for_new(type, id);
}

static OS_HOST_FUNCTION(os_abi_mutex_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    const char* name = NULL;
    char name_storage[HW_ABI_NAME_MAX];
    OsMutexHandle mutex = NULL;
    uint32_t id = 0U;
    int32_t result = hw_abi_read_name(name_address, name_length, name_storage, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    if (os_abi_open_named(OS_OBJECT_MUTEX, name, name_length, &id))
    {
        result = os_abi_handle_new(OS_OBJECT_MUTEX, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_MUTEX, id);
        }
        OS_HOST_RETURN(result);
    }
    status = os_mutex_create(&mutex);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(hw_abi_error(status));
    }
    OS_HOST_RETURN(os_abi_adopt(OS_OBJECT_MUTEX, os_mutex_get_id(mutex), name, name_length));
}

static OS_HOST_FUNCTION(os_abi_mutex_lock)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, timeout_ms);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_MUTEX);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_mutex_lock(os_mutex_find_by_id(entry->id), timeout_ms);
    *raw_return = hw_abi_error(status);
    return os_guest_end();
}

static OS_HOST_FUNCTION(os_abi_mutex_unlock)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_MUTEX);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(hw_abi_error(os_mutex_unlock(os_mutex_find_by_id(entry->id))));
}

static OS_HOST_FUNCTION(os_abi_semaphore_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, max_count);
    OS_HOST_ARG(uint32_t, initial_count);
    const char* name = NULL;
    char name_storage[HW_ABI_NAME_MAX];
    OsSemaphoreHandle semaphore = NULL;
    uint32_t id = 0U;
    int32_t result = hw_abi_read_name(name_address, name_length, name_storage, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    if (max_count == 0U || initial_count > max_count || max_count > 0x7FFFFFFFU)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_abi_open_named(OS_OBJECT_SEMAPHORE, name, name_length, &id))
    {
        if (os_semaphore_get_max_count(os_semaphore_find_by_id(id)) != max_count)
        {
            os_object_release(OS_OBJECT_SEMAPHORE, id);
            OS_HOST_RETURN(-WR_EMISMATCH);
        }
        result = os_abi_handle_new(OS_OBJECT_SEMAPHORE, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_SEMAPHORE, id);
        }
        OS_HOST_RETURN(result);
    }
    status = os_semaphore_create(&semaphore, max_count, initial_count);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(hw_abi_error(status));
    }
    OS_HOST_RETURN(os_abi_adopt(OS_OBJECT_SEMAPHORE, os_semaphore_get_id(semaphore), name, name_length));
}

static OS_HOST_FUNCTION(os_abi_semaphore_take)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, timeout_ms);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_SEMAPHORE);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_semaphore_take(os_semaphore_find_by_id(entry->id), timeout_ms);
    *raw_return = hw_abi_error(status);
    return os_guest_end();
}

static OS_HOST_FUNCTION(os_abi_semaphore_give)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_SEMAPHORE);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(hw_abi_error(os_semaphore_give(os_semaphore_find_by_id(entry->id))));
}

static OS_HOST_FUNCTION(os_abi_event_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    const char* name = NULL;
    char name_storage[HW_ABI_NAME_MAX];
    OsEventGroupHandle event_group = NULL;
    uint32_t id = 0U;
    int32_t result = hw_abi_read_name(name_address, name_length, name_storage, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    if (os_abi_open_named(OS_OBJECT_EVENT_GROUP, name, name_length, &id))
    {
        result = os_abi_handle_new(OS_OBJECT_EVENT_GROUP, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_EVENT_GROUP, id);
        }
        OS_HOST_RETURN(result);
    }
    status = os_event_group_create(&event_group);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(hw_abi_error(status));
    }
    OS_HOST_RETURN(os_abi_adopt(OS_OBJECT_EVENT_GROUP, os_event_group_get_id(event_group), name, name_length));
}

static OsEventGroupHandle os_abi_event(int32_t handle)
{
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_EVENT_GROUP);
    return entry != NULL ? os_event_group_find_by_id(entry->id) : NULL;
}

static OS_HOST_FUNCTION(os_abi_event_set)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, bits);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if ((bits & ~OS_ABI_EVENT_BITS) != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    OS_HOST_RETURN((int32_t)os_event_group_set_bits(event_group, bits));
}

static OS_HOST_FUNCTION(os_abi_event_clear)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, bits);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if ((bits & ~OS_ABI_EVENT_BITS) != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    OS_HOST_RETURN((int32_t)os_event_group_clear_bits(event_group, bits));
}

static OS_HOST_FUNCTION(os_abi_event_get)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN((int32_t)os_event_group_get_bits(event_group));
}

static OS_HOST_FUNCTION(os_abi_event_wait)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, bits);
    OS_HOST_ARG(uint32_t, flags);
    OS_HOST_ARG(uint32_t, timeout_ms);
    OsEventGroupHandle event_group = os_abi_event(handle);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (event_group == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (bits == 0U || (bits & ~OS_ABI_EVENT_BITS) != 0U ||
        (flags & ~(WR_SYNC_EVENT_ALL | WR_SYNC_EVENT_CLEAR)) != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_event_group_wait_bits(event_group, bits, (flags & WR_SYNC_EVENT_CLEAR) != 0U,
                                      (flags & WR_SYNC_EVENT_ALL) != 0U, timeout_ms);
    *raw_return = status == OS_STATUS_OK ? (int32_t)os_guest_last_wait_value() : hw_abi_error(status);
    return os_guest_end();
}

static OS_HOST_FUNCTION(os_abi_queue_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, item_size);
    OS_HOST_ARG(uint32_t, capacity);
    const char* name = NULL;
    char name_storage[HW_ABI_NAME_MAX];
    OsQueueHandle queue = NULL;
    uint32_t id = 0U;
    int32_t result = hw_abi_read_name(name_address, name_length, name_storage, &name);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    if (item_size == 0U || capacity == 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_abi_open_named(OS_OBJECT_QUEUE, name, name_length, &id))
    {
        queue = os_queue_find_by_id(id);
        if (os_queue_get_space(queue) + os_queue_get_count(queue) != capacity ||
            os_queue_get_item_size(queue) != item_size)
        {
            os_object_release(OS_OBJECT_QUEUE, id);
            OS_HOST_RETURN(-WR_EMISMATCH);
        }
        result = os_abi_handle_new(OS_OBJECT_QUEUE, id);
        if (result < 0)
        {
            os_object_release(OS_OBJECT_QUEUE, id);
        }
        OS_HOST_RETURN(result);
    }
    status = os_queue_create(&queue, item_size, capacity);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(hw_abi_error(status));
    }
    OS_HOST_RETURN(os_abi_adopt(OS_OBJECT_QUEUE, os_queue_get_id(queue), name, name_length));
}

/* Send or receive one item through guest memory at item_address. */
static int32_t os_abi_queue_transfer(int32_t handle, uint32_t item_address, uint32_t timeout_ms,
                                     uint32_t* raw_return, uint8_t receive)
{
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_QUEUE);
    OsQueueHandle queue = entry != NULL ? os_queue_find_by_id(entry->id) : NULL;
    OsGuestBuffer item;
    OsStatus status = OS_STATUS_OK;

    if (queue == NULL)
    {
        return -WR_EBADH;
    }
    /* A sent item is read now; a received one is written where it arrives. */
    if (receive)
    {
        status = os_guest_check(item_address, os_queue_get_item_size(queue));
        memset(&item, 0, sizeof(item));
    }
    else
    {
        status = os_guest_buffer_open(&item, item_address, os_queue_get_item_size(queue), OS_GUEST_IN);
    }
    if (status != OS_STATUS_OK)
    {
        return HW_GUEST_BUFFER_ERROR(status);
    }
    if (os_guest_begin(raw_return) != OS_STATUS_OK)
    {
        os_guest_buffer_drop(&item);
        return -WR_EFAULT;
    }
    if (receive)
    {
        status = os_queue_receive_wait_guest(queue, item_address, timeout_ms);
    }
    else
    {
        status = os_queue_send_wait(queue, item.data, timeout_ms);
        os_guest_buffer_drop(&item);
    }
    /* Without a timeout the caller only tries: say why it could not. */
    if (timeout_ms == 0U && status == OS_STATUS_TIMEOUT)
    {
        return receive ? -WR_EEMPTY : -WR_EFULL;
    }
    return hw_abi_error(status);
}

static OS_HOST_FUNCTION(os_abi_queue_send)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, item_address);
    OS_HOST_ARG(uint32_t, timeout_ms);
    int32_t result = 0;
    OS_ABI_UNUSED();

    result = os_abi_queue_transfer(handle, item_address, timeout_ms, (uint32_t*)raw_return, 0U);
    *raw_return = result;
    return os_guest_end();
}

static OS_HOST_FUNCTION(os_abi_queue_receive)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, item_address);
    OS_HOST_ARG(uint32_t, timeout_ms);
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

static OS_HOST_FUNCTION(os_abi_timer_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, period_ms);
    OS_HOST_ARG(uint32_t, flags);
    OsTimerHandle timer = NULL;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if ((flags & ~WR_TIMER_PERIODIC) != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    status = os_timer_create_counting(&timer, period_ms, (flags & WR_TIMER_PERIODIC) != 0U);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(hw_abi_error(status));
    }
    OS_HOST_RETURN(os_abi_adopt(OS_OBJECT_TIMER, os_timer_get_id(timer), NULL, 0U));
}

static OS_HOST_FUNCTION(os_abi_timer_start)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    OS_HOST_RETURN(timer != NULL ? hw_abi_error(os_timer_start(timer)) : -WR_EBADH);
}

static OS_HOST_FUNCTION(os_abi_timer_stop)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    OS_HOST_RETURN(timer != NULL ? hw_abi_error(os_timer_stop(timer)) : -WR_EBADH);
}

static OS_HOST_FUNCTION(os_abi_timer_set_period)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, period_ms);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    OS_HOST_RETURN(timer != NULL ? hw_abi_error(os_timer_change_period(timer, period_ms)) : -WR_EBADH);
}

static OS_HOST_FUNCTION(os_abi_timer_read)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    uint32_t expirations = 0U;
    OS_ABI_UNUSED();

    if (timer == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    expirations = os_timer_take_expirations(timer);
    OS_HOST_RETURN((int32_t)(expirations > 0x7FFFFFFFU ? 0x7FFFFFFFU : expirations));
}

/* ---------------------------------------------------------------------------
 * wait
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_abi_wait_any)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, handles_address);
    OS_HOST_ARG(uint32_t, count);
    OS_HOST_ARG(uint32_t, timeout_ms);
    OsWaitTarget targets[OS_ABI_MAX_HANDLES];
    uint32_t handles[OS_ABI_MAX_HANDLES];
    uint32_t index = 0U;
    uint32_t ready = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (count == 0U || count > OS_ABI_MAX_HANDLES)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_guest_read(handles_address, handles, count * 4U) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        OsAbiHandle* entry = os_abi_handle_get((int32_t)handles[index], OS_OBJECT_NONE);
        if (entry == NULL || entry->type == OS_OBJECT_MUTEX ||
            (entry->type == OS_OBJECT_DRIVER && entry->driver->ready == NULL)     )
        {
            OS_HOST_RETURN(-WR_EBADH);
        }
        targets[index].type = entry->type;
        targets[index].id = entry->id;
        targets[index].driver = entry->driver;
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_wait_any(targets, count, timeout_ms, &ready);
    *raw_return = status == OS_STATUS_OK ? (int32_t)ready : hw_abi_error(status);
    return os_guest_end();
}

/* ---------------------------------------------------------------------------
 * proc
 * ------------------------------------------------------------------------ */

#if OS_HAS_PROCESSES
/* Copy argc (pointer, length) string pairs out of guest memory. */
static int32_t os_abi_read_args(uint32_t address, uint32_t argc, char** out_storage, const char** out_args)
{
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
    if (os_guest_check(address, argc * 8U) != OS_STATUS_OK)
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
        uint8_t pair[8];
        uint32_t text_address = 0U;
        uint32_t length = 0U;

        (void)os_guest_read(address + index * 8U, pair, sizeof(pair));
        text_address = OS_HOST_READ32(pair);
        length = OS_HOST_READ32(pair + 4U);
        if (length > OS_ABI_ARG_MAX)
        {
            free(storage);
            return -WR_ETOOLONG;
        }
        if (os_guest_read(text_address, storage + used, length) != OS_STATUS_OK)
        {
            free(storage);
            return -WR_EFAULT;
        }
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
        return hw_abi_error(status);
    }
    handle = os_abi_handle_new(OS_OBJECT_TASK, os_task_get_id(child));
    if (handle < 0)
    {
        (void)os_child_kill(os_task_get_id(child));
    }
    return handle;
}

/* A program may not start a child above its own base priority; 0 still inherits it. */
static uint32_t os_abi_child_priority(uint32_t priority)
{
    uint32_t own = os_task_get_base_priority(os_task_get_current());

    return priority > own ? own : priority;
}

static OS_HOST_FUNCTION(os_abi_proc_spawn)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, bytes_address);
    OS_HOST_ARG(uint32_t, bytes_length);
    OS_HOST_ARG(uint32_t, args_address);
    OS_HOST_ARG(uint32_t, argc);
    OS_HOST_ARG(uint32_t, priority);
    OS_HOST_ARG(uint32_t, stack_size);
    const char* args[OS_ABI_ARGS_MAX];
    char* storage = NULL;
    OsGuestBuffer bytes;
    OsTaskHandle child = NULL;
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (bytes_length == 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    result = os_abi_read_args(args_address, argc, &storage, args);
    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    /* The kernel copies the image during the spawn. */
    status = os_guest_buffer_open(&bytes, bytes_address, bytes_length, OS_GUEST_IN);
    if (status == OS_STATUS_OK)
    {
        status = os_task_spawn(&child, bytes.data, bytes_length, argc > 0U ? args : NULL, argc,
                               os_abi_child_priority(priority), stack_size);
        os_guest_buffer_drop(&bytes);
    }
    else if (status != OS_STATUS_OUT_OF_MEMORY)
    {
        status = OS_STATUS_OUT_OF_BOUNDS;
    }
    free(storage);
    OS_HOST_RETURN(os_abi_child_handle(status, child));
}

#if OS_HAS_FS
static OS_HOST_FUNCTION(os_abi_proc_spawn_path)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, path_address);
    OS_HOST_ARG(uint32_t, path_length);
    OS_HOST_ARG(uint32_t, args_address);
    OS_HOST_ARG(uint32_t, argc);
    OS_HOST_ARG(uint32_t, priority);
    OS_HOST_ARG(uint32_t, stack_size);
    const char* args[OS_ABI_ARGS_MAX];
    char path[OS_ABI_PATH_MAX + 1U];
    char* storage = NULL;
    OsTaskHandle child = NULL;
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (path_length == 0U || path_length > OS_ABI_PATH_MAX)
    {
        OS_HOST_RETURN(path_length == 0U ? -WR_EINVAL : -WR_ETOOLONG);
    }
    if (os_guest_read(path_address, path, path_length) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    path[path_length] = '\0';
    result = os_abi_read_args(args_address, argc, &storage, args);
    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    status = os_task_spawn_path(&child, path, argc > 0U ? args : NULL, argc, os_abi_child_priority(priority),
                                stack_size);
    free(storage);
    OS_HOST_RETURN(os_abi_child_handle(status, child));
}
#endif

static OS_HOST_FUNCTION(os_abi_proc_wait)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, timeout_ms);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_TASK);
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (entry == NULL || !os_child_is_mine(entry->id))
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_child_wait(entry->id, timeout_ms, &result);
    *raw_return = status == OS_STATUS_OK ? result : hw_abi_error(status);
    return os_guest_end();
}

static OS_HOST_FUNCTION(os_abi_proc_kill)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsAbiHandle* entry = os_abi_handle_get(handle, OS_OBJECT_TASK);
    OS_ABI_UNUSED();

    if (entry == NULL)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(hw_abi_error(os_child_kill(entry->id)));
}

#endif

static OS_HOST_FUNCTION(os_abi_proc_self)
{
    OS_HOST_RETURNS(int32_t);
    (void)os_host_slot;
    OS_ABI_UNUSED();
    OS_HOST_RETURN((int32_t)os_task_get_id(os_task_get_current()));
}

static OS_HOST_FUNCTION(os_abi_proc_parent)
{
    OS_HOST_RETURNS(int32_t);
    (void)os_host_slot;
    OS_ABI_UNUSED();
#if OS_HAS_PROCESSES
    OS_HOST_RETURN((int32_t)os_current_parent_id());
#else
    OS_HOST_RETURN(0);
#endif
}

#if !OS_HAS_PROCESSES || !OS_HAS_FS
/* Calls this build leaves out; the result slot is first whatever the parameters. */
static OS_HOST_FUNCTION(os_abi_unsupported)
{
    OS_HOST_RETURNS(int32_t);
    OS_ABI_UNUSED();
    OS_HOST_RETURN(-WR_EUNSUPPORTED);
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
    OsHostImportFunction function;
    /* ABI minor version that added the call; kernel interfaces only. */
    uint8_t since;
} OsAbiImport;

/*
 * Kernel interfaces are imported as "wasm-rtos:kernel/<interface>@1.<minor>.0".
 * A program built for 1.N imports 1.N names; this kernel links every minor
 * from the one that added a call, and any newer one: a call added after
 * OS_ABI_VERSION_MINOR reports itself unsupported (docs/decisions.md, 8).
 */
#define WR_KERNEL_PREFIX "wasm-rtos:kernel/"
#define WR_MODULE(interface) WR_KERNEL_PREFIX interface

static const OsAbiImport g_abi_imports[] =
{
    { WR_MODULE("handle"), "close", "i(i)", os_abi_handle_close, 0U },

    { WR_MODULE("sync"), "mutex-create", "i(ii)", os_abi_mutex_create, 0U },
    { WR_MODULE("sync"), "mutex-lock", "i(ii)", os_abi_mutex_lock, 0U },
    { WR_MODULE("sync"), "mutex-unlock", "i(i)", os_abi_mutex_unlock, 0U },
    { WR_MODULE("sync"), "semaphore-create", "i(iiii)", os_abi_semaphore_create, 0U },
    { WR_MODULE("sync"), "semaphore-take", "i(ii)", os_abi_semaphore_take, 0U },
    { WR_MODULE("sync"), "semaphore-give", "i(i)", os_abi_semaphore_give, 0U },
    { WR_MODULE("sync"), "event-create", "i(ii)", os_abi_event_create, 0U },
    { WR_MODULE("sync"), "event-set", "i(ii)", os_abi_event_set, 0U },
    { WR_MODULE("sync"), "event-clear", "i(ii)", os_abi_event_clear, 0U },
    { WR_MODULE("sync"), "event-get", "i(i)", os_abi_event_get, 0U },
    { WR_MODULE("sync"), "event-wait", "i(iiii)", os_abi_event_wait, 0U },
    { WR_MODULE("sync"), "queue-create", "i(iiii)", os_abi_queue_create, 0U },
    { WR_MODULE("sync"), "queue-send", "i(iii)", os_abi_queue_send, 0U },
    { WR_MODULE("sync"), "queue-receive", "i(iii)", os_abi_queue_receive, 0U },

    { WR_MODULE("timer"), "create", "i(ii)", os_abi_timer_create, 0U },
    { WR_MODULE("timer"), "start", "i(i)", os_abi_timer_start, 0U },
    { WR_MODULE("timer"), "stop", "i(i)", os_abi_timer_stop, 0U },
    { WR_MODULE("timer"), "set-period", "i(ii)", os_abi_timer_set_period, 0U },
    { WR_MODULE("timer"), "read", "i(i)", os_abi_timer_read, 0U },

    { WR_MODULE("wait"), "any", "i(iii)", os_abi_wait_any, 0U },

    /* Every call is linked; what the build leaves out returns -WR_EUNSUPPORTED. */
#if OS_HAS_PROCESSES
    { WR_MODULE("proc"), "spawn", "i(iiiiii)", os_abi_proc_spawn, 0U },
#if OS_HAS_FS
    { WR_MODULE("proc"), "spawn-path", "i(iiiiii)", os_abi_proc_spawn_path, 0U },
#else
    { WR_MODULE("proc"), "spawn-path", "i(iiiiii)", os_abi_unsupported, 0U },
#endif
    { WR_MODULE("proc"), "wait", "i(ii)", os_abi_proc_wait, 0U },
    { WR_MODULE("proc"), "kill", "i(i)", os_abi_proc_kill, 0U },
#else
    { WR_MODULE("proc"), "spawn", "i(iiiiii)", os_abi_unsupported, 0U },
    { WR_MODULE("proc"), "spawn-path", "i(iiiiii)", os_abi_unsupported, 0U },
    { WR_MODULE("proc"), "wait", "i(ii)", os_abi_unsupported, 0U },
    { WR_MODULE("proc"), "kill", "i(i)", os_abi_unsupported, 0U },
#endif
    { WR_MODULE("proc"), "self", "i()", os_abi_proc_self, 0U },
    { WR_MODULE("proc"), "parent", "i()", os_abi_proc_parent, 0U },
};

/*
 * Whether module is "wasm-rtos:kernel/<interface>@1.<minor>.0": then 1, the
 * interface's length and the minor.
 */
/* Whether module is "wasm-rtos:kernel/<interface>@1.<minor>.0" (the devices' library reads these names). */
static int os_abi_kernel_module(const char* module, uint32_t length, uint32_t* out_interface_length,
                                uint32_t* out_minor)
{
    const char* interface = NULL;
    return hw_abi_module(module, length, &interface, out_interface_length, out_minor);
}

/* A call nothing in this build answers: an i32 result says so, other results stay 0. */
static OS_HOST_FUNCTION(os_abi_absent)
{
    OS_HOST_RETURNS(int32_t);
    OS_ABI_UNUSED();
    OS_HOST_RETURN(-WR_EUNSUPPORTED);
}

static OS_HOST_FUNCTION(os_abi_absent_quiet)
{
    OS_ABI_UNUSED();
    (void)os_host_slot;
    return NULL;
}

/*
 * A function of the type the program declares for the import, that reports
 * the call unsupported: -WR_EUNSUPPORTED.
 */
static WmResult os_abi_absent_function(const WmModule* program, const char* name, WmFunc** out)
{
    static const char letters[] = { 'i', 'I', 'f', 'F' };
    const uint8_t* params = NULL;
    const uint8_t* results = NULL;
    uint32_t param_count = 0U;
    uint32_t result_count = 0U;
    char signature[24];
    uint32_t length = 0U;
    uint32_t index = 0U;

    if (program == NULL ||
        !wm_module_import_type(program, name, &params, &param_count, &results, &result_count) ||
        param_count + 4U > sizeof(signature) || result_count > 1U)
    {
        return NULL;
    }
    for (index = 0U; index < param_count + result_count; ++index)
    {
        uint8_t type = index < result_count ? results[index] : params[index - result_count];
        if (type != WM_I32 && type != WM_I64 && type != WM_F32 && type != WM_F64)
        {
            return NULL;
        }
    }
    signature[length++] = result_count != 0U ? letters[WM_I32 - results[0]] : 'v';
    signature[length++] = '(';
    for (index = 0U; index < param_count; ++index)
    {
        signature[length++] = letters[WM_I32 - params[index]];
    }
    signature[length++] = ')';
    signature[length] = '\0';
    if (result_count != 1U || results[0] != WM_I32)
    {
        return os_host_function(signature, os_abi_absent_quiet, out);
    }
    return os_host_function(signature, os_abi_absent, out);
}

/*
 * The ABI call an import names, or *out NULL when it is not one: the
 * kernel's own interfaces, then the drivers'. A kernel interface no driver
 * offers, and a call named with a minor newer than this kernel's that it
 * does not have, get calls that report themselves unsupported, made to the
 * type program declares (when program is given).
 */
WmResult os_abi_resolve(const WmModule* program, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, WmFunc** out)
{
    const uint32_t prefix_length = (uint32_t)(sizeof(WR_KERNEL_PREFIX) - 1U);
    uint32_t interface_length = 0U;
    uint32_t minor = 0U;
    uint8_t versioned = (uint8_t)os_abi_kernel_module(module, module_length, &interface_length, &minor);
    const char* interface = module + prefix_length;
    uint8_t known = 0U;
    uint32_t index = 0U;

    *out = NULL;
    for (index = 0U; index < (uint32_t)(sizeof(g_abi_imports) / sizeof(g_abi_imports[0])); ++index)
    {
        const OsAbiImport* import = &g_abi_imports[index];
        uint8_t kernel = strncmp(import->module, WR_KERNEL_PREFIX, prefix_length) == 0;
        uint8_t same_module = kernel ? versioned && os_name_is(interface, interface_length, import->module + prefix_length)
                                     : os_name_is(module, module_length, import->module);
        if (!same_module)
        {
            continue;
        }
        known = 1U;
        if (os_name_is(name, name_length, import->name) && (!kernel || minor >= import->since))
        {
            return os_host_function(import->signature, import->function, out);
        }
    }
    if (!versioned)
    {
        return NULL;
    }
    if (known)
    {
        return minor > OS_ABI_VERSION_MINOR ? os_abi_absent_function(program, name, out) : NULL;
    }
    /* The devices' interfaces: their library finds the call. */
    {
        int offered = 0;
        const HwDriverCall* call = hw_driver_find_call(interface, interface_length, name, name_length, minor, &offered);
        if (call != NULL)
        {
            return os_host_function(call->signature, call->function, out);
        }
        if (offered)
        {
            return minor > OS_ABI_VERSION_MINOR ? os_abi_absent_function(program, name, out) : NULL;
        }
    }
    return os_abi_absent_function(program, name, out);
}
