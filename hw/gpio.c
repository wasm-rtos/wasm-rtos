/*
 * GPIO: pins the platform registers by name, lent to one program at a time.
 * The library knows no hardware; the platform's HwGpioPort drives the pins.
 */

#include "hw_driver.h"

#include <stdlib.h>
#include <string.h>

/* Edge counts are bumped from interrupt handlers and taken by the library. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint32_t HwGpioCounter;
#define HW_GPIO_COUNT(counter) ((void)++(counter))
static uint32_t hw_gpio_take_counter(HwGpioCounter* counter)
{
    uint32_t value = *counter;
    *counter = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uint_fast32_t HwGpioCounter;
#define HW_GPIO_COUNT(counter) ((void)atomic_fetch_add_explicit(&(counter), 1U, memory_order_relaxed))
static uint32_t hw_gpio_take_counter(HwGpioCounter* counter)
{
    return (uint32_t)atomic_exchange_explicit(counter, 0U, memory_order_relaxed);
}
#endif

#define HW_GPIO_PORT_MODE (HW_GPIO_OUTPUT | HW_GPIO_PULL_UP | HW_GPIO_PULL_DOWN | HW_GPIO_OPEN_DRAIN)
#define HW_GPIO_GUEST_FLAGS (HW_GPIO_PORT_MODE | HW_GPIO_GUEST_ACTIVE_LOW | HW_GPIO_INIT_HIGH)
#define HW_GPIO_GUEST_EDGES (HW_GPIO_GUEST_RISING | HW_GPIO_GUEST_FALLING)

extern const HwDriver hw_gpio_driver;

/* Allocated when the platform registers the pin; interrupt handlers count edges in it. */
typedef struct HwGpioPin
{
    uint32_t pin;
    uint32_t capabilities;
    uint32_t options;
    /* Owning task id, 0 when free. */
    uint32_t owner;
    uint32_t flags;
    /* The level the program last set on an output (logical), kept for a saved system. */
    uint8_t level;
    uint32_t watch;
    uint32_t events;
    HwGpioCounter rising;
    HwGpioCounter falling;
} HwGpioPin;

/* A name and the pin it means, allocated with the name's own length. */
typedef struct HwGpioName
{
    /* Index into g_gpio.pins plus one. */
    uint8_t pin;
    uint8_t length;
    char name[];
} HwGpioName;

/* Only slots: what the board registers is allocated, so unused capacity costs a pointer. */
static struct
{
    HwGpioPort port;
    uint8_t has_port;
    HwGpioPin* pins[HW_GPIO_MAX_PINS];
    HwGpioName* names[HW_GPIO_MAX_NAMES];
} g_gpio;

static HwGpioPin* hw_gpio_pin(uint32_t id)
{
    return id >= 1U && id <= HW_GPIO_MAX_PINS ? g_gpio.pins[id - 1U] : NULL;
}

/* The pin slot holding platform pin number pin, or HW_GPIO_MAX_PINS. */
static uint32_t hw_gpio_index_of(uint32_t pin)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        if (g_gpio.pins[index] != NULL && g_gpio.pins[index]->pin == pin)
        {
            break;
        }
    }
    return index;
}

/* Whether the program's level is the pin's inverted. */
static uint8_t hw_gpio_inverted(const HwGpioPin* pin)
{
    return (uint8_t)(((pin->options & HW_GPIO_ACTIVE_LOW) != 0U) ^ ((pin->flags & HW_GPIO_GUEST_ACTIVE_LOW) != 0U));
}

/* The port flags for the pin's mode and watched edges. */
static uint32_t hw_gpio_port_flags(const HwGpioPin* pin)
{
    uint32_t flags = pin->flags & HW_GPIO_PORT_MODE;
    uint8_t inverted = hw_gpio_inverted(pin);

    if ((pin->flags & HW_GPIO_OUTPUT) != 0U && (((pin->flags & HW_GPIO_INIT_HIGH) != 0U) ^ inverted))
    {
        flags |= HW_GPIO_INIT_HIGH;
    }
    if ((pin->watch & HW_GPIO_GUEST_RISING) != 0U)
    {
        flags |= inverted ? HW_GPIO_EDGE_FALLING : HW_GPIO_EDGE_RISING;
    }
    if ((pin->watch & HW_GPIO_GUEST_FALLING) != 0U)
    {
        flags |= inverted ? HW_GPIO_EDGE_RISING : HW_GPIO_EDGE_FALLING;
    }
    return flags;
}

