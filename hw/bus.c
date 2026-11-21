/*
 * Buses: I2C, SPI and 1-Wire buses the platform registers by name. Programs
 * open a device on a bus (an I2C address, an SPI chip select, a 1-Wire ROM
 * code) and run whole transactions; the platform's port moves the bytes.
 */

#include "hw_driver.h"


#include <stdlib.h>
#include <string.h>

/* A background transfer's end is recorded by interrupt handlers and taken by the kernel. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint8_t HwBusFlag;
#define HW_BUS_FLAG_SET(flag) ((flag) = 1U)
#define HW_BUS_FLAG_TAKE(flag) hw_bus_flag_take(&(flag))
static uint8_t hw_bus_flag_take(HwBusFlag* flag)
{
    uint8_t value = *flag;
    *flag = 0U;
    return value;
}
#else
#include <stdatomic.h>
typedef atomic_uchar HwBusFlag;
#define HW_BUS_FLAG_SET(flag) atomic_store_explicit(&(flag), 1U, memory_order_release)
#define HW_BUS_FLAG_TAKE(flag) atomic_exchange_explicit(&(flag), 0U, memory_order_acquire)
#endif

/* Longest delay op, in microseconds. */
#define HW_SPI_MAX_DELAY_US 65535U

/* Allocated when the platform registers the bus, with room for a background transfer's ops. */
typedef struct HwBus
{
    uint32_t bus;
    char name[HW_BUS_NAME_MAX + 1U];
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
        HwI2cOp i2c[HW_BUS_MAX_OPS];
        HwSpiOp spi[HW_BUS_MAX_OPS];
        /* The kernel's RESET and match ROM come first. */
        HwOneWireOp onewire[HW_BUS_MAX_OPS + 2U];
    } ops;
    HwBusFlag done;
    volatile HwStatus result;
    /* Copies of the program's buffers the port works on, written back when the transfer ends. */
    HwGuestKept* kept;
    /* Match ROM and the code, which the port reads during the transfer. */
    uint8_t select[9];
    /* A strong pull-up was left on (POWER 0 last). */
    uint8_t powered;
} HwBus;

static struct
{
    HwBusTraceFunction function;
    void* context;
} g_bus_trace;

static HwStatus hw_bus_add(HwBus** buses, uint32_t max, const char* name, uint32_t bus, uint32_t frequency_hz)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    HwBus* slot = NULL;
    uint32_t free_index = max;
    uint32_t index = 0U;

    if (length == 0U || length > HW_BUS_NAME_MAX)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < max; ++index)
    {
        if (buses[index] != NULL &&
            (buses[index]->bus == bus ||
             (buses[index]->name_length == length && memcmp(buses[index]->name, name, length) == 0)))
        {
            return HW_STATUS_BUSY;
        }
        if (buses[index] == NULL && free_index == max)
        {
            free_index = index;
        }
    }
    if (free_index == max)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot = (HwBus*)calloc(1U, sizeof(HwBus));
    if (slot == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    slot->bus = bus;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->frequency_hz = frequency_hz;
    buses[free_index] = slot;
    return HW_STATUS_OK;
}

/* The bus called name; HW_STATUS_UNSUPPORTED when there are no buses at all. */
static HwStatus hw_bus_find(HwBus** buses, uint32_t max, const char* name, uint32_t length, HwBus** out_bus)
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
            return HW_STATUS_OK;
        }
    }
    return any ? HW_STATUS_FILE_NOT_FOUND : HW_STATUS_UNSUPPORTED;
}

static void hw_bus_free_all(HwBus** buses, uint32_t max)
{
    uint32_t index = 0U;

    for (index = 0U; index < max; ++index)
    {
        free(buses[index]);
        buses[index] = NULL;
    }
}

