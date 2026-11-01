/*
 * GPIO: pins the platform registers by name, lent to one program at a time.
 * The kernel knows no hardware; the platform's OsGpioPort drives the pins.
 */

#include "os_internal.h"

#if OS_HAS_GPIO

#include <string.h>

/* Edge counts are bumped from interrupt handlers and taken by the kernel. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint32_t OsGpioCounter;
#define OS_GPIO_COUNT(counter) ((void)++(counter))
static uint32_t os_gpio_take_counter(OsGpioCounter* counter)
{
    uint32_t value = *counter;
    *counter = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uint_fast32_t OsGpioCounter;
#define OS_GPIO_COUNT(counter) ((void)atomic_fetch_add_explicit(&(counter), 1U, memory_order_relaxed))
static uint32_t os_gpio_take_counter(OsGpioCounter* counter)
{
    return (uint32_t)atomic_exchange_explicit(counter, 0U, memory_order_relaxed);
}
#endif

#define OS_GPIO_PORT_MODE (OS_GPIO_OUTPUT | OS_GPIO_PULL_UP | OS_GPIO_PULL_DOWN | OS_GPIO_OPEN_DRAIN)
#define OS_GPIO_GUEST_FLAGS (OS_GPIO_PORT_MODE | OS_GPIO_GUEST_ACTIVE_LOW | OS_GPIO_INIT_HIGH)
#define OS_GPIO_GUEST_EDGES (OS_GPIO_GUEST_RISING | OS_GPIO_GUEST_FALLING)

typedef struct OsGpioPin
{
    uint8_t used;
    uint32_t pin;
    uint32_t capabilities;
    uint32_t options;
    /* Owning task id, 0 when free. */
    uint32_t owner;
    uint32_t flags;
    uint32_t watch;
    uint32_t events;
    OsGpioCounter rising;
    OsGpioCounter falling;
} OsGpioPin;

typedef struct OsGpioName
{
    char name[OS_GPIO_NAME_MAX + 1U];
    uint8_t length;
    /* Index into g_gpio.pins plus one, 0 when the slot is free. */
    uint8_t pin;
} OsGpioName;

static struct
{
    OsGpioPort port;
    uint8_t has_port;
    OsGpioPin pins[OS_GPIO_MAX_PINS];
    OsGpioName names[OS_GPIO_MAX_NAMES];
} g_gpio;

static OsGpioPin* os_gpio_pin(uint32_t id)
{
    return id >= 1U && id <= OS_GPIO_MAX_PINS && g_gpio.pins[id - 1U].used ? &g_gpio.pins[id - 1U] : NULL;
}

/* Whether the program's level is the pin's inverted. */
static uint8_t os_gpio_inverted(const OsGpioPin* pin)
{
    return (uint8_t)(((pin->options & OS_GPIO_ACTIVE_LOW) != 0U) ^ ((pin->flags & OS_GPIO_GUEST_ACTIVE_LOW) != 0U));
}

/* The port flags for the pin's mode and watched edges. */
static uint32_t os_gpio_port_flags(const OsGpioPin* pin)
{
    uint32_t flags = pin->flags & OS_GPIO_PORT_MODE;
    uint8_t inverted = os_gpio_inverted(pin);

    if ((pin->flags & OS_GPIO_OUTPUT) != 0U && (((pin->flags & OS_GPIO_INIT_HIGH) != 0U) ^ inverted))
    {
        flags |= OS_GPIO_INIT_HIGH;
    }
    if ((pin->watch & OS_GPIO_GUEST_RISING) != 0U)
    {
        flags |= inverted ? OS_GPIO_EDGE_FALLING : OS_GPIO_EDGE_RISING;
    }
    if ((pin->watch & OS_GPIO_GUEST_FALLING) != 0U)
    {
        flags |= inverted ? OS_GPIO_EDGE_RISING : OS_GPIO_EDGE_FALLING;
    }
    return flags;
}

static OsStatus os_gpio_configure(const OsGpioPin* pin)
{
    return g_gpio.port.configure != NULL
        ? g_gpio.port.configure(g_gpio.port.context, pin->pin, os_gpio_port_flags(pin))
        : OS_STATUS_OK;
}

