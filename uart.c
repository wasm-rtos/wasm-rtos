/*
 * Serial ports: the platform's UART port sends; received bytes arrive in a
 * kernel buffer per port. A port's interrupt handler fills that buffer and
 * signals room to send; the scheduler wakes the programs waiting for either.
 */

#include "driver.h"

#include <stdlib.h>
#include <string.h>

#if (OS_UART_RX_BUFFER & (OS_UART_RX_BUFFER - 1U)) != 0U
#error "OS_UART_RX_BUFFER must be a power of two"
#endif

/* Counters that interrupt handlers bump and the kernel reads. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint32_t OsUartCounter;
#define OS_UART_LOAD(value) (value)
#define OS_UART_STORE(value, new_value) ((value) = (new_value))
#define OS_UART_ADD(value, amount) ((void)((value) += (amount)))
#define OS_UART_TAKE(value) os_uart_take(&(value))
static uint32_t os_uart_take(OsUartCounter* value)
{
    uint32_t taken = *value;
    *value = 0U;
    return taken;
}
#else
#include <stdatomic.h>
typedef atomic_uint_fast32_t OsUartCounter;
#define OS_UART_LOAD(value) ((uint32_t)atomic_load_explicit(&(value), memory_order_acquire))
#define OS_UART_STORE(value, new_value) atomic_store_explicit(&(value), (new_value), memory_order_release)
#define OS_UART_ADD(value, amount) ((void)atomic_fetch_add_explicit(&(value), (amount), memory_order_relaxed))
#define OS_UART_TAKE(value) ((uint32_t)atomic_exchange_explicit(&(value), 0U, memory_order_acq_rel))
#endif

/* Allocated when the platform registers the port. */
typedef struct OsUart
{
    uint32_t uart;
    char name[OS_UART_NAME_MAX + 1U];
    uint8_t name_length;
    uint32_t max_baud;
    uint32_t baud;
    /* Parity, stop bits, data bits and flow control, as the program opened it. */
    uint32_t format;
    /* Owning task id, 0 when free; bytes are kept only while owned. */
    uint32_t owner;
    /*
     * Received bytes: the port's handler writes at head, the kernel reads at
     * tail. Allocated when a program first opens the port and kept until
     * shutdown, so an interrupt handler never writes into freed memory.
     */
    uint8_t* rx;
    OsUartCounter head;
    OsUartCounter tail;
    OsUartCounter errors;
    /* The last write did not fit; the port has signalled room since (from a handler). */
    uint8_t send_blocked;
    OsUartCounter writable;
    OsUartCounter received;
} OsUart;

static struct
{
    OsUartPort port;
    uint8_t has_port;
    OsUart* uarts[OS_UART_MAX];
} g_uart;

static OsUart* os_uart_slot(uint32_t id)
{
    return id >= 1U && id <= OS_UART_MAX ? g_uart.uarts[id - 1U] : NULL;
}

static OsUart* os_uart_by_number(uint32_t number)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_UART_MAX; ++index)
    {
        if (g_uart.uarts[index] != NULL && g_uart.uarts[index]->uart == number)
        {
            return g_uart.uarts[index];
        }
    }
    return NULL;
}

static uint32_t os_uart_available(OsUart* uart)
{
    return OS_UART_LOAD(uart->head) - OS_UART_LOAD(uart->tail);
}

