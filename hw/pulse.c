/*
 * Pulse trains: timed levels on GPIO pins, sent or captured by hardware made
 * for it. The pin is owned as an opened GPIO; the work may run in the
 * background on the program's own buffers, which the library then protects
 * the same way as for background bus transfers.
 */

#include "hw_driver.h"

#include <stdlib.h>
#include <string.h>

/* The end of background work, recorded by interrupt handlers and taken by the library. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint8_t HwPulseFlag;
#define HW_PULSE_FLAG_SET(flag) ((flag) = 1U)
#define HW_PULSE_FLAG_TAKE(flag) hw_pulse_flag_take(&(flag))
static uint8_t hw_pulse_flag_take(HwPulseFlag* flag)
{
    uint8_t value = *flag;
    *flag = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uchar HwPulseFlag;
#define HW_PULSE_FLAG_SET(flag) atomic_store_explicit(&(flag), 1U, memory_order_release)
#define HW_PULSE_FLAG_TAKE(flag) atomic_exchange_explicit(&(flag), 0U, memory_order_acquire)
#endif

/* Allocated when a program opens the pin. */
typedef struct HwPulseChannel
{
    uint32_t gpio;
    uint32_t pin;
    uint32_t flags;
    uint8_t busy;
    uint32_t task_id;
    HwPulseFlag done;
    volatile HwStatus result;
    volatile uint32_t count;
    /* The program's buffer for a capture, in items; 0 when sending. */
    uint32_t max;
    /* Copies of the program's buffers the port works on, written back when it is done. */
    HwGuestKept* kept;
} HwPulseChannel;

static struct
{
    HwPulsePort port;
    uint8_t has_port;
    HwPulseChannel* channels[HW_PULSE_MAX];
} g_pulse;

static HwPulseChannel* hw_pulse_slot(uint32_t id)
{
    return id >= 1U && id <= HW_PULSE_MAX ? g_pulse.channels[id - 1U] : NULL;
}

extern const HwDriver hw_pulse_driver;

HwStatus hw_pulse_port_set(const HwPulsePort* port)
{
    if (port == NULL)
    {
        memset(&g_pulse.port, 0, sizeof(g_pulse.port));
        g_pulse.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->configure == NULL || port->send == NULL || port->finish == NULL || port->release == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_pulse.port = *port;
    g_pulse.has_port = 1U;
    /* A platform with a pulse port gives programs the pulse interface. */
    if (hw_driver_add(&hw_pulse_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

void hw_pulse_complete(uint32_t pin, HwStatus status, uint32_t count)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_PULSE_MAX; ++index)
    {
        HwPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL && channel->pin == pin && channel->busy)
        {
            channel->result = status;
            channel->count = count;
            HW_PULSE_FLAG_SET(channel->done);
            return;
        }
    }
}

static void hw_pulse_deliver(HwPulseChannel* channel)
{
    channel->busy = 0U;
    /* A port never reports more than the program's buffer holds. */
    if (channel->count > channel->max)
    {
        channel->count = channel->max;
    }
    (void)hw_host_kept_close(&channel->kept);
    hw_host_transfer_wait_end(channel->task_id, channel->result, channel->count);
}

/* Make the port end the channel's work now, then deliver it. */
static void hw_pulse_finish(HwPulseChannel* channel)
{
    if (!channel->busy)
    {
        return;
    }
    if (!HW_PULSE_FLAG_TAKE(channel->done))
    {
        g_pulse.port.finish(g_pulse.port.context, channel->pin);
        if (!HW_PULSE_FLAG_TAKE(channel->done))
        {
            channel->result = HW_STATUS_BUS_ERROR;
            channel->count = 0U;
        }
    }
    hw_pulse_deliver(channel);
}

void hw_pulse_poll(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_PULSE_MAX; ++index)
    {
        HwPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL && channel->busy && HW_PULSE_FLAG_TAKE(channel->done))
        {
            hw_pulse_deliver(channel);
        }
    }
}

void hw_pulse_finish_task(uint32_t task_id)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_PULSE_MAX; ++index)
    {
        HwPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL && channel->busy && channel->task_id == task_id)
        {
            hw_pulse_finish(channel);
        }
    }
}

/* What the port did with the work: done here, or the program waits for it (HW_STATUS_PENDING). */
static HwStatus hw_pulse_started(HwPulseChannel* channel, HwStatus status)
{
    if (status != HW_STATUS_PENDING)
    {
        return status;
    }
    channel->busy = 1U;
    channel->task_id = hw_host_task_id();
    if (hw_host_transfer_wait_begin() == HW_STATUS_OK)
    {
        return HW_STATUS_PENDING;
    }
    /* Nobody to wait (a call from the host): finish now. */
    hw_pulse_finish(channel);
    return channel->result;
}

