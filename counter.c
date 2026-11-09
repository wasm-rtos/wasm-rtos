/*
 * Counters: pulse counters and quadrature encoders the platform's hardware
 * runs. A program owns a counter, reads and sets its count, and may wait
 * for the count to leave a window: through the hardware's own watch
 * points, or by the kernel reading the count every OS_COUNTER_POLL_MS.
 */

#include "driver.h"


#include <stdlib.h>
#include <string.h>

/* The hardware's watch point fired, recorded by interrupt handlers and taken by the kernel. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint8_t OsCounterFlag;
#define OS_COUNTER_FLAG_SET(flag) ((flag) = 1U)
#define OS_COUNTER_FLAG_TAKE(flag) os_counter_flag_take(&(flag))
static uint8_t os_counter_flag_take(OsCounterFlag* flag)
{
    uint8_t value = *flag;
    *flag = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uchar OsCounterFlag;
#define OS_COUNTER_FLAG_SET(flag) atomic_store_explicit(&(flag), 1U, memory_order_release)
#define OS_COUNTER_FLAG_TAKE(flag) atomic_exchange_explicit(&(flag), 0U, memory_order_acquire)
#endif

/* Allocated when the platform registers the counter. */
typedef struct OsCounter
{
    uint32_t unit;
    char name[OS_COUNTER_NAME_MAX + 1U];
    uint8_t name_length;
    OsCounterInfo info;
    /* Owning task id, 0 when free. */
    uint32_t owner;
    uint32_t filter_ns;
    /* The window of watch; watching is off when it covers every count. */
    int32_t low;
    int32_t high;
    uint8_t watching;
    /* The count was outside the window when last read. */
    uint8_t outside;
    uint32_t last_poll_ms;
    OsCounterFlag event;
} OsCounter;

static struct
{
    OsCounterPort port;
    uint8_t has_port;
    OsCounter* counters[OS_COUNTER_MAX];
} g_counter;

static OsCounter* os_counter_slot(uint32_t id)
{
    return id >= 1U && id <= OS_COUNTER_MAX ? g_counter.counters[id - 1U] : NULL;
}

/* An open counter: the slot when its owner holds it. */
static OsCounter* os_counter_open_slot(uint32_t id)
{
    OsCounter* counter = os_counter_slot(id);
    return counter != NULL && counter->owner != 0U ? counter : NULL;
}