OsStatus os_uart_port_set(const OsUartPort* port)
{
    if (port == NULL)
    {
        memset(&g_uart.port, 0, sizeof(g_uart.port));
        g_uart.has_port = 0U;
        return OS_STATUS_OK;
    }
    if (port->configure == NULL || port->write == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    g_uart.port = *port;
    g_uart.has_port = 1U;
    return OS_STATUS_OK;
}

extern const OsDriver os_uart_driver;

OsStatus os_uart_add(const char* name, uint32_t number, uint32_t max_baud)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    OsUart* slot = NULL;
    uint32_t free_index = OS_UART_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > OS_UART_NAME_MAX)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_UART_MAX; ++index)
    {
        OsUart* entry = g_uart.uarts[index];
        if (entry != NULL && (entry->uart == number ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return OS_STATUS_BUSY;
        }
        if (entry == NULL && free_index == OS_UART_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == OS_UART_MAX)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    slot = (OsUart*)calloc(1U, sizeof(OsUart));
    if (slot == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    g_uart.uarts[free_index] = slot;
    slot->uart = number;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->max_baud = max_baud;
    /* A platform with these devices gives programs the uart interface. */
    if (os_driver_add(&os_uart_driver) != OS_STATUS_OK)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    return OS_STATUS_OK;
}

/* Single producer: only the port's receive path writes head. */
void os_uart_receive(uint32_t number, const uint8_t* data, uint32_t size)
{
    OsUart* uart = os_uart_by_number(number);
    uint32_t head = 0U;
    uint32_t index = 0U;

    if (uart == NULL || uart->owner == 0U || uart->rx == NULL || data == NULL)
    {
        return;
    }
    head = OS_UART_LOAD(uart->head);
    for (index = 0U; index < size; ++index)
    {
        if (head - OS_UART_LOAD(uart->tail) >= OS_UART_RX_BUFFER)
        {
            /* Full: the rest is lost, and counted. */
            OS_UART_ADD(uart->errors, size - index);
            break;
        }
        uart->rx[head % OS_UART_RX_BUFFER] = data[index];
        ++head;
    }
    OS_UART_STORE(uart->head, head);
    OS_UART_STORE(uart->received, 1U);
}

void os_uart_error(uint32_t number)
{
    OsUart* uart = os_uart_by_number(number);
    if (uart != NULL && uart->owner != 0U)
    {
        OS_UART_ADD(uart->errors, 1U);
    }
}

void os_uart_writable(uint32_t number)
{
    OsUart* uart = os_uart_by_number(number);
    if (uart != NULL)
    {
        OS_UART_STORE(uart->writable, 1U);
    }
}

uint32_t os_uart_get_owner(uint32_t number)
{
    OsUart* uart = os_uart_by_number(number);
    return uart != NULL ? uart->owner : 0U;
}

void os_uart_poll(void)
{
    uint8_t received = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < OS_UART_MAX; ++index)
    {
        OsUart* uart = g_uart.uarts[index];
        if (uart == NULL)
        {
            continue;
        }
        if (OS_UART_TAKE(uart->received) != 0U)
        {
            received = 1U;
        }
        if (OS_UART_TAKE(uart->writable) != 0U && uart->send_blocked)
        {
            uart->send_blocked = 0U;
            os_signal_wake(os_driver_signal_key(&os_uart_driver, index + 1U));
        }
    }
    if (received)
    {
        os_wait_any_recheck();
    }
}

OsStatus os_uart_open(const char* name, uint32_t length, uint32_t baud, uint32_t format, uint32_t* out_id)
{
    const uint32_t known = OS_UART_PARITY_EVEN | OS_UART_PARITY_ODD | OS_UART_STOP_2 | OS_UART_DATA_7 |
                           OS_UART_FLOW_RTS_CTS;
    uint8_t any = 0U;
    uint32_t index = 0U;

    if (!g_uart.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (baud == 0U || (format & ~known) != 0U ||
        ((format & OS_UART_PARITY_EVEN) != 0U && (format & OS_UART_PARITY_ODD) != 0U))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < OS_UART_MAX; ++index)
    {
        OsUart* uart = g_uart.uarts[index];
        OsStatus status = OS_STATUS_OK;
        uint32_t actual = 0U;

        any |= uart != NULL;
        if (uart == NULL || uart->name_length != length || memcmp(uart->name, name, length) != 0)
        {
            continue;
        }
        if (uart->owner != 0U)
        {
            return OS_STATUS_BUSY;
        }
        if (uart->max_baud != 0U && baud > uart->max_baud)
        {
            return OS_STATUS_UNSUPPORTED;
        }
        if (uart->rx == NULL)
        {
            uart->rx = (uint8_t*)malloc(OS_UART_RX_BUFFER);
            if (uart->rx == NULL)
            {
                return OS_STATUS_OUT_OF_MEMORY;
            }
        }
        status = g_uart.port.configure(g_uart.port.context, uart->uart, baud, format, &actual);
        if (status != OS_STATUS_OK)
        {
            return status;
        }
        uart->baud = actual != 0U ? actual : baud;
        uart->format = format;
        OS_UART_STORE(uart->tail, OS_UART_LOAD(uart->head));
        (void)OS_UART_TAKE(uart->errors);
        (void)OS_UART_TAKE(uart->writable);
        uart->send_blocked = 0U;
        uart->owner = os_task_get_id(os_task_get_current());
        *out_id = index + 1U;
        return OS_STATUS_OK;
    }
    return any ? OS_STATUS_FILE_NOT_FOUND : OS_STATUS_UNSUPPORTED;
}

void os_uart_close(uint32_t id)
{
    OsUart* uart = os_uart_slot(id);

    if (uart == NULL || uart->owner == 0U)
    {
        return;
    }
    if (g_uart.has_port && g_uart.port.close != NULL)
    {
        g_uart.port.close(g_uart.port.context, uart->uart);
    }
    uart->owner = 0U;
    uart->send_blocked = 0U;
    OS_UART_STORE(uart->tail, OS_UART_LOAD(uart->head));
}

/* Single consumer: only the owner's reads move tail. */
uint32_t os_uart_read(uint32_t id, uint8_t* out, uint32_t max)
{
    OsUart* uart = os_uart_slot(id);
    uint32_t tail = 0U;
    uint32_t count = 0U;

    if (uart == NULL || uart->owner == 0U)
    {
        return 0U;
    }
    tail = OS_UART_LOAD(uart->tail);
    while (count < max && tail != OS_UART_LOAD(uart->head))
    {
        out[count++] = uart->rx[tail % OS_UART_RX_BUFFER];
        ++tail;
    }
    OS_UART_STORE(uart->tail, tail);
    return count;
}

OsStatus os_uart_write(uint32_t id, const uint8_t* data, uint32_t size, uint32_t* out_written)
{
    OsUart* uart = os_uart_slot(id);
    OsStatus status = OS_STATUS_OK;

    *out_written = 0U;
    if (uart == NULL || uart->owner == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!g_uart.has_port)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (size == 0U)
    {
        return OS_STATUS_OK;
    }
    /* Room signalled before this write does not count for it. */
    (void)OS_UART_TAKE(uart->writable);
    status = g_uart.port.write(g_uart.port.context, uart->uart, data, size, out_written);
    if (status == OS_STATUS_OK && *out_written > size)
    {
        *out_written = size;
    }
    uart->send_blocked = status == OS_STATUS_OK && *out_written < size;
    return status;
}

OsStatus os_uart_wait_writable(uint32_t id, uint32_t timeout_ms)
{
    OsUart* uart = os_uart_slot(id);
    OsStatus status = OS_STATUS_OK;

    if (uart == NULL || uart->owner == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    /* Room that came while the program was busy. */
    if (OS_UART_TAKE(uart->writable) != 0U)
    {
        uart->send_blocked = 0U;
    }
    if (!uart->send_blocked)
    {
        return OS_STATUS_OK;
    }
    if (timeout_ms == 0U)
    {
        return OS_STATUS_QUEUE_FULL;
    }
    status = os_signal_wait(os_driver_signal_key(&os_uart_driver, id), timeout_ms);
    return status == OS_STATUS_OK ? OS_STATUS_PENDING : status;
}

OsStatus os_uart_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    OsUart* uart = os_uart_slot(id);

    if (uart == NULL || uart->owner == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case OS_UART_PROP_BAUD: *out_value = uart->baud; return OS_STATUS_OK;
        case OS_UART_PROP_AVAILABLE: *out_value = os_uart_available(uart); return OS_STATUS_OK;
        /* Errors and lost bytes since the last time a program asked. */
        case OS_UART_PROP_ERRORS: *out_value = OS_UART_TAKE(uart->errors); return OS_STATUS_OK;
        default: return OS_STATUS_UNSUPPORTED;
    }
}

uint8_t os_uart_ready(uint32_t id)
{
    OsUart* uart = os_uart_slot(id);
    return uart != NULL && uart->owner != 0U && os_uart_available(uart) > 0U;
}

void os_uart_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < OS_UART_MAX; ++index)
    {
        if (g_uart.uarts[index] != NULL)
        {
            free(g_uart.uarts[index]->rx);
            free(g_uart.uarts[index]);
        }
    }
    memset(&g_uart, 0, sizeof(g_uart));
}