static void hw_bus_report(uint8_t kind, uint32_t bus, uint32_t device, uint32_t task_id, uint32_t frequency_hz,
                          const void* ops, uint32_t count, HwStatus status)
{
    HwBusTrace trace;

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

void hw_bus_trace_set(HwBusTraceFunction function, void* context)
{
    g_bus_trace.function = function;
    g_bus_trace.context = function != NULL ? context : NULL;
}

static void hw_bus_finish(uint8_t kind, HwBus* bus);
static HwBus* hw_bus_by_number(uint8_t kind, uint32_t number);

/* The program's buffers of one transaction, open while it is set up and runs. */
typedef struct HwBusBuffers
{
    HwGuestBuffer items[2U * HW_BUS_MAX_OPS];
    uint32_t count;
} HwBusBuffers;

/* Program memory for a data range of an op; length 0 needs no memory. */
static int32_t hw_bus_guest_data(HwBusBuffers* buffers, uint32_t address, uint32_t length, uint8_t access,
                                 uint8_t** out_data)
{
    HwGuestBuffer* buffer = &buffers->items[buffers->count];
    HwStatus status = HW_STATUS_OK;

    *out_data = NULL;
    if (length == 0U)
    {
        return 0;
    }
    status = hw_host_buffer_open(buffer, address, length, access);
    if (status != HW_STATUS_OK)
    {
        return HW_GUEST_BUFFER_ERROR(status);
    }
    ++buffers->count;
    *out_data = buffer->data;
    return 0;
}

static void hw_bus_buffers_drop(HwBusBuffers* buffers)
{
    while (buffers->count > 0U)
    {
        hw_host_buffer_drop(&buffers->items[--buffers->count]);
    }
}

static void hw_bus_keep_buffers(uint8_t kind, HwBus* bus, uint32_t task_id);
static void hw_bus_each(uint8_t kind, void (*visit)(uint8_t kind, HwBus* bus, uint32_t task_id), uint32_t task_id);
static HwBusBuffers* g_bus_keeping;

/*
 * The transaction is over (status), or the port goes on in the background
 * (HW_STATUS_PENDING): then copies of the program's buffers wait in the
 * bus until the end writes them back.
 */
static void hw_bus_buffers_end(uint8_t kind, HwBusBuffers* buffers, HwStatus status)
{
    if (status == HW_STATUS_PENDING)
    {
        g_bus_keeping = buffers;
        hw_bus_each(kind, hw_bus_keep_buffers, hw_host_task_id());
        g_bus_keeping = NULL;
    }
    while (buffers->count > 0U)
    {
        (void)hw_host_buffer_close(&buffers->items[--buffers->count]);
    }
}

static void hw_bus_keep_buffers(uint8_t kind, HwBus* bus, uint32_t task_id)
{
    (void)kind;
    if (bus->busy && bus->task_id == task_id)
    {
        while (g_bus_keeping->count > 0U)
        {
            hw_host_buffer_keep(&g_bus_keeping->items[--g_bus_keeping->count], &bus->kept);
        }
    }
}

static void hw_bus_poll_kind(uint8_t kind);
static void hw_bus_finish_task_kind(uint8_t kind, uint32_t task_id);
static void hw_bus_finish_all(uint8_t kind);

/* The kernel's copy of a transfer's ops, of the bus's kind. */
static const void* hw_bus_ops(uint8_t kind, const HwBus* bus)
{
    if (kind == HW_BUS_ONEWIRE)
    {
        return bus->ops.onewire;
    }
    return kind == HW_BUS_I2C ? (const void*)bus->ops.i2c : (const void*)bus->ops.spi;
}

static size_t hw_bus_op_size(uint8_t kind)
{
    return kind == HW_BUS_I2C ? sizeof(HwI2cOp) : kind == HW_BUS_SPI ? sizeof(HwSpiOp) : sizeof(HwOneWireOp);
}

static void (*hw_bus_port_finish(uint8_t kind))(void*, uint32_t);
static void* hw_bus_port_context(uint8_t kind);

/* Hand a finished background transfer to its program: chip select up, trace, wake. */
static void hw_bus_deliver(uint8_t kind, HwBus* bus)
{
    HwStatus status = bus->result;
    uint32_t task_id = bus->task_id;

    bus->busy = 0U;
    if (bus->cs != 0U)
    {
        (void)hw_gpio_set(bus->cs, 1U);
    }
    (void)hw_host_kept_close(&bus->kept);
    hw_bus_report(kind, bus->bus, bus->device, task_id, bus->transfer_hz, hw_bus_ops(kind, bus), bus->count, status);
    hw_host_transfer_wait_end(task_id, status, 0U);
}

/*
 * Start a transaction on a bus: wait out another program's transfer, let
 * the port run it on the kernel's copy of the ops, and either finish here
 * or, when wait allows, leave the calling program waiting
 * (HW_STATUS_PENDING).
 */
static HwStatus hw_bus_start(uint8_t kind, HwBus* bus, uint32_t device, uint32_t frequency_hz, uint32_t mode,
                             uint32_t cs, const void* ops, uint32_t count, HwStatus (*run)(HwBus* bus), uint8_t wait)
{
    HwStatus status = HW_STATUS_OK;

    if (bus->busy)
    {
        hw_bus_finish(kind, bus);
    }
    if (count > 0U)
    {
        memcpy(&bus->ops, ops, count * hw_bus_op_size(kind));
    }
    bus->task_id = hw_host_task_id();
    bus->device = device;
    bus->transfer_hz = frequency_hz;
    bus->mode = mode;
    bus->cs = cs;
    bus->count = count;
    (void)HW_BUS_FLAG_TAKE(bus->done);

    if (cs != 0U)
    {
        status = hw_gpio_set(cs, 0U);
    }
    if (status == HW_STATUS_OK)
    {
        status = run(bus);
    }
    if (status == HW_STATUS_PENDING)
    {
        bus->busy = 1U;
        if (wait && hw_host_transfer_wait_begin() == HW_STATUS_OK)
        {
            return HW_STATUS_PENDING;
        }
        /* Nobody to wait (a call from the host), or needed now: finish here. */
        hw_bus_finish(kind, bus);
        return bus->result;
    }
    if (cs != 0U)
    {
        (void)hw_gpio_set(cs, 1U);
    }
    hw_bus_report(kind, bus->bus, device, bus->task_id, frequency_hz, hw_bus_ops(kind, bus), count, status);
    return status;
}

/* Make the port end a background transfer now, then deliver it. */
static void hw_bus_finish(uint8_t kind, HwBus* bus)
{
    void (*finish)(void*, uint32_t) = hw_bus_port_finish(kind);

    if (!bus->busy)
    {
        return;
    }
    if (!HW_BUS_FLAG_TAKE(bus->done))
    {
        if (finish != NULL)
        {
            finish(hw_bus_port_context(kind), bus->bus);
        }
        /* A port that did not report even now has failed the transfer. */
        if (!HW_BUS_FLAG_TAKE(bus->done))
        {
            bus->result = HW_STATUS_BUS_ERROR;
        }
    }
    hw_bus_deliver(kind, bus);
}

void hw_bus_complete(uint32_t kind, uint32_t number, HwStatus status)
{
    HwBus* bus = hw_bus_by_number((uint8_t)kind, number);

    if (bus != NULL && bus->busy)
    {
        bus->result = status;
        HW_BUS_FLAG_SET(bus->done);
    }
}

uint8_t hw_bus_is_busy(uint32_t kind, uint32_t number)
{
    HwBus* bus = hw_bus_by_number((uint8_t)kind, number);
    return bus != NULL && bus->busy;
}

/* ---------------------------------------------------------------------------
 * I2C
 * ------------------------------------------------------------------------ */

typedef struct HwI2cDevice
{
    HwBus* bus;
    uint32_t address;
    /* Owning task id, 0 when the slot is free. */
    uint32_t owner;
} HwI2cDevice;

static struct
{
    HwI2cPort port;
    uint8_t has_port;
    HwBus* buses[HW_I2C_MAX_BUSES];
    HwI2cDevice devices[HW_I2C_MAX_DEVICES];
} g_i2c;

static HwI2cDevice* hw_i2c_device(uint32_t id)
{
    return id >= 1U && id <= HW_I2C_MAX_DEVICES && g_i2c.devices[id - 1U].owner != 0U ? &g_i2c.devices[id - 1U] : NULL;
}

HwStatus hw_i2c_port_set(const HwI2cPort* port)
{
    if (port == NULL)
    {
        memset(&g_i2c.port, 0, sizeof(g_i2c.port));
        g_i2c.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->transaction == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_i2c.port = *port;
    g_i2c.has_port = 1U;
    return HW_STATUS_OK;
}

extern const HwDriver hw_i2c_driver;

HwStatus hw_i2c_add_bus(const char* name, uint32_t bus, uint32_t frequency_hz)
{
    HwStatus status = HW_STATUS_OK;

    if (frequency_hz == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    status = hw_bus_add(g_i2c.buses, HW_I2C_MAX_BUSES, name, bus, frequency_hz);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    /* A platform with these devices gives programs the i2c interface. */
    if (hw_driver_add(&hw_i2c_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

uint32_t hw_i2c_get_owner(uint32_t bus, uint32_t address)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_I2C_MAX_DEVICES; ++index)
    {
        const HwI2cDevice* device = &g_i2c.devices[index];
        if (device->owner != 0U && device->bus->bus == bus && device->address == address)
        {
            return device->owner;
        }
    }
    return 0U;
}

HwStatus hw_i2c_open(const char* bus_name, uint32_t length, uint32_t address, uint32_t* out_id)
{
    HwBus* bus = NULL;
    HwI2cDevice* slot = NULL;
    HwStatus status = HW_STATUS_OK;
    uint32_t index = 0U;

    if (!g_i2c.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = hw_bus_find(g_i2c.buses, HW_I2C_MAX_BUSES, bus_name, length, &bus);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    if (address > 0x7FU)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_I2C_MAX_DEVICES; ++index)
    {
        HwI2cDevice* device = &g_i2c.devices[index];
        if (device->owner != 0U && device->bus == bus && device->address == address)
        {
            return HW_STATUS_BUSY;
        }
        if (device->owner == 0U && slot == NULL)
        {
            slot = device;
        }
    }
    if (slot == NULL)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot->bus = bus;
    slot->address = address;
    slot->owner = hw_host_task_id();
    *out_id = (uint32_t)(slot - g_i2c.devices + 1);
    return HW_STATUS_OK;
}

void hw_i2c_close(uint32_t id)
{
    HwI2cDevice* device = hw_i2c_device(id);

    if (device != NULL)
    {
        memset(device, 0, sizeof(*device));
    }
}

static HwStatus hw_i2c_run(HwBus* bus)
{
    return g_i2c.port.transaction(g_i2c.port.context, bus->bus, bus->device, bus->transfer_hz, bus->ops.i2c,
                                  bus->count);
}

HwStatus hw_i2c_transaction(uint32_t id, const HwI2cOp* ops, uint32_t count)
{
    HwI2cDevice* device = hw_i2c_device(id);
    uint32_t index = 0U;

    if (device == NULL || count > HW_BUS_MAX_OPS)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_i2c.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < count; ++index)
    {
        if (ops[index].kind != HW_I2C_WRITE && ops[index].kind != HW_I2C_READ)
        {
            return HW_STATUS_INVALID_ARGUMENT;
        }
    }
    return hw_bus_start(HW_BUS_I2C, device->bus, device->address, device->bus->frequency_hz, 0U, 0U, ops, count,
                        hw_i2c_run, 1U);
}

static void hw_i2c_poll(void)
{
    hw_bus_poll_kind(HW_BUS_I2C);
}

static void hw_i2c_finish_task(uint32_t task_id)
{
    hw_bus_finish_task_kind(HW_BUS_I2C, task_id);
}

void hw_i2c_shutdown(void)
{
    hw_bus_finish_all(HW_BUS_I2C);
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
    hw_bus_free_all(g_i2c.buses, HW_I2C_MAX_BUSES);
    memset(&g_i2c, 0, sizeof(g_i2c));
}

/* ---------------------------------------------------------------------------
 * SPI
 * ------------------------------------------------------------------------ */

typedef struct HwSpiDevice
{
    HwBus* bus;
    uint32_t mode;
    uint32_t frequency_hz;
    /* The kernel's GPIO id of chip select, 0 when the program drives it. */
    uint32_t cs;
    uint32_t cs_pin;
    /* Owning task id, 0 when the slot is free. */
    uint32_t owner;
} HwSpiDevice;

static struct
{
    HwSpiPort port;
    uint8_t has_port;
    HwBus* buses[HW_SPI_MAX_BUSES];
    HwSpiDevice devices[HW_SPI_MAX_DEVICES];
} g_spi;

static HwSpiDevice* hw_spi_device(uint32_t id)
{
    return id >= 1U && id <= HW_SPI_MAX_DEVICES && g_spi.devices[id - 1U].owner != 0U ? &g_spi.devices[id - 1U] : NULL;
}

HwStatus hw_spi_port_set(const HwSpiPort* port)
{
    if (port == NULL)
    {
        memset(&g_spi.port, 0, sizeof(g_spi.port));
        g_spi.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->transaction == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_spi.port = *port;
    g_spi.has_port = 1U;
    return HW_STATUS_OK;
}

extern const HwDriver hw_spi_driver;

HwStatus hw_spi_add_bus(const char* name, uint32_t bus, uint32_t max_hz)
{
    HwStatus status = HW_STATUS_OK;

    if (max_hz == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    status = hw_bus_add(g_spi.buses, HW_SPI_MAX_BUSES, name, bus, max_hz);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    /* A platform with these devices gives programs the spi interface. */
    if (hw_driver_add(&hw_spi_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

uint32_t hw_spi_get_owner(uint32_t bus, uint32_t cs_pin)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_SPI_MAX_DEVICES; ++index)
    {
        const HwSpiDevice* device = &g_spi.devices[index];
        if (device->owner != 0U && device->bus->bus == bus && device->cs_pin == cs_pin)
        {
            return device->owner;
        }
    }
    return 0U;
}

HwStatus hw_spi_open(const char* bus_name, uint32_t bus_length, const char* cs, uint32_t cs_length,
                     uint32_t mode, uint32_t frequency_hz, uint32_t* out_id)
{
    HwBus* bus = NULL;
    HwSpiDevice* slot = NULL;
    HwStatus status = HW_STATUS_OK;
    uint32_t index = 0U;

    if (!g_spi.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = hw_bus_find(g_spi.buses, HW_SPI_MAX_BUSES, bus_name, bus_length, &bus);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    if ((mode & ~(HW_SPI_CPHA | HW_SPI_CPOL | HW_SPI_LSB_FIRST)) != 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    /* A program that drives chip select itself needs the bus to itself. */
    for (index = 0U; index < HW_SPI_MAX_DEVICES; ++index)
    {
        HwSpiDevice* device = &g_spi.devices[index];
        if (device->owner != 0U && device->bus == bus && (device->cs == 0U || cs_length == 0U))
        {
            return HW_STATUS_BUSY;
        }
        if (device->owner == 0U && slot == NULL)
        {
            slot = device;
        }
    }
    if (slot == NULL)
    {
        return HW_STATUS_LIMIT_REACHED;
    }

    memset(slot, 0, sizeof(*slot));
    slot->cs_pin = HW_BUS_NO_CS;
    if (cs_length > 0U)
    {
        status = hw_gpio_open_physical(cs, cs_length, &slot->cs);
        if (status != HW_STATUS_OK)
        {
            slot->cs = 0U;
            return status;
        }
        slot->cs_pin = hw_gpio_pin_number(slot->cs);
    }
    slot->bus = bus;
    slot->mode = mode;
    slot->frequency_hz = frequency_hz == 0U || frequency_hz > bus->frequency_hz ? bus->frequency_hz : frequency_hz;
    slot->owner = hw_host_task_id();
    *out_id = (uint32_t)(slot - g_spi.devices + 1);
    return HW_STATUS_OK;
}

void hw_spi_close(uint32_t id)
{
    HwSpiDevice* device = hw_spi_device(id);

    if (device == NULL)
    {
        return;
    }
    if (device->cs != 0U)
    {
        hw_gpio_close(device->cs);
    }
    memset(device, 0, sizeof(*device));
}

static HwStatus hw_spi_run(HwBus* bus)
{
    return g_spi.port.transaction(g_spi.port.context, bus->bus, bus->mode, bus->transfer_hz, bus->ops.spi,
                                  bus->count);
}

HwStatus hw_spi_transaction(uint32_t id, const HwSpiOp* ops, uint32_t count)
{
    HwSpiDevice* device = hw_spi_device(id);
    uint32_t index = 0U;

    if (device == NULL || count == 0U || count > HW_BUS_MAX_OPS)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_spi.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < count; ++index)
    {
        const HwSpiOp* op = &ops[index];
        if (op->kind < HW_SPI_WRITE || op->kind > HW_SPI_DELAY_US ||
            (op->kind == HW_SPI_DELAY_US && op->length > HW_SPI_MAX_DELAY_US))
        {
            return HW_STATUS_INVALID_ARGUMENT;
        }
    }

    return hw_bus_start(HW_BUS_SPI, device->bus, device->cs_pin, device->frequency_hz, device->mode, device->cs, ops,
                        count, hw_spi_run, 1U);
}

static void hw_spi_poll(void)
{
    hw_bus_poll_kind(HW_BUS_SPI);
}

static void hw_spi_finish_task(uint32_t task_id)
{
    hw_bus_finish_task_kind(HW_BUS_SPI, task_id);
}

void hw_spi_shutdown(void)
{
    hw_bus_finish_all(HW_BUS_SPI);
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
    hw_bus_free_all(g_spi.buses, HW_SPI_MAX_BUSES);
    memset(&g_spi, 0, sizeof(g_spi));
}

/* ---------------------------------------------------------------------------
 * 1-Wire
 * ------------------------------------------------------------------------ */

#define HW_ONEWIRE_MATCH_ROM 0x55U
#define HW_ONEWIRE_SEARCH_ROM 0xF0U
#define HW_ONEWIRE_ALARM_SEARCH 0xECU

typedef struct HwOneWireDevice
{
    HwBus* bus;
    /* HW_ONEWIRE_WHOLE_BUS: the program holds the bus. */
    uint64_t rom;
    /* Owning task id, 0 when the slot is free. */
    uint32_t owner;
} HwOneWireDevice;

static struct
{
    HwOneWirePort port;
    uint8_t has_port;
    HwBus* buses[HW_ONEWIRE_MAX_BUSES];
    HwOneWireDevice devices[HW_ONEWIRE_MAX_DEVICES];
} g_onewire;

static HwOneWireDevice* hw_onewire_device(uint32_t id)
{
    return id >= 1U && id <= HW_ONEWIRE_MAX_DEVICES && g_onewire.devices[id - 1U].owner != 0U
               ? &g_onewire.devices[id - 1U]
               : NULL;
}

uint8_t hw_onewire_crc8(const uint8_t* data, uint32_t length)
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
static void hw_onewire_rom_bytes(uint64_t rom, uint8_t bytes[8])
{
    uint32_t index = 0U;

    for (index = 0U; index < 8U; ++index)
    {
        bytes[index] = (uint8_t)(rom >> (index * 8U));
    }
}

HwStatus hw_onewire_port_set(const HwOneWirePort* port)
{
    if (port == NULL)
    {
        memset(&g_onewire.port, 0, sizeof(g_onewire.port));
        g_onewire.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->transaction == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_onewire.port = *port;
    g_onewire.has_port = 1U;
    return HW_STATUS_OK;
}

extern const HwDriver hw_onewire_driver;

HwStatus hw_onewire_add_bus(const char* name, uint32_t bus)
{
    /* The speed is the standard one; the field only matters to the other kinds. */
    HwStatus status = hw_bus_add(g_onewire.buses, HW_ONEWIRE_MAX_BUSES, name, bus, 0U);

    if (status != HW_STATUS_OK)
    {
        return status;
    }
    /* A platform with these devices gives programs the onewire interface. */
    if (hw_driver_add(&hw_onewire_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

uint32_t hw_onewire_get_owner(uint32_t bus, uint64_t rom)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_ONEWIRE_MAX_DEVICES; ++index)
    {
        const HwOneWireDevice* device = &g_onewire.devices[index];
        if (device->owner != 0U && device->bus->bus == bus && device->rom == rom)
        {
            return device->owner;
        }
    }
    return 0U;
}

/* The program holding the whole bus, 0 when none does. */
static uint32_t hw_onewire_holder(const HwBus* bus)
{
    return hw_onewire_get_owner(bus->bus, HW_ONEWIRE_WHOLE_BUS);
}

HwStatus hw_onewire_open(const char* bus_name, uint32_t length, uint64_t rom, uint32_t* out_id)
{
    HwBus* bus = NULL;
    HwOneWireDevice* slot = NULL;
    HwStatus status = HW_STATUS_OK;
    uint8_t bytes[8];
    uint32_t index = 0U;

    if (!g_onewire.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = hw_bus_find(g_onewire.buses, HW_ONEWIRE_MAX_BUSES, bus_name, length, &bus);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    hw_onewire_rom_bytes(rom, bytes);
    if (rom != HW_ONEWIRE_WHOLE_BUS && hw_onewire_crc8(bytes, 7U) != bytes[7])
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    /* The whole bus excludes every device on it, and the other way round. */
    for (index = 0U; index < HW_ONEWIRE_MAX_DEVICES; ++index)
    {
        HwOneWireDevice* device = &g_onewire.devices[index];
        if (device->owner != 0U && device->bus == bus &&
            (device->rom == rom || device->rom == HW_ONEWIRE_WHOLE_BUS || rom == HW_ONEWIRE_WHOLE_BUS))
        {
            return HW_STATUS_BUSY;
        }
        if (device->owner == 0U && slot == NULL)
        {
            slot = device;
        }
    }
    if (slot == NULL)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot->bus = bus;
    slot->rom = rom;
    slot->owner = hw_host_task_id();
    *out_id = (uint32_t)(slot - g_onewire.devices + 1);
    return HW_STATUS_OK;
}

static HwStatus hw_onewire_run(HwBus* bus)
{
    return g_onewire.port.transaction(g_onewire.port.context, bus->bus, bus->ops.onewire, bus->count);
}

/* Ops on a bus, done when this returns (a search, a pull-up to end). */
static HwStatus hw_onewire_now(HwBus* bus, const HwOneWireOp* ops, uint32_t count)
{
    bus->powered = 0U;
    return hw_bus_start(HW_BUS_ONEWIRE, bus, 0U, 0U, 0U, 0U, ops, count, hw_onewire_run, 0U);
}

void hw_onewire_close(uint32_t id)
{
    HwOneWireDevice* device = hw_onewire_device(id);
    HwBus* bus = NULL;

    if (device == NULL)
    {
        return;
    }
    bus = device->bus;
    memset(device, 0, sizeof(*device));
    /* A pull-up the program left on ends with it. */
    if (bus->powered && g_onewire.has_port)
    {
        (void)hw_onewire_now(bus, NULL, 0U);
    }
}

HwStatus hw_onewire_transaction(uint32_t id, const HwOneWireOp* ops, uint32_t count)
{
    HwOneWireDevice* device = hw_onewire_device(id);
    HwOneWireOp all[HW_BUS_MAX_OPS + 2U];
    uint32_t first = 0U;
    uint32_t index = 0U;
    HwStatus status = HW_STATUS_OK;
    HwBus* bus = NULL;

    if (device == NULL || count > HW_BUS_MAX_OPS)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_onewire.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    for (index = 0U; index < count; ++index)
    {
        const HwOneWireOp* op = &ops[index];
        uint8_t after_write = index > 0U && (ops[index - 1U].kind == HW_ONEWIRE_WRITE ||
                                             ops[index - 1U].kind == HW_ONEWIRE_WRITE_BITS);
        if (op->kind < HW_ONEWIRE_WRITE || op->kind > HW_ONEWIRE_RESET ||
            /* A device's transaction starts with the kernel's reset and ROM command. */
            (op->kind == HW_ONEWIRE_RESET && device->rom != HW_ONEWIRE_WHOLE_BUS) ||
            (op->kind == HW_ONEWIRE_POWER &&
             (!after_write || op->length > HW_ONEWIRE_MAX_POWER_MS || (op->length == 0U && index + 1U != count))))
        {
            return HW_STATUS_INVALID_ARGUMENT;
        }
    }

    bus = device->bus;
    /* select is the port's until a background transfer ends. */
    if (bus->busy)
    {
        hw_bus_finish(HW_BUS_ONEWIRE, bus);
    }
    if (device->rom != HW_ONEWIRE_WHOLE_BUS)
    {
        bus->select[0] = HW_ONEWIRE_MATCH_ROM;
        hw_onewire_rom_bytes(device->rom, bus->select + 1U);
        all[0].kind = HW_ONEWIRE_RESET;
        all[0].data = NULL;
        all[0].length = 0U;
        all[1].kind = HW_ONEWIRE_WRITE;
        all[1].data = bus->select;
        all[1].length = sizeof(bus->select);
        first = 2U;
    }
    if (count > 0U)
    {
        memcpy(all + first, ops, count * sizeof(HwOneWireOp));
    }
    bus->powered = count > 0U && ops[count - 1U].kind == HW_ONEWIRE_POWER && ops[count - 1U].length == 0U;
    status = hw_bus_start(HW_BUS_ONEWIRE, bus, 0U, 0U, 0U, 0U, all, first + count, hw_onewire_run, 1U);
    if (status != HW_STATUS_OK && status != HW_STATUS_PENDING)
    {
        bus->powered = 0U;
    }
    return status;
}

/* One step of a search: the bit, its complement and the direction taken. */
static HwStatus hw_onewire_triplet(HwBus* bus, uint8_t direction, uint8_t* out_bits)
{
    HwOneWireOp op;
    uint8_t bits = 0U;
    HwStatus status = HW_STATUS_OK;

    if (g_onewire.port.triplet != NULL)
    {
        return g_onewire.port.triplet(g_onewire.port.context, bus->bus, direction, out_bits);
    }
    op.kind = HW_ONEWIRE_READ_BITS;
    op.data = &bits;
    op.length = 2U;
    status = hw_onewire_now(bus, &op, 1U);
    if (status != HW_STATUS_OK)
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
        op.kind = HW_ONEWIRE_WRITE_BITS;
        op.data = &direction;
        op.length = 1U;
        status = hw_onewire_now(bus, &op, 1U);
    }
    *out_bits = (uint8_t)(bits | (direction << 2));
    return status;
}

HwStatus hw_onewire_search(const char* bus_name, uint32_t length, HwOneWireSearch* state, uint32_t flags,
                           uint8_t* out_found)
{
    static const uint8_t commands[2] = { HW_ONEWIRE_SEARCH_ROM, HW_ONEWIRE_ALARM_SEARCH };
    HwBus* bus = NULL;
    HwOneWireOp ops[2];
    uint8_t rom[8];
    uint32_t holder = 0U;
    uint32_t bit = 0U;
    uint32_t last_zero = 0U;
    HwStatus status = HW_STATUS_OK;

    *out_found = 0U;
    if (!g_onewire.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    status = hw_bus_find(g_onewire.buses, HW_ONEWIRE_MAX_BUSES, bus_name, length, &bus);
    if (status != HW_STATUS_OK)
    {
        return status;
    }
    if ((flags & ~HW_ONEWIRE_SEARCH_ALARM) != 0U || state->discrepancy > 64U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    /* A program holding the bus is in the middle of its own commands. */
    holder = hw_onewire_holder(bus);
    if (holder != 0U && holder != hw_host_task_id())
    {
        return HW_STATUS_BUSY;
    }
    if (state->done)
    {
        memset(state, 0, sizeof(*state));
        return HW_STATUS_OK;
    }

    ops[0].kind = HW_ONEWIRE_RESET;
    ops[0].data = NULL;
    ops[0].length = 0U;
    ops[1].kind = HW_ONEWIRE_WRITE;
    ops[1].data = (uint8_t*)&commands[(flags & HW_ONEWIRE_SEARCH_ALARM) != 0U ? 1 : 0];
    ops[1].length = 1U;
    status = hw_onewire_now(bus, ops, 2U);
    if (status == HW_STATUS_NACK)
    {
        /* No device on the bus. */
        memset(state, 0, sizeof(*state));
        return HW_STATUS_OK;
    }
    if (status != HW_STATUS_OK)
    {
        return status;
    }

    hw_onewire_rom_bytes(state->rom, rom);
    for (bit = 1U; bit <= 64U; ++bit)
    {
        uint8_t mask = (uint8_t)(1U << ((bit - 1U) % 8U));
        uint8_t* byte = &rom[(bit - 1U) / 8U];
        /* Up to the last branch, the way the last ROM went; at it, the other way; past it, 0 first. */
        uint8_t direction = bit < state->discrepancy ? (uint8_t)((*byte & mask) != 0U) : (uint8_t)(bit == state->discrepancy);
        uint8_t bits = 0U;

        status = hw_onewire_triplet(bus, direction, &bits);
        if (status != HW_STATUS_OK)
        {
            return status;
        }
        if ((bits & 3U) == 3U)
        {
            /* Every device fell silent: they left the bus. */
            memset(state, 0, sizeof(*state));
            return HW_STATUS_OK;
        }
        if ((bits & 3U) == 0U && (bits & 4U) == 0U)
        {
            last_zero = bit;
        }
        *byte = (uint8_t)((bits & 4U) != 0U ? *byte | mask : *byte & ~mask);
    }
    /* Noise on the line shows as a code that fails its CRC (or the family 0 no device has). */
    if (hw_onewire_crc8(rom, 7U) != rom[7] || rom[0] == 0U)
    {
        return HW_STATUS_BUS_ERROR;
    }
    state->rom = 0U;
    for (bit = 0U; bit < 8U; ++bit)
    {
        state->rom |= (uint64_t)rom[bit] << (bit * 8U);
    }
    state->discrepancy = last_zero;
    state->done = last_zero == 0U;
    *out_found = 1U;
    return HW_STATUS_OK;
}

static void hw_onewire_poll(void)
{
    hw_bus_poll_kind(HW_BUS_ONEWIRE);
}

static void hw_onewire_finish_task(uint32_t task_id)
{
    hw_bus_finish_task_kind(HW_BUS_ONEWIRE, task_id);
}

void hw_onewire_shutdown(void)
{
    hw_bus_finish_all(HW_BUS_ONEWIRE);
    memset(&g_bus_trace, 0, sizeof(g_bus_trace));
    hw_bus_free_all(g_onewire.buses, HW_ONEWIRE_MAX_BUSES);
    memset(&g_onewire, 0, sizeof(g_onewire));
}

static HwBus* hw_bus_by_number(uint8_t kind, uint32_t number)
{
    HwBus** buses = NULL;
    uint32_t max = 0U;
    uint32_t index = 0U;

    if (kind == HW_BUS_I2C)
    {
        buses = g_i2c.buses;
        max = HW_I2C_MAX_BUSES;
    }
    if (kind == HW_BUS_SPI)
    {
        buses = g_spi.buses;
        max = HW_SPI_MAX_BUSES;
    }
    if (kind == HW_BUS_ONEWIRE)
    {
        buses = g_onewire.buses;
        max = HW_ONEWIRE_MAX_BUSES;
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

static void (*hw_bus_port_finish(uint8_t kind))(void*, uint32_t)
{
    if (kind == HW_BUS_I2C)
    {
        return g_i2c.port.finish;
    }
    if (kind == HW_BUS_SPI)
    {
        return g_spi.port.finish;
    }
    if (kind == HW_BUS_ONEWIRE)
    {
        return g_onewire.port.finish;
    }
    return NULL;
}

static void* hw_bus_port_context(uint8_t kind)
{
    if (kind == HW_BUS_I2C)
    {
        return g_i2c.port.context;
    }
    if (kind == HW_BUS_SPI)
    {
        return g_spi.port.context;
    }
    if (kind == HW_BUS_ONEWIRE)
    {
        return g_onewire.port.context;
    }
    return NULL;
}

/* The buses of a kind, for the loops below. */
static void hw_bus_each(uint8_t kind, void (*visit)(uint8_t kind, HwBus* bus, uint32_t task_id), uint32_t task_id)
{
    HwBus** buses = NULL;
    uint32_t max = 0U;
    uint32_t index = 0U;

    if (kind == HW_BUS_I2C)
    {
        buses = g_i2c.buses;
        max = HW_I2C_MAX_BUSES;
    }
    if (kind == HW_BUS_SPI)
    {
        buses = g_spi.buses;
        max = HW_SPI_MAX_BUSES;
    }
    if (kind == HW_BUS_ONEWIRE)
    {
        buses = g_onewire.buses;
        max = HW_ONEWIRE_MAX_BUSES;
    }
    for (index = 0U; index < max; ++index)
    {
        if (buses[index] != NULL)
        {
            visit(kind, buses[index], task_id);
        }
    }
}

static void hw_bus_poll_one(uint8_t kind, HwBus* bus, uint32_t task_id)
{
    (void)task_id;
    if (bus->busy && HW_BUS_FLAG_TAKE(bus->done))
    {
        hw_bus_deliver(kind, bus);
    }
}

static void hw_bus_finish_one(uint8_t kind, HwBus* bus, uint32_t task_id)
{
    if (bus->busy && (task_id == 0U || bus->task_id == task_id))
    {
        hw_bus_finish(kind, bus);
    }
}

/* Transfers of a kind that ended in the background reach their programs. */
static void hw_bus_poll_kind(uint8_t kind)
{
    hw_bus_each(kind, hw_bus_poll_one, 0U);
}

/* End the task's transfers on buses of a kind now. */
static void hw_bus_finish_task_kind(uint8_t kind, uint32_t task_id)
{
    if (task_id != 0U)
    {
        hw_bus_each(kind, hw_bus_finish_one, task_id);
    }
}

/* Ports must be done with every buffer of a kind before the kernel lets go. */
static void hw_bus_finish_all(uint8_t kind)
{
    hw_bus_each(kind, hw_bus_finish_one, 0U);
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: the devices tasks hold on the buses, each in
 * its slot, on its bus by name. Buses keep nothing between transfers, so
 * nothing else is set up again; a transfer on its way waits for the next
 * slice.
 * ------------------------------------------------------------------------ */

/* Whether a transfer is on its way on one of the buses; the save fails then. */
static uint8_t hw_bus_save_busy(HwSystemStream* stream, HwBus** buses, uint32_t max)
{
    uint32_t index = 0U;

    for (index = 0U; index < max; ++index)
    {
        if (buses[index] != NULL && buses[index]->busy)
        {
            hw_host_system_refuse(stream, HW_STATUS_BUSY, "a bus transfer is on its way: try after the next slice");
            return 1U;
        }
    }
    return 0U;
}

/* The bus a saved device was on, by name; NULL when the platform has it no more. */
static HwBus* hw_bus_restore_find(HwSystemStream* stream, HwBus** buses, uint32_t max)
{
    char name[HW_BUS_NAME_MAX];
    uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
    HwBus* bus = NULL;

    if (!hw_host_system_ok(stream) || hw_bus_find(buses, max, name, length, &bus) != HW_STATUS_OK)
    {
        return NULL;
    }
    return bus;
}

static void hw_i2c_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    if (hw_bus_save_busy(stream, g_i2c.buses, HW_I2C_MAX_BUSES))
    {
        return;
    }
    for (index = 0U; index < HW_I2C_MAX_DEVICES; ++index)
    {
        count += g_i2c.devices[index].owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_I2C_MAX_DEVICES; ++index)
    {
        const HwI2cDevice* device = &g_i2c.devices[index];
        if (device->owner != 0U)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put32(stream, device->owner);
            hw_host_system_put_bytes(stream, device->bus->name, device->bus->name_length);
            hw_host_system_put32(stream, device->address);
        }
    }
}

static void hw_i2c_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_I2C_MAX_DEVICES);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        uint32_t id = hw_host_system_get_max(stream, HW_I2C_MAX_DEVICES);
        uint32_t owner = hw_host_system_get32(stream);
        HwBus* bus = hw_bus_restore_find(stream, g_i2c.buses, HW_I2C_MAX_BUSES);
        uint32_t address = hw_host_system_get_max(stream, 0x7FU);

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        if (id == 0U || owner == 0U || g_i2c.devices[id - 1U].owner != 0U)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        if (bus == NULL || !g_i2c.has_port)
        {
            hw_host_system_device(stream, &hw_i2c_driver, id, 0U, NULL);
            continue;
        }
        g_i2c.devices[id - 1U].bus = bus;
        g_i2c.devices[id - 1U].address = address;
        g_i2c.devices[id - 1U].owner = owner;
        hw_host_system_device(stream, &hw_i2c_driver, id, id, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The i2c interface for programs
 * ------------------------------------------------------------------------ */

static HW_HOST_FUNCTION(hw_i2c_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, bus_address);
    HW_HOST_ARG(uint32_t, bus_length);
    HW_HOST_ARG(uint32_t, address);
    HW_HOST_ARG(uint32_t, flags);
    const char* bus = NULL;
    char bus_storage[HW_ABI_NAME_MAX];
    uint32_t id = 0U;
    int32_t handle = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    handle = hw_abi_read_name(bus_address, bus_length, bus_storage, &bus);
    if (handle < 0)
    {
        HW_HOST_RETURN(handle);
    }
    if (bus == NULL || flags != 0U)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    status = hw_i2c_open(bus, bus_length, address, &id);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status));
    }
    handle = hw_driver_handle_new(&hw_i2c_driver, id);
    if (handle < 0)
    {
        hw_i2c_close(id);
    }
    HW_HOST_RETURN(handle);
}

/* ops: count records of 12 bytes, u32 kind, u32 data, u32 length. */
static HW_HOST_FUNCTION(hw_i2c_call_transaction)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, ops_address);
    HW_HOST_ARG(uint32_t, count);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_i2c_driver, handle, &id);
    HwI2cOp ops[HW_BUS_MAX_OPS];
    HwBusBuffers buffers = { { { 0 } }, 0U };
    uint32_t index = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (count > HW_BUS_MAX_OPS)
    {
        HW_HOST_RETURN(-WR_ELIMIT);
    }
    if (count > 0U && hw_host_guest_check(ops_address, count * 12U) != HW_STATUS_OK)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        uint8_t record[12];
        uint32_t kind = 0U;
        int32_t result = 0;

        (void)hw_host_guest_read(ops_address + index * 12U, record, sizeof(record));
        kind = HW_HOST_READ32(record);
        if (kind != HW_I2C_WRITE && kind != HW_I2C_READ)
        {
            hw_bus_buffers_drop(&buffers);
            HW_HOST_RETURN(-WR_EINVAL);
        }
        result = hw_bus_guest_data(&buffers, HW_HOST_READ32(record + 4U), HW_HOST_READ32(record + 8U),
                                   kind == HW_I2C_WRITE ? HW_GUEST_IN : HW_GUEST_OUT, &ops[index].data);
        if (result < 0)
        {
            hw_bus_buffers_drop(&buffers);
            HW_HOST_RETURN(result);
        }
        ops[index].kind = (uint8_t)kind;
        ops[index].length = HW_HOST_READ32(record + 8U);
    }
    /* A port working in the background leaves the program waiting; the end writes the result. */
    if (hw_host_guest_begin((uint32_t*)raw_return) != HW_STATUS_OK)
    {
        hw_bus_buffers_drop(&buffers);
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_i2c_transaction(id, ops, count);
    hw_bus_buffers_end(HW_BUS_I2C, &buffers, status);
    *raw_return = hw_abi_error(status);
    return hw_host_guest_end();
}

static const HwDriverCall hw_i2c_calls[] =
{
    { "open", "i(iiii)", hw_i2c_call_open, 3U },
    { "transaction", "i(iii)", hw_i2c_call_transaction, 3U },
};

const HwDriver hw_i2c_driver =
{
    .interface = "i2c",
    .calls = hw_i2c_calls,
    .call_count = (uint32_t)(sizeof(hw_i2c_calls) / sizeof(hw_i2c_calls[0])),
    .close = hw_i2c_close,
    .poll = hw_i2c_poll,
    .finish_task = hw_i2c_finish_task,
    .shutdown = hw_i2c_shutdown,
    .save = hw_i2c_save,
    .restore = hw_i2c_restore,
};

static void hw_spi_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    if (hw_bus_save_busy(stream, g_spi.buses, HW_SPI_MAX_BUSES))
    {
        return;
    }
    for (index = 0U; index < HW_SPI_MAX_DEVICES; ++index)
    {
        count += g_spi.devices[index].owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_SPI_MAX_DEVICES; ++index)
    {
        const HwSpiDevice* device = &g_spi.devices[index];
        if (device->owner != 0U)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put32(stream, device->owner);
            hw_host_system_put_bytes(stream, device->bus->name, device->bus->name_length);
            hw_host_system_put32(stream, device->mode);
            hw_host_system_put32(stream, device->frequency_hz);
            hw_host_system_put32(stream, device->cs);
        }
    }
}

static void hw_spi_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_SPI_MAX_DEVICES);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        uint32_t id = hw_host_system_get_max(stream, HW_SPI_MAX_DEVICES);
        uint32_t owner = hw_host_system_get32(stream);
        HwBus* bus = hw_bus_restore_find(stream, g_spi.buses, HW_SPI_MAX_BUSES);
        uint32_t mode = hw_host_system_get32(stream);
        uint32_t frequency_hz = hw_host_system_get32(stream);
        uint32_t old_cs = hw_host_system_get32(stream);
        const char* gone = NULL;
        uint32_t cs = 0U;
        HwSpiDevice* device = NULL;

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        if (id == 0U || owner == 0U || g_spi.devices[id - 1U].owner != 0U)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        /* Its chip select, a pin gpio restored first. */
        if (old_cs != 0U)
        {
            cs = hw_host_system_device_id(stream, &hw_gpio_driver, old_cs, &gone);
        }
        if (bus == NULL || !g_spi.has_port || (old_cs != 0U && cs == 0U))
        {
            if (cs != 0U)
            {
                hw_gpio_close(cs);
            }
            hw_host_system_device(stream, &hw_spi_driver, id, 0U, gone);
            continue;
        }
        device = &g_spi.devices[id - 1U];
        device->bus = bus;
        device->mode = mode;
        device->frequency_hz = frequency_hz;
        device->cs = cs;
        device->cs_pin = cs != 0U ? hw_gpio_pin_number(cs) : HW_BUS_NO_CS;
        device->owner = owner;
        hw_host_system_device(stream, &hw_spi_driver, id, id, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The spi interface for programs
 * ------------------------------------------------------------------------ */

static HW_HOST_FUNCTION(hw_spi_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, bus_address);
    HW_HOST_ARG(uint32_t, bus_length);
    HW_HOST_ARG(uint32_t, cs_address);
    HW_HOST_ARG(uint32_t, cs_length);
    HW_HOST_ARG(uint32_t, mode);
    HW_HOST_ARG(uint32_t, frequency_hz);
    const char* bus = NULL;
    char bus_storage[HW_ABI_NAME_MAX];
    const char* cs = NULL;
    char cs_storage[HW_ABI_NAME_MAX];
    uint32_t id = 0U;
    int32_t handle = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    handle = hw_abi_read_name(bus_address, bus_length, bus_storage, &bus);
    if (handle >= 0)
    {
        handle = hw_abi_read_name(cs_address, cs_length, cs_storage, &cs);
    }
    if (handle < 0)
    {
        HW_HOST_RETURN(handle);
    }
    if (bus == NULL)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    status = hw_spi_open(bus, bus_length, cs, cs_length, mode, frequency_hz, &id);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status));
    }
    handle = hw_driver_handle_new(&hw_spi_driver, id);
    if (handle < 0)
    {
        hw_spi_close(id);
    }
    HW_HOST_RETURN(handle);
}