OsStatus os_counter_port_set(const OsCounterPort* port)
{
    if (port == NULL)
    {
        memset(&g_counter.port, 0, sizeof(g_counter.port));
        g_counter.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->start == NULL || port->read == NULL || port->write == NULL || port->stop == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_counter.port = *port;
    g_counter.has_port = 1U;
    return OS_STATUS_OK;
}

extern const OsDriver os_counter_driver;

OsStatus os_counter_add(const char* name, uint32_t unit, const OsCounterInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsCounter* slot = NULL;
    uint32_t free_index = OS_COUNTER_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > OS_COUNTER_NAME_MAX || info == NULL ||
        (info->kind != OS_COUNTER_PULSE && info->kind != OS_COUNTER_QUADRATURE))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_COUNTER_MAX; ++index)
    {
        OsCounter* entry = g_counter.counters[index];
        if (entry != NULL && (entry->unit == unit ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (entry == NULL && free_index == OS_COUNTER_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == OS_COUNTER_MAX)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot = (OsCounter*)calloc(1U, sizeof(OsCounter));
    if (slot == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    slot->unit = unit;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    g_counter.counters[free_index] = slot;
    /* A platform with these devices gives programs the counter interface. */
    if (os_driver_add(&os_counter_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
}

uint32_t os_counter_get_owner(uint32_t unit)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_COUNTER_MAX; ++index)
    {
        if (g_counter.counters[index] != NULL && g_counter.counters[index]->unit == unit)
        {
            return g_counter.counters[index]->owner;
        }
    }
    return 0U;
}

void os_counter_event(uint32_t unit)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_COUNTER_MAX; ++index)
    {
        OsCounter* counter = g_counter.counters[index];
        if (counter != NULL && counter->unit == unit)
        {
            OS_COUNTER_FLAG_SET(counter->event);
            return;
        }
    }
}

static uint8_t os_counter_mode_fits(uint8_t kind, uint32_t mode)
{
    return kind == OS_COUNTER_PULSE ? mode >= OS_COUNTER_RISING && mode <= OS_COUNTER_BOTH
                                    : mode >= OS_COUNTER_X1 && mode <= OS_COUNTER_X4;
}

OsStatus os_counter_open(const char* name, uint32_t length, uint32_t mode, uint32_t filter_ns, uint32_t* out_id)
{
    OsCounter* counter = NULL;
    uint8_t any = 0U;
    uint32_t id = 0U;
    uint32_t index = 0U;
    OsStatus status = OS_STATUS_OK;

    for (index = 0U; index < OS_COUNTER_MAX; ++index)
    {
        OsCounter* entry = g_counter.counters[index];
        any |= entry != NULL;
        if (counter == NULL && entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
        {
            counter = entry;
            id = index + 1U;
        }
    }
    if (!g_counter.has_port || (counter == NULL && !any))
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (counter == NULL)
    {
        return OS_STATUS_FILE_NOT_FOUND;
    }
    if (!os_counter_mode_fits(counter->info.kind, mode))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (counter->owner != 0U)
    {
        return OS_STATUS_BUSY;
    }
    counter->filter_ns = filter_ns;
    status = g_counter.port.start(g_counter.port.context, counter->unit, mode, filter_ns, &counter->filter_ns);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    counter->owner = os_task_get_id(os_task_get_current());
    counter->low = INT32_MIN;
    counter->high = INT32_MAX;
    counter->watching = 0U;
    counter->outside = 0U;
    (void)OS_COUNTER_FLAG_TAKE(counter->event);
    *out_id = id;
    return OS_STATUS_OK;
}

void os_counter_close(uint32_t id)
{
    OsCounter* counter = os_counter_open_slot(id);

    if (counter == NULL)
    {
        return;
    }
    if (g_counter.has_port)
    {
        g_counter.port.stop(g_counter.port.context, counter->unit);
    }
    counter->owner = 0U;
    counter->watching = 0U;
    counter->outside = 0U;
}

OsStatus os_counter_read(uint32_t id, int32_t* out_count)
{
    OsCounter* counter = os_counter_open_slot(id);

    if (counter == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    return g_counter.has_port ? g_counter.port.read(g_counter.port.context, counter->unit, out_count)
                              : OS_STATUS_UNSUPPORTED;
}

/* Read the count and note whether it left the window; 1 when that changed to outside. */
static uint8_t os_counter_check(OsCounter* counter)
{
    int32_t count = 0;
    uint8_t was = counter->outside;

    if (!counter->watching || g_counter.port.read(g_counter.port.context, counter->unit, &count) != OS_STATUS_OK)
    {
        return 0U;
    }
    counter->outside = count < counter->low || count > counter->high;
    return counter->outside && !was;
}

OsStatus os_counter_write(uint32_t id, int32_t count)
{
    OsCounter* counter = os_counter_open_slot(id);
    OsStatus status = OS_STATUS_OK;

    if (counter == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_counter.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = g_counter.port.write(g_counter.port.context, counter->unit, count);
    if (status == OS_STATUS_OK && counter->watching)
    {
        counter->outside = count < counter->low || count > counter->high;
    }
    return status;
}

OsStatus os_counter_watch(uint32_t id, int32_t low, int32_t high)
{
    OsCounter* counter = os_counter_open_slot(id);
    OsStatus status = OS_STATUS_OK;

    if (counter == NULL || low > high)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_counter.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    counter->low = low;
    counter->high = high;
    counter->watching = low != INT32_MIN || high != INT32_MAX;
    counter->outside = 0U;
    counter->last_poll_ms = os_get_tick_ms();
    if (g_counter.port.watch != NULL)
    {
        status = g_counter.port.watch(g_counter.port.context, counter->unit, low, high);
        if (status != OS_STATUS_OK)
        {
            counter->watching = 0U;
            return status;
        }
    }
    /* The count may be outside already. */
    (void)os_counter_check(counter);
    return OS_STATUS_OK;
}

OsStatus os_counter_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsCounter* counter = os_counter_open_slot(id);

    if (counter == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case OS_COUNTER_PROP_KIND:
            *out_value = counter->info.kind;
            return OS_STATUS_OK;
        case OS_COUNTER_PROP_MAX_HZ:
            *out_value = counter->info.max_hz;
            return OS_STATUS_OK;
        case OS_COUNTER_PROP_FILTER_NS:
            *out_value = counter->filter_ns;
            return OS_STATUS_OK;
        default:
            return OS_STATUS_UNSUPPORTED;
    }
}

uint8_t os_counter_ready(uint32_t id)
{
    OsCounter* counter = os_counter_open_slot(id);
    return counter != NULL && counter->watching && counter->outside;
}

void os_counter_poll(void)
{
    uint32_t now = os_get_tick_ms();
    uint8_t changed = 0U;
    uint32_t index = 0U;

    if (!g_counter.has_port)
    {
        return;
    }
    for (index = 0U; index < OS_COUNTER_MAX; ++index)
    {
        OsCounter* counter = g_counter.counters[index];
        uint8_t due = 0U;
        if (counter == NULL || counter->owner == 0U)
        {
            continue;
        }
        if (OS_COUNTER_FLAG_TAKE(counter->event))
        {
            due = 1U;
        }
        if (counter->watching && g_counter.port.watch == NULL && now - counter->last_poll_ms >= OS_COUNTER_POLL_MS)
        {
            counter->last_poll_ms = now;
            due = 1U;
        }
        if (due && os_counter_check(counter))
        {
            changed = 1U;
        }
    }
    if (changed)
    {
        os_wait_any_recheck();
    }
}

uint32_t os_counter_next_deadline_ms(void)
{
    uint32_t now = os_get_tick_ms();
    uint32_t soonest = OS_WAIT_FOREVER;
    uint32_t index = 0U;

    if (!g_counter.has_port || g_counter.port.watch != NULL)
    {
        return OS_WAIT_FOREVER;
    }
    for (index = 0U; index < OS_COUNTER_MAX; ++index)
    {
        OsCounter* counter = g_counter.counters[index];
        uint32_t elapsed = 0U;
        uint32_t remaining = 0U;
        if (counter == NULL || counter->owner == 0U || !counter->watching || counter->outside)
        {
            continue;
        }
        elapsed = now - counter->last_poll_ms;
        remaining = elapsed >= OS_COUNTER_POLL_MS ? 0U : OS_COUNTER_POLL_MS - elapsed;
        if (remaining < soonest)
        {
            soonest = remaining;
        }
    }
    return soonest;
}

void os_counter_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_COUNTER_MAX; ++index)
    {
        free(g_counter.counters[index]);
    }
    memset(&g_counter, 0, sizeof(g_counter));
}

/* ---------------------------------------------------------------------------
 * The counter interface for programs
 * ------------------------------------------------------------------------ */

typedef struct OsAbiCounterOpen
{
    uint32_t mode;
    uint32_t filter_ns;
} OsAbiCounterOpen;

static OsStatus os_counter_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    const OsAbiCounterOpen* open = (const OsAbiCounterOpen*)context;
    return os_counter_open(name, length, open->mode, open->filter_ns, out_id);
}

