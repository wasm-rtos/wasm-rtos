/*
 * PWM outputs on GPIO pins: the pin is owned as an opened GPIO, and the
 * platform's PWM port drives it. Duty is normalized, 0 (off) to 65535
 * (fully on), so a program means the same on every chip.
 */

#include "os_internal.h"

#if OS_HAS_PWM

#include <string.h>

#ifndef OS_PWM_MAX
#define OS_PWM_MAX 8U
#endif

typedef struct OsPwmOutput
{
    uint8_t used;
    uint32_t gpio;
    uint32_t pin;
    uint8_t inverted;
    uint32_t frequency_hz;
    uint32_t duty;
} OsPwmOutput;

static struct
{
    OsPwmPort port;
    uint8_t has_port;
    OsPwmOutput outputs[OS_PWM_MAX];
} g_pwm;

static OsPwmOutput* os_pwm_slot(uint32_t id)
{
    return id >= 1U && id <= OS_PWM_MAX && g_pwm.outputs[id - 1U].used ? &g_pwm.outputs[id - 1U] : NULL;
}

/* The duty on the pin for the program's duty. */
static uint32_t os_pwm_physical(const OsPwmOutput* output, uint32_t duty)
{
    return output->inverted ? OS_PWM_DUTY_MAX - duty : duty;
}

OsStatus os_pwm_port_set(const OsPwmPort* port)
{
    if (port == NULL)
    {
        memset(&g_pwm.port, 0, sizeof(g_pwm.port));
        g_pwm.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->start == NULL || port->set_duty == NULL || port->stop == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_pwm.port = *port;
    g_pwm.has_port = 1U;
    return OS_STATUS_OK;
}

OsStatus os_pwm_open(const char* name, uint32_t length, uint32_t frequency_hz, uint32_t flags, uint32_t* out_id)
{
    OsPwmOutput* slot = NULL;
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;

    if (!g_pwm.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (frequency_hz == 0U || (flags & ~OS_PWM_ACTIVE_LOW) != 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_PWM_MAX && slot == NULL; ++index)
    {
        if (!g_pwm.outputs[index].used)
        {
            slot = &g_pwm.outputs[index];
        }
    }
    if (slot == NULL)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    memset(slot, 0, sizeof(*slot));
    status = os_gpio_open_pwm(name, length, (flags & OS_PWM_ACTIVE_LOW) != 0U, &slot->gpio, &slot->inverted);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    slot->pin = os_gpio_pin_number(slot->gpio);
    status = g_pwm.port.start(g_pwm.port.context, slot->pin, frequency_hz, os_pwm_physical(slot, 0U),
                              &slot->frequency_hz);
    if (status != OS_STATUS_OK)
    {
        os_gpio_close(slot->gpio);
        return status;
    }
    if (slot->frequency_hz == 0U)
    {
        slot->frequency_hz = frequency_hz;
    }
    slot->used = 1U;
    *out_id = (uint32_t)(slot - g_pwm.outputs + 1);
    return OS_STATUS_OK;
}

OsStatus os_pwm_set_duty(uint32_t id, uint32_t duty)
{
    OsPwmOutput* output = os_pwm_slot(id);
    OsStatus status = OS_STATUS_OK;

    if (output == NULL || duty > OS_PWM_DUTY_MAX)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = g_pwm.port.set_duty(g_pwm.port.context, output->pin, os_pwm_physical(output, duty));
    if (status == OS_STATUS_OK)
    {
        output->duty = duty;
    }
    return status;
}

OsStatus os_pwm_set_frequency(uint32_t id, uint32_t frequency_hz)
{
    OsPwmOutput* output = os_pwm_slot(id);
    uint32_t actual = 0U;
    OsStatus status = OS_STATUS_OK;

    if (output == NULL || frequency_hz == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (g_pwm.port.set_frequency == NULL)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = g_pwm.port.set_frequency(g_pwm.port.context, output->pin, frequency_hz, &actual);
    if (status == OS_STATUS_OK)
    {
        output->frequency_hz = actual != 0U ? actual : frequency_hz;
    }
    return status;
}

OsStatus os_pwm_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsPwmOutput* output = os_pwm_slot(id);

    if (output == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case OS_PWM_PROP_FREQUENCY: *out_value = output->frequency_hz; return OS_STATUS_OK;
        case OS_PWM_PROP_DUTY: *out_value = output->duty; return OS_STATUS_OK;
        default: return OS_STATUS_UNSUPPORTED;
    }
}

/* The output stops and the pin goes back to an input, as for a closed GPIO. */
void os_pwm_close(uint32_t id)
{
    OsPwmOutput* output = os_pwm_slot(id);

    if (output == NULL)
    {
        return;
    }
    if (g_pwm.has_port)
    {
        g_pwm.port.stop(g_pwm.port.context, output->pin);
    }
    os_gpio_close(output->gpio);
    memset(output, 0, sizeof(*output));
}

void os_pwm_shutdown(void)
{
    memset(&g_pwm, 0, sizeof(g_pwm));
}

#endif