/* ---------------------------------------------------------------------------
 * Saving the whole system: each port a task holds, by name, with its speed
 * and format and the bytes received and not yet read
 * ------------------------------------------------------------------------ */

static void os_uart_save(OsSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < OS_UART_MAX; ++index)
    {
        count += g_uart.uarts[index] != NULL && g_uart.uarts[index]->owner != 0U ? 1U : 0U;
    }
    os_system_put32(stream, count);
    for (index = 0U; index < OS_UART_MAX; ++index)
    {
        OsUart* uart = g_uart.uarts[index];
        uint32_t tail = 0U;
        uint32_t head = 0U;

        if (uart == NULL || uart->owner == 0U)
        {
            continue;
        }
        tail = OS_UART_LOAD(uart->tail);
        head = OS_UART_LOAD(uart->head);
        os_system_put32(stream, index + 1U);
        os_system_put32(stream, uart->owner);
        os_system_put_bytes(stream, uart->name, uart->name_length);
        os_system_put32(stream, uart->baud);
        os_system_put32(stream, uart->format);
        os_system_put32(stream, uart->send_blocked);
        os_system_put32(stream, head - tail);
        for (; tail != head; ++tail)
        {
            os_system_put(stream, &uart->rx[tail % OS_UART_RX_BUFFER], 1U);
        }
    }
}

