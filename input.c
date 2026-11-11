/*
 * Input devices: buttons, rotary controls, touch surfaces and keyboards the
 * platform registers by name. Events queue for the program that owns the
 * device; long presses and key repeat come from the kernel's clock, with the
 * timings the platform gave, so every program feels the same.
 */

#include "driver.h"

#include <stdlib.h>
#include <string.h>

typedef struct OsInputEvent
{
    uint8_t type;
    uint16_t code;
    int32_t value;
    uint16_t x;
    uint16_t y;
    uint32_t time_ms;
} OsInputEvent;

/* A key that is down, with the times its long press and next repeat are due (0: not due). */
typedef struct OsInputHeld
{
    uint8_t used;
    uint16_t code;
    uint64_t long_at;
    uint64_t repeat_at;
} OsInputHeld;

/* Allocated when the platform registers the device. */
typedef struct OsInputDevice
{
    uint32_t device;
    char name[OS_INPUT_NAME_MAX + 1U];
    uint8_t name_length;
    OsInputInfo info;
    /* Owning task id, 0 when free; events are kept only while owned. */
    uint32_t owner;
    /* Allocated while a program owns the device: events are kept only then. */
    OsInputEvent* queue;
    uint32_t head;
    uint32_t count;
    /* Events lost to a full queue since the program last read. */
    uint32_t dropped;
    OsInputHeld held[OS_INPUT_MAX_HELD];
} OsInputDevice;

static struct
{
    OsInputDevice* devices[OS_INPUT_MAX];
} g_input;

static OsInputDevice* os_input_slot(uint32_t id)
{
    return id >= 1U && id <= OS_INPUT_MAX ? g_input.devices[id - 1U] : NULL;
}

static OsInputDevice* os_input_by_number(uint32_t device)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_INPUT_MAX; ++index)
    {
        if (g_input.devices[index] != NULL && g_input.devices[index]->device == device)
        {
            return g_input.devices[index];
        }
    }
    return NULL;
}

static void os_input_clear(OsInputDevice* device)
{
    device->head = 0U;
    device->count = 0U;
    device->dropped = 0U;
    memset(device->held, 0, sizeof(device->held));
}

static OsInputEvent* os_input_last(OsInputDevice* device)
{
    return device->count > 0U ? &device->queue[(device->head + device->count - 1U) % OS_INPUT_QUEUE] : NULL;
}

/* Queue an event; when the queue is full the oldest goes and the loss is counted. */
static void os_input_push(OsInputDevice* device, uint8_t type, uint32_t code, int32_t value, uint32_t x, uint32_t y)
{
    OsInputEvent* event = NULL;

    if (device->count == OS_INPUT_QUEUE)
    {
        device->head = (device->head + 1U) % OS_INPUT_QUEUE;
        --device->count;
        if (device->dropped < 0x7FFFFFFFU)
        {
            ++device->dropped;
        }
    }
    event = &device->queue[(device->head + device->count) % OS_INPUT_QUEUE];
    ++device->count;
    event->type = type;
    event->code = (uint16_t)code;
    event->value = value;
    event->x = (uint16_t)x;
    event->y = (uint16_t)y;
    event->time_ms = (uint32_t)os_time_ms64();
    os_wait_any_recheck();
}

extern const OsDriver os_input_driver;