static HwStatus hw_gpio_configure(const HwGpioPin* pin)
{
    return g_gpio.port.configure != NULL
        ? g_gpio.port.configure(g_gpio.port.context, pin->pin, hw_gpio_port_flags(pin))
        : HW_STATUS_OK;
}

/* A mode the program may ask for on this pin. */
static HwStatus hw_gpio_check_flags(const HwGpioPin* pin, uint32_t flags)
{
    uint32_t caps = pin->capabilities;
    uint8_t output = (flags & HW_GPIO_OUTPUT) != 0U;

    if ((flags & ~HW_GPIO_GUEST_FLAGS) != 0U ||
        ((flags & HW_GPIO_PULL_UP) != 0U && (flags & HW_GPIO_PULL_DOWN) != 0U) ||
        (!output && (flags & (HW_GPIO_OPEN_DRAIN | HW_GPIO_INIT_HIGH)) != 0U))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if ((output && (caps & HW_GPIO_CAN_OUTPUT) == 0U) ||
        (!output && (caps & HW_GPIO_CAN_INPUT) == 0U) ||
        ((flags & HW_GPIO_PULL_UP) != 0U && (caps & HW_GPIO_CAN_PULL_UP) == 0U) ||
        ((flags & HW_GPIO_PULL_DOWN) != 0U && (caps & HW_GPIO_CAN_PULL_DOWN) == 0U) ||
        ((flags & HW_GPIO_OPEN_DRAIN) != 0U && (caps & HW_GPIO_CAN_OPEN_DRAIN) == 0U))
    {
        return HW_STATUS_UNSUPPORTED;
    }
    return HW_STATUS_OK;
}

HwStatus hw_gpio_port_set(const HwGpioPort* port)
{
    if (port == NULL)
    {
        memset(&g_gpio.port, 0, sizeof(g_gpio.port));
        g_gpio.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->write == NULL || port->read == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_gpio.port = *port;
    g_gpio.has_port = 1U;
    return HW_STATUS_OK;
}

static HwGpioName* hw_gpio_find_name(const char* name, uint32_t length)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_GPIO_MAX_NAMES; ++index)
    {
        HwGpioName* entry = g_gpio.names[index];
        if (entry != NULL && entry->length == length && memcmp(entry->name, name, length) == 0)
        {
            return entry;
        }
    }
    return NULL;
}

HwStatus hw_gpio_add_pin(const char* name, uint32_t pin, uint32_t capabilities, uint32_t options)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    uint32_t slot = hw_gpio_index_of(pin);
    uint32_t name_slot = 0U;
    uint8_t new_pin = 0U;
    HwGpioName* entry = NULL;

    if (length == 0U || length > HW_GPIO_NAME_MAX || (capabilities & ~HW_GPIO_CAN_ALL) != 0U ||
        (options & ~HW_GPIO_ACTIVE_LOW) != 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (hw_gpio_find_name(name, (uint32_t)length) != NULL)
    {
        return HW_STATUS_BUSY;
    }
    if (slot < HW_GPIO_MAX_PINS &&
        (g_gpio.pins[slot]->capabilities != capabilities || g_gpio.pins[slot]->options != options))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    while (name_slot < HW_GPIO_MAX_NAMES && g_gpio.names[name_slot] != NULL)
    {
        ++name_slot;
    }
    if (slot == HW_GPIO_MAX_PINS)
    {
        for (slot = 0U; slot < HW_GPIO_MAX_PINS && g_gpio.pins[slot] != NULL; ++slot)
        {
        }
        new_pin = 1U;
    }
    if (name_slot == HW_GPIO_MAX_NAMES || slot == HW_GPIO_MAX_PINS)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    /* A platform with pins gives programs the gpio interface. */
    if (hw_driver_add(&hw_gpio_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }

    entry = (HwGpioName*)malloc(sizeof(HwGpioName) + length);
    if (entry == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    if (new_pin)
    {
        HwGpioPin* created = (HwGpioPin*)calloc(1U, sizeof(HwGpioPin));
        if (created == NULL)
        {
            free(entry);
            return HW_STATUS_OUT_OF_MEMORY;
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
    return HW_STATUS_OK;
}

void hw_gpio_edge(uint32_t pin, uint32_t edges)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        HwGpioPin* slot = g_gpio.pins[index];
        if (slot != NULL && slot->pin == pin)
        {
            if ((edges & HW_GPIO_EDGE_RISING) != 0U)
            {
                HW_GPIO_COUNT(slot->rising);
            }
            if ((edges & HW_GPIO_EDGE_FALLING) != 0U)
            {
                HW_GPIO_COUNT(slot->falling);
            }
            return;
        }
    }
}

uint32_t hw_gpio_get_owner(uint32_t pin)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        if (g_gpio.pins[index] != NULL && g_gpio.pins[index]->pin == pin)
        {
            return g_gpio.pins[index]->owner;
        }
    }
    return 0U;
}

