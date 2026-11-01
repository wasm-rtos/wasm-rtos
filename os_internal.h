#ifndef WASM_RTOS_OS_INTERNAL_H
#define WASM_RTOS_OS_INTERNAL_H

/*
 * Kernel internals shared by os.c and the guest ABI layer (abi.c). Not part
 * of the public API: embedders use os.h, programs use the ABI.
 */

#include "os.h"

typedef enum OsObjectType
{
    OS_OBJECT_NONE = 0,
    OS_OBJECT_QUEUE,
    OS_OBJECT_MUTEX,
    OS_OBJECT_SEMAPHORE,
    OS_OBJECT_EVENT_GROUP,
    OS_OBJECT_TIMER,
    OS_OBJECT_TASK,
    OS_OBJECT_GPIO
} OsObjectType;

/* One object a task can wait for together with others. */
typedef struct OsWaitTarget
{
    OsObjectType type;
    uint32_t id;
} OsWaitTarget;

/* ---- Calls from a guest import ------------------------------------------ */

/*
 * Bracket a guest call that may block. os_guest_begin records where the
 * import's i32 result lives; os_guest_end yields when the call started a
 * wait and makes the wakeup deliver the result in ABI form (the value on
 * success, a negative ABI error otherwise).
 */
OsStatus os_guest_begin(uint32_t* raw_return);
M3Result os_guest_end(void);
/* Address range of the current task's linear memory, or an error. */
OsStatus os_guest_memory(uint32_t address, uint32_t length, uint8_t** out_memory);

/* Value the current task's last immediate wait produced (event bits). */
uint32_t os_guest_last_wait_value(void);
/* Kernel time in milliseconds, without the 32-bit wrap. */
uint64_t os_time_ms64(void);

/* Per-task state owned by the ABI layer, released when the task ends. */
void** os_task_abi_state(OsTaskHandle task);

/* ---- Implemented by abi.c ------------------------------------------------ */

M3Result os_abi_link(IM3Module module);
void os_abi_release_task(OsTaskHandle task);
/* The ABI's negative error code for a kernel status (0 for OS_STATUS_OK). */
int32_t os_abi_error(OsStatus status);

/* The ABI result of waiting for a child: its exit code, or a negative error. */
int32_t os_abi_child_result(OsTaskExitReason reason, uint32_t code);

#if OS_HAS_PROCESSES
/* ---- Children of the current task ----------------------------------------- */

/*
 * Wait for a child of the current task. When it has already ended its result
 * goes to *out_result and it is collected; otherwise a timeout of 0 gives
 * OS_STATUS_TIMEOUT and any other timeout starts a wait whose wakeup delivers
 * the result (os_abi_child_result).
 */
OsStatus os_child_wait(uint32_t child_id, uint32_t timeout_ms, int32_t* out_result);
OsStatus os_child_kill(uint32_t child_id);
/* Whether child_id is a living-or-uncollected child of the current task. */
uint8_t os_child_is_mine(uint32_t child_id);
uint32_t os_current_parent_id(void);
#endif

/* ---- Object names and guest references ----------------------------------- */

/*
 * Objects created through the ABI are guest-owned: each guest handle holds a
 * reference and the object is deleted with the last one. Objects created by
 * the host are never deleted by guests. A name makes an object findable by
 * other processes.
 */
OsStatus os_object_adopt(OsObjectType type, uint32_t id, const char* name, uint32_t name_length);
uint32_t os_object_find_by_name(OsObjectType type, const char* name, uint32_t name_length);
void os_object_retain(OsObjectType type, uint32_t id);
void os_object_release(OsObjectType type, uint32_t id);

/* ---- Timers that count expirations instead of notifying a task ------------ */

OsStatus os_timer_create_counting(OsTimerHandle* out_timer, uint32_t period_ms, uint8_t auto_reload);
/* Expirations since the last call; resets the count. */
uint32_t os_timer_take_expirations(OsTimerHandle timer);

/* ---- Waiting for any of several objects ----------------------------------- */

/*
 * Ready means: a queue holds an item, a semaphore can be taken, an event
 * group has a bit set, a timer has expired, a task has ended. Returns the
 * index of the first ready target in *out_index, OS_STATUS_TIMEOUT, or starts
 * a wait (the calling task blocks; the index is delivered on wakeup).
 */
OsStatus os_wait_any(const OsWaitTarget* targets, uint32_t count, uint32_t timeout_ms, uint32_t* out_index);
uint8_t os_wait_target_ready(const OsWaitTarget* target);
/* Wake tasks in wait.any whose targets became ready outside the kernel's objects. */
void os_wait_any_recheck(void);

#if OS_HAS_GPIO
/* ---- GPIO for programs (gpio.c) -------------------------------------------- */

/*
 * Pins are named by id (1-based). Flags are the guest's: OS_GPIO_OUTPUT,
 * PULL_UP, PULL_DOWN, OPEN_DRAIN, OS_GPIO_GUEST_ACTIVE_LOW and INIT_HIGH as
 * a logical level. Levels and edges are logical (after inversion).
 * os_gpio_open: OS_STATUS_FILE_NOT_FOUND for an unknown name, OS_STATUS_BUSY
 * when another program owns the pin.
 */
#define OS_GPIO_GUEST_ACTIVE_LOW 0x10U
#define OS_GPIO_GUEST_RISING     0x01U
#define OS_GPIO_GUEST_FALLING    0x02U

OsStatus os_gpio_open(const char* name, uint32_t length, uint32_t flags, uint32_t* out_id);
void os_gpio_close(uint32_t id);
OsStatus os_gpio_set(uint32_t id, uint32_t level);
OsStatus os_gpio_get(uint32_t id, uint32_t* out_level);
OsStatus os_gpio_set_mode(uint32_t id, uint32_t flags);
OsStatus os_gpio_watch(uint32_t id, uint32_t edges);
/* Edges since the last call; resets the count. */
uint32_t os_gpio_take_events(uint32_t id);
uint8_t os_gpio_ready(uint32_t id);
/* Deliver edges reported by os_gpio_edge; wakes waiting programs. */
void os_gpio_poll(void);
void os_gpio_shutdown(void);
#endif

#endif
