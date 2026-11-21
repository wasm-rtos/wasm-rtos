/*
 * Input devices: buttons, rotary controls, touch surfaces and keyboards the
 * platform registers by name. Events queue for the program that owns the
 * device; long presses and key repeat come from the kernel's clock, with the
 * timings the platform gave, so every program feels the same.
 */

#include "hw_driver.h"

#include <stdlib.h>
#include <string.h>

typedef struct HwInputEvent
{
    uint8_t type;
    uint16_t code;
    int32_t value;
    uint16_t x;
    uint16_t y;
    uint32_t time_ms;
} HwInputEvent;

/* A key that is down, with the times its long press and next repeat are due (0: not due). */
typedef struct HwInputHeld
{
    uint8_t used;
    uint16_t code;
    uint64_t long_at;
    uint64_t repeat_at;
} HwInputHeld;

/* Allocated when the platform registers the device. */
typedef struct HwInputDevice
{
    uint32_t device;
    char name[HW_INPUT_NAME_MAX + 1U];
    uint8_t name_length;
    HwInputInfo info;
    /* Owning task id, 0 when free; events are kept only while owned. */
    uint32_t owner;
    /* Allocated while a program owns the device: events are kept only then. */
    HwInputEvent* queue;
    uint32_t head;
    uint32_t count;
    /* Events lost to a full queue since the program last read. */
    uint32_t dropped;
    HwInputHeld held[HW_INPUT_MAX_HELD];
} HwInputDevice;

static struct
{
    HwInputDevice* devices[HW_INPUT_MAX];
} g_input;

static HwInputDevice* hw_input_slot(uint32_t id)
{
    return id >= 1U && id <= HW_INPUT_MAX ? g_input.devices[id - 1U] : NULL;
}

static HwInputDevice* hw_input_by_number(uint32_t device)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_INPUT_MAX; ++index)
    {
        if (g_input.devices[index] != NULL && g_input.devices[index]->device == device)
        {
            return g_input.devices[index];
        }
    }
    return NULL;
}

static void hw_input_clear(HwInputDevice* device)
{
    device->head = 0U;
    device->count = 0U;
    device->dropped = 0U;
    memset(device->held, 0, sizeof(device->held));
}

static HwInputEvent* hw_input_last(HwInputDevice* device)
{
    return device->count > 0U ? &device->queue[(device->head + device->count - 1U) % HW_INPUT_QUEUE] : NULL;
}

/* Queue an event; when the queue is full the oldest goes and the loss is counted. */
static void hw_input_push(HwInputDevice* device, uint8_t type, uint32_t code, int32_t value, uint32_t x, uint32_t y)
{
    HwInputEvent* event = NULL;

    if (device->count == HW_INPUT_QUEUE)
    {
        device->head = (device->head + 1U) % HW_INPUT_QUEUE;
        --device->count;
        if (device->dropped < 0x7FFFFFFFU)
        {
            ++device->dropped;
        }
    }
    event = &device->queue[(device->head + device->count) % HW_INPUT_QUEUE];
    ++device->count;
    event->type = type;
    event->code = (uint16_t)code;
    event->value = value;
    event->x = (uint16_t)x;
    event->y = (uint16_t)y;
    event->time_ms = (uint32_t)hw_host_time_ms64();
    hw_host_wait_any_recheck();
}

extern const HwDriver hw_input_driver;

