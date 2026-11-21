/*
 * Serial ports: the platform's UART port sends; received bytes arrive in a
 * kernel buffer per port. A port's interrupt handler fills that buffer and
 * signals room to send; the scheduler wakes the programs waiting for either.
 */

#include "hw_driver.h"

#include <stdlib.h>
#include <string.h>

#if (HW_UART_RX_BUFFER & (HW_UART_RX_BUFFER - 1U)) != 0U
#error "HW_UART_RX_BUFFER must be a power of two"
#endif

/* Counters that interrupt handlers bump and the kernel reads. */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint32_t HwUartCounter;
#define HW_UART_LOAD(value) (value)
#define HW_UART_STORE(value, new_value) ((value) = (new_value))
#define HW_UART_ADD(value, amount) ((void)((value) += (amount)))
#define HW_UART_TAKE(value) hw_uart_take(&(value))
static uint32_t hw_uart_take(HwUartCounter* value)
{
    uint32_t taken = *value;
    *value = 0U;
    return taken;
}
#else
#include <stdatomic.h>
typedef atomic_uint_fast32_t HwUartCounter;
#define HW_UART_LOAD(value) ((uint32_t)atomic_load_explicit(&(value), memory_order_acquire))
#define HW_UART_STORE(value, new_value) atomic_store_explicit(&(value), (new_value), memory_order_release)
#define HW_UART_ADD(value, amount) ((void)atomic_fetch_add_explicit(&(value), (amount), memory_order_relaxed))
#define HW_UART_TAKE(value) ((uint32_t)atomic_exchange_explicit(&(value), 0U, memory_order_acq_rel))
#endif

/* Allocated when the platform registers the port. */
typedef struct HwUart
{
    uint32_t uart;
    char name[HW_UART_NAME_MAX + 1U];
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
    HwUartCounter head;
    HwUartCounter tail;
    HwUartCounter errors;
    /* The last write did not fit; the port has signalled room since (from a handler). */
    uint8_t send_blocked;
    HwUartCounter writable;
    HwUartCounter received;
} HwUart;

static struct
{
    HwUartPort port;
    uint8_t has_port;
    HwUart* uarts[HW_UART_MAX];
} g_uart;

static HwUart* hw_uart_slot(uint32_t id)
{
    return id >= 1U && id <= HW_UART_MAX ? g_uart.uarts[id - 1U] : NULL;
}

static HwUart* hw_uart_by_number(uint32_t number)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_UART_MAX; ++index)
    {
        if (g_uart.uarts[index] != NULL && g_uart.uarts[index]->uart == number)
        {
            return g_uart.uarts[index];
        }
    }
    return NULL;
}

static uint32_t hw_uart_available(HwUart* uart)
{
    return HW_UART_LOAD(uart->head) - HW_UART_LOAD(uart->tail);
}

HwStatus hw_uart_port_set(const HwUartPort* port)
{
    if (port == NULL)
    {
        memset(&g_uart.port, 0, sizeof(g_uart.port));
        g_uart.has_port = 0U;
        return HW_STATUS_OK;
    }
    if (port->configure == NULL || port->write == NULL)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    g_uart.port = *port;
    g_uart.has_port = 1U;
    return HW_STATUS_OK;
}

extern const HwDriver hw_uart_driver;

HwStatus hw_uart_add(const char* name, uint32_t number, uint32_t max_baud)
{
    size_t length = name != NULL ? strlen(name) : 0U;
    HwUart* slot = NULL;
    uint32_t free_index = HW_UART_MAX;
    uint32_t index = 0U;

    if (length == 0U || length > HW_UART_NAME_MAX)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_UART_MAX; ++index)
    {
        HwUart* entry = g_uart.uarts[index];
        if (entry != NULL && (entry->uart == number ||
                              (entry->name_length == length && memcmp(entry->name, name, length) == 0)))
        {
            return HW_STATUS_BUSY;
        }
        if (entry == NULL && free_index == HW_UART_MAX)
        {
            free_index = index;
        }
    }
    if (free_index == HW_UART_MAX)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    slot = (HwUart*)calloc(1U, sizeof(HwUart));
    if (slot == NULL)
    {
        return HW_STATUS_OUT_OF_MEMORY;
    }
    g_uart.uarts[free_index] = slot;
    slot->uart = number;
    memcpy(slot->name, name, length);
    slot->name_length = (uint8_t)length;
    slot->max_baud = max_baud;
    /* A platform with these devices gives programs the uart interface. */
    if (hw_driver_add(&hw_uart_driver) != HW_STATUS_OK)
    {
        return HW_STATUS_LIMIT_REACHED;
    }
    return HW_STATUS_OK;
}