/* ops: count records of 16 bytes, u32 kind, u32 write, u32 read, u32 length. */
static HW_HOST_FUNCTION(hw_spi_call_transaction)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, ops_address);
    HW_HOST_ARG(uint32_t, count);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_spi_driver, handle, &id);
    HwSpiOp ops[HW_BUS_MAX_OPS];
    HwBusBuffers buffers = { { { 0 } }, 0U };
    uint32_t index = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (count > HW_BUS_MAX_OPS)
    {
        HW_HOST_RETURN(-WR_ELIMIT);
    }
    if (count == 0U)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    if (hw_host_guest_check(ops_address, count * 16U) != HW_STATUS_OK)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        uint8_t record[16];
        uint32_t kind = 0U;
        uint32_t length = 0U;
        uint8_t* write = NULL;
        uint8_t* read = NULL;
        int32_t result = 0;

        (void)hw_host_guest_read(ops_address + index * 16U, record, sizeof(record));
        kind = HW_HOST_READ32(record);
        length = HW_HOST_READ32(record + 12U);
        if (kind < HW_SPI_WRITE || kind > HW_SPI_DELAY_US)
        {
            hw_bus_buffers_drop(&buffers);
            HW_HOST_RETURN(-WR_EINVAL);
        }
        if (kind == HW_SPI_WRITE || kind == HW_SPI_TRANSFER)
        {
            result = hw_bus_guest_data(&buffers, HW_HOST_READ32(record + 4U), length, HW_GUEST_IN, &write);
        }
        if (result == 0 && (kind == HW_SPI_READ || kind == HW_SPI_TRANSFER))
        {
            result = hw_bus_guest_data(&buffers, HW_HOST_READ32(record + 8U), length, HW_GUEST_OUT, &read);
        }
        if (result < 0)
        {
            hw_bus_buffers_drop(&buffers);
            HW_HOST_RETURN(result);
        }
        ops[index].kind = (uint8_t)kind;
        ops[index].write = write;
        ops[index].read = read;
        ops[index].length = length;
    }
    if (hw_host_guest_begin((uint32_t*)raw_return) != HW_STATUS_OK)
    {
        hw_bus_buffers_drop(&buffers);
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_spi_transaction(id, ops, count);
    hw_bus_buffers_end(HW_BUS_SPI, &buffers, status);
    *raw_return = hw_abi_error(status);
    return hw_host_guest_end();
}