/* A mode the program may ask for on this pin. */
static OsStatus os_gpio_check_flags(const OsGpioPin* pin, uint32_t flags)
{
    uint32_t caps = pin->capabilities;
    uint8_t output = (flags & OS_GPIO_OUTPUT) != 0U;

    if ((flags & ~OS_GPIO_GUEST_FLAGS) != 0U ||
        ((flags & OS_GPIO_PULL_UP) != 0U && (flags & OS_GPIO_PULL_DOWN) != 0U) ||
        (!output && (flags & (OS_GPIO_OPEN_DRAIN | OS_GPIO_INIT_HIGH)) != 0U))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if ((output && (caps & OS_GPIO_CAN_OUTPUT) == 0U) ||
        (!output && (caps & OS_GPIO_CAN_INPUT) == 0U) ||
        ((flags & OS_GPIO_PULL_UP) != 0U && (caps & OS_GPIO_CAN_PULL_UP) == 0U) ||
        ((flags & OS_GPIO_PULL_DOWN) != 0U && (caps & OS_GPIO_CAN_PULL_DOWN) == 0U) ||
        ((flags & OS_GPIO_OPEN_DRAIN) != 0U && (caps & OS_GPIO_CAN_OPEN_DRAIN) == 0U))
    {
        return OS_STATUS_UNSUPPORTED;
    }
    return OS_STATUS_OK;
}

OsStatus os_gpio_port_set(const OsGpioPort* port)
{
    if (port == NULL)
    {
        memset(&g_gpio.port, 0, sizeof(g_gpio.port));
        g_gpio.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->write == NULL || port->read == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_gpio.port = *port;
    g_gpio.has_port = 1U;
    return OS_STATUS_OK;
}

static OsGpioName* os_gpio_find_name(const char* name, uint32_t length)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_NAMES; ++index)
    {
        OsGpioName* entry = &g_gpio.names[index];
        if (entry->pin != 0U && entry->length == length && memcmp(entry->name, name, length) == 0)
        {
            return entry;
        }
    }
    return NULL;
}

OsStatus os_gpio_add_pin(const char* name, uint32_t pin, uint32_t capabilities, uint32_t options)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsGpioPin* slot = NULL;
    OsGpioName* entry = NULL;
    uint32_t index = 0U;

    if (length == 0U || length > OS_GPIO_NAME_MAX || (capabilities & ~OS_GPIO_CAN_ALL) != 0U ||
        (options & ~OS_GPIO_ACTIVE_LOW) != 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (os_gpio_find_name(name, (uint32_t)length) != NULL)
    {
        return OS_STATUS_BUSY;
    }
    for (index = 0U; index < OS_GPIO_MAX_PINS && slot == NULL; ++index)
    {
        if (g_gpio.pins[index].used && g_gpio.pins[index].pin == pin)
        {
            slot = &g_gpio.pins[index];
        }
    }
    if (slot != NULL && (slot->capabilities != capabilities || slot->options != options))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_GPIO_MAX_NAMES && entry == NULL; ++index)
    {
        if (g_gpio.names[index].pin == 0U)
        {
            entry = &g_gpio.names[index];
        }
    }
    for (index = 0U; index < OS_GPIO_MAX_PINS && slot == NULL; ++index)
    {
        if (!g_gpio.pins[index].used)
        {
            slot = &g_gpio.pins[index];
            memset(slot, 0, sizeof(*slot));
            slot->pin = pin;
            slot->capabilities = capabilities;
            slot->options = options;
        }
    }
    if (entry == NULL || slot == NULL)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot->used = 1U;
    memcpy(entry->name, name, length);
    entry->length = (uint8_t)length;
    entry->pin = (uint8_t)(slot - g_gpio.pins + 1);
    return OS_STATUS_OK;
}

void os_gpio_edge(uint32_t pin, uint32_t edges)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_PINS; ++index)
    {
        OsGpioPin* slot = &g_gpio.pins[index];
        if (slot->used && slot->pin == pin)
        {
            if ((edges & OS_GPIO_EDGE_RISING) != 0U)
            {
                OS_GPIO_COUNT(slot->rising);
            }
            if ((edges & OS_GPIO_EDGE_FALLING) != 0U)
            {
                OS_GPIO_COUNT(slot->falling);
            }
            return;
        }
    }
}

void os_gpio_poll(void)
{
    uint8_t delivered = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_PINS; ++index)
    {
        OsGpioPin* slot = &g_gpio.pins[index];
        uint32_t rising = 0U;
        uint32_t falling = 0U;
        uint32_t counted = 0U;

        if (!slot->used)
        {
            continue;
        }
        rising = os_gpio_take_counter(&slot->rising);
        falling = os_gpio_take_counter(&slot->falling);
        if (slot->owner == 0U || slot->watch == 0U)
        {
            continue;
        }
        if (os_gpio_inverted(slot))
        {
            uint32_t swap = rising;
            rising = falling;
            falling = swap;
        }
        counted = ((slot->watch & OS_GPIO_GUEST_RISING) != 0U ? rising : 0U) +
                  ((slot->watch & OS_GPIO_GUEST_FALLING) != 0U ? falling : 0U);
        if (counted > 0U)
        {
            slot->events = counted > UINT32_MAX - slot->events ? UINT32_MAX : slot->events + counted;
            delivered = 1U;
        }
    }
    if (delivered)
    {
        os_wait_any_recheck();
    }
}