/* Single producer: only the port's receive path writes head. */
void hw_uart_receive(uint32_t number, const uint8_t* data, uint32_t size)
{
    HwUart* uart = hw_uart_by_number(number);
    uint32_t head = 0U;
    uint32_t index = 0U;

    if (uart == NULL || uart->owner == 0U || uart->rx == NULL || data == NULL)
    {
        return;
    }
    head = HW_UART_LOAD(uart->head);
    for (index = 0U; index < size; ++index)
    {
        if (head - HW_UART_LOAD(uart->tail) >= HW_UART_RX_BUFFER)
        {
            /* Full: the rest is lost, and counted. */
            HW_UART_ADD(uart->errors, size - index);
            break;
        }
        uart->rx[head % HW_UART_RX_BUFFER] = data[index];
        ++head;
    }
    HW_UART_STORE(uart->head, head);
    HW_UART_STORE(uart->received, 1U);
}

void hw_uart_error(uint32_t number)
{
    HwUart* uart = hw_uart_by_number(number);
    if (uart != NULL && uart->owner != 0U)
    {
        HW_UART_ADD(uart->errors, 1U);
    }
}

void hw_uart_writable(uint32_t number)
{
    HwUart* uart = hw_uart_by_number(number);
    if (uart != NULL)
    {
        HW_UART_STORE(uart->writable, 1U);
    }
}

uint32_t hw_uart_get_owner(uint32_t number)
{
    HwUart* uart = hw_uart_by_number(number);
    return uart != NULL ? uart->owner : 0U;
}

void hw_uart_poll(void)
{
    uint8_t received = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_UART_MAX; ++index)
    {
        HwUart* uart = g_uart.uarts[index];
        if (uart == NULL)
        {
            continue;
        }
        if (HW_UART_TAKE(uart->received) != 0U)
        {
            received = 1U;
        }
        if (HW_UART_TAKE(uart->writable) != 0U && uart->send_blocked)
        {
            uart->send_blocked = 0U;
            hw_host_signal_wake(hw_driver_signal_key(&hw_uart_driver, index + 1U));
        }
    }
    if (received)
    {
        hw_host_wait_any_recheck();
    }
}

HwStatus hw_uart_open(const char* name, uint32_t length, uint32_t baud, uint32_t format, uint32_t* out_id)
{
    const uint32_t known = HW_UART_PARITY_EVEN | HW_UART_PARITY_ODD | HW_UART_STOP_2 | HW_UART_DATA_7 |
                           HW_UART_FLOW_RTS_CTS;
    uint8_t any = 0U;
    uint32_t index = 0U;

    if (!g_uart.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (baud == 0U || (format & ~known) != 0U ||
        ((format & HW_UART_PARITY_EVEN) != 0U && (format & HW_UART_PARITY_ODD) != 0U))
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0U; index < HW_UART_MAX; ++index)
    {
        HwUart* uart = g_uart.uarts[index];
        HwStatus status = HW_STATUS_OK;
        uint32_t actual = 0U;

        any |= uart != NULL;
        if (uart == NULL || uart->name_length != length || memcmp(uart->name, name, length) != 0)
        {
            continue;
        }
        if (uart->owner != 0U)
        {
            return HW_STATUS_BUSY;
        }
        if (uart->max_baud != 0U && baud > uart->max_baud)
        {
            return HW_STATUS_UNSUPPORTED;
        }
        if (uart->rx == NULL)
        {
            uart->rx = (uint8_t*)malloc(HW_UART_RX_BUFFER);
            if (uart->rx == NULL)
            {
                return HW_STATUS_OUT_OF_MEMORY;
            }
        }
        status = g_uart.port.configure(g_uart.port.context, uart->uart, baud, format, &actual);
        if (status != HW_STATUS_OK)
        {
            return status;
        }
        uart->baud = actual != 0U ? actual : baud;
        uart->format = format;
        HW_UART_STORE(uart->tail, HW_UART_LOAD(uart->head));
        (void)HW_UART_TAKE(uart->errors);
        (void)HW_UART_TAKE(uart->writable);
        uart->send_blocked = 0U;
        uart->owner = hw_host_task_id();
        *out_id = index + 1U;
        return HW_STATUS_OK;
    }
    return any ? HW_STATUS_FILE_NOT_FOUND : HW_STATUS_UNSUPPORTED;
}