static const HwDriverCall hw_spi_calls[] =
{
    { "open", "i(iiiiii)", hw_spi_call_open, 3U },
    { "transaction", "i(iii)", hw_spi_call_transaction, 3U },
};

const HwDriver hw_spi_driver =
{
    .interface = "spi",
    .calls = hw_spi_calls,
    .call_count = (uint32_t)(sizeof(hw_spi_calls) / sizeof(hw_spi_calls[0])),
    .close = hw_spi_close,
    .poll = hw_spi_poll,
    .finish_task = hw_spi_finish_task,
    .shutdown = hw_spi_shutdown,
    .save = hw_spi_save,
    .restore = hw_spi_restore,
};

static void hw_onewire_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    if (hw_bus_save_busy(stream, g_onewire.buses, HW_ONEWIRE_MAX_BUSES))
    {
        return;
    }
    for (index = 0U; index < HW_ONEWIRE_MAX_DEVICES; ++index)
    {
        count += g_onewire.devices[index].owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_ONEWIRE_MAX_DEVICES; ++index)
    {
        const HwOneWireDevice* device = &g_onewire.devices[index];
        if (device->owner != 0U)
        {
            hw_host_system_put32(stream, index + 1U);
            hw_host_system_put32(stream, device->owner);
            hw_host_system_put_bytes(stream, device->bus->name, device->bus->name_length);
            hw_host_system_put64(stream, device->rom);
        }
    }
}

