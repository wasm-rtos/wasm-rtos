/*
 * GPIO: pins the platform registers by name, lent to one program at a time.
 * The kernel knows no hardware; the platform's OsGpioPort drives the pins.
 */

#include "os_internal.h"

#if OS_HAS_GPIO

#include <stdlib.h>
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

/* Allocated when the platform registers the pin; interrupt handlers count edges in it. */
typedef struct OsGpioPin
{
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

/* A name and the pin it means, allocated with the name's own length. */
typedef struct OsGpioName
{
    /* Index into g_gpio.pins plus one. */
    uint8_t pin;
    uint8_t length;
    char name[];
} OsGpioName;

/* Only slots: what the board registers is allocated, so unused capacity costs a pointer. */
static struct
{
    OsGpioPort port;
    uint8_t has_port;
    OsGpioPin* pins[OS_GPIO_MAX_PINS];
    OsGpioName* names[OS_GPIO_MAX_NAMES];
} g_gpio;

static OsGpioPin* os_gpio_pin(uint32_t id)
{
    return id >= 1U && id <= OS_GPIO_MAX_PINS ? g_gpio.pins[id - 1U] : NULL;
}

/* The pin slot holding platform pin number pin, or OS_GPIO_MAX_PINS. */
static uint32_t os_gpio_index_of(uint32_t pin)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_PINS; ++index)
    {
        if (g_gpio.pins[index] != NULL && g_gpio.pins[index]->pin == pin)
        {
            break;
        }
    }
    return index;
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
        OsGpioName* entry = g_gpio.names[index];
        if (entry != NULL && entry->length == length && memcmp(entry->name, name, length) == 0)
        {
            return entry;
        }
    }
    return NULL;
}

OsStatus os_gpio_add_pin(const char* name, uint32_t pin, uint32_t capabilities, uint32_t options)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    uint32_t slot = os_gpio_index_of(pin);
    uint32_t name_slot = 0U;
    uint8_t new_pin = 0U;
    OsGpioName* entry = NULL;

    if (length == 0U || length > OS_GPIO_NAME_MAX || (capabilities & ~OS_GPIO_CAN_ALL) != 0U ||
        (options & ~OS_GPIO_ACTIVE_LOW) != 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (os_gpio_find_name(name, (uint32_t)length) != NULL)
    {
        return OS_STATUS_BUSY;
    }
    if (slot < OS_GPIO_MAX_PINS &&
        (g_gpio.pins[slot]->capabilities != capabilities || g_gpio.pins[slot]->options != options))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    while (name_slot < OS_GPIO_MAX_NAMES && g_gpio.names[name_slot] != NULL)
    {
        ++name_slot;
    }
    if (slot == OS_GPIO_MAX_PINS)
    {
        for (slot = 0U; slot < OS_GPIO_MAX_PINS && g_gpio.pins[slot] != NULL; ++slot)
        {
        }
        new_pin = 1U;
    }
    if (name_slot == OS_GPIO_MAX_NAMES || slot == OS_GPIO_MAX_PINS)
    {
        return OS_STATUS_LIMIT_REACHED;
    }

    entry = (OsGpioName*)malloc(sizeof(OsGpioName) + length);
    if (entry == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    if (new_pin)
    {
        OsGpioPin* created = (OsGpioPin*)calloc(1U, sizeof(OsGpioPin));
        if (created == NULL)
        {
            free(entry);
            return OS_STATUS_OUT_OF_MEMORY;
        }
        created->pin = pin;
        created->capabilities = capabilities;
        created->options = options;
        g_gpio.pins[slot] = created;
    }
    memcpy(entry->name, name, length);
    entry->length = (uint8_t)length;
    entry->pin = (uint8_t)(slot + 1U);
    g_gpio.names[name_slot] = entry;
    return OS_STATUS_OK;
}

void os_gpio_edge(uint32_t pin, uint32_t edges)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_PINS; ++index)
    {
        OsGpioPin* slot = g_gpio.pins[index];
        if (slot != NULL && slot->pin == pin)
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

uint32_t os_gpio_get_owner(uint32_t pin)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_PINS; ++index)
    {
        if (g_gpio.pins[index] != NULL && g_gpio.pins[index]->pin == pin)
        {
            return g_gpio.pins[index]->owner;
        }
    }
    return 0U;
}

void os_gpio_poll(void)
{
    uint8_t delivered = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_PINS; ++index)
    {
        OsGpioPin* slot = g_gpio.pins[index];
        uint32_t rising = 0U;
        uint32_t falling = 0U;
        uint32_t counted = 0U;

        if (slot == NULL)
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
    OsGpioPin* pin = entry != NULL ? g_gpio.pins[entry->pin - 1U] : NULL;
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
    *out_id = entry->pin;
    return OS_STATUS_OK;
}

OsStatus os_gpio_open_capable(const char* name, uint32_t length, uint32_t capability, uint32_t flags,
                              uint32_t* out_id)
{
    OsGpioName* entry = os_gpio_find_name(name, length);

    if (entry != NULL && (g_gpio.pins[entry->pin - 1U]->capabilities & capability) == 0U)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    return os_gpio_open(name, length, flags, out_id);
}

OsStatus os_gpio_open_pwm(const char* name, uint32_t length, uint8_t active_low, uint32_t* out_id,
                          uint8_t* out_inverted)
{
    OsGpioName* entry = os_gpio_find_name(name, length);
    OsStatus status = OS_STATUS_OK;

    if (entry != NULL && (g_gpio.pins[entry->pin - 1U]->capabilities & OS_GPIO_CAN_PWM) == 0U)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    /* Logical 0 is "off" whichever way the pin is inverted. */
    status = os_gpio_open(name, length, OS_GPIO_OUTPUT | (active_low ? OS_GPIO_GUEST_ACTIVE_LOW : 0U), out_id);
    if (status == OS_STATUS_OK)
    {
        *out_inverted = os_gpio_inverted(g_gpio.pins[*out_id - 1U]);
    }
    return status;
}

uint32_t os_gpio_pin_number(uint32_t id)
{
    OsGpioPin* pin = os_gpio_pin(id);
    return pin != NULL ? pin->pin : 0U;
}

OsStatus os_gpio_open_physical(const char* name, uint32_t length, uint32_t* out_id)
{
    OsGpioName* entry = os_gpio_find_name(name, length);
    uint32_t flags = OS_GPIO_OUTPUT | OS_GPIO_INIT_HIGH;

    /* Undo the board's inversion, so 1 is high on the pin. */
    if (entry != NULL && (g_gpio.pins[entry->pin - 1U]->options & OS_GPIO_ACTIVE_LOW) != 0U)
    {
        flags |= OS_GPIO_GUEST_ACTIVE_LOW;
    }
    return os_gpio_open(name, length, flags, out_id);
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
    uint32_t index = 0U;

    for (index = 0U; index < OS_GPIO_MAX_PINS; ++index)
    {
        free(g_gpio.pins[index]);
    }
    for (index = 0U; index < OS_GPIO_MAX_NAMES; ++index)
    {
        free(g_gpio.names[index]);
    }
    memset(&g_gpio, 0, sizeof(g_gpio));
}

#endif