void hw_uart_close(uint32_t id)
{
    HwUart* uart = hw_uart_slot(id);

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
    HW_UART_STORE(uart->tail, HW_UART_LOAD(uart->head));
}

/* Single consumer: only the owner's reads move tail. */
uint32_t hw_uart_read(uint32_t id, uint8_t* out, uint32_t max)
{
    HwUart* uart = hw_uart_slot(id);
    uint32_t tail = 0U;
    uint32_t count = 0U;

    if (uart == NULL || uart->owner == 0U)
    {
        return 0U;
    }
    tail = HW_UART_LOAD(uart->tail);
    while (count < max && tail != HW_UART_LOAD(uart->head))
    {
        out[count++] = uart->rx[tail % HW_UART_RX_BUFFER];
        ++tail;
    }
    HW_UART_STORE(uart->tail, tail);
    return count;
}

HwStatus hw_uart_write(uint32_t id, const uint8_t* data, uint32_t size, uint32_t* out_written)
{
    HwUart* uart = hw_uart_slot(id);
    HwStatus status = HW_STATUS_OK;

    *out_written = 0U;
    if (uart == NULL || uart->owner == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    if (!g_uart.has_port)
    {
        return HW_STATUS_UNSUPPORTED;
    }
    if (size == 0U)
    {
        return HW_STATUS_OK;
    }
    /* Room signalled before this write does not count for it. */
    (void)HW_UART_TAKE(uart->writable);
    status = g_uart.port.write(g_uart.port.context, uart->uart, data, size, out_written);
    if (status == HW_STATUS_OK && *out_written > size)
    {
        *out_written = size;
    }
    uart->send_blocked = status == HW_STATUS_OK && *out_written < size;
    return status;
}

HwStatus hw_uart_wait_writable(uint32_t id, uint32_t timeout_ms)
{
    HwUart* uart = hw_uart_slot(id);
    HwStatus status = HW_STATUS_OK;

    if (uart == NULL || uart->owner == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    /* Room that came while the program was busy. */
    if (HW_UART_TAKE(uart->writable) != 0U)
    {
        uart->send_blocked = 0U;
    }
    if (!uart->send_blocked)
    {
        return HW_STATUS_OK;
    }
    if (timeout_ms == 0U)
    {
        return HW_STATUS_QUEUE_FULL;
    }
    status = hw_host_signal_wait(hw_driver_signal_key(&hw_uart_driver, id), timeout_ms);
    return status == HW_STATUS_OK ? HW_STATUS_PENDING : status;
}

HwStatus hw_uart_get(uint32_t id, uint32_t property, uint32_t* out_value)
{
    HwUart* uart = hw_uart_slot(id);

    if (uart == NULL || uart->owner == 0U)
    {
        return HW_STATUS_INVALID_ARGUMENT;
    }
    switch (property)
    {
        case HW_UART_PROP_BAUD: *out_value = uart->baud; return HW_STATUS_OK;
        case HW_UART_PROP_AVAILABLE: *out_value = hw_uart_available(uart); return HW_STATUS_OK;
        /* Errors and lost bytes since the last time a program asked. */
        case HW_UART_PROP_ERRORS: *out_value = HW_UART_TAKE(uart->errors); return HW_STATUS_OK;
        default: return HW_STATUS_UNSUPPORTED;
    }
}

uint8_t hw_uart_ready(uint32_t id)
{
    HwUart* uart = hw_uart_slot(id);
    return uart != NULL && uart->owner != 0U && hw_uart_available(uart) > 0U;
}

void hw_uart_shutdown(void)
{
    uint32_t index = 0U;

    for (index = 0U; index < HW_UART_MAX; ++index)
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

static void hw_uart_save(HwSystemStream* stream)
{
    uint32_t count = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < HW_UART_MAX; ++index)
    {
        count += g_uart.uarts[index] != NULL && g_uart.uarts[index]->owner != 0U ? 1U : 0U;
    }
    hw_host_system_put32(stream, count);
    for (index = 0U; index < HW_UART_MAX; ++index)
    {
        HwUart* uart = g_uart.uarts[index];
        uint32_t tail = 0U;
        uint32_t head = 0U;

        if (uart == NULL || uart->owner == 0U)
        {
            continue;
        }
        tail = HW_UART_LOAD(uart->tail);
        head = HW_UART_LOAD(uart->head);
        hw_host_system_put32(stream, index + 1U);
        hw_host_system_put32(stream, uart->owner);
        hw_host_system_put_bytes(stream, uart->name, uart->name_length);
        hw_host_system_put32(stream, uart->baud);
        hw_host_system_put32(stream, uart->format);
        hw_host_system_put32(stream, uart->send_blocked);
        hw_host_system_put32(stream, head - tail);
        for (; tail != head; ++tail)
        {
            hw_host_system_put(stream, &uart->rx[tail % HW_UART_RX_BUFFER], 1U);
        }
    }
}

static void hw_uart_restore(HwSystemStream* stream)
{
    uint32_t count = hw_host_system_get_max(stream, HW_UART_MAX);
    uint32_t read = 0U;

    for (read = 0U; read < count && hw_host_system_ok(stream); ++read)
    {
        char name[HW_UART_NAME_MAX];
        uint32_t old_id = hw_host_system_get32(stream);
        uint32_t owner = hw_host_system_get32(stream);
        uint32_t length = hw_host_system_get_bytes(stream, name, sizeof(name));
        uint32_t baud = hw_host_system_get32(stream);
        uint32_t format = hw_host_system_get32(stream);
        uint8_t send_blocked = (uint8_t)hw_host_system_get_max(stream, 1U);
        uint32_t unread = hw_host_system_get_max(stream, HW_UART_RX_BUFFER);
        HwUart* uart = NULL;
        uint32_t actual = 0U;
        uint32_t id = 0U;
        uint32_t index = 0U;
        HwStatus status = HW_STATUS_OK;

        for (index = 0U; index < HW_UART_MAX && hw_host_system_ok(stream); ++index)
        {
            HwUart* entry = g_uart.uarts[index];
            if (uart == NULL && entry != NULL && entry->name_length == length && memcmp(entry->name, name, length) == 0)
            {
                uart = entry;
                id = index + 1U;
            }
        }
        if (!hw_host_system_ok(stream))
        {
            return;
        }
        if (uart != NULL && (uart->owner != 0U || owner == 0U))
        {
            hw_host_system_fail(stream, HW_STATUS_INVALID_ARGUMENT);
            return;
        }
        if (uart != NULL && g_uart.has_port && uart->rx == NULL)
        {
            uart->rx = (uint8_t*)malloc(HW_UART_RX_BUFFER);
            if (uart->rx == NULL)
            {
                hw_host_system_fail(stream, HW_STATUS_OUT_OF_MEMORY);
                return;
            }
        }
        status = uart != NULL && g_uart.has_port
            ? g_uart.port.configure(g_uart.port.context, uart->uart, baud, format, &actual)
            : HW_STATUS_FILE_NOT_FOUND;
        if (status == HW_STATUS_OK)
        {
            /* The bytes it had not read, first in its buffer. */
            HW_UART_STORE(uart->tail, 0U);
            hw_host_system_get(stream, uart->rx, unread);
            HW_UART_STORE(uart->head, unread);
            (void)HW_UART_TAKE(uart->errors);
            /* The port is idle after the sleep: a program waiting for room gets it. */
            HW_UART_STORE(uart->writable, 1U);
            uart->send_blocked = send_blocked;
            uart->baud = actual != 0U ? actual : baud;
            uart->format = format;
            uart->owner = owner;
            hw_host_system_device(stream, &hw_uart_driver, old_id, id, NULL);
            continue;
        }
        hw_host_system_skip(stream, unread);
        hw_host_system_device(stream, &hw_uart_driver, old_id, 0U,
                         uart != NULL && g_uart.has_port ? "a device it held cannot be set up" : NULL);
    }
}

/* ---------------------------------------------------------------------------
 * The uart interface for programs
 * ------------------------------------------------------------------------ */

typedef struct HwAbiUartOpen
{
    uint32_t baud;
    uint32_t format;
} HwAbiUartOpen;

static HwStatus hw_uart_call_opener(const char* name, uint32_t length, void* context, uint32_t* out_id)
{
    const HwAbiUartOpen* open = (const HwAbiUartOpen*)context;
    return hw_uart_open(name, length, open->baud, open->format, out_id);
}

static HW_HOST_FUNCTION(hw_uart_call_open)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(uint32_t, name_address);
    HW_HOST_ARG(uint32_t, name_length);
    HW_HOST_ARG(uint32_t, baud);
    HW_HOST_ARG(uint32_t, format);
    HwAbiUartOpen open = { baud, format };
    HW_ABI_UNUSED();

    HW_HOST_RETURN(hw_driver_open(&hw_uart_driver, name_address, name_length, hw_uart_call_opener, &open));
}

static HW_HOST_FUNCTION(hw_uart_call_read)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, buffer_address);
    HW_HOST_ARG(uint32_t, max);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_uart_driver, handle, &id);
    HwGuestBuffer buffer;
    uint32_t count = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (max > 0x7FFFFFFFU)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_host_buffer_open(&buffer, buffer_address, max, HW_GUEST_OUT);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(HW_GUEST_BUFFER_ERROR(status));
    }
    count = hw_uart_read(id, buffer.data, max);
    (void)hw_host_buffer_close(&buffer);
    HW_HOST_RETURN((int32_t)count);
}

