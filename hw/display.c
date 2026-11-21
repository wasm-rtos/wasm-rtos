/*
 * Displays: screens the platform registers by name, lent to one program at a
 * time. The kernel knows no panel; the platform's HwDisplayPort draws.
 */

#include "hw_driver.h"

#include <string.h>

/* Bytes of pixels fill writes at a time when the port has no fill of its own. */
#define HW_DISPLAY_FILL_CHUNK 256U

#define HW_DISPLAY_KNOWN_FORMATS \
    (HW_DISPLAY_FORMAT_BIT(HW_DISPLAY_MONO1) | HW_DISPLAY_FORMAT_BIT(HW_DISPLAY_RGB565) | \
     HW_DISPLAY_FORMAT_BIT(HW_DISPLAY_XRGB8888))

typedef struct HwDisplay
{
    uint8_t used;
    uint32_t display;
    char name[HW_DISPLAY_NAME_MAX + 1U];
    uint8_t name_length;
    HwDisplayInfo info;
    /* Owning task id, 0 when free. */
    uint32_t owner;
} HwDisplay;

static struct
{
    HwDisplayPort port;
    uint8_t has_port;
    HwDisplay displays[HW_DISPLAY_MAX];
} g_display;

static HwDisplay* hw_display_slot(uint32_t id)
{
    return id >= 1U && id <= HW_DISPLAY_MAX && g_display.displays[id - 1U].used ? &g_display.displays[id - 1U] : NULL;
}

/* An open display a program may draw on now. */
static HwStatus hw_display_usable(uint32_t id, HwDisplay** out_display)
{
    HwDisplay* display = hw_display_slot(id);

    if (display == NULL || display->owner == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_display.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    *out_display = display;
    return HW_STATUS_OK;
}

/* Bytes a width x height rectangle takes in format. */
static uint64_t hw_display_rect_bytes(uint32_t format, uint32_t width, uint32_t height)
{
    switch (format)
    {
        case HW_DISPLAY_MONO1: return (uint64_t)((width + 7U) / 8U) * height;
        case HW_DISPLAY_RGB565: return (uint64_t)width * height * 2U;
        case HW_DISPLAY_XRGB8888: return (uint64_t)width * height * 4U;
        default: return 0U;
    }
}

static HwStatus hw_display_check_rect(const HwDisplay* display, uint32_t x, uint32_t y, uint32_t width,
                                      uint32_t height, uint32_t format)
{
    if (format == 0U || format > 31U || (display->info.formats & HW_DISPLAY_FORMAT_BIT(format)) == 0U)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if ((uint64_t)x + width > display->info.width || (uint64_t)y + height > display->info.height)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    return HW_STATUS_OK;
}

HwStatus hw_display_port_set(const HwDisplayPort* port)
{
    if (port == NULL)
    {
        memset(&g_display.port, 0, sizeof(g_display.port));
        g_display.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->write == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_display.port = *port;
    g_display.has_port = 1U;
    return HW_STATUS_OK;
}

static HwDisplay* hw_display_find_name(const char* name, uint32_t length)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_DISPLAY_MAX; ++index)
    {
        HwDisplay* display = &g_display.displays[index];
        if (display->used && display->name_length == length && memcmp(display->name, name, length) == 0)
        {
            return display;
        }
    }
    return NULL;
}

extern const HwDriver hw_display_driver;