HwStatus hw_input_add(const char* name, uint32_t device, const HwInputInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    HwInputDevice* slot = NULL;
    uint32_t free_index = HW_INPUT_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > HW_INPUT_NAME_MAX || info == NULL || info->kind < HW_INPUT_BUTTONS ||
        info->kind > HW_INPUT_KEYBOARD || (info->kind == HW_INPUT_TOUCH && (info->width == 0U || info->height == 0U)) ||
        (info->repeat_delay_ms != 0U && info->repeat_interval_ms == 0U))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_INPUT_MAX; ++index)
    {
        HwInputDevice* entry = g_input.devices[index];
        if (entry != NULL && (entry->device == device ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return HW_STATUS_BUSY;
        }
        if (entry == NULL && free_index == HW_INPUT_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == HW_INPUT_MAX)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot = (HwInputDevice*)calloc(1U, sizeof(HwInputDevice));
    if (slot == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    g_input.devices[free_index] = slot;
    slot->device = device;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    /* A platform with these devices gives programs the input interface. */
    if (hw_driver_add(&hw_input_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

HwStatus hw_input_key(uint32_t number, uint32_t code, uint8_t down)
{
    HwInputDevice* device = hw_input_by_number(number);
    HwInputHeld* held = NULL;
    HwInputHeld* free_slot = NULL;
    uint32_t index = 0U;

    if (device == NULL || device->info.kind == HW_INPUT_TOUCH || code > 0xFFFFU || down > 1U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner == 0U)
    {
        return HW_STATUS_OK;
    }
    for (index = 0U; index < HW_INPUT_MAX_HELD; ++index)
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
        hw_input_push(device, HW_INPUT_EVENT_KEY, code, HW_INPUT_KEY_RELEASED, 0U, 0U);
        return HW_STATUS_OK;
    }
    if (held != NULL)
    {
        /* Already down: the platform's own repeat; the kernel makes repeats itself. */
        return HW_STATUS_OK;
    }
    hw_input_push(device, HW_INPUT_EVENT_KEY, code, HW_INPUT_KEY_PRESSED, 0U, 0U);
    /* Past HW_INPUT_MAX_HELD keys at once, the extra ones get no long press or repeat. */
    if (free_slot != NULL && (device->info.long_press_ms != 0U || device->info.repeat_delay_ms != 0U))
    {
        uint64_t now = hw_host_time_ms64();
        free_slot->used = 1U;
        free_slot->code = (uint16_t)code;
        free_slot->long_at = device->info.long_press_ms != 0U ? now + device->info.long_press_ms : 0U;
        free_slot->repeat_at = device->info.repeat_delay_ms != 0U ? now + device->info.repeat_delay_ms : 0U;
    }
    return HW_STATUS_OK;
}

HwStatus hw_input_rotate(uint32_t number, uint32_t code, int32_t steps)
{
    HwInputDevice* device = hw_input_by_number(number);
    HwInputEvent* last = NULL;

    if (device == NULL || device->info.kind != HW_INPUT_ROTARY || code > 0xFFFFU)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner == 0U || steps == 0)
    {
        return HW_STATUS_OK;
    }
    /* Turns the program has not read yet add up, so a fast spin never fills the queue. */
    last = hw_input_last(device);
    if (last != NULL && last->type == HW_INPUT_EVENT_ROTATE && last->code == code &&
        ((steps > 0 && last->value <= INT32_MAX - steps) || (steps < 0 && last->value >= INT32_MIN - steps)))
    {
        last->value += steps;
        last->time_ms = (uint32_t)hw_host_time_ms64();
        return HW_STATUS_OK;
    }
    hw_input_push(device, HW_INPUT_EVENT_ROTATE, code, steps, 0U, 0U);
    return HW_STATUS_OK;
}

HwStatus hw_input_touch(uint32_t number, uint32_t contact, uint32_t phase, uint32_t x, uint32_t y)
{
    HwInputDevice* device = hw_input_by_number(number);
    HwInputEvent* last = NULL;

    if (device == NULL || device->info.kind != HW_INPUT_TOUCH || contact > 0xFFFFU ||
        phase > HW_INPUT_TOUCH_MOVE || x >= device->info.width || y >= device->info.height)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner == 0U)
    {
        return HW_STATUS_OK;
    }
    /* Only the latest position of an unread move matters. */
    last = hw_input_last(device);
    if (phase == HW_INPUT_TOUCH_MOVE && last != NULL && last->type == HW_INPUT_EVENT_TOUCH &&
        last->code == contact && last->value == (int32_t)HW_INPUT_TOUCH_MOVE)
    {
        last->x = (uint16_t)x;
        last->y = (uint16_t)y;
        last->time_ms = (uint32_t)hw_host_time_ms64();
        return HW_STATUS_OK;
    }
    hw_input_push(device, HW_INPUT_EVENT_TOUCH, contact, (int32_t)phase, x, y);
    return HW_STATUS_OK;
}

HwStatus hw_input_text(uint32_t number, uint32_t code_point)
{
    HwInputDevice* device = hw_input_by_number(number);

    if (device == NULL || device->info.kind != HW_INPUT_KEYBOARD || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (device->owner != 0U)
    {
        hw_input_push(device, HW_INPUT_EVENT_TEXT, 0U, (int32_t)code_point, 0U, 0U);
    }
    return HW_STATUS_OK;
}

uint32_t hw_input_get_owner(uint32_t number)
{
    HwInputDevice* device = hw_input_by_number(number);
    return device != NULL ? device->owner : 0U;
}

HwStatus hw_input_open(const char* name, uint32_t length, uint32_t* out_id)
{
    uint8_t any = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_INPUT_MAX; ++index)
    {
        HwInputDevice* device = g_input.devices[index];
        any |= device != NULL;
        if (device != NULL && device->name_length == length && memcmp(device->name, name, length) == 0)
        {
            if (device->owner != 0U)
            {
                return HW_STATUS_BUSY;
            }
            device->queue = (HwInputEvent*)calloc(HW_INPUT_QUEUE, sizeof(HwInputEvent));
            if (device->queue == NULL)
            {
                return HW_STATUS_OUT_OF_MEMORY;
            }
            hw_input_clear(device);
            device->owner = hw_host_task_id();
            *out_id = index + 1U;
            return HW_STATUS_OK;
        }
    }
    /* A device without any input is told so, like one without a display. */
    return any ? HW_STATUS_FILE_NOT_FOUND : HW_STATUS_UNSUPPORTED;
}

void hw_input_close(uint32_t id)
{
    HwInputDevice* device = hw_input_slot(id);

    if (device != NULL && device->owner != 0U)
    {
        hw_input_clear(device);
        device->owner = 0U;
        free(device->queue);
        device->queue = NULL;
    }
}

HwStatus hw_input_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwInputDevice* device = hw_input_slot(id);

    if (device == NULL || device->owner == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case HW_INPUT_PROP_KIND: *out_value = device->info.kind; return HW_STATUS_OK;
        case HW_INPUT_PROP_WIDTH: *out_value = device->info.width; return HW_STATUS_OK;
        case HW_INPUT_PROP_HEIGHT: *out_value = device->info.height; return HW_STATUS_OK;
        case HW_INPUT_PROP_STEPS_PER_TURN: *out_value = device->info.steps_per_turn; return HW_STATUS_OK;
        default: return HW_STATUS_UNSUPPORTED;
    }
}

static void hw_input_write_event(uint8_t* out, uint8_t type, uint16_t code, int32_t value, uint16_t x, uint16_t y,
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

uint32_t hw_input_read(uint32_t id, uint8_t* out, uint32_t max_events)
{
    HwInputDevice* device = hw_input_slot(id);
    uint32_t written = 0U;

    if (device == NULL || device->owner == 0U)
    {
        return 0U;
    }
    hw_input_update_time();
    /* A loss is reported where it happened: before the events that survived it. */
    if (device->dropped > 0U && written < max_events)
    {
        hw_input_write_event(out, HW_INPUT_EVENT_DROPPED, 0U, (int32_t)device->dropped, 0U, 0U,
                             (uint32_t)hw_host_time_ms64());
        device->dropped = 0U;
        ++written;
    }
    while (device->count > 0U && written < max_events)
    {
        const HwInputEvent* event = &device->queue[device->head];
        hw_input_write_event(out + written * HW_INPUT_EVENT_SIZE, event->type, event->code, event->value, event->x,
                             event->y, event->time_ms);
        device->head = (device->head + 1U) % HW_INPUT_QUEUE;
        --device->count;
        ++written;
    }
    return written;
}

uint8_t hw_input_ready(uint32_t id)
{
    HwInputDevice* device = hw_input_slot(id);
    return device != NULL && device->owner != 0U && (device->count > 0U || device->dropped > 0U);
}

void hw_input_update_time(void)
{
    uint64_t now = hw_host_time_ms64();
    uint32_t index = 0U;

    for (index = 0U; index < HW_INPUT_MAX; ++index)
    {
        HwInputDevice* device = g_input.devices[index];
        uint32_t key = 0U;

        if (device == NULL || device->owner == 0U)
        {
            continue;
        }
        for (key = 0U; key < HW_INPUT_MAX_HELD; ++key)
        {
            HwInputHeld* held = &device->held[key];
            if (!held->used)
            {
                continue;
            }
            if (held->long_at != 0U && now >= held->long_at)
            {
                held->long_at = 0U;
                hw_input_push(device, HW_INPUT_EVENT_KEY, held->code, HW_INPUT_KEY_LONG_PRESS, 0U, 0U);
            }
            if (held->repeat_at != 0U && now >= held->repeat_at)
            {
                /* One repeat however late the clock comes, then the interval again. */
                held->repeat_at = now + device->info.repeat_interval_ms;
                hw_input_push(device, HW_INPUT_EVENT_KEY, held->code, HW_INPUT_KEY_REPEATED, 0U, 0U);
            }
        }
    }
}

uint32_t hw_input_next_deadline_ms(void)
{
    uint64_t now = hw_host_time_ms64();
    uint64_t next = UINT64_MAX;
    uint32_t index = 0U;

    for (index = 0U; index < HW_INPUT_MAX; ++index)
    {
        const HwInputDevice* device = g_input.devices[index];
        uint32_t key = 0U;

        if (device == NULL || device->owner == 0U)
        {
            continue;
        }
        for (key = 0U; key < HW_INPUT_MAX_HELD; ++key)
        {
            const HwInputHeld* held = &device->held[key];
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
        return HW_WAIT_FOREVER;
    }
    return next <= now ? 0U : (uint32_t)(next - now);
}

void hw_input_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_INPUT_MAX; ++index)
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
 * Saving the whole system: each device a task holds, by name, with the
 * events it has not read; keys held come back released
 * ------------------------------------------------------------------------ */

static void hw_input_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_INPUT_MAX; ++index)
    {
        count += g_input.devices[index] != NULL && g_input.devices[index]->owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_INPUT_MAX; ++index)
    {
        const HwInputDevice* device = g_input.devices[index];
        uint32_t event = 0U;

        if (device == NULL || device->owner == 0U)
        {
            continue;
        }
        hw_host_system_put32(stream, index + 1U);
        hw_host_system_put32(stream, device->owner);
        hw_host_system_put_bytes(stream, device->name, device->name_length);
        hw_host_system_put32(stream, device->dropped);
        hw_host_system_put32(stream, device->count);
        for (event = 0U; event < device->count; ++event)
        {
            const HwInputEvent* queued = &device->queue[(device->head + event) % HW_INPUT_QUEUE];
            hw_host_system_put32(stream, queued->type);
            hw_host_system_put32(stream, queued->code);
            hw_host_system_put32(stream, (uint32_t)queued->value);
            hw_host_system_put32(stream, queued->x);
            hw_host_system_put32(stream, queued->y);
            hw_host_system_put32(stream, queued->time_ms);
        }
    }
}