OsStatus os_gpio_open(const char* name, uint32_t length, uint32_t flags, uint32_t* out_id)
{
    OsTaskHandle task = os_task_get_current();
    OsGpioName* entry = os_gpio_find_name(name, length);
    OsGpioPin* pin = entry != NULL ? &g_gpio.pins[entry->pin - 1U] : NULL;
    OsStatus status = OS_STATUS_OK;

    if (!g_gpio.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (pin == NULL)
    {
        return OS_STATUS_FILE_NOT_FOUND;
    }
    if (pin->owner != 0U)
    {
        return OS_STATUS_BUSY;
    }
    status = os_gpio_check_flags(pin, flags);
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    pin->flags = flags;
    pin->watch = 0U;
    pin->events = 0U;
    (void)os_gpio_take_counter(&pin->rising);
    (void)os_gpio_take_counter(&pin->falling);
    status = os_gpio_configure(pin);
    if (status != OS_STATUS_OK)
    {
        pin->flags = 0U;
        return status;
    }
    pin->owner = os_task_get_id(task);
    *out_id = (uint32_t)(pin - g_gpio.pins + 1);
    return OS_STATUS_OK;
}

void os_gpio_close(uint32_t id)
{
    OsGpioPin* pin = os_gpio_pin(id);

    if (pin == NULL || pin->owner == 0U)
    {
        return;
    }
    /* Back to the safe state: an input without pulls, no edge reports. */
    pin->flags = 0U;
    pin->watch = 0U;
    pin->events = 0U;
    pin->owner = 0U;
    if (g_gpio.has_port)
    {
        (void)os_gpio_configure(pin);
    }
}

OsStatus os_gpio_set(uint32_t id, uint32_t level)
{
    OsGpioPin* pin = os_gpio_pin(id);

    if (pin == NULL || !g_gpio.has_port)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if ((pin->flags & OS_GPIO_OUTPUT) == 0U || level > 1U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    return g_gpio.port.write(g_gpio.port.context, pin->pin, (uint8_t)(level ^ os_gpio_inverted(pin)));
}

OsStatus os_gpio_get(uint32_t id, uint32_t* out_level)
{
    OsGpioPin* pin = os_gpio_pin(id);
    uint8_t level = 0U;
    OsStatus status = OS_STATUS_OK;

    if (pin == NULL || !g_gpio.has_port)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = g_gpio.port.read(g_gpio.port.context, pin->pin, &level);
    if (status == OS_STATUS_OK)
    {
        *out_level = (uint32_t)((level != 0U) ^ os_gpio_inverted(pin));
    }
    return status;
}

OsStatus os_gpio_set_mode(uint32_t id, uint32_t flags)
{
    OsGpioPin* pin = os_gpio_pin(id);
    uint32_t previous = 0U;
    OsStatus status = OS_STATUS_OK;

    if (pin == NULL || !g_gpio.has_port)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_gpio_check_flags(pin, flags);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    previous = pin->flags;
    pin->flags = flags;
    status = os_gpio_configure(pin);
    if (status != OS_STATUS_OK)
    {
        pin->flags = previous;
    }
    return status;
}

OsStatus os_gpio_watch(uint32_t id, uint32_t edges)
{
    OsGpioPin* pin = os_gpio_pin(id);
    uint32_t previous = 0U;
    OsStatus status = OS_STATUS_OK;

    if (pin == NULL || !g_gpio.has_port || (edges & ~OS_GPIO_GUEST_EDGES) != 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (edges != 0U && (pin->capabilities & OS_GPIO_CAN_EDGES) == 0U)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    previous = pin->watch;
    pin->watch = edges;
    status = os_gpio_configure(pin);
    if (status != OS_STATUS_OK)
    {
        pin->watch = previous;
        return status;
    }
    /* Edges from before the watch began do not count. */
    pin->events = 0U;
    (void)os_gpio_take_counter(&pin->rising);
    (void)os_gpio_take_counter(&pin->falling);
    return OS_STATUS_OK;
}

uint32_t os_gpio_take_events(uint32_t id)
{
    OsGpioPin* pin = os_gpio_pin(id);
    uint32_t events = 0U;

    os_gpio_poll();
    if (pin != NULL)
    {
        events = pin->events;
        pin->events = 0U;
    }
    return events;
}

uint8_t os_gpio_ready(uint32_t id)
{
    OsGpioPin* pin = os_gpio_pin(id);

    os_gpio_poll();
    return pin != NULL && pin->events > 0U;
}

void os_gpio_shutdown(void)
{
    memset(&g_gpio, 0, sizeof(g_gpio));
}

#endif
