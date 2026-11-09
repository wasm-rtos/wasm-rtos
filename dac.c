/*
 * Analog outputs: channels the platform registers by name, one owner each.
 * A channel named like a GPIO pin takes that pin while it is open.
 */

#include "os_internal.h"

#if OS_HAS_DAC

#include <stdlib.h>
#include <string.h>

/* Allocated when the platform registers the channel. */
typedef struct OsDacChannel
{
    uint32_t channel;
    char name[OS_DAC_NAME_MAX + 1U];
    uint8_t name_length;
    OsDacInfo info;
    /* Owning task id, 0 when free. */
    uint32_t owner;
    /* The GPIO pin it took, 0 when the output is no GPIO pin. */
    uint32_t gpio;
    uint32_t value;
} OsDacChannel;

static struct
{
    OsDacPort port;
    uint8_t has_port;
    OsDacChannel* channels[OS_DAC_MAX];
} g_dac;

static OsDacChannel* os_dac_slot(uint32_t id)
{
    OsDacChannel* channel = id >= 1U && id <= OS_DAC_MAX ? g_dac.channels[id - 1U] : NULL;
    return channel != NULL && channel->owner != 0U ? channel : NULL;
}

static uint32_t os_dac_max_raw(const OsDacChannel* channel)
{
    return (1U << channel->info.resolution_bits) - 1U;
}

OsStatus os_dac_port_set(const OsDacPort* port)
{
    if (port == NULL)
    {
        memset(&g_dac.port, 0, sizeof(g_dac.port));
        g_dac.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->write == NULL || port->stop == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_dac.port = *port;
    g_dac.has_port = 1U;
    return OS_STATUS_OK;
}

OsStatus os_dac_add_channel(const char* name, uint32_t channel, const OsDacInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsDacChannel* slot = NULL;
    uint32_t free_index = OS_DAC_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > OS_DAC_NAME_MAX || info == NULL || info->resolution_bits == 0U ||
        info->resolution_bits > 24U || info->full_scale_mv == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_DAC_MAX; ++index)
    {
        OsDacChannel* entry = g_dac.channels[index];
        if (entry != NULL && (entry->channel == channel ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (entry == NULL && free_index == OS_DAC_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == OS_DAC_MAX)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot = (OsDacChannel*)calloc(1U, sizeof(OsDacChannel));
    if (slot == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    slot->channel = channel;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    g_dac.channels[free_index] = slot;
    return OS_STATUS_OK;
}

uint32_t os_dac_get_owner(uint32_t channel)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_DAC_MAX; ++index)
    {
        if (g_dac.channels[index] != NULL && g_dac.channels[index]->channel == channel)
        {
            return g_dac.channels[index]->owner;
        }
    }
    return 0U;
}

OsStatus os_dac_open(const char* name, uint32_t length, uint32_t* out_id)
{
    OsDacChannel* channel = NULL;
    uint8_t any = 0U;
    uint32_t id = 0U;
    uint32_t index = 0U;
    OsStatus status = OS_STATUS_OK;

    for (index = 0U; index < OS_DAC_MAX; ++index)
    {
        OsDacChannel* entry = g_dac.channels[index];
        any |= entry != NULL;
        if (channel == NULL && entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
        {
            channel = entry;
            id = index + 1U;
        }
    }
    if (!g_dac.has_port || (channel == NULL && !any))
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (channel == NULL)
    {
        return OS_STATUS_FILE_NOT_FOUND;
    }
    if (channel->owner != 0U)
    {
        return OS_STATUS_BUSY;
    }
    channel->gpio = 0U;
    /* The output is a GPIO pin: take it, so no program drives it digitally meanwhile. */
    status = os_gpio_open(name, length, 0U, &channel->gpio);
    if (status == OS_STATUS_FILE_NOT_FOUND || status == OS_STATUS_UNSUPPORTED)
    {
        channel->gpio = 0U;
        status = OS_STATUS_OK;
    }
    if (status != OS_STATUS_OK)
    {
        channel->gpio = 0U;
        return status;
    }
    if (g_dac.port.start != NULL)
    {
        status = g_dac.port.start(g_dac.port.context, channel->channel);
    }
    if (status == OS_STATUS_OK)
    {
        status = g_dac.port.write(g_dac.port.context, channel->channel, 0U);
    }
    if (status != OS_STATUS_OK)
    {
        if (channel->gpio != 0U)
        {
            os_gpio_close(channel->gpio);
            channel->gpio = 0U;
        }
        return status;
    }
    channel->value = 0U;
    channel->owner = os_task_get_id(os_task_get_current());
    *out_id = id;
    return OS_STATUS_OK;
}

void os_dac_close(uint32_t id)
{
    OsDacChannel* channel = os_dac_slot(id);

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
        os_gpio_close(channel->gpio);
    }
    channel->gpio = 0U;
    channel->owner = 0U;
    channel->value = 0U;
}

OsStatus os_dac_write(uint32_t id, uint32_t raw)
{
    OsDacChannel* channel = os_dac_slot(id);
    OsStatus status = OS_STATUS_OK;

    if (channel == NULL || raw > os_dac_max_raw(channel))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_dac.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = g_dac.port.write(g_dac.port.context, channel->channel, raw);
    if (status == OS_STATUS_OK)
    {
        channel->value = raw;
    }
    return status;
}

/* The nearest raw value on the channel's scale. */
OsStatus os_dac_write_mv(uint32_t id, uint32_t mv)
{
    OsDacChannel* channel = os_dac_slot(id);
    uint64_t max = 0U;

    if (channel == NULL || mv > channel->info.full_scale_mv)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    max = os_dac_max_raw(channel);
    return os_dac_write(id, (uint32_t)(((uint64_t)mv * max + channel->info.full_scale_mv / 2U) /
                                       channel->info.full_scale_mv));
}

OsStatus os_dac_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsDacChannel* channel = os_dac_slot(id);

    if (channel == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case OS_DAC_PROP_RESOLUTION: *out_value = channel->info.resolution_bits; return OS_STATUS_OK;
        case OS_DAC_PROP_MAX: *out_value = os_dac_max_raw(channel); return OS_STATUS_OK;
        case OS_DAC_PROP_FULL_SCALE: *out_value = channel->info.full_scale_mv; return OS_STATUS_OK;
        case OS_DAC_PROP_VALUE: *out_value = channel->value; return OS_STATUS_OK;
        default: return OS_STATUS_UNSUPPORTED;
    }
}

void os_dac_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_DAC_MAX; ++index)
    {
        free(g_dac.channels[index]);
    }
    memset(&g_dac, 0, sizeof(g_dac));
}

#endif