OsStatus os_input_add(const char* name, uint32_t device, const OsInputInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsInputDevice* slot = NULL;
    uint32_t free_index = OS_INPUT_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > OS_INPUT_NAME_MAX || info == NULL || info->kind < OS_INPUT_BUTTONS ||
        info->kind > OS_INPUT_KEYBOARD || (info->kind == OS_INPUT_TOUCH && (info->width == 0U || info->height == 0U)) ||
        (info->repeat_delay_ms != 0U && info->repeat_interval_ms == 0U))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_INPUT_MAX; ++index)
    {
        OsInputDevice* entry = g_input.devices[index];
        if (entry != NULL && (entry->device == device ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (entry == NULL && free_index == OS_INPUT_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == OS_INPUT_MAX)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot = (OsInputDevice*)calloc(1U, sizeof(OsInputDevice));
    if (slot == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    g_input.devices[free_index] = slot;
    slot->device = device;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    /* A platform with these devices gives programs the input interface. */
    if (os_driver_add(&os_input_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
}

OsStatus os_input_key(uint32_t number, uint32_t code, uint8_t down)
{
    OsInputDevice* device = os_input_by_number(number);
    OsInputHeld* held = NULL;
    OsInputHeld* free_slot = NULL;
    uint32_t index = 0U;

    if (device == NULL || device->info.kind == OS_INPUT_TOUCH || code > 0xFFFFU || down > 1U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner == 0U)
    {
        return OS_STATUS_OK;
    }
    for (index = 0U; index < OS_INPUT_MAX_HELD; ++index)
    {
        if (device->held[index].used && device->held[index].code == code)
        {
            held = &device->held[index];
        }
        else if (!device->held[index].used && free_slot == NULL)
        {
            free_slot = &device->held[index];
        }
    }

    if (!down)
    {
        if (held != NULL)
        {
            held->used = 0U;
        }
        os_input_push(device, OS_INPUT_EVENT_KEY, code, OS_INPUT_KEY_RELEASED, 0U, 0U);
        return OS_STATUS_OK;
    }
    if (held != NULL)
    {
        /* Already down: the platform's own repeat; the kernel makes repeats itself. */
        return OS_STATUS_OK;
    }
    os_input_push(device, OS_INPUT_EVENT_KEY, code, OS_INPUT_KEY_PRESSED, 0U, 0U);
    /* Past OS_INPUT_MAX_HELD keys at once, the extra ones get no long press or repeat. */
    if (free_slot != NULL && (device->info.long_press_ms != 0U || device->info.repeat_delay_ms != 0U))
    {
        uint64_t now = os_time_ms64();
        free_slot->used = 1U;
        free_slot->code = (uint16_t)code;
        free_slot->long_at = device->info.long_press_ms != 0U ? now + device->info.long_press_ms : 0U;
        free_slot->repeat_at = device->info.repeat_delay_ms != 0U ? now + device->info.repeat_delay_ms : 0U;
    }
    return OS_STATUS_OK;
}

OsStatus os_input_rotate(uint32_t number, uint32_t code, int32_t steps)
{
    OsInputDevice* device = os_input_by_number(number);
    OsInputEvent* last = NULL;

    if (device == NULL || device->info.kind != OS_INPUT_ROTARY || code > 0xFFFFU)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner == 0U || steps == 0)
    {
        return OS_STATUS_OK;
    }
    /* Turns the program has not read yet add up, so a fast spin never fills the queue. */
    last = os_input_last(device);
    if (last != NULL && last->type == OS_INPUT_EVENT_ROTATE && last->code == code &&
        ((steps > 0 && last->value <= INT32_MAX - steps) || (steps < 0 && last->value >= INT32_MIN - steps)))
    {
        last->value += steps;
        last->time_ms = (uint32_t)os_time_ms64();
        return OS_STATUS_OK;
    }
    os_input_push(device, OS_INPUT_EVENT_ROTATE, code, steps, 0U, 0U);
    return OS_STATUS_OK;
}

OsStatus os_input_touch(uint32_t number, uint32_t contact, uint32_t phase, uint32_t x, uint32_t y)
{
    OsInputDevice* device = os_input_by_number(number);
    OsInputEvent* last = NULL;

    if (device == NULL || device->info.kind != OS_INPUT_TOUCH || contact > 0xFFFFU ||
        phase > OS_INPUT_TOUCH_MOVE || x >= device->info.width || y >= device->info.height)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner == 0U)
    {
        return OS_STATUS_OK;
    }
    /* Only the latest position of an unread move matters. */
    last = os_input_last(device);
    if (phase == OS_INPUT_TOUCH_MOVE && last != NULL && last->type == OS_INPUT_EVENT_TOUCH &&
        last->code == contact && last->value == (int32_t)OS_INPUT_TOUCH_MOVE)
    {
        last->x = (uint16_t)x;
        last->y = (uint16_t)y;
        last->time_ms = (uint32_t)os_time_ms64();
        return OS_STATUS_OK;
    }
    os_input_push(device, OS_INPUT_EVENT_TOUCH, contact, (int32_t)phase, x, y);
    return OS_STATUS_OK;
}

OsStatus os_input_text(uint32_t number, uint32_t code_point)
{
    OsInputDevice* device = os_input_by_number(number);

    if (device == NULL || device->info.kind != OS_INPUT_KEYBOARD || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner != 0U)
    {
        os_input_push(device, OS_INPUT_EVENT_TEXT, 0U, (int32_t)code_point, 0U, 0U);
    }
    return OS_STATUS_OK;
}

uint32_t os_input_get_owner(uint32_t number)
{
    OsInputDevice* device = os_input_by_number(number);
    return device != NULL ? device->owner : 0U;
}

OsStatus os_input_open(const char* name, uint32_t length, uint32_t* out_id)
{
    uint8_t any = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < OS_INPUT_MAX; ++index)
    {
        OsInputDevice* device = g_input.devices[index];
        any |= device != NULL;
        if (device != NULL && device->name_length == length && memcmp(device->name, name, length) == 0)
        {
            if (device->owner != 0U)
            {
                return OS_STATUS_BUSY;
            }
            device->queue = (OsInputEvent*)calloc(OS_INPUT_QUEUE, sizeof(OsInputEvent));
            if (device->queue == NULL)
            {
                return OS_STATUS_OUT_OF_MEMORY;
            }
            os_input_clear(device);
            device->owner = os_task_get_id(os_task_get_current());
            *out_id = index + 1U;
            return OS_STATUS_OK;
        }
    }
    /* A device without any input is told so, like one without a display. */
    return any ? OS_STATUS_FILE_NOT_FOUND : OS_STATUS_UNSUPPORTED;
}

void os_input_close(uint32_t id)
{
    OsInputDevice* device = os_input_slot(id);

    if (device != NULL && device->owner != 0U)
    {
        os_input_clear(device);
        device->owner = 0U;
        free(device->queue);
        device->queue = NULL;
    }
}

OsStatus os_input_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsInputDevice* device = os_input_slot(id);

    if (device == NULL || device->owner == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case OS_INPUT_PROP_KIND: *out_value = device->info.kind; return OS_STATUS_OK;
        case OS_INPUT_PROP_WIDTH: *out_value = device->info.width; return OS_STATUS_OK;
        case OS_INPUT_PROP_HEIGHT: *out_value = device->info.height; return OS_STATUS_OK;
        case OS_INPUT_PROP_STEPS_PER_TURN: *out_value = device->info.steps_per_turn; return OS_STATUS_OK;
        default: return OS_STATUS_UNSUPPORTED;
    }
}

static void os_input_write_event(uint8_t* out, uint8_t type, uint16_t code, int32_t value, uint16_t x, uint16_t y,
                                 uint32_t time_ms)
{
    uint32_t raw = (uint32_t)value;

    out[0] = type;
    out[1] = 0U;
    out[2] = (uint8_t)code;
    out[3] = (uint8_t)(code >> 8U);
    out[4] = (uint8_t)raw;
    out[5] = (uint8_t)(raw >> 8U);
    out[6] = (uint8_t)(raw >> 16U);
    out[7] = (uint8_t)(raw >> 24U);
    out[8] = (uint8_t)x;
    out[9] = (uint8_t)(x >> 8U);
    out[10] = (uint8_t)y;
    out[11] = (uint8_t)(y >> 8U);
    out[12] = (uint8_t)time_ms;
    out[13] = (uint8_t)(time_ms >> 8U);
    out[14] = (uint8_t)(time_ms >> 16U);
    out[15] = (uint8_t)(time_ms >> 24U);
}

uint32_t os_input_read(uint32_t id, uint8_t* out, uint32_t max_events)
{
    OsInputDevice* device = os_input_slot(id);
    uint32_t written = 0U;

    if (device == NULL || device->owner == 0U)
    {
        return 0U;
    }
    os_input_update_time();
    /* A loss is reported where it happened: before the events that survived it. */
    if (device->dropped > 0U && written < max_events)
    {
        os_input_write_event(out, OS_INPUT_EVENT_DROPPED, 0U, (int32_t)device->dropped, 0U, 0U,
                             (uint32_t)os_time_ms64());
        device->dropped = 0U;
        ++written;
    }
    while (device->count > 0U && written < max_events)
    {
        const OsInputEvent* event = &device->queue[device->head];
        os_input_write_event(out + written * OS_INPUT_EVENT_SIZE, event->type, event->code, event->value, event->x,
                             event->y, event->time_ms);
        device->head = (device->head + 1U) % OS_INPUT_QUEUE;
        --device->count;
        ++written;
    }
    return written;
}

uint8_t os_input_ready(uint32_t id)
{
    OsInputDevice* device = os_input_slot(id);
    return device != NULL && device->owner != 0U && (device->count > 0U || device->dropped > 0U);
}

void os_input_update_time(void)
{
    uint64_t now = os_time_ms64();
    uint32_t index = 0U;

    for (index = 0U; index < OS_INPUT_MAX; ++index)
    {
        OsInputDevice* device = g_input.devices[index];
        uint32_t key = 0U;

        if (device == NULL || device->owner == 0U)
        {
            continue;
        }
        for (key = 0U; key < OS_INPUT_MAX_HELD; ++key)
        {
            OsInputHeld* held = &device->held[key];
            if (!held->used)
            {
                continue;
            }
            if (held->long_at != 0U && now >= held->long_at)
            {
                held->long_at = 0U;
                os_input_push(device, OS_INPUT_EVENT_KEY, held->code, OS_INPUT_KEY_LONG_PRESS, 0U, 0U);
            }
            if (held->repeat_at != 0U && now >= held->repeat_at)
            {
                /* One repeat however late the clock comes, then the interval again. */
                held->repeat_at = now + device->info.repeat_interval_ms;
                os_input_push(device, OS_INPUT_EVENT_KEY, held->code, OS_INPUT_KEY_REPEATED, 0U, 0U);
            }
        }
    }
}

uint32_t os_input_next_deadline_ms(void)
{
    uint64_t now = os_time_ms64();
    uint64_t next = UINT64_MAX;
    uint32_t index = 0U;

    for (index = 0U; index < OS_INPUT_MAX; ++index)
    {
        const OsInputDevice* device = g_input.devices[index];
        uint32_t key = 0U;

        if (device == NULL || device->owner == 0U)
        {
            continue;
        }
        for (key = 0U; key < OS_INPUT_MAX_HELD; ++key)
        {
            const OsInputHeld* held = &device->held[key];
            if (held->used && held->long_at != 0U && held->long_at < next)
            {
                next = held->long_at;
            }
            if (held->used && held->repeat_at != 0U && held->repeat_at < next)
            {
                next = held->repeat_at;
            }
        }
    }
    if (next == UINT64_MAX)
    {
        return OS_WAIT_FOREVER;
    }
    return next <= now ? 0U : (uint32_t)(next - now);
}

void os_input_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_INPUT_MAX; ++index)
    {
        if (g_input.devices[index] != NULL)
        {
            free(g_input.devices[index]->queue);
            free(g_input.devices[index]);
        }
    }
    memset(&g_input, 0, sizeof(g_input));
}