/* A strong pull-up left on is off after the sleep: what it powered lost its power too. */
static void hw_onewire_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_ONEWIRE_MAX_DEVICES);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        uint32_t id = hw_host_system_get_max(stream, HW_ONEWIRE_MAX_DEVICES);
        uint32_t owner = hw_host_system_get32(stream);
        HwBus* bus = hw_bus_restore_find(stream, g_onewire.buses, HW_ONEWIRE_MAX_BUSES);
        uint64_t rom = hw_host_system_get64(stream);

        if (!hw_host_system_ok(stream))
        {
            return;
        }
        if (id == 0U || owner == 0U || g_onewire.devices[id - 1U].owner != 0U)
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        if (bus == NULL || !g_onewire.has_port)
        {
            hw_host_system_device(stream, &hw_onewire_driver, id, 0U, NULL);
            continue;
        }
        g_onewire.devices[id - 1U].bus = bus;
        g_onewire.devices[id - 1U].rom = rom;
        g_onewire.devices[id - 1U].owner = owner;
        hw_host_system_device(stream, &hw_onewire_driver, id, id, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The onewire interface for programs
 * ------------------------------------------------------------------------ */

static HW_HOST_FUNCTION(hw_onewire_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, bus_address);
    HW_HOST_ARG(uint32_t, bus_length);
    HW_HOST_ARG(uint64_t, rom);
    HW_HOST_ARG(uint32_t, flags);
    const char* bus = NULL;
    char bus_storage[HW_ABI_NAME_MAX];
    uint32_t id = 0U;
    int32_t handle = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    handle = hw_abi_read_name(bus_address, bus_length, bus_storage, &bus);
    if (handle < 0)
    {
        HW_HOST_RETURN(handle);
    }
    if (bus == NULL || flags != 0U)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    status = hw_onewire_open(bus, bus_length, rom, &id);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status));
    }
    handle = hw_driver_handle_new(&hw_onewire_driver, id);
    if (handle < 0)
    {
        hw_onewire_close(id);
    }
    HW_HOST_RETURN(handle);
}

