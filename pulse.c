/*
 * Pulse trains: timed levels on GPIO pins, sent or captured by hardware made
 * for it. The pin is owned as an opened GPIO; the work may run in the
 * background on the program's own buffers, which the kernel then protects
 * the same way as for background bus transfers.
 */

#include "os_internal.h"

#if OS_HAS_PULSE

#include <stdlib.h>
#include <string.h>

/* The end of background work, recorded by interrupt handlers and taken by the kernel. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint8_t OsPulseFlag;
#define OS_PULSE_FLAG_SET(flag) ((flag) = 1U)
#define OS_PULSE_FLAG_TAKE(flag) os_pulse_flag_take(&(flag))
static uint8_t os_pulse_flag_take(OsPulseFlag* flag)
{
    uint8_t value = *flag;
    *flag = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uchar OsPulseFlag;
#define OS_PULSE_FLAG_SET(flag) atomic_store_explicit(&(flag), 1U, memory_order_release)
#define OS_PULSE_FLAG_TAKE(flag) atomic_exchange_explicit(&(flag), 0U, memory_order_acquire)
#endif

/* Allocated when a program opens the pin. */
typedef struct OsPulseChannel
{
    uint32_t gpio;
    uint32_t pin;
    uint32_t flags;
    uint8_t busy;
    uint32_t task_id;
    OsPulseFlag done;
    volatile OsStatus result;
    volatile uint32_t count;
    /* The program's buffer for a capture, in items; 0 when sending. */
    uint32_t max;
} OsPulseChannel;

static struct
{
    OsPulsePort port;
    uint8_t has_port;
    OsPulseChannel* channels[OS_PULSE_MAX];
} g_pulse;

static OsPulseChannel* os_pulse_slot(uint32_t id)
{
    return id >= 1U && id <= OS_PULSE_MAX ? g_pulse.channels[id - 1U] : NULL;
}

OsStatus os_pulse_port_set(const OsPulsePort* port)
{
    if (port == NULL)
    {
        memset(&g_pulse.port, 0, sizeof(g_pulse.port));
        g_pulse.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->configure == NULL || port->send == NULL || port->finish == NULL || port->release == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_pulse.port = *port;
    g_pulse.has_port = 1U;
    return OS_STATUS_OK;
}

void os_pulse_complete(uint32_t pin, OsStatus status, uint32_t count)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_PULSE_MAX; ++index)
    {
        OsPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL && channel->pin == pin && channel->busy)
        {
            channel->result = status;
            channel->count = count;
            OS_PULSE_FLAG_SET(channel->done);
            return;
        }
    }
}

static void os_pulse_deliver(OsPulseChannel* channel)
{
    channel->busy = 0U;
    /* A port never reports more than the program's buffer holds. */
    if (channel->count > channel->max)
    {
        channel->count = channel->max;
    }
    os_transfer_wait_end(channel->task_id, channel->result, channel->count);
}

/* Make the port end the channel's work now, then deliver it. */
static void os_pulse_finish(OsPulseChannel* channel)
{
    if (!channel->busy)
    {
        return;
    }
    if (!OS_PULSE_FLAG_TAKE(channel->done))
    {
        g_pulse.port.finish(g_pulse.port.context, channel->pin);
        if (!OS_PULSE_FLAG_TAKE(channel->done))
        {
            channel->result = OS_STATUS_BUS_ERROR;
            channel->count = 0U;
        }
    }
    os_pulse_deliver(channel);
}

void os_pulse_poll(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_PULSE_MAX; ++index)
    {
        OsPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL && channel->busy && OS_PULSE_FLAG_TAKE(channel->done))
        {
            os_pulse_deliver(channel);
        }
    }
}

void os_pulse_finish_task(uint32_t task_id)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_PULSE_MAX; ++index)
    {
        OsPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL && channel->busy && channel->task_id == task_id)
        {
            os_pulse_finish(channel);
        }
    }
}

/* What the port did with the work: done here, or the program waits for it (OS_STATUS_PENDING). */
static OsStatus os_pulse_started(OsPulseChannel* channel, OsStatus status)
{
    if (status != OS_STATUS_PENDING)
    {
        return status;
    }
    channel->busy = 1U;
    channel->task_id = os_task_get_id(os_task_get_current());
    if (os_transfer_wait_begin() == OS_STATUS_OK)
    {
        return OS_STATUS_PENDING;
    }
    /* Nobody to wait (a call from the host): finish now. */
    os_pulse_finish(channel);
    return channel->result;
}

