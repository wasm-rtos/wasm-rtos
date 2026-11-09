/*
 * Guest ABI, version 1: the calls a program imports from wasm-rtos.
 *
 * Every call returns an i32: zero or a value on success, a negative WR_E*
 * error otherwise. Strings come as (pointer, length) in UTF-8. Objects are
 * reached through handles that belong to the calling process; an object made
 * by a program goes away with the last handle to it, and every handle closes
 * when its process ends. docs/abi.md and wit/ describe the interfaces.
 */

#include "driver.h"
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
/* Longest object name, in bytes. */
#define OS_ABI_NAME_MAX 32U
/* Longest path and argument a program may pass to spawn. */
#define OS_ABI_PATH_MAX 256U
#define OS_ABI_ARG_MAX 256U
#define OS_ABI_ARGS_MAX 32U
/* Event groups keep 24 bits so every value fits a non-negative i32. */
#define OS_ABI_EVENT_BITS 0x00FFFFFFU

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
        case OS_STATUS_NACK: return -WR_ENACK;
        case OS_STATUS_BUS_ERROR: return -WR_EBUS;
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
    /* For OS_OBJECT_DRIVER. */
    const OsDriver* driver;
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

int32_t os_driver_handle_new(const OsDriver* driver, uint32_t id)
{
    int32_t handle = os_abi_handle_new(OS_OBJECT_DRIVER, id);
    if (handle > 0)
    {
        os_abi_state(0U)->handles[handle - 1].driver = driver;
    }
    return handle;
}

int os_driver_handle_get(const OsDriver* driver, int32_t handle, uint32_t* out_id)
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

int32_t os_driver_property(OsStatus status, uint32_t value)
{
    return status == OS_STATUS_OK ? (int32_t)(value & 0x7FFFFFFFU) : os_abi_error(status);
}

int32_t os_driver_open(const OsDriver* driver, uint32_t name_address, uint32_t name_length,
                       OsStatus (*opener)(const char* name, uint32_t length, void* context, uint32_t* out_id),
                       void* context)
{
    const char* name = NULL;
    uint32_t id = 0U;
    int32_t handle = os_abi_read_name(name_address, name_length, &name);
    OsStatus status = OS_STATUS_OK;

    if (handle < 0)
    {
        return handle;
    }
    if (name == NULL)
    {
        return -WR_EINVAL;
    }
    status = opener(name, name_length, context, &id);
    if (status != OS_STATUS_OK)
    {
        return status == OS_STATUS_BUSY ? -WR_EINUSE : os_abi_error(status);
    }
    handle = os_driver_handle_new(driver, id);
    if (handle < 0 && driver->close != NULL)
    {
        driver->close(id);
    }
    return handle;
}

static const OsDriver* g_drivers[OS_MAX_DRIVERS];
static uint32_t g_driver_count;

OsStatus os_driver_add(const OsDriver* driver)
{
    uint32_t index = 0U;

    if (driver == NULL || driver->interface == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index] == driver)
        {
            return OS_STATUS_OK;
        }
    }
    if (g_driver_count == OS_MAX_DRIVERS)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    g_drivers[g_driver_count++] = driver;
    return OS_STATUS_OK;
}

uint32_t os_driver_signal_key(const OsDriver* driver, uint32_t id)
{
    uint32_t index = 0U;

    /* Kernel objects use the low type numbers; drivers take theirs from 0x80 up, by place. */
    while (index < g_driver_count && g_drivers[index] != driver)
    {
        ++index;
    }
    return OS_SIGNAL_KEY(0x80U + index, id);
}

void os_drivers_poll(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->poll != NULL)
        {
            g_drivers[index]->poll();
        }
    }
}

uint32_t os_drivers_next_deadline_ms(void)
{
    uint32_t next = UINT32_MAX;
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->next_deadline_ms != NULL)
        {
            uint32_t due = g_drivers[index]->next_deadline_ms();
            if (due < next)
            {
                next = due;
            }
        }
    }
    return next;
}

void os_drivers_time(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->time != NULL)
        {
            g_drivers[index]->time();
        }
    }
}

void os_drivers_finish_task(uint32_t task_id)
{
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->finish_task != NULL)
        {
            g_drivers[index]->finish_task(task_id);
        }
    }
}