static void os_uart_restore(OsSystemStream* stream)
{
    uint32_t count = os_system_get_max(stream, OS_UART_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && os_system_ok(stream); ++read)
    {
        char name[OS_UART_NAME_MAX];
        uint32_t old_id = os_system_get32(stream);
        uint32_t owner = os_system_get32(stream);
        uint32_t length = os_system_get_bytes(stream, name, sizeof(name));
        uint32_t baud = os_system_get32(stream);
        uint32_t format = os_system_get32(stream);
        uint8_t send_blocked = (uint8_t)os_system_get_max(stream, 1U);
        uint32_t unread = os_system_get_max(stream, OS_UART_RX_BUFFER);
        OsUart* uart = NULL;
        uint32_t actual = 0U;
        uint32_t id = 0U;
        uint32_t index = 0U;
        OsStatus status = OS_STATUS_OK;

        for (index = 0U; index < OS_UART_MAX && os_system_ok(stream); ++index)
        {
            OsUart* entry = g_uart.uarts[index];
            if (uart == NULL && entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
            {
                uart = entry;
                id = index + 1U;
            }
        }
        if (!os_system_ok(stream))
        {
            return;
        }
        if (uart != NULL && (uart->owner != 0U || owner == 0U))
        {
            os_system_fail(stream, OS_STATUS_INVALID_ARGUMENT);
            return;
        }
        if (uart != NULL && g_uart.has_port && uart->rx == NULL)
        {
            uart->rx = (uint8_t*)malloc(OS_UART_RX_BUFFER);
            if (uart->rx == NULL)
            {
                os_system_fail(stream, OS_STATUS_OUT_OF_MEMORY);
                return;
            }
        }
        status = uart != NULL && g_uart.has_port
            ? g_uart.port.configure(g_uart.port.context, uart->uart, baud, format, &actual)
            : OS_STATUS_FILE_NOT_FOUND;
        if (status == OS_STATUS_OK)
        {
            /* The bytes it had not read, first in its buffer. */
            OS_UART_STORE(uart->tail, 0U);
            os_system_get(stream, uart->rx, unread);
            OS_UART_STORE(uart->head, unread);
            (void)OS_UART_TAKE(uart->errors);
            /* The port is idle after the sleep: a program waiting for room gets it. */
            OS_UART_STORE(uart->writable, 1U);
            uart->send_blocked = send_blocked;
            uart->baud = actual != 0U ? actual : baud;
            uart->format = format;
            uart->owner = owner;
            os_system_device(stream, &os_uart_driver, old_id, id, NULL);
            continue;
        }
        os_system_skip(stream, unread);
        os_system_device(stream, &os_uart_driver, old_id, 0U,
                         uart != NULL && g_uart.has_port ? "a device it held cannot be set up" : NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The uart interface for programs
 * ------------------------------------------------------------------------ */

typedef struct OsAbiUartOpen
{
    uint32_t baud;
    uint32_t format;
} OsAbiUartOpen;

static OsStatus os_uart_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    const OsAbiUartOpen* open = (const OsAbiUartOpen*)context;
    return os_uart_open(name, length, open->baud, open->format, out_id);
}

static OS_HOST_FUNCTION(os_uart_call_open)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(uint32_t, name_address);
    OS_HOST_ARG(uint32_t, name_length);
    OS_HOST_ARG(uint32_t, baud);
    OS_HOST_ARG(uint32_t, format);
    OsAbiUartOpen open = { baud, format };
    OS_ABI_UNUSED();

    OS_HOST_RETURN(os_driver_open(&os_uart_driver, name_address, name_length, os_uart_call_opener, &open));
}

static OS_HOST_FUNCTION(os_uart_call_read)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, buffer_address);
    OS_HOST_ARG(uint32_t, max);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_uart_driver, handle, &id);
    OsGuestBuffer buffer;
    uint32_t count = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (max > 0x7FFFFFFFU)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_guest_buffer_open(&buffer, buffer_address, max, OS_GUEST_OUT);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(OS_GUEST_BUFFER_ERROR(status));
    }
    count = os_uart_read(id, buffer.data, max);
    (void)os_guest_buffer_close(&buffer);
    OS_HOST_RETURN((int32_t)count);
}

