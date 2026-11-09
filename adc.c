/*
 * Analog inputs: channels the platform registers by name. Reading changes
 * nothing, so any number of programs may read a channel.
 */

#include "driver.h"

#include <stdlib.h>
#include <string.h>

/* Allocated when the platform registers the channel. */
typedef struct OsAdcChannel
{
    uint32_t channel;
    char name[OS_ADC_NAME_MAX + 1U];
    uint8_t name_length;
    OsAdcInfo info;
} OsAdcChannel;

static struct
{
    OsAdcPort port;
    uint8_t has_port;
    OsAdcChannel* channels[OS_ADC_MAX];
} g_adc;

static OsAdcChannel* os_adc_slot(uint32_t id)
{
    return id >= 1U && id <= OS_ADC_MAX ? g_adc.channels[id - 1U] : NULL;
}

static uint32_t os_adc_max_raw(const OsAdcChannel* channel)
{
    return channel->info.resolution_bits >= 32U ? UINT32_MAX : (1U << channel->info.resolution_bits) - 1U;
}

OsStatus os_adc_port_set(const OsAdcPort* port)
{
    if (port == NULL)
    {
        memset(&g_adc.port, 0, sizeof(g_adc.port));
        g_adc.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->read == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_adc.port = *port;
    g_adc.has_port = 1U;
    return OS_STATUS_OK;
}

extern const OsDriver os_adc_driver;

OsStatus os_adc_add_channel(const char* name, uint32_t channel, const OsAdcInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsAdcChannel* slot = NULL;
    uint32_t free_index = OS_ADC_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > OS_ADC_NAME_MAX || info == NULL || info->resolution_bits == 0U ||
        info->resolution_bits > 24U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_ADC_MAX; ++index)
    {
        OsAdcChannel* entry = g_adc.channels[index];
        if (entry != NULL && (entry->channel == channel ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (entry == NULL && free_index == OS_ADC_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == OS_ADC_MAX)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot = (OsAdcChannel*)calloc(1U, sizeof(OsAdcChannel));
    if (slot == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    g_adc.channels[free_index] = slot;
    slot->channel = channel;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    /* A platform with these devices gives programs the adc interface. */
    if (os_driver_add(&os_adc_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
}

OsStatus os_adc_open(const char* name, uint32_t length, uint32_t* out_id)
{
    uint8_t any = 0U;
    uint32_t index = 0U;

    if (!g_adc.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < OS_ADC_MAX; ++index)
    {
        OsAdcChannel* entry = g_adc.channels[index];
        any |= entry != NULL;
        if (entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
        {
            *out_id = index + 1U;
            return OS_STATUS_OK;
        }
    }
    return any ? OS_STATUS_FILE_NOT_FOUND : OS_STATUS_UNSUPPORTED;
}

OsStatus os_adc_read(uint32_t id, uint32_t* out_raw)
{
    OsAdcChannel* channel = os_adc_slot(id);
    OsStatus status = OS_STATUS_OK;

    if (channel == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_adc.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = g_adc.port.read(g_adc.port.context, channel->channel, out_raw);
    if (status == OS_STATUS_OK && *out_raw > os_adc_max_raw(channel))
    {
        *out_raw = os_adc_max_raw(channel);
    }
    return status;
}

/* The port's calibrated reading, or the raw value on the channel's scale, rounded. */
OsStatus os_adc_read_mv(uint32_t id, uint32_t* out_mv)
{
    OsAdcChannel* channel = os_adc_slot(id);
    uint32_t raw = 0U;
    OsStatus status = OS_STATUS_OK;

    if (channel == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_adc.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (g_adc.port.read_mv != NULL)
    {
        status = g_adc.port.read_mv(g_adc.port.context, channel->channel, out_mv);
        if (status != OS_STATUS_UNSUPPORTED)
        {
            return status;
        }
    }
    status = os_adc_read(id, &raw);
    if (status == OS_STATUS_OK)
    {
        uint64_t max = os_adc_max_raw(channel);
        *out_mv = (uint32_t)(((uint64_t)raw * channel->info.full_scale_mv + max / 2U) / max);
    }
    return status;
}

OsStatus os_adc_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsAdcChannel* channel = os_adc_slot(id);

    if (channel == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case OS_ADC_PROP_RESOLUTION: *out_value = channel->info.resolution_bits; return OS_STATUS_OK;
        case OS_ADC_PROP_MAX: *out_value = os_adc_max_raw(channel); return OS_STATUS_OK;
        case OS_ADC_PROP_FULL_SCALE: *out_value = channel->info.full_scale_mv; return OS_STATUS_OK;
        default: return OS_STATUS_UNSUPPORTED;
    }
}

void os_adc_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_ADC_MAX; ++index)
    {
        free(g_adc.channels[index]);
    }
    memset(&g_adc, 0, sizeof(g_adc));
}

/* ---------------------------------------------------------------------------
 * The adc interface for programs
 * ------------------------------------------------------------------------ */

static OsStatus os_adc_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    (void)context;
    return os_adc_open(name, length, out_id);
}

static OS_HOST_FUNCTION(os_adc_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, flags);
    OS_ABI_UNUSED();

    if (flags != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    OS_HOST_RETURN(os_driver_open(&os_adc_driver, name_address, name_length, os_adc_call_opener, NULL));
}

static OS_HOST_FUNCTION(os_adc_call_read)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_adc_driver, handle, &id);
    uint32_t value = 0U;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        OsStatus status = os_adc_read(id, &value);
        OS_HOST_RETURN(os_driver_property(status, value));
    }
}

static OS_HOST_FUNCTION(os_adc_call_read_mv)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_adc_driver, handle, &id);
    uint32_t value = 0U;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        OsStatus status = os_adc_read_mv(id, &value);
        OS_HOST_RETURN(os_driver_property(status, value));
    }
}

static OS_HOST_FUNCTION(os_adc_call_get)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_adc_driver, handle, &id);
    uint32_t value = 0U;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        OsStatus status = os_adc_get(id, property, &value);
        OS_HOST_RETURN(os_driver_property(status, value));
    }
}

static const OsDriverCall os_adc_calls[] =
{
    { "open", "i(iii)", os_adc_call_open, 4U },
    { "read", "i(i)", os_adc_call_read, 4U },
    { "read-mv", "i(i)", os_adc_call_read_mv, 4U },
    { "get", "i(ii)", os_adc_call_get, 4U },
};

const OsDriver os_adc_driver =
{
    .interface = "adc",
    .calls = os_adc_calls,
    .call_count = (uint32_t)(sizeof(os_adc_calls) / sizeof(os_adc_calls[0])),
    .shutdown = os_adc_shutdown,
};