void hw_gpio_poll(void)
{
    uint8_t delivered = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        HwGpioPin* slot = g_gpio.pins[index];
        uint32_t rising = 0U;
        uint32_t falling = 0U;
        uint32_t counted = 0U;

        if (slot == NULL)
        {
            continue;
        }
        rising = hw_gpio_take_counter(&slot->rising);
        falling = hw_gpio_take_counter(&slot->falling);
        if (slot->owner == 0U || slot->watch == 0U)
        {
            continue;
        }
        if (hw_gpio_inverted(slot))
        {
            uint32_t swap = rising;
            rising = falling;
            falling = swap;
        }
        counted = ((slot->watch & HW_GPIO_GUEST_RISING) != 0U ? rising : 0U) +
                  ((slot->watch & HW_GPIO_GUEST_FALLING) != 0U ? falling : 0U);
        if (counted > 0U)
        {
            slot->events = counted > UINT32_MAX - slot->events ? UINT32_MAX : slot->events + counted;
            delivered = 1U;
        }
    }
    if (delivered)
    {
        hw_host_wait_any_recheck();
    }
}

HwStatus hw_gpio_open(const char* name, uint32_t length, uint32_t flags, uint32_t* out_id)
{
    HwGpioName* entry = hw_gpio_find_name(name, length);
    HwGpioPin* pin = entry != NULL ? g_gpio.pins[entry->pin - 1U] : NULL;
    HwStatus status = HW_STATUS_OK;

    if (!g_gpio.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (pin == NULL)
    {
        return HW_STATUS_FILE_NOT_FOUND;
    }
    if (pin->owner != 0U)
    {
        return HW_STATUS_BUSY;
    }
    status = hw_gpio_check_flags(pin, flags);
    if (status != HW_STATUS_OK)
    {
        return status;
    }

    pin->flags = flags;
    pin->level = (flags & HW_GPIO_INIT_HIGH) != 0U ? 1U : 0U;
    pin->watch = 0U;
    pin->events = 0U;
    (void)hw_gpio_take_counter(&pin->rising);
    (void)hw_gpio_take_counter(&pin->falling);
    status = hw_gpio_configure(pin);
    if (status != HW_STATUS_OK)
    {
        pin->flags = 0U;
        return status;
    }
    pin->owner = hw_host_task_id();
    *out_id = entry->pin;
    return HW_STATUS_OK;
}

HwStatus hw_gpio_open_capable(const char* name, uint32_t length, uint32_t capability, uint32_t flags,
                              uint32_t* out_id)
{
    HwGpioName* entry = hw_gpio_find_name(name, length);

    if (entry != NULL && (g_gpio.pins[entry->pin - 1U]->capabilities & capability) == 0U)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    return hw_gpio_open(name, length, flags, out_id);
}

HwStatus hw_gpio_open_pwm(const char* name, uint32_t length, uint8_t active_low, uint32_t* out_id,
                          uint8_t* out_inverted)
{
    HwGpioName* entry = hw_gpio_find_name(name, length);
    HwStatus status = HW_STATUS_OK;

    if (entry != NULL && (g_gpio.pins[entry->pin - 1U]->capabilities & HW_GPIO_CAN_PWM) == 0U)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    /* Logical 0 is "off" whichever way the pin is inverted. */
    status = hw_gpio_open(name, length, HW_GPIO_OUTPUT | (active_low ? HW_GPIO_GUEST_ACTIVE_LOW : 0U), out_id);
    if (status == HW_STATUS_OK)
    {
        *out_inverted = hw_gpio_inverted(g_gpio.pins[*out_id - 1U]);
    }
    return status;
}

uint32_t hw_gpio_pin_number(uint32_t id)
{
    HwGpioPin* pin = hw_gpio_pin(id);
    return pin != NULL ? pin->pin : 0U;
}

HwStatus hw_gpio_open_physical(const char* name, uint32_t length, uint32_t* out_id)
{
    HwGpioName* entry = hw_gpio_find_name(name, length);
    uint32_t flags = HW_GPIO_OUTPUT | HW_GPIO_INIT_HIGH;

    /* Undo the board's inversion, so 1 is high on the pin. */
    if (entry != NULL && (g_gpio.pins[entry->pin - 1U]->options & HW_GPIO_ACTIVE_LOW) != 0U)
    {
        flags |= HW_GPIO_GUEST_ACTIVE_LOW;
    }
    return hw_gpio_open(name, length, flags, out_id);
}

void hw_gpio_close(uint32_t id)
{
    HwGpioPin* pin = hw_gpio_pin(id);

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
        (void)hw_gpio_configure(pin);
    }
}