void os_drivers_shutdown(void)
{
    while (g_driver_count > 0U)
    {
        const OsDriver* driver = g_drivers[--g_driver_count];
        if (driver->shutdown != NULL)
        {
            driver->shutdown();
        }
    }
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
int32_t os_abi_read_name(uint32_t address, uint32_t length, const char** out_name)
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

static void os_abi_write_le32(uint8_t* out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

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
        return os_abi_error(status);
    }
    return os_abi_handle_for_new(type, id);
}

static OS_HOST_FUNCTION(os_abi_mutex_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    const char* name = NULL;
    OsMutexHandle mutex = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
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
        OS_HOST_RETURN(os_abi_error(status));
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
    *raw_return = os_abi_error(status);
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
    OS_HOST_RETURN(os_abi_error(os_mutex_unlock(os_mutex_find_by_id(entry->id))));
}

static OS_HOST_FUNCTION(os_abi_semaphore_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, max_count);
    OS_HOST_ARG(uint32_t, initial_count);
    const char* name = NULL;
    OsSemaphoreHandle semaphore = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
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
        OS_HOST_RETURN(os_abi_error(status));
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
    *raw_return = os_abi_error(status);
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
    OS_HOST_RETURN(os_abi_error(os_semaphore_give(os_semaphore_find_by_id(entry->id))));
}

static OS_HOST_FUNCTION(os_abi_event_create)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    const char* name = NULL;
    OsEventGroupHandle event_group = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
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
        OS_HOST_RETURN(os_abi_error(status));
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
    *raw_return = status == OS_STATUS_OK ? (int32_t)os_guest_last_wait_value() : os_abi_error(status);
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
    OsQueueHandle queue = NULL;
    uint32_t id = 0U;
    int32_t result = os_abi_read_name(name_address, name_length, &name);
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
        OS_HOST_RETURN(os_abi_error(status));
    }
    OS_HOST_RETURN(os_abi_adopt(OS_OBJECT_QUEUE, os_queue_get_id(queue), name, name_length));
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
        OS_HOST_RETURN(os_abi_error(status));
    }
    OS_HOST_RETURN(os_abi_adopt(OS_OBJECT_TIMER, os_timer_get_id(timer), NULL, 0U));
}

static OS_HOST_FUNCTION(os_abi_timer_start)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    OS_HOST_RETURN(timer != NULL ? os_abi_error(os_timer_start(timer)) : -WR_EBADH);
}

static OS_HOST_FUNCTION(os_abi_timer_stop)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    OS_HOST_RETURN(timer != NULL ? os_abi_error(os_timer_stop(timer)) : -WR_EBADH);
}

static OS_HOST_FUNCTION(os_abi_timer_set_period)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, period_ms);
    OsTimerHandle timer = os_abi_timer(handle);
    OS_ABI_UNUSED();

    OS_HOST_RETURN(timer != NULL ? os_abi_error(os_timer_change_period(timer, period_ms)) : -WR_EBADH);
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
    uint8_t* handles = NULL;
    uint32_t index = 0U;
    uint32_t ready = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (count == 0U || count > OS_ABI_MAX_HANDLES)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_guest_memory(handles_address, count * 4U, &handles) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        OsAbiHandle* entry = os_abi_handle_get((int32_t)os_abi_read_le32(handles + index * 4U), OS_OBJECT_NONE);
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
    uint8_t* bytes = NULL;
    OsTaskHandle child = NULL;
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (bytes_length == 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_guest_memory(bytes_address, bytes_length, &bytes) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    result = os_abi_read_args(args_address, argc, &storage, args);
    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    status = os_task_spawn(&child, bytes, bytes_length, argc > 0U ? args : NULL, argc, priority, stack_size);
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
    uint8_t* text = NULL;
    OsTaskHandle child = NULL;
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (path_length == 0U || path_length > OS_ABI_PATH_MAX)
    {
        OS_HOST_RETURN(path_length == 0U ? -WR_EINVAL : -WR_ETOOLONG);
    }
    if (os_guest_memory(path_address, path_length, &text) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    memcpy(path, text, path_length);
    path[path_length] = '\0';
    result = os_abi_read_args(args_address, argc, &storage, args);
    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    status = os_task_spawn_path(&child, path, argc > 0U ? args : NULL, argc, priority, stack_size);
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
    *raw_return = status == OS_STATUS_OK ? result : os_abi_error(status);
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
    OS_HOST_RETURN(os_abi_error(os_child_kill(entry->id)));
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
 * WASI calls the kernel answers itself: time is kernel time, and sleeping or
 * yielding blocks only the calling task.
 * ------------------------------------------------------------------------ */

#if defined(OS_ABI_HAS_WASI)
static OS_HOST_FUNCTION(os_abi_wasi_clock_res_get)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG(uint32_t, clock_id);
    OS_HOST_ARG(uint32_t, resolution_address);
    uint8_t* out = NULL;
    OS_ABI_UNUSED();

    if (clock_id > OS_WASI_CLOCKID_THREAD_CPUTIME_ID)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_INVAL);
    }
    if (os_guest_memory(resolution_address, 8U, &out) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_FAULT);
    }
    os_abi_write_le64(out, 1000000U);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

