/*
 * Counters: pulse counters and quadrature encoders the platform's hardware
 * runs. A program owns a counter, reads and sets its count, and may wait
 * for the count to leave a window: through the hardware's own watch
 * points, or by the library reading the count every HW_COUNTER_POLL_MS.
 */

#include "hw_driver.h"

#include <stdlib.h>
#include <string.h>

/* The hardware's watch point fired, recorded by interrupt handlers and taken by the library. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint8_t HwCounterFlag;
#define HW_COUNTER_FLAG_SET(flag) ((flag) = 1U)
#define HW_COUNTER_FLAG_TAKE(flag) hw_counter_flag_take(&(flag))
static uint8_t hw_counter_flag_take(HwCounterFlag* flag)
{
    uint8_t value = *flag;
    *flag = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uchar HwCounterFlag;
#define HW_COUNTER_FLAG_SET(flag) atomic_store_explicit(&(flag), 1U, memory_order_release)
#define HW_COUNTER_FLAG_TAKE(flag) atomic_exchange_explicit(&(flag), 0U, memory_order_acquire)
#endif

/* Allocated when the platform registers the counter. */
typedef struct HwCounter
{
    uint32_t unit;
    char name[HW_COUNTER_NAME_MAX + 1U];
    uint8_t name_length;
    HwCounterInfo info;
    /* Owning task id, 0 when free. */
    uint32_t owner;
    /* What it counts, as the program opened it. */
    uint32_t mode;
    uint32_t filter_ns;
    /* The window of watch; watching is off when it covers every count. */
    int32_t low;
    int32_t high;
    uint8_t watching;
    /* The count was outside the window when last read. */
    uint8_t outside;
    uint32_t last_poll_ms;
    HwCounterFlag event;
} HwCounter;

static struct
{
    HwCounterPort port;
    uint8_t has_port;
    HwCounter* counters[HW_COUNTER_MAX];
} g_counter;

static HwCounter* hw_counter_slot(uint32_t id)
{
    return id >= 1U && id <= HW_COUNTER_MAX ? g_counter.counters[id - 1U] : NULL;
}

/* An open counter: the slot when its owner holds it. */
static HwCounter* hw_counter_open_slot(uint32_t id)
{
    HwCounter* counter = hw_counter_slot(id);
    return counter != NULL && counter->owner != 0U ? counter : NULL;
}

