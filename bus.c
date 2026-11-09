/*
 * Buses: I2C, SPI and 1-Wire buses the platform registers by name. Programs
 * open a device on a bus (an I2C address, an SPI chip select, a 1-Wire ROM
 * code) and run whole transactions; the platform's port moves the bytes.
 */

#include "driver.h"


#include <stdlib.h>
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

/* Allocated when the platform registers the bus, with room for a background transfer's ops. */
typedef struct OsBus
{
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
        /* The kernel's RESET and match ROM come first. */
        OsOneWireOp onewire[OS_BUS_MAX_OPS + 2U];
    } ops;
    OsBusFlag done;
    volatile OsStatus result;
    /* Match ROM and the code, which the port reads during the transfer. */
    uint8_t select[9];
    /* A strong pull-up was left on (POWER 0 last). */
    uint8_t powered;
} OsBus;

static struct
{
    OsBusTraceFunction function;
    void* context;
} g_bus_trace;

static OsStatus os_bus_add(OsBus** buses, uint32_t max, const char* name, uint32_t bus, uint32_t frequency_hz)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsBus* slot = NULL;
    uint32_t free_index = max;
    uint32_t index = 0U;

    if (length == 0U || length > OS_BUS_NAME_MAX)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < max; ++index)
    {
        if (buses[index] != NULL &&
            (buses[index]->bus == bus ||
             (buses[index]->name_length == length && memcmp(buses[index]->name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (buses[index] == NULL && free_index == max)
        {
            free_index = index;
        }
    }
    if (free_index == max)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot = (OsBus*)calloc(1U, sizeof(OsBus));
    if (slot == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    slot->bus = bus;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->frequency_hz = frequency_hz;
    buses[free_index] = slot;
    return OS_STATUS_OK;
}

/* The bus called name; OS_STATUS_UNSUPPORTED when there are no buses at all. */
static OsStatus os_bus_find(OsBus** buses, uint32_t max, const char* name, uint32_t length, OsBus** out_bus)
{
    uint8_t any = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < max; ++index)
    {
        any |= buses[index] != NULL;
        if (buses[index] != NULL && buses[index]->name_length == length &&
            memcmp(buses[index]->name, name, length) == 0)
        {
            *out_bus = buses[index];
            return OS_STATUS_OK;
        }
    }
    return any ? OS_STATUS_FILE_NOT_FOUND : OS_STATUS_UNSUPPORTED;
}

static void os_bus_free_all(OsBus** buses, uint32_t max)
{
    uint32_t index = 0U;

    for (index = 0U; index < max; ++index)
    {
        free(buses[index]);
        buses[index] = NULL;
    }
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
/* Program memory for a data range of an op; length 0 needs no memory. */
static int32_t os_bus_guest_data(uint32_t address, uint32_t length, uint8_t** out_data)
{
    *out_data = NULL;
    if (length == 0U)
    {
        return 0;
    }
    return os_guest_memory(address, length, out_data) == OS_STATUS_OK ? 0 : -WR_EFAULT;
}

static void os_bus_poll_kind(uint8_t kind);
static void os_bus_finish_task_kind(uint8_t kind, uint32_t task_id);
static void os_bus_finish_all(uint8_t kind);

/* The kernel's copy of a transfer's ops, of the bus's kind. */
static const void* os_bus_ops(uint8_t kind, const OsBus* bus)
{
    if (kind == OS_BUS_ONEWIRE)
    {
        return bus->ops.onewire;
    }
    return kind == OS_BUS_I2C ? (const void*)bus->ops.i2c : (const void*)bus->ops.spi;
}

static size_t os_bus_op_size(uint8_t kind)
{
    return kind == OS_BUS_I2C ? sizeof(OsI2cOp) : kind == OS_BUS_SPI ? sizeof(OsSpiOp) : sizeof(OsOneWireOp);
}

static void (*os_bus_port_finish(uint8_t kind))(void*, uint32_t);
static void* os_bus_port_context(uint8_t kind);

/* Hand a finished background transfer to its program: chip select up, trace, wake. */
static void os_bus_deliver(uint8_t kind, OsBus* bus)
{
    OsStatus status = bus->result;
    uint32_t task_id = bus->task_id;

    bus->busy = 0U;
    if (bus->cs != 0U)
    {
        (void)os_gpio_set(bus->cs, 1U);
    }
    os_bus_report(kind, bus->bus, bus->device, task_id, bus->transfer_hz, os_bus_ops(kind, bus), bus->count, status);
    os_transfer_wait_end(task_id, status, 0U);
}

/*
 * Start a transaction on a bus: wait out another program's transfer, let
 * the port run it on the kernel's copy of the ops, and either finish here
 * or, when wait allows, leave the calling program waiting
 * (OS_STATUS_PENDING).
 */
static OsStatus os_bus_start(uint8_t kind, OsBus* bus, uint32_t device, uint32_t frequency_hz, uint32_t mode,
                             uint32_t cs, const void* ops, uint32_t count, OsStatus (*run)(OsBus* bus), uint8_t wait)
{
    OsStatus status = OS_STATUS_OK;

    if (bus->busy)
    {
        os_bus_finish(kind, bus);
    }
    if (count > 0U)
    {
        memcpy(&bus->ops, ops, count * os_bus_op_size(kind));
    }
    bus->task_id = os_task_get_id(os_task_get_current());
    bus->device = device;
    bus->transfer_hz = frequency_hz;
    bus->mode = mode;
    bus->cs = cs;
    bus->count = count;
    (void)OS_BUS_FLAG_TAKE(bus->done);

    if (cs != 0U)
    {
        status = os_gpio_set(cs, 0U);
    }
    if (status == OS_STATUS_OK)
    {
        status = run(bus);
    }
    if (status == OS_STATUS_PENDING)
    {
        bus->busy = 1U;
        if (wait && os_transfer_wait_begin() == OS_STATUS_OK)
        {
            return OS_STATUS_PENDING;
        }
        /* Nobody to wait (a call from the host), or needed now: finish here. */
        os_bus_finish(kind, bus);
        return bus->result;
    }
    if (cs != 0U)
    {
        (void)os_gpio_set(cs, 1U);
    }
    os_bus_report(kind, bus->bus, device, bus->task_id, frequency_hz, os_bus_ops(kind, bus), count, status);
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
    OsBus* buses[OS_I2C_MAX_BUSES];
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

extern const OsDriver os_i2c_driver;

OsStatus os_i2c_add_bus(const char* name, uint32_t bus, uint32_t frequency_hz)
{
    OsStatus status = OS_STATUS_OK;

    if (frequency_hz == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_bus_add(g_i2c.buses, OS_I2C_MAX_BUSES, name, bus, frequency_hz);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    /* A platform with these devices gives programs the i2c interface. */
    if (os_driver_add(&os_i2c_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
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
                        os_i2c_run, 1U);
}

static void os_i2c_poll(void)
{
    os_bus_poll_kind(OS_BUS_I2C);
}

static void os_i2c_finish_task(uint32_t task_id)
{
    os_bus_finish_task_kind(OS_BUS_I2C, task_id);
}

void os_i2c_shutdown(void)
{
    os_bus_finish_all(OS_BUS_I2C);
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
    os_bus_free_all(g_i2c.buses, OS_I2C_MAX_BUSES);
    memset(&g_i2c, 0, sizeof(g_i2c));
}

/* ---------------------------------------------------------------------------
 * SPI
 * ------------------------------------------------------------------------ */

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
    OsBus* buses[OS_SPI_MAX_BUSES];
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

extern const OsDriver os_spi_driver;

OsStatus os_spi_add_bus(const char* name, uint32_t bus, uint32_t max_hz)
{
    OsStatus status = OS_STATUS_OK;

    if (max_hz == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_bus_add(g_spi.buses, OS_SPI_MAX_BUSES, name, bus, max_hz);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    /* A platform with these devices gives programs the spi interface. */
    if (os_driver_add(&os_spi_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
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
        status = os_gpio_open_physical(cs, cs_length, &slot->cs);
        if (status != OS_STATUS_OK)
        {
            slot->cs = 0U;
            return status;
        }
        slot->cs_pin = os_gpio_pin_number(slot->cs);
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
    if (device->cs != 0U)
    {
        os_gpio_close(device->cs);
    }
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
                        count, os_spi_run, 1U);
}

static void os_spi_poll(void)
{
    os_bus_poll_kind(OS_BUS_SPI);
}

static void os_spi_finish_task(uint32_t task_id)
{
    os_bus_finish_task_kind(OS_BUS_SPI, task_id);
}

void os_spi_shutdown(void)
{
    os_bus_finish_all(OS_BUS_SPI);
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
    os_bus_free_all(g_spi.buses, OS_SPI_MAX_BUSES);
    memset(&g_spi, 0, sizeof(g_spi));
}

/* ---------------------------------------------------------------------------
 * 1-Wire
 * ------------------------------------------------------------------------ */

#define OS_ONEWIRE_MATCH_ROM 0x55U
#define OS_ONEWIRE_SEARCH_ROM 0xF0U
#define OS_ONEWIRE_ALARM_SEARCH 0xECU

typedef struct OsOneWireDevice
{
    OsBus* bus;
    /* OS_ONEWIRE_WHOLE_BUS: the program holds the bus. */
    uint64_t rom;
    /* Owning task id, 0 when the slot is free. */
    uint32_t owner;
} OsOneWireDevice;

static struct
{
    OsOneWirePort port;
    uint8_t has_port;
    OsBus* buses[OS_ONEWIRE_MAX_BUSES];
    OsOneWireDevice devices[OS_ONEWIRE_MAX_DEVICES];
} g_onewire;

static OsOneWireDevice* os_onewire_device(uint32_t id)
{
    return id >= 1U && id <= OS_ONEWIRE_MAX_DEVICES && g_onewire.devices[id - 1U].owner != 0U
               ? &g_onewire.devices[id - 1U]
               : NULL;
}

uint8_t os_onewire_crc8(const uint8_t* data, uint32_t length)
{
    uint8_t crc = 0U;
    uint32_t index = 0U;
    uint8_t bit = 0U;

    for (index = 0U; index < length; ++index)
    {
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (uint8_t)((crc & 1U) != 0U ? (crc >> 1) ^ 0x8CU : crc >> 1);
        }
    }
    return crc;
}

/* A ROM code as it goes over the wire: family code first. */
static void os_onewire_rom_bytes(uint64_t rom, uint8_t bytes[8])
{
    uint32_t index = 0U;

    for (index = 0U; index < 8U; ++index)
    {
        bytes[index] = (uint8_t)(rom >> (index * 8U));
    }
}

OsStatus os_onewire_port_set(const OsOneWirePort* port)
{
    if (port == NULL)
    {
        memset(&g_onewire.port, 0, sizeof(g_onewire.port));
        g_onewire.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->transaction == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_onewire.port = *port;
    g_onewire.has_port = 1U;
    return OS_STATUS_OK;
}

extern const OsDriver os_onewire_driver;

OsStatus os_onewire_add_bus(const char* name, uint32_t bus)
{
    /* The speed is the standard one; the field only matters to the other kinds. */
    OsStatus status = os_bus_add(g_onewire.buses, OS_ONEWIRE_MAX_BUSES, name, bus, 0U);

    if (status != OS_STATUS_OK)
    {
        return status;
    }
    /* A platform with these devices gives programs the onewire interface. */
    if (os_driver_add(&os_onewire_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
}

uint32_t os_onewire_get_owner(uint32_t bus, uint64_t rom)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_ONEWIRE_MAX_DEVICES; ++index)
    {
        const OsOneWireDevice* device = &g_onewire.devices[index];
        if (device->owner != 0U && device->bus->bus == bus && device->rom == rom)
        {
            return device->owner;
        }
    }
    return 0U;
}

/* The program holding the whole bus, 0 when none does. */
static uint32_t os_onewire_holder(const OsBus* bus)
{
    return os_onewire_get_owner(bus->bus, OS_ONEWIRE_WHOLE_BUS);
}

OsStatus os_onewire_open(const char* bus_name, uint32_t length, uint64_t rom, uint32_t* out_id)
{
    OsBus* bus = NULL;
    OsOneWireDevice* slot = NULL;
    OsStatus status = OS_STATUS_OK;
    uint8_t bytes[8];
    uint32_t index = 0U;

    if (!g_onewire.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = os_bus_find(g_onewire.buses, OS_ONEWIRE_MAX_BUSES, bus_name, length, &bus);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    os_onewire_rom_bytes(rom, bytes);
    if (rom != OS_ONEWIRE_WHOLE_BUS && os_onewire_crc8(bytes, 7U) != bytes[7])
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    /* The whole bus excludes every device on it, and the other way round. */
    for (index = 0U; index < OS_ONEWIRE_MAX_DEVICES; ++index)
    {
        OsOneWireDevice* device = &g_onewire.devices[index];
        if (device->owner != 0U && device->bus == bus &&
            (device->rom == rom || device->rom == OS_ONEWIRE_WHOLE_BUS || rom == OS_ONEWIRE_WHOLE_BUS))
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
    slot->rom = rom;
    slot->owner = os_task_get_id(os_task_get_current());
    *out_id = (uint32_t)(slot - g_onewire.devices + 1);
    return OS_STATUS_OK;
}

static OsStatus os_onewire_run(OsBus* bus)
{
    return g_onewire.port.transaction(g_onewire.port.context, bus->bus, bus->ops.onewire, bus->count);
}

/* Ops on a bus, done when this returns (a search, a pull-up to end). */
static OsStatus os_onewire_now(OsBus* bus, const OsOneWireOp* ops, uint32_t count)
{
    bus->powered = 0U;
    return os_bus_start(OS_BUS_ONEWIRE, bus, 0U, 0U, 0U, 0U, ops, count, os_onewire_run, 0U);
}

void os_onewire_close(uint32_t id)
{
    OsOneWireDevice* device = os_onewire_device(id);
    OsBus* bus = NULL;

    if (device == NULL)
    {
        return;
    }
    bus = device->bus;
    memset(device, 0, sizeof(*device));
    /* A pull-up the program left on ends with it. */
    if (bus->powered && g_onewire.has_port)
    {
        (void)os_onewire_now(bus, NULL, 0U);
    }
}

OsStatus os_onewire_transaction(uint32_t id, const OsOneWireOp* ops, uint32_t count)
{
    OsOneWireDevice* device = os_onewire_device(id);
    OsOneWireOp all[OS_BUS_MAX_OPS + 2U];
    uint32_t first = 0U;
    uint32_t index = 0U;
    OsStatus status = OS_STATUS_OK;
    OsBus* bus = NULL;

    if (device == NULL || count > OS_BUS_MAX_OPS)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_onewire.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < count; ++index)
    {
        const OsOneWireOp* op = &ops[index];
        uint8_t after_write = index > 0U && (ops[index - 1U].kind == OS_ONEWIRE_WRITE ||
                                             ops[index - 1U].kind == OS_ONEWIRE_WRITE_BITS);
        if (op->kind < OS_ONEWIRE_WRITE || op->kind > OS_ONEWIRE_RESET ||
            /* A device's transaction starts with the kernel's reset and ROM command. */
            (op->kind == OS_ONEWIRE_RESET && device->rom != OS_ONEWIRE_WHOLE_BUS) ||
            (op->kind == OS_ONEWIRE_POWER &&
             (!after_write || op->length > OS_ONEWIRE_MAX_POWER_MS || (op->length == 0U && index + 1U != count))))
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
    }

    bus = device->bus;
    /* select is the port's until a background transfer ends. */
    if (bus->busy)
    {
        os_bus_finish(OS_BUS_ONEWIRE, bus);
    }
    if (device->rom != OS_ONEWIRE_WHOLE_BUS)
    {
        bus->select[0] = OS_ONEWIRE_MATCH_ROM;
        os_onewire_rom_bytes(device->rom, bus->select + 1U);
        all[0].kind = OS_ONEWIRE_RESET;
        all[0].data = NULL;
        all[0].length = 0U;
        all[1].kind = OS_ONEWIRE_WRITE;
        all[1].data = bus->select;
        all[1].length = sizeof(bus->select);
        first = 2U;
    }
    if (count > 0U)
    {
        memcpy(all + first, ops, count * sizeof(OsOneWireOp));
    }
    bus->powered = count > 0U && ops[count - 1U].kind == OS_ONEWIRE_POWER && ops[count - 1U].length == 0U;
    status = os_bus_start(OS_BUS_ONEWIRE, bus, 0U, 0U, 0U, 0U, all, first + count, os_onewire_run, 1U);
    if (status != OS_STATUS_OK && status != OS_STATUS_PENDING)
    {
        bus->powered = 0U;
    }
    return status;
}

/* One step of a search: the bit, its complement and the direction taken. */
static OsStatus os_onewire_triplet(OsBus* bus, uint8_t direction, uint8_t* out_bits)
{
    OsOneWireOp op;
    uint8_t bits = 0U;
    OsStatus status = OS_STATUS_OK;

    if (g_onewire.port.triplet != NULL)
    {
        return g_onewire.port.triplet(g_onewire.port.context, bus->bus, direction, out_bits);
    }
    op.kind = OS_ONEWIRE_READ_BITS;
    op.data = &bits;
    op.length = 2U;
    status = os_onewire_now(bus, &op, 1U);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    bits &= 3U;
    /* Devices agree on the bit: follow it. Both bits 1: nobody answered, nothing to write. */
    if (bits == 1U || bits == 2U)
    {
        direction = bits & 1U;
    }
    if (bits != 3U)
    {
        op.kind = OS_ONEWIRE_WRITE_BITS;
        op.data = &direction;
        op.length = 1U;
        status = os_onewire_now(bus, &op, 1U);
    }
    *out_bits = (uint8_t)(bits | (direction << 2));
    return status;
}

OsStatus os_onewire_search(const char* bus_name, uint32_t length, OsOneWireSearch* state, uint32_t flags,
                           uint8_t* out_found)
{
    static const uint8_t commands[2] = { OS_ONEWIRE_SEARCH_ROM, OS_ONEWIRE_ALARM_SEARCH };
    OsBus* bus = NULL;
    OsOneWireOp ops[2];
    uint8_t rom[8];
    uint32_t holder = 0U;
    uint32_t bit = 0U;
    uint32_t last_zero = 0U;
    OsStatus status = OS_STATUS_OK;

    *out_found = 0U;
    if (!g_onewire.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    status = os_bus_find(g_onewire.buses, OS_ONEWIRE_MAX_BUSES, bus_name, length, &bus);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if ((flags & ~OS_ONEWIRE_SEARCH_ALARM) != 0U || state->discrepancy > 64U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    /* A program holding the bus is in the middle of its own commands. */
    holder = os_onewire_holder(bus);
    if (holder != 0U && holder != os_task_get_id(os_task_get_current()))
    {
        return OS_STATUS_BUSY;
    }
    if (state->done)
    {
        memset(state, 0, sizeof(*state));
        return OS_STATUS_OK;
    }

    ops[0].kind = OS_ONEWIRE_RESET;
    ops[0].data = NULL;
    ops[0].length = 0U;
    ops[1].kind = OS_ONEWIRE_WRITE;
    ops[1].data = (uint8_t*)&commands[(flags & OS_ONEWIRE_SEARCH_ALARM) != 0U ? 1 : 0];
    ops[1].length = 1U;
    status = os_onewire_now(bus, ops, 2U);
    if (status == OS_STATUS_NACK)
    {
        /* No device on the bus. */
        memset(state, 0, sizeof(*state));
        return OS_STATUS_OK;
    }
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    os_onewire_rom_bytes(state->rom, rom);
    for (bit = 1U; bit <= 64U; ++bit)
    {
        uint8_t mask = (uint8_t)(1U << ((bit - 1U) % 8U));
        uint8_t* byte = &rom[(bit - 1U) / 8U];
        /* Up to the last branch, the way the last ROM went; at it, the other way; past it, 0 first. */
        uint8_t direction = bit < state->discrepancy ? (uint8_t)((*byte & mask) != 0U) : (uint8_t)(bit == state->discrepancy);
        uint8_t bits = 0U;

        status = os_onewire_triplet(bus, direction, &bits);
        if (status != OS_STATUS_OK)
        {
            return status;
        }
        if ((bits & 3U) == 3U)
        {
            /* Every device fell silent: they left the bus. */
            memset(state, 0, sizeof(*state));
            return OS_STATUS_OK;
        }
        if ((bits & 3U) == 0U && (bits & 4U) == 0U)
        {
            last_zero = bit;
        }
        *byte = (uint8_t)((bits & 4U) != 0U ? *byte | mask : *byte & ~mask);
    }
    /* Noise on the line shows as a code that fails its CRC (or the family 0 no device has). */
    if (os_onewire_crc8(rom, 7U) != rom[7] || rom[0] == 0U)
    {
        return OS_STATUS_BUS_ERROR;
    }
    state->rom = 0U;
    for (bit = 0U; bit < 8U; ++bit)
    {
        state->rom |= (uint64_t)rom[bit] << (bit * 8U);
    }
    state->discrepancy = last_zero;
    state->done = last_zero == 0U;
    *out_found = 1U;
    return OS_STATUS_OK;
}

static void os_onewire_poll(void)
{
    os_bus_poll_kind(OS_BUS_ONEWIRE);
}

static void os_onewire_finish_task(uint32_t task_id)
{
    os_bus_finish_task_kind(OS_BUS_ONEWIRE, task_id);
}

void os_onewire_shutdown(void)
{
    os_bus_finish_all(OS_BUS_ONEWIRE);
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
    os_bus_free_all(g_onewire.buses, OS_ONEWIRE_MAX_BUSES);
    memset(&g_onewire, 0, sizeof(g_onewire));
}

static OsBus* os_bus_by_number(uint8_t kind, uint32_t number)
{
    OsBus** buses = NULL;
    uint32_t max = 0U;
    uint32_t index = 0U;

    if (kind == OS_BUS_I2C)
    {
        buses = g_i2c.buses;
        max = OS_I2C_MAX_BUSES;
    }
    if (kind == OS_BUS_SPI)
    {
        buses = g_spi.buses;
        max = OS_SPI_MAX_BUSES;
    }
    if (kind == OS_BUS_ONEWIRE)
    {
        buses = g_onewire.buses;
        max = OS_ONEWIRE_MAX_BUSES;
    }
    for (index = 0U; index < max; ++index)
    {
        if (buses[index] != NULL && buses[index]->bus == number)
        {
            return buses[index];
        }
    }
    return NULL;
}

static void (*os_bus_port_finish(uint8_t kind))(void*, uint32_t)
{
    if (kind == OS_BUS_I2C)
    {
        return g_i2c.port.finish;
    }
    if (kind == OS_BUS_SPI)
    {
        return g_spi.port.finish;
    }
    if (kind == OS_BUS_ONEWIRE)
    {
        return g_onewire.port.finish;
    }
    return NULL;
}

static void* os_bus_port_context(uint8_t kind)
{
    if (kind == OS_BUS_I2C)
    {
        return g_i2c.port.context;
    }
    if (kind == OS_BUS_SPI)
    {
        return g_spi.port.context;
    }
    if (kind == OS_BUS_ONEWIRE)
    {
        return g_onewire.port.context;
    }
    return NULL;
}

/* The buses of a kind, for the loops below. */
static void os_bus_each(uint8_t kind, void (*visit)(uint8_t kind, OsBus* bus, uint32_t task_id), uint32_t task_id)
{
    OsBus** buses = NULL;
    uint32_t max = 0U;
    uint32_t index = 0U;

    if (kind == OS_BUS_I2C)
    {
        buses = g_i2c.buses;
        max = OS_I2C_MAX_BUSES;
    }
    if (kind == OS_BUS_SPI)
    {
        buses = g_spi.buses;
        max = OS_SPI_MAX_BUSES;
    }
    if (kind == OS_BUS_ONEWIRE)
    {
        buses = g_onewire.buses;
        max = OS_ONEWIRE_MAX_BUSES;
    }
    for (index = 0U; index < max; ++index)
    {
        if (buses[index] != NULL)
        {
            visit(kind, buses[index], task_id);
        }
    }
}

static void os_bus_poll_one(uint8_t kind, OsBus* bus, uint32_t task_id)
{
    (void)task_id;
    if (bus->busy && OS_BUS_FLAG_TAKE(bus->done))
    {
        os_bus_deliver(kind, bus);
    }
}

static void os_bus_finish_one(uint8_t kind, OsBus* bus, uint32_t task_id)
{
    if (bus->busy && (task_id == 0U || bus->task_id == task_id))
    {
        os_bus_finish(kind, bus);
    }
}

/* Transfers of a kind that ended in the background reach their programs. */
static void os_bus_poll_kind(uint8_t kind)
{
    os_bus_each(kind, os_bus_poll_one, 0U);
}

/* End the task's transfers on buses of a kind now. */
static void os_bus_finish_task_kind(uint8_t kind, uint32_t task_id)
{
    if (task_id != 0U)
    {
        os_bus_each(kind, os_bus_finish_one, task_id);
    }
}

/* Ports must be done with every buffer of a kind before the kernel lets go. */
static void os_bus_finish_all(uint8_t kind)
{
    os_bus_each(kind, os_bus_finish_one, 0U);
}

/* ---------------------------------------------------------------------------
 * The i2c interface for programs
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_i2c_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, bus_address);
    OS_HOST_ARG(uint32_t, bus_length);
    OS_HOST_ARG(uint32_t, address);
    OS_HOST_ARG(uint32_t, flags);
    const char* bus = NULL;
    uint32_t id = 0U;
    int32_t handle = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    handle = os_abi_read_name(bus_address, bus_length, &bus);
    if (handle < 0)
    {
        OS_HOST_RETURN(handle);
    }
    if (bus == NULL || flags != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    status = os_i2c_open(bus, bus_length, address, &id);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(status == OS_STATUS_BUSY ? -WR_EINUSE : os_abi_error(status));
    }
    handle = os_driver_handle_new(&os_i2c_driver, id);
    if (handle < 0)
    {
        os_i2c_close(id);
    }
    OS_HOST_RETURN(handle);
}

/* ops: count records of 12 bytes, u32 kind, u32 data, u32 length. */
static OS_HOST_FUNCTION(os_i2c_call_transaction)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, ops_address);
    OS_HOST_ARG(uint32_t, count);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_i2c_driver, handle, &id);
    OsI2cOp ops[OS_BUS_MAX_OPS];
    uint8_t* records = NULL;
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (count > OS_BUS_MAX_OPS)
    {
        OS_HOST_RETURN(-WR_ELIMIT);
    }
    if (count > 0U && os_guest_memory(ops_address, count * 12U, &records) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        const uint8_t* record = records + index * 12U;
        int32_t result = os_bus_guest_data(OS_HOST_READ32(record + 4U), OS_HOST_READ32(record + 8U), &ops[index].data);
        uint32_t kind = OS_HOST_READ32(record);
        if (result < 0)
        {
            OS_HOST_RETURN(result);
        }
        if (kind != OS_I2C_WRITE && kind != OS_I2C_READ)
        {
            OS_HOST_RETURN(-WR_EINVAL);
        }
        ops[index].kind = (uint8_t)kind;
        ops[index].length = OS_HOST_READ32(record + 8U);
    }
    /* A port working in the background leaves the program waiting; the end writes the result. */
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    *raw_return = os_abi_error(os_i2c_transaction(id, ops, count));
    return os_guest_end();
}

static const OsDriverCall os_i2c_calls[] =
{
    { "open", "i(iiii)", os_i2c_call_open, 3U },
    { "transaction", "i(iii)", os_i2c_call_transaction, 3U },
};

const OsDriver os_i2c_driver =
{
    .interface = "i2c",
    .calls = os_i2c_calls,
    .call_count = (uint32_t)(sizeof(os_i2c_calls) / sizeof(os_i2c_calls[0])),
    .close = os_i2c_close,
    .poll = os_i2c_poll,
    .finish_task = os_i2c_finish_task,
    .shutdown = os_i2c_shutdown,
};

/* ---------------------------------------------------------------------------
 * The spi interface for programs
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_spi_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, bus_address);
    OS_HOST_ARG(uint32_t, bus_length);
    OS_HOST_ARG(uint32_t, cs_address);
    OS_HOST_ARG(uint32_t, cs_length);
    OS_HOST_ARG(uint32_t, mode);
    OS_HOST_ARG(uint32_t, frequency_hz);
    const char* bus = NULL;
    const char* cs = NULL;
    uint32_t id = 0U;
    int32_t handle = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    handle = os_abi_read_name(bus_address, bus_length, &bus);
    if (handle >= 0)
    {
        handle = os_abi_read_name(cs_address, cs_length, &cs);
    }
    if (handle < 0)
    {
        OS_HOST_RETURN(handle);
    }
    if (bus == NULL)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    status = os_spi_open(bus, bus_length, cs, cs_length, mode, frequency_hz, &id);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(status == OS_STATUS_BUSY ? -WR_EINUSE : os_abi_error(status));
    }
    handle = os_driver_handle_new(&os_spi_driver, id);
    if (handle < 0)
    {
        os_spi_close(id);
    }
    OS_HOST_RETURN(handle);
}

/* ops: count records of 16 bytes, u32 kind, u32 write, u32 read, u32 length. */
static OS_HOST_FUNCTION(os_spi_call_transaction)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, ops_address);
    OS_HOST_ARG(uint32_t, count);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_spi_driver, handle, &id);
    OsSpiOp ops[OS_BUS_MAX_OPS];
    uint8_t* records = NULL;
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (count > OS_BUS_MAX_OPS)
    {
        OS_HOST_RETURN(-WR_ELIMIT);
    }
    if (count == 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_guest_memory(ops_address, count * 16U, &records) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        const uint8_t* record = records + index * 16U;
        uint32_t kind = OS_HOST_READ32(record);
        uint32_t length = OS_HOST_READ32(record + 12U);
        uint8_t* write = NULL;
        uint8_t* read = NULL;
        int32_t result = 0;

        if (kind < OS_SPI_WRITE || kind > OS_SPI_DELAY_US)
        {
            OS_HOST_RETURN(-WR_EINVAL);
        }
        if (kind == OS_SPI_WRITE || kind == OS_SPI_TRANSFER)
        {
            result = os_bus_guest_data(OS_HOST_READ32(record + 4U), length, &write);
        }
        if (result == 0 && (kind == OS_SPI_READ || kind == OS_SPI_TRANSFER))
        {
            result = os_bus_guest_data(OS_HOST_READ32(record + 8U), length, &read);
        }
        if (result < 0)
        {
            OS_HOST_RETURN(result);
        }
        ops[index].kind = (uint8_t)kind;
        ops[index].write = write;
        ops[index].read = read;
        ops[index].length = length;
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    *raw_return = os_abi_error(os_spi_transaction(id, ops, count));
    return os_guest_end();
}

static const OsDriverCall os_spi_calls[] =
{
    { "open", "i(iiiiii)", os_spi_call_open, 3U },
    { "transaction", "i(iii)", os_spi_call_transaction, 3U },
};

const OsDriver os_spi_driver =
{
    .interface = "spi",
    .calls = os_spi_calls,
    .call_count = (uint32_t)(sizeof(os_spi_calls) / sizeof(os_spi_calls[0])),
    .close = os_spi_close,
    .poll = os_spi_poll,
    .finish_task = os_spi_finish_task,
    .shutdown = os_spi_shutdown,
};

/* ---------------------------------------------------------------------------
 * The onewire interface for programs
 * ------------------------------------------------------------------------ */

static OS_HOST_FUNCTION(os_onewire_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, bus_address);
    OS_HOST_ARG(uint32_t, bus_length);
    OS_HOST_ARG(uint64_t, rom);
    OS_HOST_ARG(uint32_t, flags);
    const char* bus = NULL;
    uint32_t id = 0U;
    int32_t handle = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    handle = os_abi_read_name(bus_address, bus_length, &bus);
    if (handle < 0)
    {
        OS_HOST_RETURN(handle);
    }
    if (bus == NULL || flags != 0U)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    status = os_onewire_open(bus, bus_length, rom, &id);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(status == OS_STATUS_BUSY ? -WR_EINUSE : os_abi_error(status));
    }
    handle = os_driver_handle_new(&os_onewire_driver, id);
    if (handle < 0)
    {
        os_onewire_close(id);
    }
    OS_HOST_RETURN(handle);
}

/* ops: count records of 12 bytes, u32 kind, u32 data, u32 length (bits for the _BITS kinds, ms for POWER). */
static OS_HOST_FUNCTION(os_onewire_call_transaction)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, ops_address);
    OS_HOST_ARG(uint32_t, count);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_onewire_driver, handle, &id);
    OsOneWireOp ops[OS_BUS_MAX_OPS];
    uint8_t* records = NULL;
    uint32_t index = 0U;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (count > OS_BUS_MAX_OPS)
    {
        OS_HOST_RETURN(-WR_ELIMIT);
    }
    if (count > 0U && os_guest_memory(ops_address, count * 12U, &records) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        const uint8_t* record = records + index * 12U;
        uint32_t kind = OS_HOST_READ32(record);
        uint32_t length = OS_HOST_READ32(record + 8U);
        /* Bit ops use whole bytes, a pull-up and a reset none. */
        uint32_t bytes = kind == OS_ONEWIRE_WRITE || kind == OS_ONEWIRE_READ ? length
                         : kind == OS_ONEWIRE_WRITE_BITS || kind == OS_ONEWIRE_READ_BITS ? (length + 7U) / 8U
                                                                                         : 0U;
        int32_t result = 0;
        if (kind < OS_ONEWIRE_WRITE || kind > OS_ONEWIRE_RESET ||
            ((kind == OS_ONEWIRE_WRITE_BITS || kind == OS_ONEWIRE_READ_BITS) && length > 0xFFFFFFF8U))
        {
            OS_HOST_RETURN(-WR_EINVAL);
        }
        result = os_bus_guest_data(OS_HOST_READ32(record + 4U), bytes, &ops[index].data);
        if (result < 0)
        {
            OS_HOST_RETURN(result);
        }
        ops[index].kind = (uint8_t)kind;
        ops[index].length = length;
    }
    /* A port working in the background leaves the program waiting; the end writes the result. */
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    *raw_return = os_abi_error(os_onewire_transaction(id, ops, count));
    return os_guest_end();
}

/* state: 16 bytes, u64 rom, u32 discrepancy, u32 done. 1 and the next ROM, or 0 when there are no more. */
static OS_HOST_FUNCTION(os_onewire_call_search)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, bus_address);
    OS_HOST_ARG(uint32_t, bus_length);
    OS_HOST_ARG(uint32_t, state_address);
    OS_HOST_ARG(uint32_t, flags);
    const char* bus = NULL;
    uint8_t* memory = NULL;
    OsOneWireSearch state;
    uint8_t found = 0U;
    int32_t result = 0;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    result = os_abi_read_name(bus_address, bus_length, &bus);
    if (result < 0)
    {
        OS_HOST_RETURN(result);
    }
    if (bus == NULL)
    {
        OS_HOST_RETURN(-WR_EINVAL);
    }
    if (os_guest_memory(state_address, 16U, &memory) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    state.rom = OS_HOST_READ64(memory);
    state.discrepancy = OS_HOST_READ32(memory + 8U);
    state.done = OS_HOST_READ32(memory + 12U);
    status = os_onewire_search(bus, bus_length, &state, flags, &found);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(status == OS_STATUS_BUSY ? -WR_EINUSE : os_abi_error(status));
    }
    OS_HOST_WRITE64(memory, state.rom);
    OS_HOST_WRITE32(memory + 8U, state.discrepancy);
    OS_HOST_WRITE32(memory + 12U, state.done);
    OS_HOST_RETURN((int32_t)found);
}

static const OsDriverCall os_onewire_calls[] =
{
    { "open", "i(iiIi)", os_onewire_call_open, 6U },
    { "transaction", "i(iii)", os_onewire_call_transaction, 6U },
    { "search", "i(iiii)", os_onewire_call_search, 6U },
};

const OsDriver os_onewire_driver =
{
    .interface = "onewire",
    .calls = os_onewire_calls,
    .call_count = (uint32_t)(sizeof(os_onewire_calls) / sizeof(os_onewire_calls[0])),
    .close = os_onewire_close,
    .poll = os_onewire_poll,
    .finish_task = os_onewire_finish_task,
    .shutdown = os_onewire_shutdown,
};