/* ops: count records of 12 bytes, u32 kind, u32 data, u32 length (bits for the _BITS kinds, ms for POWER). */
static HW_HOST_FUNCTION(hw_onewire_call_transaction)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, ops_address);
    HW_HOST_ARG(uint32_t, count);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_onewire_driver, handle, &id);
    HwOneWireOp ops[HW_BUS_MAX_OPS];
    HwBusBuffers buffers = { { { 0 } }, 0U };
    uint32_t index = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (count > HW_BUS_MAX_OPS)
    {
        HW_HOST_RETURN(-WR_ELIMIT);
    }
    if (count > 0U && hw_host_guest_check(ops_address, count * 12U) != HW_STATUS_OK)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    for (index = 0U; index < count; ++index)
    {
        uint8_t record[12];
        uint32_t kind = 0U;
        uint32_t length = 0U;
        uint32_t bytes = 0U;
        int32_t result = 0;

        (void)hw_host_guest_read(ops_address + index * 12U, record, sizeof(record));
        kind = HW_HOST_READ32(record);
        length = HW_HOST_READ32(record + 8U);
        /* Bit ops use whole bytes, a pull-up and a reset none. */
        bytes = kind == HW_ONEWIRE_WRITE || kind == HW_ONEWIRE_READ ? length
                : kind == HW_ONEWIRE_WRITE_BITS || kind == HW_ONEWIRE_READ_BITS ? (length + 7U) / 8U
                                                                                : 0U;
        if (kind < HW_ONEWIRE_WRITE || kind > HW_ONEWIRE_RESET ||
            ((kind == HW_ONEWIRE_WRITE_BITS || kind == HW_ONEWIRE_READ_BITS) && length > 0xFFFFFFF8U))
        {
            hw_bus_buffers_drop(&buffers);
            HW_HOST_RETURN(-WR_EINVAL);
        }
        result = hw_bus_guest_data(&buffers, HW_HOST_READ32(record + 4U), bytes,
                                   kind == HW_ONEWIRE_WRITE || kind == HW_ONEWIRE_WRITE_BITS ? HW_GUEST_IN : HW_GUEST_OUT,
                                   &ops[index].data);
        if (result < 0)
        {
            hw_bus_buffers_drop(&buffers);
            HW_HOST_RETURN(result);
        }
        ops[index].kind = (uint8_t)kind;
        ops[index].length = length;
    }
    /* A port working in the background leaves the program waiting; the end writes the result. */
    if (hw_host_guest_begin((uint32_t*)raw_return) != HW_STATUS_OK)
    {
        hw_bus_buffers_drop(&buffers);
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_onewire_transaction(id, ops, count);
    hw_bus_buffers_end(HW_BUS_ONEWIRE, &buffers, status);
    *raw_return = hw_abi_error(status);
    return hw_host_guest_end();
}

