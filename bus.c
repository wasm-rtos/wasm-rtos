/*
 * Buses: I2C and SPI buses the platform registers by name. Programs open a
 * device on a bus (an I2C address, an SPI chip select) and run whole
 * transactions; the platform's port moves the bytes.
 */

#include "os_internal.h"

#if OS_HAS_I2C || OS_HAS_SPI

#include <string.h>

/* Longest delay op, in microseconds. */
#define OS_SPI_MAX_DELAY_US 65535U

typedef struct OsBus
{
    uint8_t used;
    uint32_t bus;
    char name[OS_BUS_NAME_MAX + 1U];
    uint8_t name_length;
    uint32_t frequency_hz;
} OsBus;

static struct
{
    OsBusTraceFunction function;
    void* context;
} g_bus_trace;

static OsStatus os_bus_add(OsBus* buses, uint32_t max, const char* name, uint32_t bus, uint32_t frequency_hz)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsBus* slot = NULL;
    uint32_t index = 0U;

    if (length == 0U || length > OS_BUS_NAME_MAX || frequency_hz == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < max; ++index)
    {
        if (buses[index].used &&
            (buses[index].bus == bus ||
             (buses[index].name_length == length && memcmp(buses[index].name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (!buses[index].used && slot == NULL)
        {
            slot = &buses[index];
        }
    }
    if (slot == NULL)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1U;
    slot->bus = bus;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->frequency_hz = frequency_hz;
    return OS_STATUS_OK;
}

/* The bus called name; OS_STATUS_UNSUPPORTED when there are no buses at all. */
static OsStatus os_bus_find(OsBus* buses, uint32_t max, const char* name, uint32_t length, OsBus** out_bus)
{
    uint8_t any = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < max; ++index)
    {
        any |= buses[index].used;
        if (buses[index].used && buses[index].name_length == length && memcmp(buses[index].name, name, length) == 0)
        {
            *out_bus = &buses[index];
            return OS_STATUS_OK;
        }
    }
    return any ? OS_STATUS_FILE_NOT_FOUND : OS_STATUS_UNSUPPORTED;
}

static void os_bus_report(uint8_t kind, uint32_t bus, uint32_t device, uint32_t frequency_hz, const void* ops,
                          uint32_t count, OsStatus status)
{
    OsBusTrace trace;

    if (g_bus_trace.function == NULL)
    {
        return;
    }
    trace.kind = kind;
    trace.bus = bus;
    trace.device = device;
    trace.task_id = os_task_get_id(os_task_get_current());
    trace.frequency_hz = frequency_hz;
    trace.ops = ops;
    trace.count = count;
    trace.status = status;
    g_bus_trace.function(g_bus_trace.context, &trace);
}

void os_bus_trace_set(OsBusTraceFunction function, void* context)
{
    g_bus_trace.function = function;
    g_bus_trace.context = function != NULL ? context : NULL;
}

/* ---------------------------------------------------------------------------
 * I2C
 * ------------------------------------------------------------------------ */

#if OS_HAS_I2C
typedef struct OsI2cDevice
{
    OsBus* bus;
    uint32_t address;
    /* Owning task id, 0 when the slot is free. */
    uint32_t owner;
} OsI2cDevice;

static struct
{
    OsI2cPort port;
    uint8_t has_port;
    OsBus buses[OS_I2C_MAX_BUSES];
    OsI2cDevice devices[OS_I2C_MAX_DEVICES];
} g_i2c;

static OsI2cDevice* os_i2c_device(uint32_t id)
{
    return id >= 1U && id <= OS_I2C_MAX_DEVICES && g_i2c.devices[id - 1U].owner != 0U ? &g_i2c.devices[id - 1U] : NULL;
}

OsStatus os_i2c_port_set(const OsI2cPort* port)
{
    if (port == NULL)
    {
        memset(&g_i2c.port, 0, sizeof(g_i2c.port));
        g_i2c.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->transaction == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_i2c.port = *port;
    g_i2c.has_port = 1U;
    return OS_STATUS_OK;
}

OsStatus os_i2c_add_bus(const char* name, uint32_t bus, uint32_t frequency_hz)
{
    return os_bus_add(g_i2c.buses, OS_I2C_MAX_BUSES, name, bus, frequency_hz);
}

uint32_t os_i2c_get_owner(uint32_t bus, uint32_t address)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_I2C_MAX_DEVICES; ++index)
    {
        const OsI2cDevice* device = &g_i2c.devices[index];
        if (device->owner != 0U && device->bus->bus == bus && device->address == address)
        {
            return device->owner;
        }
    }
    return 0U;
}

OsStatus os_i2c_open(const char* bus_name, uint32_t length, uint32_t address, uint32_t* out_id)
{
    OsBus* bus = NULL;
    OsI2cDevice* slot = NULL;
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;

    if (!g_i2c.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = os_bus_find(g_i2c.buses, OS_I2C_MAX_BUSES, bus_name, length, &bus);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (address > 0x7FU)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_I2C_MAX_DEVICES; ++index)
    {
        OsI2cDevice* device = &g_i2c.devices[index];
        if (device->owner != 0U && device->bus == bus && device->address == address)
        {
            return OS_STATUS_BUSY;
        }
        if (device->owner == 0U && slot == NULL)
        {
            slot = device;
        }
    }
    if (slot == NULL)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot->bus = bus;
    slot->address = address;
    slot->owner = os_task_get_id(os_task_get_current());
    *out_id = (uint32_t)(slot - g_i2c.devices + 1);
    return OS_STATUS_OK;
}

void os_i2c_close(uint32_t id)
{
    OsI2cDevice* device = os_i2c_device(id);

    if (device != NULL)
    {
        memset(device, 0, sizeof(*device));
    }
}

OsStatus os_i2c_transaction(uint32_t id, const OsI2cOp* ops, uint32_t count)
{
    OsI2cDevice* device = os_i2c_device(id);
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;

    if (device == NULL || count > OS_BUS_MAX_OPS)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_i2c.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < count; ++index)
    {
        if (ops[index].kind != OS_I2C_WRITE && ops[index].kind != OS_I2C_READ)
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
    }
    status = g_i2c.port.transaction(g_i2c.port.context, device->bus->bus, device->address,
                                    device->bus->frequency_hz, ops, count);
    os_bus_report(OS_BUS_I2C, device->bus->bus, device->address, device->bus->frequency_hz, ops, count, status);
    return status;
}