static OS_HOST_FUNCTION(os_abi_wasi_clock_time_get)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG(uint32_t, clock_id);
    OS_HOST_ARG(uint64_t, precision);
    OS_HOST_ARG(uint32_t, time_address);
    uint8_t* out = NULL;
    (void)precision;
    OS_ABI_UNUSED();

    /* Every clock reads kernel time until a platform supplies wall time. */
    if (clock_id > OS_WASI_CLOCKID_THREAD_CPUTIME_ID)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_INVAL);
    }
    if (os_guest_memory(time_address, 8U, &out) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_FAULT);
    }
    os_abi_write_le64(out, os_time_ms64() * 1000000U);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

static OS_HOST_FUNCTION(os_abi_wasi_sched_yield)
{
    OS_HOST_RETURNS(uint32_t);
    (void)os_host_slot;
    OS_ABI_UNUSED();

    *raw_return = OS_WASI_ERRNO_SUCCESS;
    if (os_task_yield() != OS_STATUS_OK)
    {
        return NULL;
    }
    return NULL;
}

/*
 * fd_write: stdout and stderr go to the output port with the writer's task
 * id; files are read only.
 */
static OS_HOST_FUNCTION(os_abi_wasi_fd_write)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG(uint32_t, fd);
    OS_HOST_ARG(uint32_t, iovs_address);
    OS_HOST_ARG(uint32_t, iovs_count);
    OS_HOST_ARG(uint32_t, written_address);
    uint8_t* iovs = NULL;
    uint8_t* written = NULL;
    uint32_t total = 0U;
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    if (iovs_count > UINT32_MAX / 8U ||
        os_guest_memory(iovs_address, iovs_count * 8U, &iovs) != OS_STATUS_OK ||
        os_guest_memory(written_address, 4U, &written) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_FAULT);
    }
    if (fd != 1U && fd != 2U)
    {
#if OS_HAS_FS
        OS_HOST_RETURN(os_fs_write_error(fd));
#else
        OS_HOST_RETURN(OS_WASI_ERRNO_BADF);
#endif
    }

    /* Check every buffer first, so a bad one writes nothing. */
    for (index = 0U; index < iovs_count; ++index)
    {
        uint8_t* data = NULL;
        uint32_t length = os_abi_read_le32(iovs + index * 8U + 4U);
        if (os_guest_memory(os_abi_read_le32(iovs + index * 8U), length, &data) != OS_STATUS_OK ||
            length > UINT32_MAX - total)
        {
            OS_HOST_RETURN(OS_WASI_ERRNO_FAULT);
        }
        total += length;
    }
    for (index = 0U; index < iovs_count; ++index)
    {
        uint8_t* data = NULL;
        uint32_t length = os_abi_read_le32(iovs + index * 8U + 4U);
        (void)os_guest_memory(os_abi_read_le32(iovs + index * 8U), length, &data);
        os_output_write(fd, data, length);
    }

    os_abi_write_le32(written, total);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

/*
 * poll_oneoff: file subscriptions are always ready (files and stdio never
 * block); otherwise the task sleeps until the earliest clock subscription.
 */