HwStatus hw_counter_port_set(const HwCounterPort* port)
{
    if (port == NULL)
    {
        memset(&g_counter.port, 0, sizeof(g_counter.port));
        g_counter.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->start == NULL || port->read == NULL || port->write == NULL || port->stop == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_counter.port = *port;
    g_counter.has_port = 1U;
    return HW_STATUS_OK;
}

extern const HwDriver hw_counter_driver;

HwStatus hw_counter_add(const char* name, uint32_t unit, const HwCounterInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    HwCounter* slot = NULL;
    uint32_t free_index = HW_COUNTER_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > HW_COUNTER_NAME_MAX || info == NULL ||
        (info->kind != HW_COUNTER_PULSE && info->kind != HW_COUNTER_QUADRATURE))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        HwCounter* entry = g_counter.counters[index];
        if (entry != NULL && (entry->unit == unit ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return HW_STATUS_BUSY;
        }
        if (entry == NULL && free_index == HW_COUNTER_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == HW_COUNTER_MAX)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot = (HwCounter*)calloc(1U, sizeof(HwCounter));
    if (slot == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    slot->unit = unit;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    g_counter.counters[free_index] = slot;
    /* A platform with these devices gives programs the counter interface. */
    if (hw_driver_add(&hw_counter_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

uint32_t hw_counter_get_owner(uint32_t unit)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        if (g_counter.counters[index] != NULL && g_counter.counters[index]->unit == unit)
        {
            return g_counter.counters[index]->owner;
        }
    }
    return 0U;
}

void hw_counter_event(uint32_t unit)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        HwCounter* counter = g_counter.counters[index];
        if (counter != NULL && counter->unit == unit)
        {
            HW_COUNTER_FLAG_SET(counter->event);
            return;
        }
    }
}

static uint8_t hw_counter_mode_fits(uint8_t kind, uint32_t mode)
{
    return kind == HW_COUNTER_PULSE ? mode >= HW_COUNTER_RISING && mode <= HW_COUNTER_BOTH
                                    : mode >= HW_COUNTER_X1 && mode <= HW_COUNTER_X4;
}

HwStatus hw_counter_open(const char* name, uint32_t length, uint32_t mode, uint32_t filter_ns, uint32_t* out_id)
{
    HwCounter* counter = NULL;
    uint8_t any = 0U;
    uint32_t id = 0U;
    uint32_t index = 0U;
    HwStatus status = HW_STATUS_OK;

    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        HwCounter* entry = g_counter.counters[index];
        any |= entry != NULL;
        if (counter == NULL && entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
        {
            counter = entry;
            id = index + 1U;
        }
    }
    if (!g_counter.has_port || (counter == NULL && !any))
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (counter == NULL)
    {
        return HW_STATUS_FILE_NOT_FOUND;
    }
    if (!hw_counter_mode_fits(counter->info.kind, mode))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (counter->owner != 0U)
    {
        return HW_STATUS_BUSY;
    }
    counter->filter_ns = filter_ns;
    status = g_counter.port.start(g_counter.port.context, counter->unit, mode, filter_ns, &counter->filter_ns);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    counter->owner = hw_host_task_id();
    counter->mode = mode;
    counter->low = INT32_MIN;
    counter->high = INT32_MAX;
    counter->watching = 0U;
    counter->outside = 0U;
    (void)HW_COUNTER_FLAG_TAKE(counter->event);
    *out_id = id;
    return HW_STATUS_OK;
}

void hw_counter_close(uint32_t id)
{
    HwCounter* counter = hw_counter_open_slot(id);

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

HwStatus hw_counter_read(uint32_t id, int32_t* out_count)
{
    HwCounter* counter = hw_counter_open_slot(id);

    if (counter == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    return g_counter.has_port ? g_counter.port.read(g_counter.port.context, counter->unit, out_count)
                              : HW_STATUS_UNSUPPORTED;
}

/* Read the count and note whether it left the window; 1 when that changed to outside. */
static uint8_t hw_counter_check(HwCounter* counter)
{
    int32_t count = 0;
    uint8_t was = counter->outside;

    if (!counter->watching || g_counter.port.read(g_counter.port.context, counter->unit, &count) != HW_STATUS_OK)
    {
        return 0U;
    }
    counter->outside = count < counter->low || count > counter->high;
    return counter->outside && !was;
}

HwStatus hw_counter_write(uint32_t id, int32_t count)
{
    HwCounter* counter = hw_counter_open_slot(id);
    HwStatus status = HW_STATUS_OK;

    if (counter == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_counter.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = g_counter.port.write(g_counter.port.context, counter->unit, count);
    if (status == HW_STATUS_OK && counter->watching)
    {
        counter->outside = count < counter->low || count > counter->high;
    }
    return status;
}

HwStatus hw_counter_watch(uint32_t id, int32_t low, int32_t high)
{
    HwCounter* counter = hw_counter_open_slot(id);
    HwStatus status = HW_STATUS_OK;

    if (counter == NULL || low > high)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_counter.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    counter->low = low;
    counter->high = high;
    counter->watching = low != INT32_MIN || high != INT32_MAX;
    counter->outside = 0U;
    counter->last_poll_ms = hw_host_tick_ms();
    if (g_counter.port.watch != NULL)
    {
        status = g_counter.port.watch(g_counter.port.context, counter->unit, low, high);
        if (status != HW_STATUS_OK)
        {
            counter->watching = 0U;
            return status;
        }
    }
    /* The count may be outside already. */
    (void)hw_counter_check(counter);
    return HW_STATUS_OK;
}

HwStatus hw_counter_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwCounter* counter = hw_counter_open_slot(id);

    if (counter == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case HW_COUNTER_PROP_KIND:
            *out_value = counter->info.kind;
            return HW_STATUS_OK;
        case HW_COUNTER_PROP_MAX_HZ:
            *out_value = counter->info.max_hz;
            return HW_STATUS_OK;
        case HW_COUNTER_PROP_FILTER_NS:
            *out_value = counter->filter_ns;
            return HW_STATUS_OK;
        default:
            return HW_STATUS_UNSUPPORTED;
    }
}

uint8_t hw_counter_ready(uint32_t id)
{
    HwCounter* counter = hw_counter_open_slot(id);
    return counter != NULL && counter->watching && counter->outside;
}

void hw_counter_poll(void)
{
    uint32_t now = hw_host_tick_ms();
    uint8_t changed = 0U;
    uint32_t index = 0U;

    if (!g_counter.has_port)
    {
        return;
    }
    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        HwCounter* counter = g_counter.counters[index];
        uint8_t due = 0U;
        if (counter == NULL || counter->owner == 0U)
        {
            continue;
        }
        if (HW_COUNTER_FLAG_TAKE(counter->event))
        {
            due = 1U;
        }
        if (counter->watching && g_counter.port.watch == NULL && now - counter->last_poll_ms >= HW_COUNTER_POLL_MS)
        {
            counter->last_poll_ms = now;
            due = 1U;
        }
        if (due && hw_counter_check(counter))
        {
            changed = 1U;
        }
    }
    if (changed)
    {
        hw_host_wait_any_recheck();
    }
}

uint32_t hw_counter_next_deadline_ms(void)
{
    uint32_t now = hw_host_tick_ms();
    uint32_t soonest = HW_WAIT_FOREVER;
    uint32_t index = 0U;

    if (!g_counter.has_port || g_counter.port.watch != NULL)
    {
        return HW_WAIT_FOREVER;
    }
    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        HwCounter* counter = g_counter.counters[index];
        uint32_t elapsed = 0U;
        uint32_t remaining = 0U;
        if (counter == NULL || counter->owner == 0U || !counter->watching || counter->outside)
        {
            continue;
        }
        elapsed = now - counter->last_poll_ms;
        remaining = elapsed >= HW_COUNTER_POLL_MS ? 0U : HW_COUNTER_POLL_MS - elapsed;
        if (remaining < soonest)
        {
            soonest = remaining;
        }
    }
    return soonest;
}

void hw_counter_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        free(g_counter.counters[index]);
    }
    memset(&g_counter, 0, sizeof(g_counter));
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: each counter a task holds, by name, with what
 * it counts, its window and the count
 * ------------------------------------------------------------------------ */