void os_i2c_shutdown(void)
{
    memset(&g_i2c, 0, sizeof(g_i2c));
}
#endif

/* ---------------------------------------------------------------------------
 * SPI
 * ------------------------------------------------------------------------ */

#if OS_HAS_SPI
typedef struct OsSpiDevice
{
    OsBus* bus;
    uint32_t mode;
    uint32_t frequency_hz;
    /* The kernel's GPIO id of chip select, 0 when the program drives it. */
    uint32_t cs;
    uint32_t cs_pin;
    /* Owning task id, 0 when the slot is free. */
    uint32_t owner;
} OsSpiDevice;

static struct
{
    OsSpiPort port;
    uint8_t has_port;
    OsBus buses[OS_SPI_MAX_BUSES];
    OsSpiDevice devices[OS_SPI_MAX_DEVICES];
} g_spi;

static OsSpiDevice* os_spi_device(uint32_t id)
{
    return id >= 1U && id <= OS_SPI_MAX_DEVICES && g_spi.devices[id - 1U].owner != 0U ? &g_spi.devices[id - 1U] : NULL;
}

OsStatus os_spi_port_set(const OsSpiPort* port)
{
    if (port == NULL)
    {
        memset(&g_spi.port, 0, sizeof(g_spi.port));
        g_spi.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->transaction == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_spi.port = *port;
    g_spi.has_port = 1U;
    return OS_STATUS_OK;
}

OsStatus os_spi_add_bus(const char* name, uint32_t bus, uint32_t max_hz)
{
    return os_bus_add(g_spi.buses, OS_SPI_MAX_BUSES, name, bus, max_hz);
}

uint32_t os_spi_get_owner(uint32_t bus, uint32_t cs_pin)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_SPI_MAX_DEVICES; ++index)
    {
        const OsSpiDevice* device = &g_spi.devices[index];
        if (device->owner != 0U && device->bus->bus == bus && device->cs_pin == cs_pin)
        {
            return device->owner;
        }
    }
    return 0U;
}

