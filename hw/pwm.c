/*
 * PWM outputs on GPIO pins: the pin is owned as an opened GPIO, and the
 * platform's PWM port drives it. Duty is normalized, 0 (off) to 65535
 * (fully on), so a program means the same on every chip.
 */

#include "hw_driver.h"

#include <string.h>

#ifndef HW_PWM_MAX
#define HW_PWM_MAX 8U
#endif

typedef struct HwPwmOutput
{
    uint8_t used;
    uint32_t gpio;
    uint32_t pin;
    uint8_t inverted;
    uint32_t frequency_hz;
    uint32_t duty;
} HwPwmOutput;

static struct
{
    HwPwmPort port;
    uint8_t has_port;
    HwPwmOutput outputs[HW_PWM_MAX];
} g_pwm;

static HwPwmOutput* hw_pwm_slot(uint32_t id)
{
    return id >= 1U && id <= HW_PWM_MAX && g_pwm.outputs[id - 1U].used ? &g_pwm.outputs[id - 1U] : NULL;
}

/* The duty on the pin for the program's duty. */
static uint32_t hw_pwm_physical(const HwPwmOutput* output, uint32_t duty)
{
    return output->inverted ? HW_PWM_DUTY_MAX - duty : duty;
}

extern const HwDriver hw_pwm_driver;

