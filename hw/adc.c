/*
 * Analog inputs: channels the platform registers by name. Reading changes
 * nothing, so any number of programs may read a channel.
 */

#include "hw_driver.h"

#include <stdlib.h>
#include <string.h>

/* Allocated when the platform registers the channel. */
typedef struct HwAdcChannel
{
    uint32_t channel;
    char name[HW_ADC_NAME_MAX + 1U];
    uint8_t name_length;
    HwAdcInfo info;
} HwAdcChannel;

static struct
{
    HwAdcPort port;
    uint8_t has_port;
    HwAdcChannel* channels[HW_ADC_MAX];
} g_adc;

static HwAdcChannel* hw_adc_slot(uint32_t id)
{
    return id >= 1U && id <= HW_ADC_MAX ? g_adc.channels[id - 1U] : NULL;
}

static uint32_t hw_adc_max_raw(const HwAdcChannel* channel)
{
    return channel->info.resolution_bits >= 32U ? UINT32_MAX : (1U << channel->info.resolution_bits) - 1U;
}

HwStatus hw_adc_port_set(const HwAdcPort* port)
{
    if (port == NULL)
    {
        memset(&g_adc.port, 0, sizeof(g_adc.port));
        g_adc.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->read == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_adc.port = *port;
    g_adc.has_port = 1U;
    return HW_STATUS_OK;
}

extern const HwDriver hw_adc_driver;

HwStatus hw_adc_add_channel(const char* name, uint32_t channel, const HwAdcInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    HwAdcChannel* slot = NULL;
    uint32_t free_index = HW_ADC_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > HW_ADC_NAME_MAX || info == NULL || info->resolution_bits == 0U ||
        info->resolution_bits > 24U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_ADC_MAX; ++index)
    {
        HwAdcChannel* entry = g_adc.channels[index];
        if (entry != NULL && (entry->channel == channel ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return HW_STATUS_BUSY;
        }
        if (entry == NULL && free_index == HW_ADC_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == HW_ADC_MAX)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot = (HwAdcChannel*)calloc(1U, sizeof(HwAdcChannel));
    if (slot == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    g_adc.channels[free_index] = slot;
    slot->channel = channel;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    /* A platform with these devices gives programs the adc interface. */
    if (hw_driver_add(&hw_adc_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

HwStatus hw_adc_open(const char* name, uint32_t length, uint32_t* out_id)
{
    uint8_t any = 0U;
    uint32_t index = 0U;

    if (!g_adc.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < HW_ADC_MAX; ++index)
    {
        HwAdcChannel* entry = g_adc.channels[index];
        any |= entry != NULL;
        if (entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
        {
            *out_id = index + 1U;
            return HW_STATUS_OK;
        }
    }
    return any ? HW_STATUS_FILE_NOT_FOUND : HW_STATUS_UNSUPPORTED;
}

HwStatus hw_adc_read(uint32_t id, uint32_t* out_raw)
{
    HwAdcChannel* channel = hw_adc_slot(id);
    HwStatus status = HW_STATUS_OK;

    if (channel == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_adc.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = g_adc.port.read(g_adc.port.context, channel->channel, out_raw);
    if (status == HW_STATUS_OK && *out_raw > hw_adc_max_raw(channel))
    {
        *out_raw = hw_adc_max_raw(channel);
    }
    return status;
}

/* The port's calibrated reading, or the raw value on the channel's scale, rounded. */
HwStatus hw_adc_read_mv(uint32_t id, uint32_t* out_mv)
{
    HwAdcChannel* channel = hw_adc_slot(id);
    uint32_t raw = 0U;
    HwStatus status = HW_STATUS_OK;

    if (channel == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_adc.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (g_adc.port.read_mv != NULL)
    {
        status = g_adc.port.read_mv(g_adc.port.context, channel->channel, out_mv);
        if (status != HW_STATUS_UNSUPPORTED)
        {
            return status;
        }
    }
    status = hw_adc_read(id, &raw);
    if (status == HW_STATUS_OK)
    {
        uint64_t max = hw_adc_max_raw(channel);
        *out_mv = (uint32_t)(((uint64_t)raw * channel->info.full_scale_mv + max / 2U) / max);
    }
    return status;
}

HwStatus hw_adc_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwAdcChannel* channel = hw_adc_slot(id);

    if (channel == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case HW_ADC_PROP_RESOLUTION: *out_value = channel->info.resolution_bits; return HW_STATUS_OK;
        case HW_ADC_PROP_MAX: *out_value = hw_adc_max_raw(channel); return HW_STATUS_OK;
        case HW_ADC_PROP_FULL_SCALE: *out_value = channel->info.full_scale_mv; return HW_STATUS_OK;
        default: return HW_STATUS_UNSUPPORTED;
    }
}

void hw_adc_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_ADC_MAX; ++index)
    {
        free(g_adc.channels[index]);
    }
    memset(&g_adc, 0, sizeof(g_adc));
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: every channel by name, so handles find theirs
 * ------------------------------------------------------------------------ */

static void hw_adc_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_ADC_MAX; ++index)
    {
        count += g_adc.channels[index] != NULL ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_ADC_MAX; ++index)
    {
        if (g_adc.channels[index] != NULL)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put_bytes(stream, g_adc.channels[index]->name, g_adc.channels[index]->name_length);
        }
    }
}

static void hw_adc_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_ADC_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        char name[HW_ADC_NAME_MAX];
        uint32_t old_id = hw_host_system_get32(stream);
        uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
        uint32_t id = 0U;
        uint32_t index = 0U;

        for (index = 0U; index < HW_ADC_MAX && hw_host_system_ok(stream); ++index)
        {
            const HwAdcChannel* channel = g_adc.channels[index];
            if (channel != NULL && channel->name_length == length && memcmp(channel->name, name, length) == 0)
            {
                id = index + 1U;
            }
        }
        if (hw_host_system_ok(stream))
        {
            hw_host_system_device(stream, &hw_adc_driver, old_id, g_adc.has_port ? id : 0U, NULL);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The adc interface for programs
 * ------------------------------------------------------------------------ */

static HwStatus hw_adc_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    (void)context;
    return hw_adc_open(name, length, out_id);
}

static HW_HOST_FUNCTION(hw_adc_call_open)
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
    HW_HOST_RETURN(hw_driver_open(&hw_adc_driver, name_address, name_length, hw_adc_call_opener, NULL));
}

static HW_HOST_FUNCTION(hw_adc_call_read)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_adc_driver, handle, &id);
    uint32_t value = 0U;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        HwStatus status = hw_adc_read(id, &value);
        HW_HOST_RETURN(hw_driver_property(status, value));
    }
}

static HW_HOST_FUNCTION(hw_adc_call_read_mv)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_adc_driver, handle, &id);
    uint32_t value = 0U;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        HwStatus status = hw_adc_read_mv(id, &value);
        HW_HOST_RETURN(hw_driver_property(status, value));
    }
}

static HW_HOST_FUNCTION(hw_adc_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_adc_driver, handle, &id);
    uint32_t value = 0U;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        HwStatus status = hw_adc_get(id, property, &value);
        HW_HOST_RETURN(hw_driver_property(status, value));
    }
}

static const HwDriverCall hw_adc_calls[] =
{
    { "open", "i(iii)", hw_adc_call_open, 4U },
    { "read", "i(i)", hw_adc_call_read, 4U },
    { "read-mv", "i(i)", hw_adc_call_read_mv, 4U },
    { "get", "i(ii)", hw_adc_call_get, 4U },
};

const HwDriver hw_adc_driver =
{
    .interface = "adc",
    .calls = hw_adc_calls,
    .call_count = (uint32_t)(sizeof(hw_adc_calls) / sizeof(hw_adc_calls[0])),
    .shutdown = hw_adc_shutdown,
    .save = hw_adc_save,
    .restore = hw_adc_restore,
};