HwStatus hw_gpio_set(uint32_t id, uint32_t level)
{
    HwGpioPin* pin = hw_gpio_pin(id);
    HwStatus status = HW_STATUS_OK;

    if (pin == NULL || !g_gpio.has_port)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if ((pin->flags & HW_GPIO_OUTPUT) == 0U || level > 1U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    status = g_gpio.port.write(g_gpio.port.context, pin->pin, (uint8_t)(level ^ hw_gpio_inverted(pin)));
    if (status == HW_STATUS_OK)
    {
        pin->level = (uint8_t)level;
    }
    return status;
}

HwStatus hw_gpio_get(uint32_t id, uint32_t* out_level)
{
    HwGpioPin* pin = hw_gpio_pin(id);
    uint8_t level = 0U;
    HwStatus status = HW_STATUS_OK;

    if (pin == NULL || !g_gpio.has_port)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    status = g_gpio.port.read(g_gpio.port.context, pin->pin, &level);
    if (status == HW_STATUS_OK)
    {
        *out_level = (uint32_t)((level != 0U) ^ hw_gpio_inverted(pin));
    }
    return status;
}

HwStatus hw_gpio_set_mode(uint32_t id, uint32_t flags)
{
    HwGpioPin* pin = hw_gpio_pin(id);
    uint32_t previous = 0U;
    HwStatus status = HW_STATUS_OK;

    if (pin == NULL || !g_gpio.has_port)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    status = hw_gpio_check_flags(pin, flags);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    previous = pin->flags;
    pin->flags = flags;
    status = hw_gpio_configure(pin);
    if (status != HW_STATUS_OK)
    {
        pin->flags = previous;
    }
    else
    {
        pin->level = (flags & HW_GPIO_INIT_HIGH) != 0U ? 1U : 0U;
    }
    return status;
}

HwStatus hw_gpio_watch(uint32_t id, uint32_t edges)
{
    HwGpioPin* pin = hw_gpio_pin(id);
    uint32_t previous = 0U;
    HwStatus status = HW_STATUS_OK;

    if (pin == NULL || !g_gpio.has_port || (edges & ~HW_GPIO_GUEST_EDGES) != 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (edges != 0U && (pin->capabilities & HW_GPIO_CAN_EDGES) == 0U)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    previous = pin->watch;
    pin->watch = edges;
    status = hw_gpio_configure(pin);
    if (status != HW_STATUS_OK)
    {
        pin->watch = previous;
        return status;
    }
    /* Edges from before the watch began do not count. */
    pin->events = 0U;
    (void)hw_gpio_take_counter(&pin->rising);
    (void)hw_gpio_take_counter(&pin->falling);
    return HW_STATUS_OK;
}

uint32_t hw_gpio_take_events(uint32_t id)
{
    HwGpioPin* pin = hw_gpio_pin(id);
    uint32_t events = 0U;

    hw_gpio_poll();
    if (pin != NULL)
    {
        events = pin->events;
        pin->events = 0U;
    }
    return events;
}

uint8_t hw_gpio_ready(uint32_t id)
{
    HwGpioPin* pin = hw_gpio_pin(id);

    hw_gpio_poll();
    return pin != NULL && pin->events > 0U;
}

void hw_gpio_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        free(g_gpio.pins[index]);
    }
    for (index = 0U; index < HW_GPIO_MAX_NAMES; ++index)
    {
        free(g_gpio.names[index]);
    }
    memset(&g_gpio, 0, sizeof(g_gpio));
}