HwStatus hw_pwm_port_set(const HwPwmPort* port)
{
    if (port == NULL)
    {
        memset(&g_pwm.port, 0, sizeof(g_pwm.port));
        g_pwm.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->start == NULL || port->set_duty == NULL || port->stop == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_pwm.port = *port;
    g_pwm.has_port = 1U;
    /* A platform with a PWM port gives programs the pwm interface. */
    if (hw_driver_add(&hw_pwm_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

HwStatus hw_pwm_open(const char* name, uint32_t length, uint32_t frequency_hz, uint32_t flags, uint32_t* out_id)
{
    HwPwmOutput* slot = NULL;
    HwStatus status = HW_STATUS_OK;
    uint32_t index = 0U;

    if (!g_pwm.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (frequency_hz == 0U || (flags & ~HW_PWM_ACTIVE_LOW) != 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_PWM_MAX && slot == NULL; ++index)
    {
        if (!g_pwm.outputs[index].used)
        {
            slot = &g_pwm.outputs[index];
        }
    }
    if (slot == NULL)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    memset(slot, 0, sizeof(*slot));
    status = hw_gpio_open_pwm(name, length, (flags & HW_PWM_ACTIVE_LOW) != 0U, &slot->gpio, &slot->inverted);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    slot->pin = hw_gpio_pin_number(slot->gpio);
    status = g_pwm.port.start(g_pwm.port.context, slot->pin, frequency_hz, hw_pwm_physical(slot, 0U),
                              &slot->frequency_hz);
    if (status != HW_STATUS_OK)
    {
        hw_gpio_close(slot->gpio);
        return status;
    }
    if (slot->frequency_hz == 0U)
    {
        slot->frequency_hz = frequency_hz;
    }
    slot->used = 1U;
    *out_id = (uint32_t)(slot - g_pwm.outputs + 1);
    return HW_STATUS_OK;
}

HwStatus hw_pwm_set_duty(uint32_t id, uint32_t duty)
{
    HwPwmOutput* output = hw_pwm_slot(id);
    HwStatus status = HW_STATUS_OK;

    if (output == NULL || duty > HW_PWM_DUTY_MAX)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    status = g_pwm.port.set_duty(g_pwm.port.context, output->pin, hw_pwm_physical(output, duty));
    if (status == HW_STATUS_OK)
    {
        output->duty = duty;
    }
    return status;
}

HwStatus hw_pwm_set_frequency(uint32_t id, uint32_t frequency_hz)
{
    HwPwmOutput* output = hw_pwm_slot(id);
    uint32_t actual = 0U;
    HwStatus status = HW_STATUS_OK;

    if (output == NULL || frequency_hz == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (g_pwm.port.set_frequency == NULL)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = g_pwm.port.set_frequency(g_pwm.port.context, output->pin, frequency_hz, &actual);
    if (status == HW_STATUS_OK)
    {
        output->frequency_hz = actual != 0U ? actual : frequency_hz;
    }
    return status;
}

HwStatus hw_pwm_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwPwmOutput* output = hw_pwm_slot(id);

    if (output == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case HW_PWM_PROP_FREQUENCY: *out_value = output->frequency_hz; return HW_STATUS_OK;
        case HW_PWM_PROP_DUTY: *out_value = output->duty; return HW_STATUS_OK;
        default: return HW_STATUS_UNSUPPORTED;
    }
}

/* The output stops and the pin goes back to an input, as for a closed GPIO. */
void hw_pwm_close(uint32_t id)
{
    HwPwmOutput* output = hw_pwm_slot(id);

    if (output == NULL)
    {
        return;
    }
    if (g_pwm.has_port)
    {
        g_pwm.port.stop(g_pwm.port.context, output->pin);
    }
    hw_gpio_close(output->gpio);
    memset(output, 0, sizeof(*output));
}

void hw_pwm_shutdown(void)
{
    memset(&g_pwm, 0, sizeof(g_pwm));
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: each output in its slot, on its pin, with its
 * frequency and duty
 * ------------------------------------------------------------------------ */

static void hw_pwm_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_PWM_MAX; ++index)
    {
        count += g_pwm.outputs[index].used ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_PWM_MAX; ++index)
    {
        const HwPwmOutput* output = &g_pwm.outputs[index];
        if (output->used)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put32(stream, output->gpio);
            hw_host_system_put32(stream, output->inverted);
            hw_host_system_put32(stream, output->frequency_hz);
            hw_host_system_put32(stream, output->duty);
        }
    }
}

static void hw_pwm_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_PWM_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        uint32_t id = hw_host_system_get_max(stream, HW_PWM_MAX);
        uint32_t old_gpio = hw_host_system_get32(stream);
        uint8_t inverted = (uint8_t)hw_host_system_get_max(stream, 1U);
        uint32_t frequency_hz = hw_host_system_get32(stream);
        uint32_t duty = hw_host_system_get_max(stream, HW_PWM_DUTY_MAX);
        const char* gone = NULL;
        uint32_t gpio = 0U;
        HwPwmOutput* output = NULL;

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        if (id == 0U || g_pwm.outputs[id - 1U].used || frequency_hz == 0U)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        /* Its pin, which gpio restored first. */
        gpio = hw_host_system_device_id(stream, &hw_gpio_driver, old_gpio, &gone);
        if (gpio == 0U || !g_pwm.has_port)
        {
            hw_host_system_device(stream, &hw_pwm_driver, id, 0U, gone);
            continue;
        }
        output = &g_pwm.outputs[id - 1U];
        memset(output, 0, sizeof(*output));
        output->gpio = gpio;
        output->pin = hw_gpio_pin_number(gpio);
        output->inverted = inverted;
        output->duty = duty;
        if (g_pwm.port.start(g_pwm.port.context, output->pin, frequency_hz, hw_pwm_physical(output, duty),
                             &output->frequency_hz) != HW_STATUS_OK)
        {
            hw_gpio_close(gpio);
            memset(output, 0, sizeof(*output));
            hw_host_system_device(stream, &hw_pwm_driver, id, 0U, "a device it held cannot be set up");
            continue;
        }
        if (output->frequency_hz == 0U)
        {
            output->frequency_hz = frequency_hz;
        }
        output->used = 1U;
        hw_host_system_device(stream, &hw_pwm_driver, id, id, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The pwm interface for programs
 * ------------------------------------------------------------------------ */

typedef struct HwAbiPwmOpen
{
    uint32_t frequency_hz;
    uint32_t flags;
} HwAbiPwmOpen;

static HwStatus hw_pwm_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    const HwAbiPwmOpen* open = (const HwAbiPwmOpen*)context;
    return hw_pwm_open(name, length, open->frequency_hz, open->flags, out_id);
}

static HW_HOST_FUNCTION(hw_pwm_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, name_address);
    HW_HOST_ARG(uint32_t, name_length);
    HW_HOST_ARG(uint32_t, frequency_hz);
    HW_HOST_ARG(uint32_t, flags);
    HwAbiPwmOpen open = { frequency_hz, flags };
    HW_ABI_UNUSED();

    HW_HOST_RETURN(hw_driver_open(&hw_pwm_driver, name_address, name_length, hw_pwm_call_opener, &open));
}

static HW_HOST_FUNCTION(hw_pwm_call_set_duty)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, duty);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_pwm_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_pwm_set_duty(id, duty)));
}

static HW_HOST_FUNCTION(hw_pwm_call_set_frequency)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, frequency_hz);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_pwm_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_pwm_set_frequency(id, frequency_hz)));
}

static HW_HOST_FUNCTION(hw_pwm_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_pwm_driver, handle, &id);
    uint32_t value = 0U;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        HwStatus status = hw_pwm_get(id, property, &value);
        HW_HOST_RETURN(hw_driver_property(status, value));
    }
}

static const HwDriverCall hw_pwm_calls[] =
{
    { "open", "i(iiii)", hw_pwm_call_open, 4U },
    { "set-duty", "i(ii)", hw_pwm_call_set_duty, 4U },
    { "set-frequency", "i(ii)", hw_pwm_call_set_frequency, 4U },
    { "get", "i(ii)", hw_pwm_call_get, 4U },
};

const HwDriver hw_pwm_driver =
{
    .interface = "pwm",
    .calls = hw_pwm_calls,
    .call_count = (uint32_t)(sizeof(hw_pwm_calls) / sizeof(hw_pwm_calls[0])),
    .close = hw_pwm_close,
    .shutdown = hw_pwm_shutdown,
    .save = hw_pwm_save,
    .restore = hw_pwm_restore,
};