HwStatus hw_display_add(const char* name, uint32_t display, const HwDisplayInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    HwDisplay* slot = NULL;
    uint32_t index = 0U;

    if (length == 0U || length > HW_DISPLAY_NAME_MAX || info == NULL || info->width == 0U || info->height == 0U ||
        info->shape > HW_DISPLAY_ROUND || info->format == 0U || info->format > HW_DISPLAY_XRGB8888 ||
        (info->formats & ~HW_DISPLAY_KNOWN_FORMATS) != 0U ||
        (info->formats & HW_DISPLAY_FORMAT_BIT(info->format)) == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_DISPLAY_MAX; ++index)
    {
        HwDisplay* entry = &g_display.displays[index];
        if (entry->used && (entry->display == display ||
                            (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return HW_STATUS_BUSY;
        }
        if (!entry->used && slot == NULL)
        {
            slot = entry;
        }
    }
    if (slot == NULL)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1U;
    slot->display = display;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    /* A platform with these devices gives programs the display interface. */
    if (hw_driver_add(&hw_display_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

uint32_t hw_display_get_owner(uint32_t display)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_DISPLAY_MAX; ++index)
    {
        if (g_display.displays[index].used && g_display.displays[index].display == display)
        {
            return g_display.displays[index].owner;
        }
    }
    return 0U;
}

HwStatus hw_display_open(const char* name, uint32_t length, uint32_t* out_id)
{
    HwDisplay* display = NULL;

    if (!g_display.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    display = hw_display_find_name(name, length);
    if (display == NULL)
    {
        return HW_STATUS_FILE_NOT_FOUND;
    }
    if (display->owner != 0U)
    {
        return HW_STATUS_BUSY;
    }
    display->owner = hw_host_task_id();
    *out_id = (uint32_t)(display - g_display.displays + 1);
    return HW_STATUS_OK;
}

/* The picture stays; the next program to open the display draws over it. */
void hw_display_close(uint32_t id)
{
    HwDisplay* display = hw_display_slot(id);

    if (display != NULL)
    {
        display->owner = 0U;
    }
}

HwStatus hw_display_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwDisplay* display = hw_display_slot(id);

    if (display == NULL || display->owner == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case HW_DISPLAY_PROP_WIDTH: *out_value = display->info.width; return HW_STATUS_OK;
        case HW_DISPLAY_PROP_HEIGHT: *out_value = display->info.height; return HW_STATUS_OK;
        case HW_DISPLAY_PROP_SHAPE: *out_value = display->info.shape; return HW_STATUS_OK;
        case HW_DISPLAY_PROP_FORMAT: *out_value = display->info.format; return HW_STATUS_OK;
        case HW_DISPLAY_PROP_FORMATS: *out_value = display->info.formats; return HW_STATUS_OK;
        case HW_DISPLAY_PROP_DPI: *out_value = display->info.dpi; return HW_STATUS_OK;
        case HW_DISPLAY_PROP_BRIGHTNESS:
            if (!display->info.has_brightness)
            {
                return HW_STATUS_UNSUPPORTED;
            }
            *out_value = display->info.brightness;
            return HW_STATUS_OK;
        default:
            /* A property of a later ABI version: this kernel cannot answer it. */
            return HW_STATUS_UNSUPPORTED;
    }
}

HwStatus hw_display_write(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          uint32_t format, const void* pixels, uint32_t size)
{
    HwDisplay* display = NULL;
    HwStatus status = hw_display_usable(id, &display);

    if (status == HW_STATUS_OK)
    {
        status = hw_display_check_rect(display, x, y, width, height, format);
    }
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    if (hw_display_rect_bytes(format, width, height) != size)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (width == 0U || height == 0U)
    {
        return HW_STATUS_OK;
    }
    return g_display.port.write(g_display.port.context, display->display, x, y, width, height, format, pixels, size);
}

/* Fill through write, a run of pixels at a time, for ports without fill. */
static HwStatus hw_display_fill_by_rows(const HwDisplay* display, uint32_t x, uint32_t y, uint32_t width,
                                        uint32_t height, uint32_t format, uint32_t color)
{
    uint8_t run[HW_DISPLAY_FILL_CHUNK];
    uint32_t run_pixels = 0U;
    uint32_t index = 0U;
    uint32_t row = 0U;

    if (format == HW_DISPLAY_MONO1)
    {
        /* Whole bytes of pixels, so every run but the last starts on a byte. */
        memset(run, color != 0U ? 0xFF : 0x00, sizeof(run));
        run_pixels = HW_DISPLAY_FILL_CHUNK * 8U;
    }
    else
    {
        uint32_t bytes = format == HW_DISPLAY_RGB565 ? 2U : 4U;
        run_pixels = HW_DISPLAY_FILL_CHUNK / bytes;
        for (index = 0U; index < HW_DISPLAY_FILL_CHUNK; ++index)
        {
            run[index] = (uint8_t)(color >> (8U * (index % bytes)));
        }
    }

    for (row = 0U; row < height; ++row)
    {
        uint32_t done = 0U;
        while (done < width)
        {
            uint32_t count = width - done < run_pixels ? width - done : run_pixels;
            HwStatus status = g_display.port.write(g_display.port.context, display->display, x + done, y + row, count,
                                                   1U, format, run, (uint32_t)hw_display_rect_bytes(format, count, 1U));
            if (status != HW_STATUS_OK)
            {
                return status;
            }
            done += count;
        }
    }
    return HW_STATUS_OK;
}

HwStatus hw_display_fill(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                         uint32_t format, uint32_t color)
{
    HwDisplay* display = NULL;
    HwStatus status = hw_display_usable(id, &display);

    if (status == HW_STATUS_OK)
    {
        status = hw_display_check_rect(display, x, y, width, height, format);
    }
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    if ((format == HW_DISPLAY_MONO1 && color > 1U) || (format == HW_DISPLAY_RGB565 && color > 0xFFFFU) ||
        (format == HW_DISPLAY_XRGB8888 && color > 0xFFFFFFU))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (width == 0U || height == 0U)
    {
        return HW_STATUS_OK;
    }
    if (g_display.port.fill != NULL)
    {
        status = g_display.port.fill(g_display.port.context, display->display, x, y, width, height, format, color);
        if (status != HW_STATUS_UNSUPPORTED)
        {
            return status;
        }
    }
    return hw_display_fill_by_rows(display, x, y, width, height, format, color);
}

HwStatus hw_display_flush(uint32_t id)
{
    HwDisplay* display = NULL;
    HwStatus status = hw_display_usable(id, &display);

    if (status != HW_STATUS_OK || g_display.port.flush == NULL)
    {
        return status;
    }
    return g_display.port.flush(g_display.port.context, display->display);
}

HwStatus hw_display_set_brightness(uint32_t id, uint32_t level)
{
    HwDisplay* display = NULL;
    HwStatus status = hw_display_usable(id, &display);

    if (status != HW_STATUS_OK)
    {
        return status;
    }
    if (!display->info.has_brightness || g_display.port.set_brightness == NULL)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (level > 255U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    status = g_display.port.set_brightness(g_display.port.context, display->display, (uint8_t)level);
    if (status == HW_STATUS_OK)
    {
        display->info.brightness = (uint8_t)level;
    }
    return status;
}

void hw_display_shutdown(void)
{
    memset(&g_display, 0, sizeof(g_display));
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: each display a task holds, by name, with its
 * brightness; only one that keeps its picture through the sleep
 * ------------------------------------------------------------------------ */

static void hw_display_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_DISPLAY_MAX; ++index)
    {
        const HwDisplay* display = &g_display.displays[index];
        if (!display->used || display->owner == 0U)
        {
            continue;
        }
        if (!display->info.keeps_image)
        {
            /* It would come back blank, and the program would not know to draw again. */
            hw_host_system_refuse(stream, HW_STATUS_UNSUPPORTED,
                             "unsupported: a display in use loses its picture in the sleep");
            return;
        }
        ++count;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_DISPLAY_MAX; ++index)
    {
        const HwDisplay* display = &g_display.displays[index];
        if (display->used && display->owner != 0U)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put32(stream, display->owner);
            hw_host_system_put_bytes(stream, display->name, display->name_length);
            hw_host_system_put32(stream, display->info.brightness);
        }
    }
}

static void hw_display_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_DISPLAY_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        char name[HW_DISPLAY_NAME_MAX];
        uint32_t old_id = hw_host_system_get32(stream);
        uint32_t owner = hw_host_system_get32(stream);
        uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
        uint32_t brightness = hw_host_system_get_max(stream, 255U);
        HwDisplay* display = NULL;

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        display = g_display.has_port ? hw_display_find_name(name, length) : NULL;
        if (display == NULL)
        {
            hw_host_system_device(stream, &hw_display_driver, old_id, 0U, NULL);
            continue;
        }
        if (display->owner != 0U || owner == 0U)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        if (display->info.has_brightness && g_display.port.set_brightness != NULL &&
            brightness != display->info.brightness)
        {
            if (g_display.port.set_brightness(g_display.port.context, display->display, (uint8_t)brightness) !=
                HW_STATUS_OK)
            {
                hw_host_system_device(stream, &hw_display_driver, old_id, 0U, "a device it held cannot be set up");
                continue;
            }
            display->info.brightness = (uint8_t)brightness;
        }
        display->owner = owner;
        hw_host_system_device(stream, &hw_display_driver, old_id, (uint32_t)(display - g_display.displays + 1), NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The display interface for programs
 * ------------------------------------------------------------------------ */

static HW_HOST_FUNCTION(hw_display_call_open)
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
    status = hw_display_open(name, name_length, &id);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status));
    }
    handle = hw_driver_handle_new(&hw_display_driver, id);
    if (handle < 0)
    {
        hw_display_close(id);
    }
    HW_HOST_RETURN(handle);
}