HwStatus hw_pulse_open(const char* name, uint32_t length, uint32_t flags, uint32_t* out_id)
{
    HwPulseChannel* channel = NULL;
    uint32_t free_index = HW_PULSE_MAX;
    uint32_t index = 0U;
    HwStatus status = HW_STATUS_OK;
    uint8_t receive = (flags & HW_PULSE_RECEIVE) != 0U;

    if (!g_pulse.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if ((flags & ~(HW_PULSE_RECEIVE | HW_PULSE_IDLE_HIGH)) != 0U || (receive && (flags & HW_PULSE_IDLE_HIGH) != 0U))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (receive && g_pulse.port.receive == NULL)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < HW_PULSE_MAX && free_index == HW_PULSE_MAX; ++index)
    {
        if (g_pulse.channels[index] == NULL)
        {
            free_index = index;
        }
    }
    if (free_index == HW_PULSE_MAX)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    channel = (HwPulseChannel*)calloc(1U, sizeof(HwPulseChannel));
    if (channel == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    /* Physical levels: the hardware sends the program's items as they are. */
    status = hw_gpio_open_capable(name, length, HW_GPIO_CAN_PULSE,
                                  receive ? 0U : HW_GPIO_OUTPUT | ((flags & HW_PULSE_IDLE_HIGH) != 0U ? HW_GPIO_INIT_HIGH : 0U),
                                  &channel->gpio);
    if (status == HW_STATUS_OK)
    {
        channel->pin = hw_gpio_pin_number(channel->gpio);
        channel->flags = flags;
        status = g_pulse.port.configure(g_pulse.port.context, channel->pin, flags);
        if (status != HW_STATUS_OK)
        {
            hw_gpio_close(channel->gpio);
        }
    }
    if (status != HW_STATUS_OK)
    {
        free(channel);
        return status;
    }
    g_pulse.channels[free_index] = channel;
    *out_id = free_index + 1U;
    return HW_STATUS_OK;
}

void hw_pulse_close(uint32_t id)
{
    HwPulseChannel* channel = hw_pulse_slot(id);

    if (channel == NULL)
    {
        return;
    }
    hw_pulse_finish(channel);
    if (g_pulse.has_port)
    {
        g_pulse.port.release(g_pulse.port.context, channel->pin);
    }
    hw_gpio_close(channel->gpio);
    g_pulse.channels[id - 1U] = NULL;
    free(channel);
}

static HwStatus hw_pulse_ready(HwPulseChannel* channel, uint8_t receive)
{
    if (channel == NULL || ((channel->flags & HW_PULSE_RECEIVE) != 0U) != receive)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    return g_pulse.has_port ? HW_STATUS_OK : HW_STATUS_UNSUPPORTED;
}

HwStatus hw_pulse_send(uint32_t id, const uint32_t* items, uint32_t count, uint32_t carrier_hz)
{
    HwPulseChannel* channel = hw_pulse_slot(id);
    HwStatus status = hw_pulse_ready(channel, 0U);

    if (status != HW_STATUS_OK || count == 0U)
    {
        return status;
    }
    channel->count = 0U;
    channel->max = 0U;
    return hw_pulse_started(channel, g_pulse.port.send(g_pulse.port.context, channel->pin, items, count, carrier_hz));
}

HwStatus hw_pulse_send_bits(uint32_t id, const uint8_t* data, uint32_t length, const uint32_t timing[4])
{
    HwPulseChannel* channel = hw_pulse_slot(id);
    HwStatus status = hw_pulse_ready(channel, 0U);
    uint32_t index = 0U;

    if (status != HW_STATUS_OK)
    {
        return status;
    }
    for (index = 0U; index < 4U; ++index)
    {
        if (timing[index] == 0U || timing[index] > HW_PULSE_TIME_MASK)
        {
            return HW_STATUS_INVALID_ARGUMENT;
        }
    }
    if (g_pulse.port.send_bits == NULL)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (length == 0U)
    {
        return HW_STATUS_OK;
    }
    channel->count = 0U;
    channel->max = 0U;
    return hw_pulse_started(channel, g_pulse.port.send_bits(g_pulse.port.context, channel->pin, data, length, timing));
}

HwStatus hw_pulse_receive(uint32_t id, uint32_t* items, uint32_t max, uint32_t idle_ns, uint32_t timeout_ms,
                          uint32_t* out_count)
{
    HwPulseChannel* channel = hw_pulse_slot(id);
    HwStatus status = hw_pulse_ready(channel, 1U);
    uint32_t count = 0U;

    *out_count = 0U;
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    if (max == 0U || idle_ns == 0U || idle_ns > HW_PULSE_TIME_MASK)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    channel->count = 0U;
    channel->max = max;
    status = hw_pulse_started(channel, g_pulse.port.receive(g_pulse.port.context, channel->pin, items, max, idle_ns,
                                                            timeout_ms, &count));
    /* Done at once, here or by finish for a call without a task. */
    *out_count = status == HW_STATUS_OK ? (channel->count != 0U ? channel->count : (count < max ? count : max)) : 0U;
    return status;
}

HwStatus hw_pulse_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwPulseChannel* channel = hw_pulse_slot(id);

    if (channel == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (property == HW_PULSE_PROP_RESOLUTION)
    {
        *out_value = g_pulse.port.resolution_ns;
        return HW_STATUS_OK;
    }
    return HW_STATUS_UNSUPPORTED;
}

void hw_pulse_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_PULSE_MAX; ++index)
    {
        free(g_pulse.channels[index]);
    }
    memset(&g_pulse, 0, sizeof(g_pulse));
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: each channel in its slot, on its pin, with its
 * direction and idle level
 * ------------------------------------------------------------------------ */

