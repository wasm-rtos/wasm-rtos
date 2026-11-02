/*
 * Serial ports: the platform's UART port sends; received bytes arrive in a
 * kernel buffer per port. A port's interrupt handler fills that buffer and
 * signals room to send; the scheduler wakes the programs waiting for either.
 */

#include "os_internal.h"

#if OS_HAS_UART

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
            os_signal_wake(OS_SIGNAL_KEY(OS_OBJECT_UART, index + 1U));
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
    status = os_signal_wait(OS_SIGNAL_KEY(OS_OBJECT_UART, id), timeout_ms);
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

#endif
