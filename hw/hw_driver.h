#ifndef WASM_RTOS_HW_DRIVER_H
#define WASM_RTOS_HW_DRIVER_H

/*
 * Drivers: what programs reach besides the operating system's own objects
 * (pins, buses, displays...). The operating system knows none of them by
 * name. A driver is a HwDriver: the calls of its guest interface and what
 * the operating system asks of it. It joins with hw_driver_add, which its platform registration
 * (hw_gpio_add_pin and the like) does for it, so a build carries the
 * drivers its platform uses and no others. A program that imports an
 * interface no driver offers still loads; those calls return
 * -WR_EUNSUPPORTED.
 */

#include "hw_internal.h"

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
typedef struct HwDriverCall
{
    const char* name;
    /* "i(iii)": i i32, I i64, f f32, F f64, * an address; v for no result. */
    const char* signature;
    WmHostFunction function;
    /* ABI minor version that added the call. */
    uint8_t since;
} HwDriverCall;

typedef struct HwDriver
{
    /* The interface name: "gpio" for wasm-rtos:kernel/gpio. */
    const char* interface;
    const HwDriverCall* calls;
    uint32_t call_count;
    /* A program's handle to one of the driver's objects is gone (closed, or the program ended); may be NULL. */
    void (*close)(uint32_t id);
    /* Whether the object is ready for wait.any; NULL when its objects cannot be waited for. */
    uint8_t (*ready)(uint32_t id);
    /* Called before every scheduling decision: deliver what happened in the background. May be NULL. */
    void (*poll)(void);
    /* Milliseconds until poll is due even if nothing happens (UINT32_MAX: never), for a sleeping system. May be NULL. */
    uint32_t (*next_deadline_ms)(void);
    /* The library clock moved: deliver what is due by now (held keys...). May be NULL. */
    void (*time)(void);
    /*
     * End the background work of a task now (it ends, is saved or moves out
     * of RAM): the device must be done with the task's memory. May be NULL.
     */
    void (*finish_task)(uint32_t task_id);
    /* Shutdown (hw_drivers_shutdown): release everything. May be NULL. */
    void (*shutdown)(void);
    /*
     * Saving the whole system (docs/decisions.md, 7): write every object a
     * task holds, by name, with how it is set up and what it has kept for
     * the task. May be NULL: then a task holding one cannot be saved. A
     * driver that cannot save now fails the stream (hw_host_system_fail).
     */
    void (*save)(HwSystemStream* stream);
    /*
     * Read what save wrote into the driver as the platform registered it
     * again, take each object for its task and set the hardware up again;
     * tell the library what became of each (hw_host_system_device).
     */
    void (*restore)(HwSystemStream* stream);
} HwDriver;

/* gpio, whose pins other drivers take (a PWM output, a chip select...). */
extern const HwDriver hw_gpio_driver;

/* Most drivers at once. */
#ifndef HW_MAX_DRIVERS
#define HW_MAX_DRIVERS 16U
#endif

/* Add a driver; adding it again does nothing. HW_STATUS_LIMIT_REACHED past HW_MAX_DRIVERS. */
HwStatus hw_driver_add(const HwDriver* driver);

/* ---- For a driver's calls --------------------------------------------------- */

/* The key for hw_host_signal_wait and hw_host_signal_wake on the driver's object id (24 bits). */
uint32_t hw_driver_signal_key(const HwDriver* driver, uint32_t id);

/* ---- Implemented by the operating system (hw_host.h) ---------------------------------- */

/* A handle of the current program for the driver's object id, or a negative ABI error. */
int32_t hw_driver_handle_new(const HwDriver* driver, uint32_t id);
/* The driver's object behind a handle of the current program: 1 and *out_id, or 0. */
int hw_driver_handle_get(const HwDriver* driver, int32_t handle, uint32_t* out_id);
/*
 * Open an object by the name a program passes (address, length): opener
 * finds it; the result is a handle, or a negative ABI error (-WR_EINUSE
 * when it is busy). The object is closed again when no handle is left.
 */
int32_t hw_driver_open(const HwDriver* driver, uint32_t name_address, uint32_t name_length,
                       HwStatus (*opener)(const char* name, uint32_t length, void* context, uint32_t* out_id),
                       void* context);
/*
 * A driver's restore says what became of each device it saved: the device
 * saved as old_id is new_id now, or 0 when it is gone or cannot be set up
 * again (reason, or NULL for "a device it held is gone"). A task holding a
 * device that is gone is not restored.
 */
void hw_host_system_device(HwSystemStream* stream, const HwDriver* driver, uint32_t old_id, uint32_t new_id,
                           const char* reason);
/* The id a saved device has now; 0 and why when it is gone. */
uint32_t hw_host_system_device_id(HwSystemStream* stream, const HwDriver* driver, uint32_t old_id,
                                  const char** out_reason);

/* ---- In the library ---------------------------------------------------------------- */

/* A property value for a program (31 bits), or the ABI error for status. */
int32_t hw_driver_property(HwStatus status, uint32_t value);
/*
 * An optional object name from the program's memory, copied into storage
 * (HW_ABI_NAME_MAX bytes): (0, 0) means none and leaves *out_name NULL; 0
 * or a negative ABI error.
 */
#define HW_ABI_NAME_MAX 32U
int32_t hw_abi_read_name(uint32_t address, uint32_t length, char* storage, const char** out_name);

/* The ABI error (negative) for a status; 0 for HW_STATUS_OK. */
int32_t hw_abi_error(HwStatus status);
/* Whether length bytes are UTF-8. */
int hw_abi_utf8_valid(const uint8_t* text, uint32_t length);
/* The ABI error for a buffer that did not open: no memory for its copy, or a bad range. */
#define HW_GUEST_BUFFER_ERROR(STATUS) ((STATUS) == HW_STATUS_OUT_OF_MEMORY ? -WR_ENOMEM : -WR_EFAULT)
/* The key of a driver's object for hw_host_signal_wait; distinct from the operating system's own keys below 0x80 << 24. */
#define HW_SIGNAL_KEY(type, id) (((uint32_t)(type) << 24) | ((id) & 0x00FFFFFFU))

/* The registry, for the operating system: how many, each in the order added, one by its interface. */
uint32_t hw_driver_count(void);
const HwDriver* hw_driver_at(uint32_t index);
const HwDriver* hw_driver_find(const char* interface);
/* What the operating system asks of every driver: before scheduling, its next deadline, the clock moved, a task ends, shutdown. */
void hw_drivers_poll(void);
uint32_t hw_drivers_next_deadline_ms(void);
void hw_drivers_time(void);
void hw_drivers_finish_task(uint32_t task_id);
void hw_drivers_shutdown(void);

/* What a call may leave unused. */
#define HW_ABI_UNUSED() \
    (void)runtime; \
    (void)hw_host_user; \
    (void)hw_host_memory

#endif
