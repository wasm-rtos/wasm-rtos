#ifndef WASM_RTOS_DRIVER_H
#define WASM_RTOS_DRIVER_H

/*
 * Drivers: what programs reach besides the kernel's own objects (pins,
 * buses, displays...). The kernel knows none of them by name. A driver is
 * an OsDriver: the calls of its guest interface and what the kernel asks
 * of it. It joins with os_driver_add, which its platform registration
 * (os_gpio_add_pin and the like) does for it, so a build carries the
 * drivers its platform uses and no others. A program that imports an
 * interface no driver offers still loads; those calls return
 * -WR_EUNSUPPORTED.
 */

#include "os_internal.h"

/* ABI error codes; the calls return their negatives. Keep in sync with wasm-rtos.h. */
enum
{
    WR_EFAULT = 1,
    WR_EINVAL = 2,
    WR_EBADH = 3,
    WR_ENOMEM = 4,
    WR_ELIMIT = 5,
    WR_ENOTFOUND = 6,
    WR_ETIMEOUT = 7,
    WR_EABORTED = 8,
    WR_EBUSY = 9,
    WR_ENOTOWNER = 10,
    WR_EFULL = 11,
    WR_EEMPTY = 12,
    WR_EUNSUPPORTED = 13,
    WR_ETOOLONG = 14,
    WR_EBADPROGRAM = 15,
    WR_EISDIR = 16,
    WR_ENOTDIR = 17,
    WR_EDEAD = 18,
    WR_EMISMATCH = 19,
    WR_ETRAPPED = 20,
    WR_EKILLED = 21,
    WR_EINUSE = 22,
    WR_ENACK = 23,
    WR_EBUS = 24
};

/* A call of a driver's interface: imported as "<name>" from "wasm-rtos:kernel/<interface>@1.<minor>.0". */
typedef struct OsDriverCall
{
    const char* name;
    /* As os_host_function takes it: "i(iii)". */
    const char* signature;
    OsHostImportFunction function;
    /* ABI minor version that added the call. */
    uint8_t since;
} OsDriverCall;

typedef struct OsDriver
{
    /* The interface name: "gpio" for wasm-rtos:kernel/gpio. */
    const char* interface;
    const OsDriverCall* calls;
    uint32_t call_count;
    /* A program's handle to one of the driver's objects is gone (closed, or the program ended); may be NULL. */
    void (*close)(uint32_t id);
    /* Whether the object is ready for wait.any; NULL when its objects cannot be waited for. */
    uint8_t (*ready)(uint32_t id);
    /* Called before every scheduling decision: deliver what happened in the background. May be NULL. */
    void (*poll)(void);
    /* Milliseconds until poll is due even if nothing happens (UINT32_MAX: never), for a sleeping kernel. May be NULL. */
    uint32_t (*next_deadline_ms)(void);
    /* os_shutdown: release everything. May be NULL. */
    void (*shutdown)(void);
} OsDriver;

/* Most drivers at once. */
#ifndef OS_MAX_DRIVERS
#define OS_MAX_DRIVERS 16U
#endif

/* Add a driver; adding it again does nothing. OS_STATUS_LIMIT_REACHED past OS_MAX_DRIVERS. */
OsStatus os_driver_add(const OsDriver* driver);

/* ---- For a driver's calls --------------------------------------------------- */

/* A handle of the current program for the driver's object id, or a negative ABI error. */
int32_t os_driver_handle_new(const OsDriver* driver, uint32_t id);
/* The driver's object behind a handle of the current program: 1 and *out_id, or 0. */
int os_driver_handle_get(const OsDriver* driver, int32_t handle, uint32_t* out_id);
/*
 * Open an object by the name a program passes (address, length): opener
 * finds it; the result is a handle, or a negative ABI error (-WR_EINUSE
 * when it is busy). The object is closed again when no handle is left.
 */
int32_t os_driver_open(const OsDriver* driver, uint32_t name_address, uint32_t name_length,
                       OsStatus (*opener)(const char* name, uint32_t length, void* context, uint32_t* out_id),
                       void* context);
/* A property value for a program (31 bits), or the ABI error for status. */
int32_t os_driver_property(OsStatus status, uint32_t value);
/* An optional object name from guest memory: (0, 0) means none; 0 or a negative ABI error. */
int32_t os_abi_read_name(uint32_t address, uint32_t length, const char** out_name);

/* What a call may leave unused. */
#define OS_ABI_UNUSED() \
    (void)runtime; \
    (void)os_host_user; \
    (void)os_host_memory

#endif