uint32_t hw_gpio_wake_pins(HwGpioWake* out, uint32_t max)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        const HwGpioPin* pin = g_gpio.pins[index];
        if (pin == NULL || pin->owner == 0U || pin->watch == 0U)
        {
            continue;
        }
        if (count < max && out != NULL)
        {
            out[count].pin = pin->pin;
            out[count].edges = hw_gpio_port_flags(pin) & (HW_GPIO_EDGE_RISING | HW_GPIO_EDGE_FALLING);
        }
        ++count;
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: every pin a task holds, by its first name
 * ------------------------------------------------------------------------ */

static void hw_gpio_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    /* Edges the interrupt handlers counted become the programs' events. */
    hw_gpio_poll();
    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        count += g_gpio.pins[index] != NULL && g_gpio.pins[index]->owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_GPIO_MAX_PINS; ++index)
    {
        const HwGpioPin* pin = g_gpio.pins[index];
        uint32_t name = 0U;

        if (pin == NULL || pin->owner == 0U)
        {
            continue;
        }
        while (name < HW_GPIO_MAX_NAMES && (g_gpio.names[name] == NULL || g_gpio.names[name]->pin != index + 1U))
        {
            ++name;
        }
        hw_host_system_put32(stream, index + 1U);
        hw_host_system_put32(stream, pin->owner);
        hw_host_system_put_bytes(stream, g_gpio.names[name]->name, g_gpio.names[name]->length);
        hw_host_system_put32(stream, pin->flags);
        hw_host_system_put32(stream, pin->watch);
        hw_host_system_put32(stream, pin->events);
        hw_host_system_put32(stream, pin->level);
    }
}