OsStatus os_pulse_open(const char* name, uint32_t length, uint32_t flags, uint32_t* out_id)
{
    OsPulseChannel* channel = NULL;
    uint32_t free_index = OS_PULSE_MAX;
    uint32_t index = 0U;
    OsStatus status = OS_STATUS_OK;
    uint8_t receive = (flags & OS_PULSE_RECEIVE) != 0U;

    if (!g_pulse.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if ((flags & ~(OS_PULSE_RECEIVE | OS_PULSE_IDLE_HIGH)) != 0U || (receive && (flags & OS_PULSE_IDLE_HIGH) != 0U))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (receive && g_pulse.port.receive == NULL)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < OS_PULSE_MAX && free_index == OS_PULSE_MAX; ++index)
    {
        if (g_pulse.channels[index] == NULL)
        {
            free_index = index;
        }
    }
    if (free_index == OS_PULSE_MAX)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    channel = (OsPulseChannel*)calloc(1U, sizeof(OsPulseChannel));
    if (channel == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    /* Physical levels: the hardware sends the program's items as they are. */
    status = os_gpio_open_capable(name, length, OS_GPIO_CAN_PULSE,
                                  receive ? 0U : OS_GPIO_OUTPUT | ((flags & OS_PULSE_IDLE_HIGH) != 0U ? OS_GPIO_INIT_HIGH : 0U),
                                  &channel->gpio);
    if (status == OS_STATUS_OK)
    {
        channel->pin = os_gpio_pin_number(channel->gpio);
        channel->flags = flags;
        status = g_pulse.port.configure(g_pulse.port.context, channel->pin, flags);
        if (status != OS_STATUS_OK)
        {
            os_gpio_close(channel->gpio);
        }
    }
    if (status != OS_STATUS_OK)
    {
        free(channel);
        return status;
    }
    g_pulse.channels[free_index] = channel;
    *out_id = free_index + 1U;
    return OS_STATUS_OK;
}

void os_pulse_close(uint32_t id)
{
    OsPulseChannel* channel = os_pulse_slot(id);

    if (channel == NULL)
    {
        return;
    }
    os_pulse_finish(channel);
    if (g_pulse.has_port)
    {
        g_pulse.port.release(g_pulse.port.context, channel->pin);
    }
    os_gpio_close(channel->gpio);
    g_pulse.channels[id - 1U] = NULL;
    free(channel);
}

static OsStatus os_pulse_ready(OsPulseChannel* channel, uint8_t receive)
{
    if (channel == NULL || ((channel->flags & OS_PULSE_RECEIVE) != 0U) != receive)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    return g_pulse.has_port ? OS_STATUS_OK : OS_STATUS_UNSUPPORTED;
}

OsStatus os_pulse_send(uint32_t id, const uint32_t* items, uint32_t count, uint32_t carrier_hz)
{
    OsPulseChannel* channel = os_pulse_slot(id);
    OsStatus status = os_pulse_ready(channel, 0U);

    if (status != OS_STATUS_OK || count == 0U)
    {
        return status;
    }
    channel->count = 0U;
    channel->max = 0U;
    return os_pulse_started(channel, g_pulse.port.send(g_pulse.port.context, channel->pin, items, count, carrier_hz));
}

OsStatus os_pulse_send_bits(uint32_t id, const uint8_t* data, uint32_t length, const uint32_t timing[4])
{
    OsPulseChannel* channel = os_pulse_slot(id);
    OsStatus status = os_pulse_ready(channel, 0U);
    uint32_t index = 0U;

    if (status != OS_STATUS_OK)
    {
        return status;
    }
    for (index = 0U; index < 4U; ++index)
    {
        if (timing[index] == 0U || timing[index] > OS_PULSE_TIME_MASK)
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
    }
    if (g_pulse.port.send_bits == NULL)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (length == 0U)
    {
        return OS_STATUS_OK;
    }
    channel->count = 0U;
    channel->max = 0U;
    return os_pulse_started(channel, g_pulse.port.send_bits(g_pulse.port.context, channel->pin, data, length, timing));
}

OsStatus os_pulse_receive(uint32_t id, uint32_t* items, uint32_t max, uint32_t idle_ns, uint32_t timeout_ms,
                          uint32_t* out_count)
{
    OsPulseChannel* channel = os_pulse_slot(id);
    OsStatus status = os_pulse_ready(channel, 1U);
    uint32_t count = 0U;

    *out_count = 0U;
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (max == 0U || idle_ns == 0U || idle_ns > OS_PULSE_TIME_MASK)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    channel->count = 0U;
    channel->max = max;
    status = os_pulse_started(channel, g_pulse.port.receive(g_pulse.port.context, channel->pin, items, max, idle_ns,
                                                            timeout_ms, &count));
    /* Done at once, here or by finish for a call without a task. */
    *out_count = status == OS_STATUS_OK ? (channel->count != 0U ? channel->count : (count < max ? count : max)) : 0U;
    return status;
}

OsStatus os_pulse_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsPulseChannel* channel = os_pulse_slot(id);

    if (channel == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (property == OS_PULSE_PROP_RESOLUTION)
    {
        *out_value = g_pulse.port.resolution_ns;
        return OS_STATUS_OK;
    }
    return OS_STATUS_UNSUPPORTED;
}

void os_pulse_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_PULSE_MAX; ++index)
    {
        free(g_pulse.channels[index]);
    }
    memset(&g_pulse, 0, sizeof(g_pulse));
}

#endif
