/*
 * Displays: screens the platform registers by name, lent to one program at a
 * time. The kernel knows no panel; the platform's OsDisplayPort draws.
 */

#include "driver.h"


#include <string.h>

/* Bytes of pixels fill writes at a time when the port has no fill of its own. */
#define OS_DISPLAY_FILL_CHUNK 256U

#define OS_DISPLAY_KNOWN_FORMATS \
    (OS_DISPLAY_FORMAT_BIT(OS_DISPLAY_MONO1) | OS_DISPLAY_FORMAT_BIT(OS_DISPLAY_RGB565) | \
     OS_DISPLAY_FORMAT_BIT(OS_DISPLAY_XRGB8888))

typedef struct OsDisplay
{
    uint8_t used;
    uint32_t display;
    char name[OS_DISPLAY_NAME_MAX + 1U];
    uint8_t name_length;
    OsDisplayInfo info;
    /* Owning task id, 0 when free. */
    uint32_t owner;
} OsDisplay;

static struct
{
    OsDisplayPort port;
    uint8_t has_port;
    OsDisplay displays[OS_DISPLAY_MAX];
} g_display;

static OsDisplay* os_display_slot(uint32_t id)
{
    return id >= 1U && id <= OS_DISPLAY_MAX && g_display.displays[id - 1U].used ? &g_display.displays[id - 1U] : NULL;
}

/* An open display a program may draw on now. */
static OsStatus os_display_usable(uint32_t id, OsDisplay** out_display)
{
    OsDisplay* display = os_display_slot(id);

    if (display == NULL || display->owner == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_display.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    *out_display = display;
    return OS_STATUS_OK;
}

/* Bytes a width x height rectangle takes in format. */
static uint64_t os_display_rect_bytes(uint32_t format, uint32_t width, uint32_t height)
{
    switch (format)
    {
        case OS_DISPLAY_MONO1: return (uint64_t)((width + 7U) / 8U) * height;
        case OS_DISPLAY_RGB565: return (uint64_t)width * height * 2U;
        case OS_DISPLAY_XRGB8888: return (uint64_t)width * height * 4U;
        default: return 0U;
    }
}

static OsStatus os_display_check_rect(const OsDisplay* display, uint32_t x, uint32_t y, uint32_t width,
                                      uint32_t height, uint32_t format)
{
    if (format == 0U || format > 31U || (display->info.formats & OS_DISPLAY_FORMAT_BIT(format)) == 0U)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if ((uint64_t)x + width > display->info.width || (uint64_t)y + height > display->info.height)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    return OS_STATUS_OK;
}

OsStatus os_display_port_set(const OsDisplayPort* port)
{
    if (port == NULL)
    {
        memset(&g_display.port, 0, sizeof(g_display.port));
        g_display.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->write == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_display.port = *port;
    g_display.has_port = 1U;
    return OS_STATUS_OK;
}

static OsDisplay* os_display_find_name(const char* name, uint32_t length)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_DISPLAY_MAX; ++index)
    {
        OsDisplay* display = &g_display.displays[index];
        if (display->used && display->name_length == length && memcmp(display->name, name, length) == 0)
        {
            return display;
        }
    }
    return NULL;
}

extern const OsDriver os_display_driver;