static void hw_gpio_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_GPIO_MAX_PINS);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        char name[HW_GPIO_NAME_MAX];
        uint32_t old_id = hw_host_system_get32(stream);
        uint32_t owner = hw_host_system_get32(stream);
        uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
        uint32_t flags = hw_host_system_get32(stream);
        uint32_t watch = hw_host_system_get32(stream);
        uint32_t events = hw_host_system_get32(stream);
        uint8_t level = (uint8_t)hw_host_system_get_max(stream, 1U);
        HwGpioName* entry = NULL;
        HwGpioPin* pin = NULL;

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        entry = hw_gpio_find_name(name, length);
        pin = entry != NULL ? g_gpio.pins[entry->pin - 1U] : NULL;
        if (pin == NULL || !g_gpio.has_port)
        {
            hw_host_system_device(stream, &hw_gpio_driver, old_id, 0U, NULL);
            continue;
        }
        if (pin->owner != 0U || owner == 0U)
        {
            /* Two saved pins on one: the stream is broken. */
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        if (hw_gpio_check_flags(pin, flags) != HW_STATUS_OK || (watch & ~HW_GPIO_GUEST_EDGES) != 0U ||
            (watch != 0U && (pin->capabilities & HW_GPIO_CAN_EDGES) == 0U))
        {
            hw_host_system_device(stream, &hw_gpio_driver, old_id, 0U, "a device it held cannot be set up");
            continue;
        }
        /* An output comes back at the level the program left it. */
        pin->flags = (flags & HW_GPIO_OUTPUT) != 0U
            ? (flags & ~HW_GPIO_INIT_HIGH) | (level != 0U ? HW_GPIO_INIT_HIGH : 0U)
            : flags;
        pin->level = level;
        pin->watch = watch;
        (void)hw_gpio_take_counter(&pin->rising);
        (void)hw_gpio_take_counter(&pin->falling);
        if (hw_gpio_configure(pin) != HW_STATUS_OK)
        {
            pin->flags = 0U;
            pin->watch = 0U;
            (void)hw_gpio_configure(pin);
            hw_host_system_device(stream, &hw_gpio_driver, old_id, 0U, "a device it held cannot be set up");
            continue;
        }
        pin->events = events;
        pin->owner = owner;
        hw_host_system_device(stream, &hw_gpio_driver, old_id, entry->pin, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The gpio interface for programs
 * ------------------------------------------------------------------------ */

static HW_HOST_FUNCTION(hw_gpio_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, name_address);
    HW_HOST_ARG(uint32_t, name_length);
    HW_HOST_ARG(uint32_t, flags);
    const char* name = NULL;
    char name_storage[HW_ABI_NAME_MAX];
    uint32_t id = 0U;
    int32_t handle = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    handle = hw_abi_read_name(name_address, name_length, name_storage, &name);
    if (handle < 0)
    {
        HW_HOST_RETURN(handle);
    }
    if (name == NULL)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    status = hw_gpio_open(name, name_length, flags, &id);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status));
    }
    handle = hw_driver_handle_new(&hw_gpio_driver, id);
    if (handle < 0)
    {
        hw_gpio_close(id);
    }
    HW_HOST_RETURN(handle);
}

static HW_HOST_FUNCTION(hw_gpio_call_set)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, level);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_gpio_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_gpio_set(id, level)));
}

static HW_HOST_FUNCTION(hw_gpio_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_gpio_driver, handle, &id);
    uint32_t level = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    status = hw_gpio_get(id, &level);
    HW_HOST_RETURN(status == HW_STATUS_OK ? (int32_t)level : hw_abi_error(status));
}

static HW_HOST_FUNCTION(hw_gpio_call_set_mode)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, flags);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_gpio_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_gpio_set_mode(id, flags)));
}

static HW_HOST_FUNCTION(hw_gpio_call_watch)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, edges);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_gpio_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_gpio_watch(id, edges)));
}

static HW_HOST_FUNCTION(hw_gpio_call_events)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_gpio_driver, handle, &id);
    uint32_t events = 0U;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    events = hw_gpio_take_events(id);
    HW_HOST_RETURN((int32_t)(events > 0x7FFFFFFFU ? 0x7FFFFFFFU : events));
}

static const HwDriverCall hw_gpio_calls[] =
{
    { "open", "i(iii)", hw_gpio_call_open, 1U },
    { "set", "i(ii)", hw_gpio_call_set, 1U },
    { "get", "i(i)", hw_gpio_call_get, 1U },
    { "set-mode", "i(ii)", hw_gpio_call_set_mode, 1U },
    { "watch", "i(ii)", hw_gpio_call_watch, 1U },
    { "events", "i(i)", hw_gpio_call_events, 1U },
};

const HwDriver hw_gpio_driver =
{
    .interface = "gpio",
    .calls = hw_gpio_calls,
    .call_count = (uint32_t)(sizeof(hw_gpio_calls) / sizeof(hw_gpio_calls[0])),
    .close = hw_gpio_close,
    .ready = hw_gpio_ready,
    .poll = hw_gpio_poll,
    .shutdown = hw_gpio_shutdown,
    .save = hw_gpio_save,
    .restore = hw_gpio_restore,
};