static OS_HOST_FUNCTION(os_uart_call_write)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, data_address);
    OS_HOST_ARG(uint32_t, length);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_uart_driver, handle, &id);
    OsGuestBuffer data;
    uint32_t written = 0U;
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (length > 0x7FFFFFFFU)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_guest_buffer_open(&data, data_address, length, OS_GUEST_IN);
    if (status != OS_STATUS_OK)
    {
        OS_HOST_RETURN(OS_GUEST_BUFFER_ERROR(status));
    }
    /* The port takes the bytes during this call. */
    status = os_uart_write(id, data.data, length, &written);
    os_guest_buffer_drop(&data);
    OS_HOST_RETURN(status == OS_STATUS_OK ? (int32_t)written : os_abi_error(status));
}

static OS_HOST_FUNCTION(os_uart_call_wait_writable)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, timeout_ms);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_uart_driver, handle, &id);
    OsStatus status = OS_STATUS_OK;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    if (os_guest_begin((uint32_t*)raw_return) != OS_STATUS_OK)
    {
        OS_HOST_RETURN(-WR_EFAULT);
    }
    status = os_uart_wait_writable(id, timeout_ms);
    *raw_return = os_abi_error(status);
    return os_guest_end();
}

static OS_HOST_FUNCTION(os_uart_call_get)
{
    OS_HOST_RETURNS(int32_t);
    OS_HOST_ARG(int32_t, handle);
    OS_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = os_driver_handle_get(&os_uart_driver, handle, &id);
    uint32_t value = 0U;
    OS_ABI_UNUSED();

    if (!found)
    {
        OS_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        OsStatus status = os_uart_get(id, property, &value);
        OS_HOST_RETURN(os_driver_property(status, value));
    }
}

static const OsDriverCall os_uart_calls[] =
{
    { "open", "i(iiii)", os_uart_call_open, 4U },
    { "read", "i(iii)", os_uart_call_read, 4U },
    { "write", "i(iii)", os_uart_call_write, 4U },
    { "wait-writable", "i(ii)", os_uart_call_wait_writable, 4U },
    { "get", "i(ii)", os_uart_call_get, 4U },
};

const OsDriver os_uart_driver =
{
    .interface = "uart",
    .calls = os_uart_calls,
    .call_count = (uint32_t)(sizeof(os_uart_calls) / sizeof(os_uart_calls[0])),
    .close = os_uart_close,
    .ready = os_uart_ready,
    .poll = os_uart_poll,
    .shutdown = os_uart_shutdown,
    .save = os_uart_save,
    .restore = os_uart_restore,
};