static OS_HOST_FUNCTION(os_counter_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, mode);
    OS_HOST_ARG(uint32_t, filter_ns);
    OsAbiCounterOpen open = { mode, filter_ns };
    OS_ABI_UNUSED();

    OS_HOST_RETURN(os_driver_open(&os_counter_driver, name_address, name_length, os_counter_call_opener, &open));
}

/* The count is signed, so it goes to memory: 4 bytes, little-endian. */
static OS_HOST_FUNCTION(os_counter_call_read)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, out_address);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_counter_driver, handle, &id);
    uint8_t* out = NULL;
    int32_t count = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (os_guest_memory(out_address, 4U, &out) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_counter_read(id, &count);
    if (status == OS_STATUS_OK)
    {
        OS_HOST_WRITE32(out, (uint32_t)count);
    }
    OS_HOST_RETURN(os_abi_error(status));
}

static OS_HOST_FUNCTION(os_counter_call_set)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(int32_t, count);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_counter_driver, handle, &id);
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(os_abi_error(os_counter_write(id, count)));
}

static OS_HOST_FUNCTION(os_counter_call_watch)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(int32_t, low);
    OS_HOST_ARG(int32_t, high);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_counter_driver, handle, &id);
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(os_abi_error(os_counter_watch(id, low, high)));
}

static OS_HOST_FUNCTION(os_counter_call_get)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_counter_driver, handle, &id);
    uint32_t value = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    status = os_counter_get(id, property, &value);
    OS_HOST_RETURN(os_driver_property(status, value));
}

static const OsDriverCall os_counter_calls[] =
{
    { "open", "i(iiii)", os_counter_call_open, 7U },
    { "read", "i(ii)", os_counter_call_read, 7U },
    { "set", "i(ii)", os_counter_call_set, 7U },
    { "watch", "i(iii)", os_counter_call_watch, 7U },
    { "get", "i(ii)", os_counter_call_get, 7U },
};

const OsDriver os_counter_driver =
{
    .interface = "counter",
    .calls = os_counter_calls,
    .call_count = (uint32_t)(sizeof(os_counter_calls) / sizeof(os_counter_calls[0])),
    .close = os_counter_close,
    .ready = os_counter_ready,
    .poll = os_counter_poll,
    .next_deadline_ms = os_counter_next_deadline_ms,
    .shutdown = os_counter_shutdown,
};