static HW_HOST_FUNCTION(hw_display_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_display_driver, handle, &id);
    uint32_t value = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    status = hw_display_get(id, property, &value);
    HW_HOST_RETURN(status == HW_STATUS_OK ? (int32_t)(value & 0x7FFFFFFFU) : hw_abi_error(status));
}

static HW_HOST_FUNCTION(hw_display_call_write)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, x);
    HW_HOST_ARG(uint32_t, y);
    HW_HOST_ARG(uint32_t, width);
    HW_HOST_ARG(uint32_t, height);
    HW_HOST_ARG(uint32_t, format);
    HW_HOST_ARG(uint32_t, pixels_address);
    HW_HOST_ARG(uint32_t, length);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_display_driver, handle, &id);
    HwGuestBuffer pixels;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    status = hw_host_buffer_open(&pixels, pixels_address, length, HW_GUEST_IN);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(HW_GUEST_BUFFER_ERROR(status));
    }
    /* The port reads the pixels during this call only. */
    status = hw_display_write(id, x, y, width, height, format, pixels.data, length);
    hw_host_buffer_drop(&pixels);
    HW_HOST_RETURN(hw_abi_error(status));
}

static HW_HOST_FUNCTION(hw_display_call_fill)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, x);
    HW_HOST_ARG(uint32_t, y);
    HW_HOST_ARG(uint32_t, width);
    HW_HOST_ARG(uint32_t, height);
    HW_HOST_ARG(uint32_t, format);
    HW_HOST_ARG(uint32_t, color);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_display_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_display_fill(id, x, y, width, height, format, color)));
}

static HW_HOST_FUNCTION(hw_display_call_flush)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_display_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_display_flush(id)));
}

static HW_HOST_FUNCTION(hw_display_call_set_brightness)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, level);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_display_driver, handle, &id);
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    HW_HOST_RETURN(hw_abi_error(hw_display_set_brightness(id, level)));
}

static const HwDriverCall hw_display_calls[] =
{
    { "open", "i(iii)", hw_display_call_open, 2U },
    { "get", "i(ii)", hw_display_call_get, 2U },
    { "write", "i(iiiiiiii)", hw_display_call_write, 2U },
    { "fill", "i(iiiiiii)", hw_display_call_fill, 2U },
    { "flush", "i(i)", hw_display_call_flush, 2U },
    { "set-brightness", "i(ii)", hw_display_call_set_brightness, 2U },
};

const HwDriver hw_display_driver =
{
    .interface = "display",
    .calls = hw_display_calls,
    .call_count = (uint32_t)(sizeof(hw_display_calls) / sizeof(hw_display_calls[0])),
    .close = hw_display_close,
    .shutdown = hw_display_shutdown,
    .save = hw_display_save,
    .restore = hw_display_restore,
};