static OS_HOST_FUNCTION(os_abi_wasi_poll_oneoff)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG(uint32_t, in_address);
    OS_HOST_ARG(uint32_t, out_address);
    OS_HOST_ARG(uint32_t, count);
    OS_HOST_ARG(uint32_t, events_address);
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
        OS_HOST_RETURN(OS_WASI_ERRNO_INVAL);
    }
    if (count > 0xFFFFFFFFU / 48U ||
        os_guest_memory(in_address, count * 48U, &in) != OS_STATUS_OK ||
        os_guest_memory(out_address, count * 32U, &out) != OS_STATUS_OK ||
        os_guest_memory(events_address, 4U, &events) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_FAULT);
    }

    for (index = 0U; index < count; ++index)
    {
        const uint8_t* subscription = in + index * 48U;
        uint8_t tag = subscription[8];

        if (tag == OS_WASI_EVENTTYPE_CLOCK)
        {
            uint64_t timeout = os_abi_read_le64(subscription + 24U);
            uint16_t flags = (uint16_t)(subscription[40] | (subscription[41] << 8U));
            uint64_t due = (flags & OS_WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME) != 0U
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
        event[10] = OS_WASI_EVENTTYPE_CLOCK;
        written = 1U;
        events[0] = 1U;
        events[1] = events[2] = events[3] = 0U;
        *raw_return = OS_WASI_ERRNO_SUCCESS;
        if (wait_ms > 0U && os_task_delay_ms(wait_ms > 0x7FFFFFFFU ? 0x7FFFFFFFU : (uint32_t)wait_ms) == OS_STATUS_OK)
        {
            return NULL;
        }
        return NULL;
    }

    events[0] = (uint8_t)written;
    events[1] = (uint8_t)(written >> 8U);
    events[2] = (uint8_t)(written >> 16U);
    events[3] = (uint8_t)(written >> 24U);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}
#endif

#if defined(OS_ABI_HAS_WASI)
/* ---------------------------------------------------------------------------
 * WASI basics: arguments, environment, exit, random bytes and the standard
 * streams, for what neither the table below nor the file system answers.
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_wasi_args_sizes_get)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG_MEM(uint8_t*, count);
    OS_HOST_ARG_MEM(uint8_t*, size);
    const char* const* argv = NULL;
    uint32_t argc = os_guest_args(&argv);
    uint32_t bytes = 0U;
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    OS_HOST_CHECK_MEM(count, 4U);
    OS_HOST_CHECK_MEM(size, 4U);
    for (index = 0U; index < argc; ++index)
    {
        bytes += (uint32_t)strlen(argv[index]) + 1U;
    }
    OS_HOST_WRITE32(count, argc);
    OS_HOST_WRITE32(size, bytes);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

static OS_HOST_FUNCTION(os_wasi_args_get)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG_MEM(uint8_t*, pointers);
    OS_HOST_ARG_MEM(uint8_t*, buffer);
    const char* const* argv = NULL;
    uint32_t argc = os_guest_args(&argv);
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    for (index = 0U; index < argc; ++index)
    {
        uint32_t length = (uint32_t)strlen(argv[index]) + 1U;
        OS_HOST_CHECK_MEM(pointers + index * 4U, 4U);
        OS_HOST_CHECK_MEM(buffer, length);
        OS_HOST_WRITE32(pointers + index * 4U, OS_HOST_OFFSET(buffer));
        memcpy(buffer, argv[index], length);
        buffer += length;
    }
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

/* No environment variables. */
static OS_HOST_FUNCTION(os_wasi_environ_sizes_get)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG_MEM(uint8_t*, count);
    OS_HOST_ARG_MEM(uint8_t*, size);
    OS_ABI_UNUSED();

    OS_HOST_CHECK_MEM(count, 4U);
    OS_HOST_CHECK_MEM(size, 4U);
    OS_HOST_WRITE32(count, 0U);
    OS_HOST_WRITE32(size, 0U);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

