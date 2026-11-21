/*
 * The devices' side of the operating system (hw/hw_host.h): each call is
 * the kernel's own, under the name the devices know it by.
 */

#include "os_internal.h"

uint32_t hw_host_task_id(void) { return os_task_get_id(os_task_get_current()); }

HwStatus hw_host_guest_begin(uint32_t* raw_return) { return os_guest_begin(raw_return); }
WmResult hw_host_guest_end(void) { return os_guest_end(); }

HwStatus hw_host_guest_check(uint32_t address, uint32_t length) { return os_guest_check(address, length); }
HwStatus hw_host_guest_read(uint32_t address, void* out, uint32_t length) { return os_guest_read(address, out, length); }
HwStatus hw_host_guest_write(uint32_t address, const void* data, uint32_t length)
{
    return os_guest_write(address, data, length);
}

HwStatus hw_host_buffer_open(HwGuestBuffer* buffer, uint32_t address, uint32_t length, uint8_t access)
{
    return os_guest_buffer_open(buffer, address, length, access);
}
HwStatus hw_host_buffer_close(HwGuestBuffer* buffer) { return os_guest_buffer_close(buffer); }
void hw_host_buffer_drop(HwGuestBuffer* buffer) { os_guest_buffer_drop(buffer); }
void hw_host_buffer_keep(HwGuestBuffer* buffer, HwGuestKept** list) { os_guest_buffer_keep(buffer, list); }
HwStatus hw_host_kept_close(HwGuestKept** list) { return os_guest_kept_close(list); }

HwStatus hw_host_signal_wait(uint32_t key, uint32_t timeout_ms) { return os_signal_wait(key, timeout_ms); }
void hw_host_signal_wake(uint32_t key) { os_signal_wake(key); }
void hw_host_wait_any_recheck(void) { os_wait_any_recheck(); }

HwStatus hw_host_transfer_wait_begin(void) { return os_transfer_wait_begin(); }
void hw_host_transfer_wait_end(uint32_t task_id, HwStatus status, uint32_t value)
{
    os_transfer_wait_end(task_id, status, value);
}

uint64_t hw_host_time_ms64(void) { return os_time_ms64(); }
uint32_t hw_host_tick_ms(void) { return os_get_tick_ms(); }

void hw_host_system_put32(HwSystemStream* stream, uint32_t value) { os_system_put32(stream, value); }
void hw_host_system_put64(HwSystemStream* stream, uint64_t value) { os_system_put64(stream, value); }
void hw_host_system_put(HwSystemStream* stream, const void* data, uint32_t size) { os_system_put(stream, data, size); }
void hw_host_system_get(HwSystemStream* stream, void* data, uint32_t size) { os_system_get(stream, data, size); }
void hw_host_system_skip(HwSystemStream* stream, uint32_t size) { os_system_skip(stream, size); }
void hw_host_system_put_bytes(HwSystemStream* stream, const void* data, uint32_t size)
{
    os_system_put_bytes(stream, data, size);
}
uint32_t hw_host_system_get32(HwSystemStream* stream) { return os_system_get32(stream); }
uint64_t hw_host_system_get64(HwSystemStream* stream) { return os_system_get64(stream); }
uint32_t hw_host_system_get_max(HwSystemStream* stream, uint32_t max) { return os_system_get_max(stream, max); }
uint32_t hw_host_system_get_bytes(HwSystemStream* stream, void* out, uint32_t max)
{
    return os_system_get_bytes(stream, out, max);
}
void hw_host_system_fail(HwSystemStream* stream, HwStatus status) { os_system_fail(stream, status); }
void hw_host_system_refuse(HwSystemStream* stream, HwStatus status, const char* why)
{
    os_system_refuse(stream, status, why);
}
uint8_t hw_host_system_ok(const HwSystemStream* stream) { return os_system_ok(stream); }
void hw_host_system_device(HwSystemStream* stream, const HwDriver* driver, uint32_t old_id, uint32_t new_id,
                           const char* reason)
{
    os_system_device(stream, driver, old_id, new_id, reason);
}
uint32_t hw_host_system_device_id(HwSystemStream* stream, const HwDriver* driver, uint32_t old_id,
                                  const char** out_reason)
{
    return os_system_device_id(stream, driver, old_id, out_reason);
}
