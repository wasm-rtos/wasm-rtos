/*
 * Analog outputs: channels the platform registers by name, one owner each.
 * A channel named like a GPIO pin takes that pin while it is open.
 */

#include "hw_driver.h"

#include <stdlib.h>
#include <string.h>

/* Allocated when the platform registers the channel. */
typedef struct HwDacChannel
{
    uint32_t channel;
    char name[HW_DAC_NAME_MAX + 1U];
    uint8_t name_length;
    HwDacInfo info;
    /* Owning task id, 0 when free. */
    uint32_t owner;
    /* The GPIO pin it took, 0 when the output is no GPIO pin. */
    uint32_t gpio;
    uint32_t value;
} HwDacChannel;

static struct
{
    HwDacPort port;
    uint8_t has_port;
    HwDacChannel* channels[HW_DAC_MAX];
} g_dac;

static HwDacChannel* hw_dac_slot(uint32_t id)
{
    HwDacChannel* channel = id >= 1U && id <= HW_DAC_MAX ? g_dac.channels[id - 1U] : NULL;
    return channel != NULL && channel->owner != 0U ? channel : NULL;
}

static uint32_t hw_dac_max_raw(const HwDacChannel* channel)
{
    return (1U << channel->info.resolution_bits) - 1U;
}

HwStatus hw_dac_port_set(const HwDacPort* port)
{
    if (port == NULL)
    {
        memset(&g_dac.port, 0, sizeof(g_dac.port));
        g_dac.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->write == NULL || port->stop == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_dac.port = *port;
    g_dac.has_port = 1U;
    return HW_STATUS_OK;
}

extern const HwDriver hw_dac_driver;

HwStatus hw_dac_add_channel(const char* name, uint32_t channel, const HwDacInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    HwDacChannel* slot = NULL;
    uint32_t free_index = HW_DAC_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > HW_DAC_NAME_MAX || info == NULL || info->resolution_bits == 0U ||
        info->resolution_bits > 24U || info->full_scale_mv == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_DAC_MAX; ++index)
    {
        HwDacChannel* entry = g_dac.channels[index];
        if (entry != NULL && (entry->channel == channel ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return HW_STATUS_BUSY;
        }
        if (entry == NULL && free_index == HW_DAC_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == HW_DAC_MAX)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot = (HwDacChannel*)calloc(1U, sizeof(HwDacChannel));
    if (slot == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    slot->channel = channel;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    g_dac.channels[free_index] = slot;
    /* A platform with these devices gives programs the dac interface. */
    if (hw_driver_add(&hw_dac_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

uint32_t hw_dac_get_owner(uint32_t channel)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_DAC_MAX; ++index)
    {
        if (g_dac.channels[index] != NULL && g_dac.channels[index]->channel == channel)
        {
            return g_dac.channels[index]->owner;
        }
    }
    return 0U;
}

HwStatus hw_dac_open(const char* name, uint32_t length, uint32_t* out_id)
{
    HwDacChannel* channel = NULL;
    uint8_t any = 0U;
    uint32_t id = 0U;
    uint32_t index = 0U;
    HwStatus status = HW_STATUS_OK;

    for (index = 0U; index < HW_DAC_MAX; ++index)
    {
        HwDacChannel* entry = g_dac.channels[index];
        any |= entry != NULL;
        if (channel == NULL && entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
        {
            channel = entry;
            id = index + 1U;
        }
    }
    if (!g_dac.has_port || (channel == NULL && !any))
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (channel == NULL)
    {
        return HW_STATUS_FILE_NOT_FOUND;
    }
    if (channel->owner != 0U)
    {
        return HW_STATUS_BUSY;
    }
    channel->gpio = 0U;
    /* The output is a GPIO pin: take it, so no program drives it digitally meanwhile. */
    status = hw_gpio_open(name, length, 0U, &channel->gpio);
    if (status == HW_STATUS_FILE_NOT_FOUND || status == HW_STATUS_UNSUPPORTED)
    {
        channel->gpio = 0U;
        status = HW_STATUS_OK;
    }
    if (status != HW_STATUS_OK)
    {
        channel->gpio = 0U;
        return status;
    }
    if (g_dac.port.start != NULL)
    {
        status = g_dac.port.start(g_dac.port.context, channel->channel);
    }
    if (status == HW_STATUS_OK)
    {
        status = g_dac.port.write(g_dac.port.context, channel->channel, 0U);
    }
    if (status != HW_STATUS_OK)
    {
        if (channel->gpio != 0U)
        {
            hw_gpio_close(channel->gpio);
            channel->gpio = 0U;
        }
        return status;
    }
    channel->value = 0U;
    channel->owner = hw_host_task_id();
    *out_id = id;
    return HW_STATUS_OK;
}

void hw_dac_close(uint32_t id)
{
    HwDacChannel* channel = hw_dac_slot(id);

    if (channel == NULL)
    {
        return;
    }
    if (g_dac.has_port)
    {
        g_dac.port.stop(g_dac.port.context, channel->channel);
    }
    if (channel->gpio != 0U)
    {
        hw_gpio_close(channel->gpio);
    }
    channel->gpio = 0U;
    channel->owner = 0U;
    channel->value = 0U;
}

HwStatus hw_dac_write(uint32_t id, uint32_t raw)
{
    HwDacChannel* channel = hw_dac_slot(id);
    HwStatus status = HW_STATUS_OK;

    if (channel == NULL || raw > hw_dac_max_raw(channel))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_dac.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = g_dac.port.write(g_dac.port.context, channel->channel, raw);
    if (status == HW_STATUS_OK)
    {
        channel->value = raw;
    }
    return status;
}

/* The nearest raw value on the channel's scale. */
HwStatus hw_dac_write_mv(uint32_t id, uint32_t mv)
{
    HwDacChannel* channel = hw_dac_slot(id);
    uint64_t max = 0U;

    if (channel == NULL || mv > channel->info.full_scale_mv)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    max = hw_dac_max_raw(channel);
    return hw_dac_write(id, (uint32_t)(((uint64_t)mv * max + channel->info.full_scale_mv / 2U) /
                                       channel->info.full_scale_mv));
}

HwStatus hw_dac_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwDacChannel* channel = hw_dac_slot(id);

    if (channel == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case HW_DAC_PROP_RESOLUTION: *out_value = channel->info.resolution_bits; return HW_STATUS_OK;
        case HW_DAC_PROP_MAX: *out_value = hw_dac_max_raw(channel); return HW_STATUS_OK;
        case HW_DAC_PROP_FULL_SCALE: *out_value = channel->info.full_scale_mv; return HW_STATUS_OK;
        case HW_DAC_PROP_VALUE: *out_value = channel->value; return HW_STATUS_OK;
        default: return HW_STATUS_UNSUPPORTED;
    }
}

void hw_dac_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_DAC_MAX; ++index)
    {
        free(g_dac.channels[index]);
    }
    memset(&g_dac, 0, sizeof(g_dac));
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: each output a task holds, by name, with the
 * value written
 * ------------------------------------------------------------------------ */

static void hw_dac_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_DAC_MAX; ++index)
    {
        count += g_dac.channels[index] != NULL && g_dac.channels[index]->owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_DAC_MAX; ++index)
    {
        const HwDacChannel* channel = g_dac.channels[index];
        if (channel != NULL && channel->owner != 0U)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put32(stream, channel->owner);
            hw_host_system_put_bytes(stream, channel->name, channel->name_length);
            hw_host_system_put32(stream, channel->gpio);
            hw_host_system_put32(stream, channel->value);
        }
    }
}