/* ---------------------------------------------------------------------------
 * The input interface for programs
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_input_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, flags);
    const char* name = NULL;
    char name_storage[OS_ABI_NAME_MAX];
    uint32_t id = 0U;
    int32_t handle = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    handle = os_abi_read_name(name_address, name_length, name_storage, &name);
    if (handle < 0)
    {
        OS_HOST_RETURN(handle);
    }
    if (name == NULL || flags != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    status = os_input_open(name, name_length, &id);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(status == OS_STATUS_BUSY ? -WR_EINUSE : os_abi_error(status));
    }
    handle = os_driver_handle_new(&os_input_driver, id);
    if (handle < 0)
    {
        os_input_close(id);
    }
    OS_HOST_RETURN(handle);
}

static OS_HOST_FUNCTION(os_input_call_get)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_input_driver, handle, &id);
    uint32_t value = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    status = os_input_get(id, property, &value);
    OS_HOST_RETURN(status == OS_STATUS_OK ? (int32_t)(value & 0x7FFFFFFFU) : os_abi_error(status));
}

static OS_HOST_FUNCTION(os_input_call_read)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, events_address);
    OS_HOST_ARG(uint32_t, max_events);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_input_driver, handle, &id);
    OsGuestBuffer events;
    uint32_t count = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (max_events > UINT32_MAX / OS_INPUT_EVENT_SIZE)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_guest_buffer_open(&events, events_address, max_events * OS_INPUT_EVENT_SIZE, OS_GUEST_OUT);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(OS_GUEST_BUFFER_ERROR(status));
    }
    count = os_input_read(id, events.data, max_events);
    (void)os_guest_buffer_close(&events);
    OS_HOST_RETURN((int32_t)count);
}

static const OsDriverCall os_input_calls[] =
{
    { "open", "i(iii)", os_input_call_open, 2U },
    { "get", "i(ii)", os_input_call_get, 2U },
    { "read", "i(iii)", os_input_call_read, 2U },
};

const OsDriver os_input_driver =
{
    .interface = "input",
    .calls = os_input_calls,
    .call_count = (uint32_t)(sizeof(os_input_calls) / sizeof(os_input_calls[0])),
    .close = os_input_close,
    .ready = os_input_ready,
    .next_deadline_ms = os_input_next_deadline_ms,
    .time = os_input_update_time,
    .shutdown = os_input_shutdown,
};