static void hw_input_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_INPUT_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        char name[HW_INPUT_NAME_MAX];
        uint32_t old_id = hw_host_system_get32(stream);
        uint32_t owner = hw_host_system_get32(stream);
        uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
        uint32_t dropped = hw_host_system_get32(stream);
        uint32_t events = hw_host_system_get_max(stream, HW_INPUT_QUEUE);
        HwInputDevice* device = NULL;
        uint32_t id = 0U;
        uint32_t index = 0U;

        for (index = 0U; index < HW_INPUT_MAX && hw_host_system_ok(stream); ++index)
        {
            HwInputDevice* entry = g_input.devices[index];
            if (device == NULL && entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
            {
                device = entry;
                id = index + 1U;
            }
        }
        if (hw_host_system_ok(stream) && device != NULL && (device->owner != 0U || owner == 0U))
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
        }
        if (hw_host_system_ok(stream) && device != NULL)
        {
            device->queue = (HwInputEvent*)calloc(HW_INPUT_QUEUE, sizeof(HwInputEvent));
            if (device->queue == NULL)
            {
                hw_host_system_fail(stream, HW_STATUS_OUT_OF_MEMORY);
            }
            else
            {
                hw_input_clear(device);
                device->dropped = dropped;
                device->count = events;
                device->owner = owner;
            }
        }
        for (index = 0U; index < events && hw_host_system_ok(stream); ++index)
        {
            HwInputEvent event;
            event.type = (uint8_t)hw_host_system_get32(stream);
            event.code = (uint16_t)hw_host_system_get32(stream);
            event.value = (int32_t)hw_host_system_get32(stream);
            event.x = (uint16_t)hw_host_system_get32(stream);
            event.y = (uint16_t)hw_host_system_get32(stream);
            event.time_ms = hw_host_system_get32(stream);
            if (device != NULL && device->queue != NULL)
            {
                device->queue[index] = event;
            }
        }
        if (hw_host_system_ok(stream))
        {
            hw_host_system_device(stream, &hw_input_driver, old_id, device != NULL ? id : 0U, NULL);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The input interface for programs
 * ------------------------------------------------------------------------ */

static HW_HOST_FUNCTION(hw_input_call_open)
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
    if (name == NULL || flags != 0U)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    status = hw_input_open(name, name_length, &id);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status));
    }
    handle = hw_driver_handle_new(&hw_input_driver, id);
    if (handle < 0)
    {
        hw_input_close(id);
    }
    HW_HOST_RETURN(handle);
}