OsStatus os_display_add(const char* name, uint32_t display, const OsDisplayInfo* info)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsDisplay* slot = NULL;
    uint32_t index = 0U;

    if (length == 0U || length > OS_DISPLAY_NAME_MAX || info == NULL || info->width == 0U || info->height == 0U ||
        info->shape > OS_DISPLAY_ROUND || info->format == 0U || info->format > OS_DISPLAY_XRGB8888 ||
        (info->formats & ~OS_DISPLAY_KNOWN_FORMATS) != 0U ||
        (info->formats & OS_DISPLAY_FORMAT_BIT(info->format)) == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_DISPLAY_MAX; ++index)
    {
        OsDisplay* entry = &g_display.displays[index];
        if (entry->used && (entry->display == display ||
                            (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (!entry->used && slot == NULL)
        {
            slot = entry;
        }
    }
    if (slot == NULL)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1U;
    slot->display = display;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->info = *info;
    /* A platform with these devices gives programs the display interface. */
    if (os_driver_add(&os_display_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
}

uint32_t os_display_get_owner(uint32_t display)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_DISPLAY_MAX; ++index)
    {
        if (g_display.displays[index].used && g_display.displays[index].display == display)
        {
            return g_display.displays[index].owner;
        }
    }
    return 0U;
}

OsStatus os_display_open(const char* name, uint32_t length, uint32_t* out_id)
{
    OsDisplay* display = NULL;

    if (!g_display.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    display = os_display_find_name(name, length);
    if (display == NULL)
    {
        return OS_STATUS_FILE_NOT_FOUND;
    }
    if (display->owner != 0U)
    {
        return OS_STATUS_BUSY;
    }
    display->owner = os_task_get_id(os_task_get_current());
    *out_id = (uint32_t)(display - g_display.displays + 1);
    return OS_STATUS_OK;
}

/* The picture stays; the next program to open the display draws over it. */
void os_display_close(uint32_t id)
{
    OsDisplay* display = os_display_slot(id);

    if (display != NULL)
    {
        display->owner = 0U;
    }
}

OsStatus os_display_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsDisplay* display = os_display_slot(id);

    if (display == NULL || display->owner == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case OS_DISPLAY_PROP_WIDTH: *out_value = display->info.width; return OS_STATUS_OK;
        case OS_DISPLAY_PROP_HEIGHT: *out_value = display->info.height; return OS_STATUS_OK;
        case OS_DISPLAY_PROP_SHAPE: *out_value = display->info.shape; return OS_STATUS_OK;
        case OS_DISPLAY_PROP_FORMAT: *out_value = display->info.format; return OS_STATUS_OK;
        case OS_DISPLAY_PROP_FORMATS: *out_value = display->info.formats; return OS_STATUS_OK;
        case OS_DISPLAY_PROP_DPI: *out_value = display->info.dpi; return OS_STATUS_OK;
        case OS_DISPLAY_PROP_BRIGHTNESS:
            if (!display->info.has_brightness)
            {
                return OS_STATUS_UNSUPPORTED;
            }
            *out_value = display->info.brightness;
            return OS_STATUS_OK;
        default:
            /* A property of a later ABI version: this kernel cannot answer it. */
            return OS_STATUS_UNSUPPORTED;
    }
}

OsStatus os_display_write(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          uint32_t format, const void* pixels, uint32_t size)
{
    OsDisplay* display = NULL;
    OsStatus status = os_display_usable(id, &display);

    if (status == OS_STATUS_OK)
    {
        status = os_display_check_rect(display, x, y, width, height, format);
    }
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (os_display_rect_bytes(format, width, height) != size)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (width == 0U || height == 0U)
    {
        return OS_STATUS_OK;
    }
    return g_display.port.write(g_display.port.context, display->display, x, y, width, height, format, pixels, size);
}

/* Fill through write, a run of pixels at a time, for ports without fill. */
static OsStatus os_display_fill_by_rows(const OsDisplay* display, uint32_t x, uint32_t y, uint32_t width,
                                        uint32_t height, uint32_t format, uint32_t color)
{
    uint8_t run[OS_DISPLAY_FILL_CHUNK];
    uint32_t run_pixels = 0U;
    uint32_t index = 0U;
    uint32_t row = 0U;

    if (format == OS_DISPLAY_MONO1)
    {
        /* Whole bytes of pixels, so every run but the last starts on a byte. */
        memset(run, color != 0U ? 0xFF : 0x00, sizeof(run));
        run_pixels = OS_DISPLAY_FILL_CHUNK * 8U;
    }
    else
    {
        uint32_t bytes = format == OS_DISPLAY_RGB565 ? 2U : 4U;
        run_pixels = OS_DISPLAY_FILL_CHUNK / bytes;
        for (index = 0U; index < OS_DISPLAY_FILL_CHUNK; ++index)
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
            OsStatus status = g_display.port.write(g_display.port.context, display->display, x + done, y + row, count,
                                                   1U, format, run, (uint32_t)os_display_rect_bytes(format, count, 1U));
            if (status != OS_STATUS_OK)
            {
                return status;
            }
            done += count;
        }
    }
    return OS_STATUS_OK;
}

OsStatus os_display_fill(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                         uint32_t format, uint32_t color)
{
    OsDisplay* display = NULL;
    OsStatus status = os_display_usable(id, &display);

    if (status == OS_STATUS_OK)
    {
        status = os_display_check_rect(display, x, y, width, height, format);
    }
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if ((format == OS_DISPLAY_MONO1 && color > 1U) || (format == OS_DISPLAY_RGB565 && color > 0xFFFFU) ||
        (format == OS_DISPLAY_XRGB8888 && color > 0xFFFFFFU))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (width == 0U || height == 0U)
    {
        return OS_STATUS_OK;
    }
    if (g_display.port.fill != NULL)
    {
        status = g_display.port.fill(g_display.port.context, display->display, x, y, width, height, format, color);
        if (status != OS_STATUS_UNSUPPORTED)
        {
            return status;
        }
    }
    return os_display_fill_by_rows(display, x, y, width, height, format, color);
}

OsStatus os_display_flush(uint32_t id)
{
    OsDisplay* display = NULL;
    OsStatus status = os_display_usable(id, &display);

    if (status != OS_STATUS_OK || g_display.port.flush == NULL)
    {
        return status;
    }
    return g_display.port.flush(g_display.port.context, display->display);
}

OsStatus os_display_set_brightness(uint32_t id, uint32_t level)
{
    OsDisplay* display = NULL;
    OsStatus status = os_display_usable(id, &display);

    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (!display->info.has_brightness || g_display.port.set_brightness == NULL)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (level > 255U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = g_display.port.set_brightness(g_display.port.context, display->display, (uint8_t)level);
    if (status == OS_STATUS_OK)
    {
        display->info.brightness = (uint8_t)level;
    }
    return status;
}

void os_display_shutdown(void)
{
    memset(&g_display, 0, sizeof(g_display));
}

/* ---------------------------------------------------------------------------
 * The display interface for programs
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_display_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, flags);
    const char* name = NULL;
    uint32_t id = 0U;
    int32_t handle = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    handle = os_abi_read_name(name_address, name_length, &name);
    if (handle < 0)
    {
        OS_HOST_RETURN(handle);
    }
    if (name == NULL || flags != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    status = os_display_open(name, name_length, &id);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(status == OS_STATUS_BUSY ? -WR_EINUSE : os_abi_error(status));
    }
    handle = os_driver_handle_new(&os_display_driver, id);
    if (handle < 0)
    {
        os_display_close(id);
    }
    OS_HOST_RETURN(handle);
}

static OS_HOST_FUNCTION(os_display_call_get)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_display_driver, handle, &id);
    uint32_t value = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    status = os_display_get(id, property, &value);
    OS_HOST_RETURN(status == OS_STATUS_OK ? (int32_t)(value & 0x7FFFFFFFU) : os_abi_error(status));
}

static OS_HOST_FUNCTION(os_display_call_write)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, x);
    OS_HOST_ARG(uint32_t, y);
    OS_HOST_ARG(uint32_t, width);
    OS_HOST_ARG(uint32_t, height);
    OS_HOST_ARG(uint32_t, format);
    OS_HOST_ARG(uint32_t, pixels_address);
    OS_HOST_ARG(uint32_t, length);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_display_driver, handle, &id);
    uint8_t* pixels = NULL;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (length > 0U && os_guest_memory(pixels_address, length, &pixels) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    /* The port reads the program's memory in place, during this call only. */
    OS_HOST_RETURN(os_abi_error(os_display_write(id, x, y, width, height, format, pixels, length)));
}

static OS_HOST_FUNCTION(os_display_call_fill)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, x);
    OS_HOST_ARG(uint32_t, y);
    OS_HOST_ARG(uint32_t, width);
    OS_HOST_ARG(uint32_t, height);
    OS_HOST_ARG(uint32_t, format);
    OS_HOST_ARG(uint32_t, color);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_display_driver, handle, &id);
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(os_abi_error(os_display_fill(id, x, y, width, height, format, color)));
}

static OS_HOST_FUNCTION(os_display_call_flush)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_display_driver, handle, &id);
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(os_abi_error(os_display_flush(id)));
}

static OS_HOST_FUNCTION(os_display_call_set_brightness)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, level);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_display_driver, handle, &id);
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    OS_HOST_RETURN(os_abi_error(os_display_set_brightness(id, level)));
}

static const OsDriverCall os_display_calls[] =
{
    { "open", "i(iii)", os_display_call_open, 2U },
    { "get", "i(ii)", os_display_call_get, 2U },
    { "write", "i(iiiiiiii)", os_display_call_write, 2U },
    { "fill", "i(iiiiiii)", os_display_call_fill, 2U },
    { "flush", "i(i)", os_display_call_flush, 2U },
    { "set-brightness", "i(ii)", os_display_call_set_brightness, 2U },
};

const OsDriver os_display_driver =
{
    .interface = "display",
    .calls = os_display_calls,
    .call_count = (uint32_t)(sizeof(os_display_calls) / sizeof(os_display_calls[0])),
    .close = os_display_close,
    .shutdown = os_display_shutdown,
};

