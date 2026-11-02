/*
 * Buses: I2C and SPI buses the platform registers by name. Programs open a
 * device on a bus (an I2C address, an SPI chip select) and run whole
 * transactions; the platform's port moves the bytes.
 */

#include "os_internal.h"

#if OS_HAS_I2C || OS_HAS_SPI

#include <string.h>

/* A background transfer's end is recorded by interrupt handlers and taken by the kernel. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint8_t OsBusFlag;
#define OS_BUS_FLAG_SET(flag) ((flag) = 1U)
#define OS_BUS_FLAG_TAKE(flag) os_bus_flag_take(&(flag))
static uint8_t os_bus_flag_take(OsBusFlag* flag)
{
    uint8_t value = *flag;
    *flag = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uchar OsBusFlag;
#define OS_BUS_FLAG_SET(flag) atomic_store_explicit(&(flag), 1U, memory_order_release)
#define OS_BUS_FLAG_TAKE(flag) atomic_exchange_explicit(&(flag), 0U, memory_order_acquire)
#endif

/* Longest delay op, in microseconds. */
#define OS_SPI_MAX_DELAY_US 65535U

typedef struct OsBus
{
    uint8_t used;
    uint32_t bus;
    char name[OS_BUS_NAME_MAX + 1U];
    uint8_t name_length;
    uint32_t frequency_hz;

    /* A transfer the port runs in the background, with the kernel's copy of its ops. */
    uint8_t busy;
    uint32_t task_id;
    uint32_t device;
    uint32_t transfer_hz;
    uint32_t mode;
    uint32_t cs;
    uint32_t count;
    union
    {
        OsI2cOp i2c[OS_BUS_MAX_OPS];
        OsSpiOp spi[OS_BUS_MAX_OPS];
    } ops;
    OsBusFlag done;
    volatile OsStatus result;
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

static void os_bus_report(uint8_t kind, uint32_t bus, uint32_t device, uint32_t task_id, uint32_t frequency_hz,
                          const void* ops, uint32_t count, OsStatus status)
{
    OsBusTrace trace;

    if (g_bus_trace.function == NULL)
    {
        return;
    }
    trace.kind = kind;
    trace.bus = bus;
    trace.device = device;
    trace.task_id = task_id;
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

static void os_bus_finish(uint8_t kind, OsBus* bus);
static OsBus* os_bus_by_number(uint8_t kind, uint32_t number);
static void (*os_bus_port_finish(uint8_t kind))(void*, uint32_t);
static void* os_bus_port_context(uint8_t kind);

/* Hand a finished background transfer to its program: chip select up, trace, wake. */
static void os_bus_deliver(uint8_t kind, OsBus* bus)
{
    OsStatus status = bus->result;
    uint32_t task_id = bus->task_id;

    bus->busy = 0U;
#if OS_HAS_GPIO
    if (bus->cs != 0U)
    {
        (void)os_gpio_set(bus->cs, 1U);
    }
#endif
    os_bus_report(kind, bus->bus, bus->device, task_id, bus->transfer_hz,
                  kind == OS_BUS_I2C ? (const void*)bus->ops.i2c : (const void*)bus->ops.spi, bus->count, status);
    os_bus_wait_end(task_id, status);
}

/*
 * Start a transaction on a bus: wait out another program's transfer, let
 * the port run it on the kernel's copy of the ops, and either finish here
 * or leave the calling program waiting (OS_STATUS_PENDING).
 */
static OsStatus os_bus_start(uint8_t kind, OsBus* bus, uint32_t device, uint32_t frequency_hz, uint32_t mode,
                             uint32_t cs, const void* ops, uint32_t count, OsStatus (*run)(OsBus* bus))
{
    OsStatus status = OS_STATUS_OK;

    if (bus->busy)
    {
        os_bus_finish(kind, bus);
    }
    memcpy(&bus->ops, ops, count * (kind == OS_BUS_I2C ? sizeof(OsI2cOp) : sizeof(OsSpiOp)));
    bus->task_id = os_task_get_id(os_task_get_current());
    bus->device = device;
    bus->transfer_hz = frequency_hz;
    bus->mode = mode;
    bus->cs = cs;
    bus->count = count;
    (void)OS_BUS_FLAG_TAKE(bus->done);

#if OS_HAS_GPIO
    if (cs != 0U)
    {
        status = os_gpio_set(cs, 0U);
    }
#endif
    if (status == OS_STATUS_OK)
    {
        status = run(bus);
    }
    if (status == OS_STATUS_PENDING)
    {
        bus->busy = 1U;
        if (os_bus_wait_begin() == OS_STATUS_OK)
        {
            return OS_STATUS_PENDING;
        }
        /* Nobody to wait (a call from the host): finish now. */
        os_bus_finish(kind, bus);
        return bus->result;
    }
#if OS_HAS_GPIO
    if (cs != 0U)
    {
        (void)os_gpio_set(cs, 1U);
    }
#endif
    os_bus_report(kind, bus->bus, device, bus->task_id, frequency_hz,
                  kind == OS_BUS_I2C ? (const void*)bus->ops.i2c : (const void*)bus->ops.spi, count, status);
    return status;
}

/* Make the port end a background transfer now, then deliver it. */
static void os_bus_finish(uint8_t kind, OsBus* bus)
{
    void (*finish)(void*, uint32_t) = os_bus_port_finish(kind);

    if (!bus->busy)
    {
        return;
    }
    if (!OS_BUS_FLAG_TAKE(bus->done))
    {
        if (finish != NULL)
        {
            finish(os_bus_port_context(kind), bus->bus);
        }
        /* A port that did not report even now has failed the transfer. */
        if (!OS_BUS_FLAG_TAKE(bus->done))
        {
            bus->result = OS_STATUS_BUS_ERROR;
        }
    }
    os_bus_deliver(kind, bus);
}

void os_bus_complete(uint32_t kind, uint32_t number, OsStatus status)
{
    OsBus* bus = os_bus_by_number((uint8_t)kind, number);

    if (bus != NULL && bus->busy)
    {
        bus->result = status;
        OS_BUS_FLAG_SET(bus->done);
    }
}

uint8_t os_bus_is_busy(uint32_t kind, uint32_t number)
{
    OsBus* bus = os_bus_by_number((uint8_t)kind, number);
    return bus != NULL && bus->busy;
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

static OsStatus os_i2c_run(OsBus* bus)
{
    return g_i2c.port.transaction(g_i2c.port.context, bus->bus, bus->device, bus->transfer_hz, bus->ops.i2c,
                                  bus->count);
}

OsStatus os_i2c_transaction(uint32_t id, const OsI2cOp* ops, uint32_t count)
{
    OsI2cDevice* device = os_i2c_device(id);
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
    return os_bus_start(OS_BUS_I2C, device->bus, device->address, device->bus->frequency_hz, 0U, 0U, ops, count,
                        os_i2c_run);
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

static OsStatus os_spi_run(OsBus* bus)
{
    return g_spi.port.transaction(g_spi.port.context, bus->bus, bus->mode, bus->transfer_hz, bus->ops.spi,
                                  bus->count);
}

OsStatus os_spi_transaction(uint32_t id, const OsSpiOp* ops, uint32_t count)
{
    OsSpiDevice* device = os_spi_device(id);
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

    return os_bus_start(OS_BUS_SPI, device->bus, device->cs_pin, device->frequency_hz, device->mode, device->cs, ops,
                        count, os_spi_run);
}

void os_spi_shutdown(void)
{
    memset(&g_spi, 0, sizeof(g_spi));
}
#endif

static OsBus* os_bus_by_number(uint8_t kind, uint32_t number)
{
    OsBus* buses = NULL;
    uint32_t max = 0U;
    uint32_t index = 0U;

#if OS_HAS_I2C
    if (kind == OS_BUS_I2C)
    {
        buses = g_i2c.buses;
        max = OS_I2C_MAX_BUSES;
    }
#endif
#if OS_HAS_SPI
    if (kind == OS_BUS_SPI)
    {
        buses = g_spi.buses;
        max = OS_SPI_MAX_BUSES;
    }
#endif
    for (index = 0U; index < max; ++index)
    {
        if (buses[index].used && buses[index].bus == number)
        {
            return &buses[index];
        }
    }
    return NULL;
}

static void (*os_bus_port_finish(uint8_t kind))(void*, uint32_t)
{
#if OS_HAS_I2C
    if (kind == OS_BUS_I2C)
    {
        return g_i2c.port.finish;
    }
#endif
#if OS_HAS_SPI
    if (kind == OS_BUS_SPI)
    {
        return g_spi.port.finish;
    }
#endif
    return NULL;
}

static void* os_bus_port_context(uint8_t kind)
{
#if OS_HAS_I2C
    if (kind == OS_BUS_I2C)
    {
        return g_i2c.port.context;
    }
#endif
#if OS_HAS_SPI
    if (kind == OS_BUS_SPI)
    {
        return g_spi.port.context;
    }
#endif
    return NULL;
}

/* Every bus of both kinds, for the loops below. */
static void os_bus_each(void (*visit)(uint8_t kind, OsBus* bus, uint32_t task_id), uint32_t task_id)
{
    uint32_t index = 0U;
#if OS_HAS_I2C
    for (index = 0U; index < OS_I2C_MAX_BUSES; ++index)
    {
        visit(OS_BUS_I2C, &g_i2c.buses[index], task_id);
    }
#endif
#if OS_HAS_SPI
    for (index = 0U; index < OS_SPI_MAX_BUSES; ++index)
    {
        visit(OS_BUS_SPI, &g_spi.buses[index], task_id);
    }
#endif
    (void)index;
}

static void os_bus_poll_one(uint8_t kind, OsBus* bus, uint32_t task_id)
{
    (void)task_id;
    if (bus->used && bus->busy && OS_BUS_FLAG_TAKE(bus->done))
    {
        os_bus_deliver(kind, bus);
    }
}

static void os_bus_finish_one(uint8_t kind, OsBus* bus, uint32_t task_id)
{
    if (bus->used && bus->busy && (task_id == 0U || bus->task_id == task_id))
    {
        os_bus_finish(kind, bus);
    }
}

void os_bus_poll(void)
{
    os_bus_each(os_bus_poll_one, 0U);
}

void os_bus_finish_task(uint32_t task_id)
{
    if (task_id != 0U)
    {
        os_bus_each(os_bus_finish_one, task_id);
    }
}

void os_bus_shutdown(void)
{
    /* Ports must be done with every buffer before the kernel lets go. */
    os_bus_each(os_bus_finish_one, 0U);
#if OS_HAS_I2C
    os_i2c_shutdown();
#endif
#if OS_HAS_SPI
    os_spi_shutdown();
#endif
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
}

#endif