static HW_HOST_FUNCTION(hw_uart_call_write)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, data_address);
    HW_HOST_ARG(uint32_t, length);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_uart_driver, handle, &id);
    HwGuestBuffer data;
    uint32_t written = 0U;
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (length > 0x7FFFFFFFU)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_host_buffer_open(&data, data_address, length, HW_GUEST_IN);
    if (status != HW_STATUS_OK)
    {
        HW_HOST_RETURN(HW_GUEST_BUFFER_ERROR(status));
    }
    /* The port takes the bytes during this call. */
    status = hw_uart_write(id, data.data, length, &written);
    hw_host_buffer_drop(&data);
    HW_HOST_RETURN(status == HW_STATUS_OK ? (int32_t)written : hw_abi_error(status));
}

static HW_HOST_FUNCTION(hw_uart_call_wait_writable)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, timeout_ms);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_uart_driver, handle, &id);
    HwStatus status = HW_STATUS_OK;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    if (hw_host_guest_begin((uint32_t*)raw_return) != HW_STATUS_OK)
    {
        HW_HOST_RETURN(-WR_EFAULT);
    }
    status = hw_uart_wait_writable(id, timeout_ms);
    *raw_return = hw_abi_error(status);
    return hw_host_guest_end();
}

static HW_HOST_FUNCTION(hw_uart_call_get)
{
    HW_HOST_RETURNS(int32_t);
    HW_HOST_ARG(int32_t, handle);
    HW_HOST_ARG(uint32_t, property);
    uint32_t id = 0U;
    int found = hw_driver_handle_get(&hw_uart_driver, handle, &id);
    uint32_t value = 0U;
    HW_ABI_UNUSED();

    if (!found)
    {
        HW_HOST_RETURN(-WR_EBADH);
    }
    {
        /* The call fills value: it must happen before value is read. */
        HwStatus status = hw_uart_get(id, property, &value);
        HW_HOST_RETURN(hw_driver_property(status, value));
    }
}

static const HwDriverCall hw_uart_calls[] =
{
    { "open", "i(iiii)", hw_uart_call_open, 4U },
    { "read", "i(iii)", hw_uart_call_read, 4U },
    { "write", "i(iii)", hw_uart_call_write, 4U },
    { "wait-writable", "i(ii)", hw_uart_call_wait_writable, 4U },
    { "get", "i(ii)", hw_uart_call_get, 4U },
};

const HwDriver hw_uart_driver =
{
    .interface = "uart",
    .calls = hw_uart_calls,
    .call_count = (uint32_t)(sizeof(hw_uart_calls) / sizeof(hw_uart_calls[0])),
    .close = hw_uart_close,
    .ready = hw_uart_ready,
    .poll = hw_uart_poll,
    .shutdown = hw_uart_shutdown,
    .save = hw_uart_save,
    .restore = hw_uart_restore,
};