static void hw_pulse_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_PULSE_MAX; ++index)
    {
        const HwPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL && channel->busy)
        {
            hw_host_system_refuse(stream, HW_STATUS_BUSY, "a pulse train is on its way: try after the next slice");
            return;
        }
        count += channel != NULL ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_PULSE_MAX; ++index)
    {
        const HwPulseChannel* channel = g_pulse.channels[index];
        if (channel != NULL)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put32(stream, channel->gpio);
            hw_host_system_put32(stream, channel->flags);
        }
    }
}

static void hw_pulse_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_PULSE_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        uint32_t id = hw_host_system_get_max(stream, HW_PULSE_MAX);
        uint32_t old_gpio = hw_host_system_get32(stream);
        uint32_t flags = hw_host_system_get32(stream);
        const char* gone = NULL;
        uint32_t gpio = 0U;
        HwPulseChannel* channel = NULL;

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        if (id == 0U || g_pulse.channels[id - 1U] != NULL)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        /* Its pin, which gpio restored first. */
        gpio = hw_host_system_device_id(stream, &hw_gpio_driver, old_gpio, &gone);
        if (gpio == 0U || !g_pulse.has_port)
        {
            hw_host_system_device(stream, &hw_pulse_driver, id, 0U, gone);
            continue;
        }
        channel = (HwPulseChannel*)calloc(1U, sizeof(HwPulseChannel));
        if (channel == NULL)
        {
            hw_host_system_fail(stream, HW_STATUS_OUT_OF_MEMORY);
            return;
        }
        channel->gpio = gpio;
        channel->pin = hw_gpio_pin_number(gpio);
        channel->flags = flags;
        if (g_pulse.port.configure(g_pulse.port.context, channel->pin, flags) != HW_STATUS_OK)
        {
            hw_gpio_close(gpio);
            free(channel);
            hw_host_system_device(stream, &hw_pulse_driver, id, 0U, "a device it held cannot be set up");
            continue;
        }
        g_pulse.channels[id - 1U] = channel;
        hw_host_system_device(stream, &hw_pulse_driver, id, id, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The pulse interface for programs
 * ------------------------------------------------------------------------ */

static HwStatus hw_pulse_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    return hw_pulse_open(name, length, *(const uint32_t*)context, out_id);
}

static HW_HOST_FUNCTION(hw_pulse_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, name_address);
    HW_HOST_ARG(uint32_t, name_length);
    HW_HOST_ARG(uint32_t, flags);
    HW_ABI_UNUSED();

    HW_HOST_RETURN(hw_driver_open(&hw_pulse_driver, name_address, name_length, hw_pulse_call_opener, &flags));
}

/* Items are 32-bit words the port works on in place: they must be aligned. */
static int32_t hw_pulse_guest_items(uint32_t address, uint32_t count, uint8_t access, HwGuestBuffer* out_items)
{
    HwStatus status = HW_STATUS_OK;

    if (count > 0x3FFFFFFFU || (address & 3U) != 0U)
    {
        return count > 0x3FFFFFFFU ? -WR_EFAULT : -WR_EINVAL;
    }
    status = hw_host_buffer_open(out_items, address, count * 4U, access);
    return status == HW_STATUS_OK ? 0 : HW_GUEST_BUFFER_ERROR(status);
}

/*
 * The port is done with a program's buffer (status), or works on in the
 * background (HW_STATUS_PENDING): then a copy waits in the channel until
 * the end writes it back.
 */
