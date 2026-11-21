/*
 * The devices' side of the operating system (hw/hw_host.h). Most of it is
 * the kernel's own functions, compiled under the devices' names
 * (os_internal.h); these two are public kernel calls under other names.
 */

#include "os_internal.h"

uint32_t hw_host_task_id(void) { return os_task_get_id(os_task_get_current()); }

uint32_t hw_host_tick_ms(void) { return os_get_tick_ms(); }