static OS_HOST_FUNCTION(os_wasi_environ_get)
{
    OS_HOST_RETURNS(uint32_t);
    (void)os_host_slot;
    OS_ABI_UNUSED();
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

static OS_HOST_FUNCTION(os_wasi_proc_exit)
{
    OS_HOST_ARG(uint32_t, code);
    OS_ABI_UNUSED();
    return os_guest_exit(code);
}

static OS_HOST_FUNCTION(os_wasi_random_get)
{
    static uint32_t state = 0x2545F491U;
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG_MEM(uint8_t*, buffer);
    OS_HOST_ARG(uint32_t, length);
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    OS_HOST_CHECK_MEM(buffer, length);
    for (index = 0U; index < length; ++index)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        buffer[index] = (uint8_t)state;
    }
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

/* The standard streams are character devices; without a file system nothing else is open. */
static OS_HOST_FUNCTION(os_wasi_fd_fdstat_get)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG(uint32_t, fd);
    OS_HOST_ARG_MEM(uint8_t*, stat);
    OS_ABI_UNUSED();

    if (fd > 2U)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_BADF);
    }
    OS_HOST_CHECK_MEM(stat, 24U);
    memset(stat, 0, 24U);
    stat[0] = OS_WASI_FILETYPE_CHARACTER_DEVICE;
    memset(stat + 8, 0xFF, 16U);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

static OS_HOST_FUNCTION(os_wasi_fd_close)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG(uint32_t, fd);
    OS_ABI_UNUSED();
    OS_HOST_RETURN(fd <= 2U ? OS_WASI_ERRNO_SUCCESS : OS_WASI_ERRNO_BADF);
}

static OS_HOST_FUNCTION(os_wasi_fd_seek)
{
    OS_HOST_RETURNS(uint32_t);
    (void)os_host_slot;
    OS_ABI_UNUSED();
    OS_HOST_RETURN(OS_WASI_ERRNO_SPIPE);
}

static OS_HOST_FUNCTION(os_wasi_badf)
{
    OS_HOST_RETURNS(uint32_t);
    (void)os_host_slot;
    OS_ABI_UNUSED();
    OS_HOST_RETURN(OS_WASI_ERRNO_BADF);
}

/* Standard input has nothing to read. */
static OS_HOST_FUNCTION(os_wasi_fd_read)
{
    OS_HOST_RETURNS(uint32_t);
    OS_HOST_ARG(uint32_t, fd);
    OS_HOST_ARG(uint32_t, iovs);
    OS_HOST_ARG(uint32_t, count);
    OS_HOST_ARG_MEM(uint8_t*, nread);
    OS_ABI_UNUSED();
    (void)iovs;
    (void)count;

    if (fd != 0U)
    {
        OS_HOST_RETURN(OS_WASI_ERRNO_BADF);
    }
    OS_HOST_CHECK_MEM(nread, 4U);
    OS_HOST_WRITE32(nread, 0U);
    OS_HOST_RETURN(OS_WASI_ERRNO_SUCCESS);
}

typedef struct OsWasiImport
{
    const char* name;
    const char* signature;
    OsHostImportFunction function;
} OsWasiImport;

static const OsWasiImport g_wasi_basics[] =
{
    { "args_sizes_get", "i(**)", os_wasi_args_sizes_get },
    { "args_get", "i(**)", os_wasi_args_get },
    { "environ_sizes_get", "i(**)", os_wasi_environ_sizes_get },
    { "environ_get", "i(**)", os_wasi_environ_get },
    { "proc_exit", "v(i)", os_wasi_proc_exit },
    { "random_get", "i(*i)", os_wasi_random_get },
    { "fd_fdstat_get", "i(i*)", os_wasi_fd_fdstat_get },
    { "fd_close", "i(i)", os_wasi_fd_close },
    { "fd_seek", "i(iIi*)", os_wasi_fd_seek },
    { "fd_read", "i(i*i*)", os_wasi_fd_read },
    { "fd_prestat_get", "i(i*)", os_wasi_badf },
    { "fd_prestat_dir_name", "i(i*i)", os_wasi_badf },
};