static void hw_pulse_buffer_end(uint32_t id, HwGuestBuffer* buffer, HwStatus status)
{
    HwPulseChannel* channel = hw_pulse_slot(id);

    if (status == HW_STATUS_PENDING && channel != NULL)
    {
        hw_host_buffer_keep(buffer, &channel->kept);
    }
    else
    {
        (void)hw_host_buffer_close(buffer);
    }
}

static HW_HOST_FUNCTION(hw_pulse_call_send)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, items_address);
    HW_HOST_ARG(uint32_t, count);
    HW_HOST_ARG(uint32_t, carrier_hz);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_pulse_driver, handle, &id);
    HwGuestBuffer items;
    int32_t result = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    result = hw_pulse_guest_items(items_address, count, HW_GUEST_IN, &items);
    if (result < 0)
    {
        HW_HOST_RETURN(result);
    }
    if (hw_host_guest_begin((uint32_t*)raw_return) != HW_STATUS_OK)
    {
        hw_host_buffer_drop(&items);
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_pulse_send(id, (const uint32_t*)(void*)items.data, count, carrier_hz);
    hw_pulse_buffer_end(id, &items, status);
    *raw_return = hw_abi_error(status);
    return hw_host_guest_end();
}

static HW_HOST_FUNCTION(hw_pulse_call_send_bits)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, data_address);
    HW_HOST_ARG(uint32_t, length);
    HW_HOST_ARG(uint32_t, zero_high_ns);
    HW_HOST_ARG(uint32_t, zero_low_ns);
    HW_HOST_ARG(uint32_t, one_high_ns);
    HW_HOST_ARG(uint32_t, one_low_ns);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_pulse_driver, handle, &id);
    uint32_t timing[4] = { zero_high_ns, zero_low_ns, one_high_ns, one_low_ns };
    HwGuestBuffer data;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (length > 0x7FFFFFFFU)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_host_buffer_open(&data, data_address, length, HW_GUEST_IN);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(HW_GUEST_BUFFER_ERROR(status));
    }
    if (hw_host_guest_begin((uint32_t*)raw_return) != HW_STATUS_OK)
    {
        hw_host_buffer_drop(&data);
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_pulse_send_bits(id, data.data, length, timing);
    hw_pulse_buffer_end(id, &data, status);
    *raw_return = hw_abi_error(status);
    return hw_host_guest_end();
}

static HW_HOST_FUNCTION(hw_pulse_call_receive)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, items_address);
    HW_HOST_ARG(uint32_t, max);
    HW_HOST_ARG(uint32_t, idle_ns);
    HW_HOST_ARG(uint32_t, timeout_ms);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_pulse_driver, handle, &id);
    HwGuestBuffer items;
    uint32_t count = 0U;
    int32_t result = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    result = hw_pulse_guest_items(items_address, max, HW_GUEST_OUT, &items);
    if (result < 0)
    {
        HW_HOST_RETURN(result);
    }
    if (hw_host_guest_begin((uint32_t*)raw_return) != HW_STATUS_OK)
    {
        hw_host_buffer_drop(&items);
        HW_HOST_RETURN(-WR_EFAULT);
    }
    /* Done at once: the count now; in the background, the wakeup writes it. */
    status = hw_pulse_receive(id, (uint32_t*)(void*)items.data, max, idle_ns, timeout_ms, &count);
    hw_pulse_buffer_end(id, &items, status);
    *raw_return = status == HW_STATUS_OK ? (int32_t)count : hw_abi_error(status);
    return hw_host_guest_end();
}

static HW_HOST_FUNCTION(hw_pulse_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_pulse_driver, handle, &id);
    uint32_t value = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    status = hw_pulse_get(id, property, &value);
    HW_HOST_RETURN(hw_driver_property(status, value));
}

static const HwDriverCall hw_pulse_calls[] =
{
    { "open", "i(iii)", hw_pulse_call_open, 5U },
    { "send", "i(iiii)", hw_pulse_call_send, 5U },
    { "send-bits", "i(iiiiiii)", hw_pulse_call_send_bits, 5U },
    { "receive", "i(iiiii)", hw_pulse_call_receive, 5U },
    { "get", "i(ii)", hw_pulse_call_get, 5U },
};

const HwDriver hw_pulse_driver =
{
    .interface = "pulse",
    .calls = hw_pulse_calls,
    .call_count = (uint32_t)(sizeof(hw_pulse_calls) / sizeof(hw_pulse_calls[0])),
    .close = hw_pulse_close,
    .poll = hw_pulse_poll,
    .finish_task = hw_pulse_finish_task,
    .shutdown = hw_pulse_shutdown,
    .save = hw_pulse_save,
    .restore = hw_pulse_restore,
};