static HW_HOST_FUNCTION(hw_input_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_input_driver, handle, &id);
    uint32_t value = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    status = hw_input_get(id, property, &value);
    HW_HOST_RETURN(status == HW_STATUS_OK ? (int32_t)(value & 0x7FFFFFFFU) : hw_abi_error(status));
}

static HW_HOST_FUNCTION(hw_input_call_read)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, events_address);
    HW_HOST_ARG(uint32_t, max_events);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_input_driver, handle, &id);
    HwGuestBuffer events;
    uint32_t count = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (max_events > UINT32_MAX / HW_INPUT_EVENT_SIZE)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_host_buffer_open(&events, events_address, max_events * HW_INPUT_EVENT_SIZE, HW_GUEST_OUT);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(HW_GUEST_BUFFER_ERROR(status));
    }
    count = hw_input_read(id, events.data, max_events);
    (void)hw_host_buffer_close(&events);
    HW_HOST_RETURN((int32_t)count);
}

static const HwDriverCall hw_input_calls[] =
{
    { "open", "i(iii)", hw_input_call_open, 2U },
    { "get", "i(ii)", hw_input_call_get, 2U },
    { "read", "i(iii)", hw_input_call_read, 2U },
};

const HwDriver hw_input_driver =
{
    .interface = "input",
    .calls = hw_input_calls,
    .call_count = (uint32_t)(sizeof(hw_input_calls) / sizeof(hw_input_calls[0])),
    .close = hw_input_close,
    .ready = hw_input_ready,
    .next_deadline_ms = hw_input_next_deadline_ms,
    .time = hw_input_update_time,
    .shutdown = hw_input_shutdown,
    .save = hw_input_save,
    .restore = hw_input_restore,
};