static void hw_counter_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        count += g_counter.counters[index] != NULL && g_counter.counters[index]->owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_COUNTER_MAX; ++index)
    {
        const HwCounter* counter = g_counter.counters[index];
        int32_t value = 0;

        if (counter == NULL || counter->owner == 0U)
        {
            continue;
        }
        if (g_counter.port.read(g_counter.port.context, counter->unit, &value) != HW_STATUS_OK)
        {
            hw_host_system_refuse(stream, HW_STATUS_ERROR, "a counter cannot be read");
            return;
        }
        hw_host_system_put32(stream, index + 1U);
        hw_host_system_put32(stream, counter->owner);
        hw_host_system_put_bytes(stream, counter->name, counter->name_length);
        hw_host_system_put32(stream, counter->mode);
        hw_host_system_put32(stream, counter->filter_ns);
        hw_host_system_put32(stream, (uint32_t)value);
        hw_host_system_put32(stream, (uint32_t)counter->low);
        hw_host_system_put32(stream, (uint32_t)counter->high);
        hw_host_system_put32(stream, counter->watching);
        hw_host_system_put32(stream, counter->outside);
        hw_host_system_put32(stream, counter->last_poll_ms);
    }
}

static void hw_counter_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_COUNTER_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        char name[HW_COUNTER_NAME_MAX];
        uint32_t old_id = hw_host_system_get32(stream);
        uint32_t owner = hw_host_system_get32(stream);
        uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
        uint32_t mode = hw_host_system_get32(stream);
        uint32_t filter_ns = hw_host_system_get32(stream);
        int32_t value = (int32_t)hw_host_system_get32(stream);
        int32_t low = (int32_t)hw_host_system_get32(stream);
        int32_t high = (int32_t)hw_host_system_get32(stream);
        uint8_t watching = (uint8_t)hw_host_system_get_max(stream, 1U);
        uint8_t outside = (uint8_t)hw_host_system_get_max(stream, 1U);
        uint32_t last_poll_ms = hw_host_system_get32(stream);
        HwCounter* counter = NULL;
        uint32_t id = 0U;
        uint32_t index = 0U;
        HwStatus status = HW_STATUS_OK;

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        for (index = 0U; index < HW_COUNTER_MAX; ++index)
        {
            HwCounter* entry = g_counter.counters[index];
            if (counter == NULL && entry != NULL && entry->name_length == length &&
                memcmp(entry->name, name, length) == 0)
            {
                counter = entry;
                id = index + 1U;
            }
        }
        if (counter == NULL || !g_counter.has_port)
        {
            hw_host_system_device(stream, &hw_counter_driver, old_id, 0U, NULL);
            continue;
        }
        if (counter->owner != 0U || owner == 0U || low > high)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        status = hw_counter_mode_fits(counter->info.kind, mode)
            ? g_counter.port.start(g_counter.port.context, counter->unit, mode, filter_ns, &counter->filter_ns)
            : HW_STATUS_UNSUPPORTED;
        if (status == HW_STATUS_OK)
        {
            status = g_counter.port.write(g_counter.port.context, counter->unit, value);
            if (status != HW_STATUS_OK)
            {
                g_counter.port.stop(g_counter.port.context, counter->unit);
            }
        }
        /* The window as the program set it; a port that watches is told again. */
        if (status == HW_STATUS_OK && watching && g_counter.port.watch != NULL)
        {
            status = g_counter.port.watch(g_counter.port.context, counter->unit, low, high);
            if (status != HW_STATUS_OK)
            {
                g_counter.port.stop(g_counter.port.context, counter->unit);
            }
        }
        if (status != HW_STATUS_OK)
        {
            hw_host_system_device(stream, &hw_counter_driver, old_id, 0U, "a device it held cannot be set up");
            continue;
        }
        counter->owner = owner;
        counter->mode = mode;
        counter->low = low;
        counter->high = high;
        counter->watching = watching;
        counter->outside = outside;
        /* Polled at the same moments as before the sleep. */
        counter->last_poll_ms = last_poll_ms;
        (void)HW_COUNTER_FLAG_TAKE(counter->event);
        hw_host_system_device(stream, &hw_counter_driver, old_id, id, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The counter interface for programs
 * ------------------------------------------------------------------------ */

typedef struct HwAbiCounterOpen
{
    uint32_t mode;
    uint32_t filter_ns;
} HwAbiCounterOpen;

static HwStatus hw_counter_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    const HwAbiCounterOpen* open = (const HwAbiCounterOpen*)context;
    return hw_counter_open(name, length, open->mode, open->filter_ns, out_id);
}