/* state: 16 bytes, u64 rom, u32 discrepancy, u32 done. 1 and the next ROM, or 0 when there are no more. */
static HW_HOST_FUNCTION(hw_onewire_call_search)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, bus_address);
    HW_HOST_ARG(uint32_t, bus_length);
    HW_HOST_ARG(uint32_t, state_address);
    HW_HOST_ARG(uint32_t, flags);
    const char* bus = NULL;
    char bus_storage[HW_ABI_NAME_MAX];
    uint8_t memory[16];
    HwOneWireSearch state;
    uint8_t found = 0U;
    int32_t result = 0;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    result = hw_abi_read_name(bus_address, bus_length, bus_storage, &bus);
    if (result < 0)
    {
        HW_HOST_RETURN(result);
    }
    if (bus == NULL)
    {
        HW_HOST_RETURN(-WR_EINVAL);
    }
    if (hw_host_guest_read(state_address, memory, sizeof(memory)) != HW_STATUS_OK)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    state.rom = HW_HOST_READ64(memory);
    state.discrepancy = HW_HOST_READ32(memory + 8U);
    state.done = HW_HOST_READ32(memory + 12U);
    status = hw_onewire_search(bus, bus_length, &state, flags, &found);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(status == HW_STATUS_BUSY ? -WR_EINUSE : hw_abi_error(status));
    }
    HW_HOST_WRITE64(memory, state.rom);
    HW_HOST_WRITE32(memory + 8U, state.discrepancy);
    HW_HOST_WRITE32(memory + 12U, state.done);
    (void)hw_host_guest_write(state_address, memory, sizeof(memory));
    HW_HOST_RETURN((int32_t)found);
}

static const HwDriverCall hw_onewire_calls[] =
{
    { "open", "i(iiIi)", hw_onewire_call_open, 6U },
    { "transaction", "i(iii)", hw_onewire_call_transaction, 6U },
    { "search", "i(iiii)", hw_onewire_call_search, 6U },
};

const HwDriver hw_onewire_driver =
{
    .interface = "onewire",
    .calls = hw_onewire_calls,
    .call_count = (uint32_t)(sizeof(hw_onewire_calls) / sizeof(hw_onewire_calls[0])),
    .close = hw_onewire_close,
    .poll = hw_onewire_poll,
    .finish_task = hw_onewire_finish_task,
    .shutdown = hw_onewire_shutdown,
    .save = hw_onewire_save,
    .restore = hw_onewire_restore,
};