static void hw_dac_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_DAC_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        char name[HW_DAC_NAME_MAX];
        uint32_t old_id = hw_host_system_get32(stream);
        uint32_t owner = hw_host_system_get32(stream);
        uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
        uint32_t old_gpio = hw_host_system_get32(stream);
        uint32_t value = hw_host_system_get32(stream);
        HwDacChannel* channel = NULL;
        const char* gone = NULL;
        uint32_t gpio = 0U;
        uint32_t index = 0U;
        HwStatus status = HW_STATUS_OK;

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        for (index = 0U; index < HW_DAC_MAX && channel == NULL; ++index)
        {
            HwDacChannel* entry = g_dac.channels[index];
            if (entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
            {
                channel = entry;
            }
        }
        if (channel == NULL || !g_dac.has_port)
        {
            hw_host_system_device(stream, &hw_dac_driver, old_id, 0U, NULL);
            continue;
        }
        if (channel->owner != 0U || owner == 0U)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        /* The pin under the output, which gpio restored first. */
        if (old_gpio != 0U)
        {
            gpio = hw_host_system_device_id(stream, &hw_gpio_driver, old_gpio, &gone);
            if (gpio == 0U)
            {
                hw_host_system_device(stream, &hw_dac_driver, old_id, 0U, gone);
                continue;
            }
        }
        if (value > hw_dac_max_raw(channel))
        {
            status = HW_STATUS_INVALID_ARGUMENT;
        }
        if (status == HW_STATUS_OK && g_dac.port.start != NULL)
        {
            status = g_dac.port.start(g_dac.port.context, channel->channel);
        }
        if (status == HW_STATUS_OK)
        {
            status = g_dac.port.write(g_dac.port.context, channel->channel, value);
        }
        if (status != HW_STATUS_OK)
        {
            if (gpio != 0U)
            {
                hw_gpio_close(gpio);
            }
            hw_host_system_device(stream, &hw_dac_driver, old_id, 0U, "a device it held cannot be set up");
            continue;
        }
        channel->gpio = gpio;
        channel->value = value;
        channel->owner = owner;
        hw_host_system_device(stream, &hw_dac_driver, old_id, (uint32_t)(index), NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The dac interface for programs
 * ------------------------------------------------------------------------ */

static HwStatus hw_dac_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    (void)context;
    return hw_dac_open(name, length, out_id);
}

static HW_HOST_FUNCTION(hw_dac_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, name_address);
    HW_HOST_ARG(uint32_t, name_length);
    HW_HOST_ARG(uint32_t, flags);
    HW_ABI_UNUSED();

    if (flags != 0U)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    HW_HOST_RETURN(hw_driver_open(&hw_dac_driver, name_address, name_length, hw_dac_call_opener, NULL));
}

static HW_HOST_FUNCTION(hw_dac_call_write)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, raw);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_dac_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_dac_write(id, raw)));
}

static HW_HOST_FUNCTION(hw_dac_call_write_mv)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, mv);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_dac_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_dac_write_mv(id, mv)));
}

static HW_HOST_FUNCTION(hw_dac_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_dac_driver, handle, &id);
    uint32_t value = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    status = hw_dac_get(id, property, &value);
    HW_HOST_RETURN(hw_driver_property(status, value));
}

static const HwDriverCall hw_dac_calls[] =
{
    { "open", "i(iii)", hw_dac_call_open, 8U },
    { "write", "i(ii)", hw_dac_call_write, 8U },
    { "write-mv", "i(ii)", hw_dac_call_write_mv, 8U },
    { "get", "i(ii)", hw_dac_call_get, 8U },
};

const HwDriver hw_dac_driver =
{
    .interface = "dac",
    .calls = hw_dac_calls,
    .call_count = (uint32_t)(sizeof(hw_dac_calls) / sizeof(hw_dac_calls[0])),
    .close = hw_dac_close,
    .shutdown = hw_dac_shutdown,
    .save = hw_dac_save,
    .restore = hw_dac_restore,
};