/* A WASI basic call, in either WASI namespace, or *out NULL. */
WmResult os_abi_resolve_wasi(const char* module, uint32_t module_length, const char* name, uint32_t name_length,
                             WmFunc** out)
{
    uint32_t index = 0U;

    *out = NULL;
    if (!os_name_is(module, module_length, "wasi_unstable") &&
        !os_name_is(module, module_length, "wasi_snapshot_preview1"))
    {
        return NULL;
    }
    for (index = 0U; index < (uint32_t)(sizeof(g_wasi_basics) / sizeof(g_wasi_basics[0])); ++index)
    {
        if (os_name_is(name, name_length, g_wasi_basics[index].name))
        {
            return os_host_function(g_wasi_basics[index].signature, g_wasi_basics[index].function, out);
        }
    }
    return NULL;
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
 * from the one that added a call up to OS_ABI_VERSION_MINOR.
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

#if defined(OS_ABI_HAS_WASI)
    { "wasi_snapshot_preview1", "clock_res_get", "i(ii)", os_abi_wasi_clock_res_get, 0U },
    { "wasi_snapshot_preview1", "clock_time_get", "i(iIi)", os_abi_wasi_clock_time_get, 0U },
    { "wasi_snapshot_preview1", "sched_yield", "i()", os_abi_wasi_sched_yield, 0U },
    { "wasi_snapshot_preview1", "poll_oneoff", "i(iiii)", os_abi_wasi_poll_oneoff, 0U },
    { "wasi_snapshot_preview1", "fd_write", "i(iiii)", os_abi_wasi_fd_write, 0U },
    { "wasi_unstable", "clock_res_get", "i(ii)", os_abi_wasi_clock_res_get, 0U },
    { "wasi_unstable", "clock_time_get", "i(iIi)", os_abi_wasi_clock_time_get, 0U },
    { "wasi_unstable", "sched_yield", "i()", os_abi_wasi_sched_yield, 0U },
    { "wasi_unstable", "poll_oneoff", "i(iiii)", os_abi_wasi_poll_oneoff, 0U },
    { "wasi_unstable", "fd_write", "i(iiii)", os_abi_wasi_fd_write, 0U },
#endif
};

/*
 * Whether module is "wasm-rtos:kernel/<interface>@1.<minor>.0": then 1, the
 * interface's length and the minor.
 */
static int os_abi_kernel_module(const char* module, uint32_t length, uint32_t* out_interface_length,
                                uint32_t* out_minor)
{
    const uint32_t prefix_length = (uint32_t)(sizeof(WR_KERNEL_PREFIX) - 1U);
    uint32_t at = prefix_length;
    uint32_t minor = 0U;

    if (length <= prefix_length || memcmp(module, WR_KERNEL_PREFIX, prefix_length) != 0)
    {
        return 0;
    }
    while (at < length && module[at] != '@')
    {
        ++at;
    }
    *out_interface_length = at - prefix_length;
    if (*out_interface_length == 0U || length < at + 6U || memcmp(module + at, "@1.", 3U) != 0)
    {
        return 0;
    }
    at += 3U;
    if (module[at] < '0' || module[at] > '9' || (module[at] == '0' && module[at + 1U] != '.'))
    {
        return 0;
    }
    while (at < length && module[at] >= '0' && module[at] <= '9' && minor < 1000U)
    {
        minor = minor * 10U + (uint32_t)(module[at++] - '0');
    }
    *out_minor = minor;
    return length - at == 2U && memcmp(module + at, ".0", 2U) == 0 && minor <= OS_ABI_VERSION_MINOR;
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

/* A function of the type the program declares for the import, that reports the call unsupported. */
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
    return os_host_function(signature, result_count == 1U && results[0] == WM_I32 ? os_abi_absent : os_abi_absent_quiet,
                            out);
}

/*
 * The ABI call an import names, or *out NULL when it is not one: the
 * kernel's own interfaces, then the drivers'. A kernel interface no driver
 * offers gets calls that report themselves unsupported, made to the type
 * program declares (when program is given).
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
    if (!versioned || known)
    {
        return NULL;
    }
    for (index = 0U; index < g_driver_count; ++index)
    {
        const OsDriver* driver = g_drivers[index];
        if (!os_name_is(interface, interface_length, driver->interface))
        {
            continue;
        }
        for (uint32_t call = 0U; call < driver->call_count; ++call)
        {
            if (os_name_is(name, name_length, driver->calls[call].name) && minor >= driver->calls[call].since)
            {
                return os_host_function(driver->calls[call].signature, driver->calls[call].function, out);
            }
        }
        return NULL;
    }
    return os_abi_absent_function(program, name, out);
}