static HW_HOST_FUNCTION(hw_counter_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, name_address);
    HW_HOST_ARG(uint32_t, name_length);
    HW_HOST_ARG(uint32_t, mode);
    HW_HOST_ARG(uint32_t, filter_ns);
    HwAbiCounterOpen open = { mode, filter_ns };
    HW_ABI_UNUSED();

    HW_HOST_RETURN(hw_driver_open(&hw_counter_driver, name_address, name_length, hw_counter_call_opener, &open));
}

/* The count is signed, so it goes to memory: 4 bytes, little-endian. */
static HW_HOST_FUNCTION(hw_counter_call_read)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, out_address);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_counter_driver, handle, &id);
    int32_t count = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (hw_host_guest_check(out_address, sizeof(count)) != HW_STATUS_OK)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_counter_read(id, &count);
    if (status == HW_STATUS_OK)
    {
        (void)hw_host_guest_write(out_address, &count, sizeof(count));
    }
    HW_HOST_RETURN(hw_abi_error(status));
}

static HW_HOST_FUNCTION(hw_counter_call_set)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(int32_t, count);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_counter_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_counter_write(id, count)));
}

static HW_HOST_FUNCTION(hw_counter_call_watch)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(int32_t, low);
    HW_HOST_ARG(int32_t, high);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_counter_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_counter_watch(id, low, high)));
}

static HW_HOST_FUNCTION(hw_counter_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_counter_driver, handle, &id);
    uint32_t value = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    status = hw_counter_get(id, property, &value);
    HW_HOST_RETURN(hw_driver_property(status, value));
}

static const HwDriverCall hw_counter_calls[] =
{
    { "open", "i(iiii)", hw_counter_call_open, 7U },
    { "read", "i(ii)", hw_counter_call_read, 7U },
    { "set", "i(ii)", hw_counter_call_set, 7U },
    { "watch", "i(iii)", hw_counter_call_watch, 7U },
    { "get", "i(ii)", hw_counter_call_get, 7U },
};

const HwDriver hw_counter_driver =
{
    .interface = "counter",
    .calls = hw_counter_calls,
    .call_count = (uint32_t)(sizeof(hw_counter_calls) / sizeof(hw_counter_calls[0])),
    .close = hw_counter_close,
    .ready = hw_counter_ready,
    .poll = hw_counter_poll,
    .next_deadline_ms = hw_counter_next_deadline_ms,
    .shutdown = hw_counter_shutdown,
    .save = hw_counter_save,
    .restore = hw_counter_restore,
};

