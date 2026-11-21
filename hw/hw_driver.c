/*
 * The registry of drivers and what every driver's calls share: opening by
 * name, properties, ABI errors, names from a program's memory.
 */

#include "hw_driver.h"

/* The trap HW_HOST_CHECK_ADDR returns. */
const char hw_host_out_of_bounds[] = "trap: out of bounds memory access";

int32_t hw_abi_error(HwStatus status)
{
    switch (status)
    {
        case HW_STATUS_OK: return 0;
        case HW_STATUS_OUT_OF_MEMORY: return -WR_ENOMEM;
        case HW_STATUS_WASM_ERROR: return -WR_EBADPROGRAM;
        case HW_STATUS_TASK_DEAD: return -WR_EDEAD;
        case HW_STATUS_TASK_NOT_FOUND:
        case HW_STATUS_QUEUE_NOT_FOUND:
        case HW_STATUS_MUTEX_NOT_FOUND:
        case HW_STATUS_SEMAPHORE_NOT_FOUND:
        case HW_STATUS_EVENT_GROUP_NOT_FOUND:
        case HW_STATUS_TIMER_NOT_FOUND: return -WR_EBADH;
        case HW_STATUS_BUFFER_TOO_SMALL: return -WR_ETOOLONG;
        case HW_STATUS_UNSUPPORTED:
        case HW_STATUS_READ_ONLY: return -WR_EUNSUPPORTED;
        case HW_STATUS_OUT_OF_BOUNDS: return -WR_EFAULT;
        case HW_STATUS_QUEUE_FULL:
        case HW_STATUS_SEMAPHORE_FULL: return -WR_EFULL;
        case HW_STATUS_QUEUE_EMPTY: return -WR_EEMPTY;
        case HW_STATUS_TIMEOUT: return -WR_ETIMEOUT;
        case HW_STATUS_BUSY: return -WR_EBUSY;
        case HW_STATUS_NOT_OWNER: return -WR_ENOTOWNER;
        case HW_STATUS_ABORTED: return -WR_EABORTED;
        case HW_STATUS_LIMIT_REACHED: return -WR_ELIMIT;
        case HW_STATUS_FILE_NOT_FOUND:
        case HW_STATUS_END_OF_DIRECTORY: return -WR_ENOTFOUND;
        case HW_STATUS_NACK: return -WR_ENACK;
        case HW_STATUS_BUS_ERROR: return -WR_EBUS;
        case HW_STATUS_NOT_A_DIRECTORY: return -WR_ENOTDIR;
        case HW_STATUS_IS_A_DIRECTORY: return -WR_EISDIR;
        default: return -WR_EINVAL;
    }
}

int32_t hw_driver_property(HwStatus status, uint32_t value)
{
    return status == HW_STATUS_OK ? (int32_t)(value & 0x7FFFFFFFU) : hw_abi_error(status);
}

int32_t hw_driver_open(const HwDriver* driver, uint32_t name_address, uint32_t name_length,
                       HwStatus (*opener)(const char* name, uint32_t length, void* context, uint32_t* out_id),
                       void* context)
{
    const char* name = NULL;
    char name_storage[HW_ABI_NAME_MAX];
    uint32_t id = 0U;
    int32_t handle = hw_abi_read_name(name_address, name_length, name_storage, &name);
    HwStatus status = HW_STATUS_OK;

    if (handle < 0)
    {
        return handle;
    }
    if (name == NULL)
    {
        return -WR_EINVAL;
    }
    status = opener(name, name_length, context, &id);
    if (status != HW_STATUS_OK)
    {
        return status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status);
    }
    handle = hw_driver_handle_new(driver, id);
    if (handle < 0 && driver->close != NULL)
    {
        driver->close(id);
    }
    return handle;
}

static const HwDriver* g_drivers[HW_MAX_DRIVERS];
static uint32_t g_driver_count;

HwStatus hw_driver_add(const HwDriver* driver)
{
    uint32_t index = 0U;

    if (driver == NULL || driver->interface == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index] == driver)
        {
            return HW_STATUS_OK;
        }
    }
    if (g_driver_count == HW_MAX_DRIVERS)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    g_drivers[g_driver_count++] = driver;
    return HW_STATUS_OK;
}

uint32_t hw_driver_count(void)
{
    return g_driver_count;
}

const HwDriver* hw_driver_at(uint32_t index)
{
    return index < g_driver_count ? g_drivers[index] : NULL;
}

const HwDriver* hw_driver_find(const char* interface)
{
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (strcmp(g_drivers[index]->interface, interface) == 0)
        {
            return g_drivers[index];
        }
    }
    return NULL;
}

uint32_t hw_driver_signal_key(const HwDriver* driver, uint32_t id)
{
    uint32_t index = 0U;

    /* Kernel objects use the low type numbers; drivers take theirs from 0x80 up, by place. */
    while (index < g_driver_count && g_drivers[index] != driver)
    {
        ++index;
    }
    return HW_SIGNAL_KEY(0x80U + index, id);
}

void hw_drivers_poll(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->poll != NULL)
        {
            g_drivers[index]->poll();
        }
    }
}

uint32_t hw_drivers_next_deadline_ms(void)
{
    uint32_t next = UINT32_MAX;
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->next_deadline_ms != NULL)
        {
            uint32_t due = g_drivers[index]->next_deadline_ms();
            if (due < next)
            {
                next = due;
            }
        }
    }
    return next;
}

void hw_drivers_time(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->time != NULL)
        {
            g_drivers[index]->time();
        }
    }
}

void hw_drivers_finish_task(uint32_t task_id)
{
    uint32_t index = 0U;

    for (index = 0U; index < g_driver_count; ++index)
    {
        if (g_drivers[index]->finish_task != NULL)
        {
            g_drivers[index]->finish_task(task_id);
        }
    }
}

void hw_drivers_shutdown(void)
{
    while (g_driver_count > 0U)
    {
        const HwDriver* driver = g_drivers[--g_driver_count];
        if (driver->shutdown != NULL)
        {
            driver->shutdown();
        }
    }
}

int hw_abi_utf8_valid(const uint8_t* text, uint32_t length)
{
    uint32_t index = 0U;

    while (index < length)
    {
        uint8_t byte = text[index];
        uint32_t extra = byte < 0x80U ? 0U : (byte & 0xE0U) == 0xC0U ? 1U
                       : (byte & 0xF0U) == 0xE0U ? 2U : (byte & 0xF8U) == 0xF0U ? 3U : 4U;
        uint32_t next = 0U;

        if (extra == 4U || (extra == 1U && byte < 0xC2U) || length - index <= extra)
        {
            return 0U;
        }
        for (next = 1U; next <= extra; ++next)
        {
            if ((text[index + next] & 0xC0U) != 0x80U)
            {
                return 0U;
            }
        }
        index += extra + 1U;
    }
    return 1U;
}

/* An optional object name: (0, 0) means none. */
int32_t hw_abi_read_name(uint32_t address, uint32_t length, char* storage, const char** out_name)
{
    *out_name = NULL;
    if (length == 0U)
    {
        return 0;
    }
    if (length > HW_ABI_NAME_MAX)
    {
        return -WR_ETOOLONG;
    }
    if (hw_host_guest_read(address, storage, length) != HW_STATUS_OK)
    {
        return -WR_EFAULT;
    }
    if (!hw_abi_utf8_valid((const uint8_t*)storage, length))
    {
        return -WR_EINVAL;
    }
    *out_name = storage;
    return 0;
}