OsStatus os_spi_open(const char* bus_name, uint32_t bus_length, const char* cs, uint32_t cs_length,
                     uint32_t mode, uint32_t frequency_hz, uint32_t* out_id)
{
    OsBus* bus = NULL;
    OsSpiDevice* slot = NULL;
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;

    if (!g_spi.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = os_bus_find(g_spi.buses, OS_SPI_MAX_BUSES, bus_name, bus_length, &bus);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if ((mode & ~(OS_SPI_CPHA | OS_SPI_CPOL | OS_SPI_LSB_FIRST)) != 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    /* A program that drives chip select itself needs the bus to itself. */
    for (index = 0U; index < OS_SPI_MAX_DEVICES; ++index)
    {
        OsSpiDevice* device = &g_spi.devices[index];
        if (device->owner != 0U && device->bus == bus && (device->cs == 0U || cs_length == 0U))
        {
            return OS_STATUS_BUSY;
        }
        if (device->owner == 0U && slot == NULL)
        {
            slot = device;
        }
    }
    if (slot == NULL)
    {
        return OS_STATUS_LIMIT_REACHED;
    }

    memset(slot, 0, sizeof(*slot));
    slot->cs_pin = OS_BUS_NO_CS;
    if (cs_length > 0U)
    {
#if OS_HAS_GPIO
        status = os_gpio_open_physical(cs, cs_length, &slot->cs);
        if (status != OS_STATUS_OK)
        {
            slot->cs = 0U;
            return status;
        }
        slot->cs_pin = os_gpio_pin_number(slot->cs);
#else
        (void)cs;
        return OS_STATUS_UNSUPPORTED;
#endif
    }
    slot->bus = bus;
    slot->mode = mode;
    slot->frequency_hz = frequency_hz == 0U || frequency_hz > bus->frequency_hz ? bus->frequency_hz : frequency_hz;
    slot->owner = os_task_get_id(os_task_get_current());
    *out_id = (uint32_t)(slot - g_spi.devices + 1);
    return OS_STATUS_OK;
}

void os_spi_close(uint32_t id)
{
    OsSpiDevice* device = os_spi_device(id);

    if (device == NULL)
    {
        return;
    }
#if OS_HAS_GPIO
    if (device->cs != 0U)
    {
        os_gpio_close(device->cs);
    }
#endif
    memset(device, 0, sizeof(*device));
}

OsStatus os_spi_transaction(uint32_t id, const OsSpiOp* ops, uint32_t count)
{
    OsSpiDevice* device = os_spi_device(id);
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;

    if (device == NULL || count == 0U || count > OS_BUS_MAX_OPS)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_spi.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < count; ++index)
    {
        const OsSpiOp* op = &ops[index];
        if (op->kind < OS_SPI_WRITE || op->kind > OS_SPI_DELAY_US ||
            (op->kind == OS_SPI_DELAY_US && op->length > OS_SPI_MAX_DELAY_US))
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
    }

#if OS_HAS_GPIO
    if (device->cs != 0U)
    {
        status = os_gpio_set(device->cs, 0U);
    }
#endif
    if (status == OS_STATUS_OK)
    {
        status = g_spi.port.transaction(g_spi.port.context, device->bus->bus, device->mode, device->frequency_hz, ops,
                                        count);
    }
#if OS_HAS_GPIO
    if (device->cs != 0U)
    {
        (void)os_gpio_set(device->cs, 1U);
    }
#endif
    os_bus_report(OS_BUS_SPI, device->bus->bus, device->cs_pin, device->frequency_hz, ops, count, status);
    return status;
}

void os_spi_shutdown(void)
{
    memset(&g_spi, 0, sizeof(g_spi));
}
#endif

void os_bus_shutdown(void)
{
#if OS_HAS_I2C
    os_i2c_shutdown();
#endif
#if OS_HAS_SPI
    os_spi_shutdown();
#endif
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
}

#endif
