#ifndef WASM_RTOS_OS_INTERNAL_H
#define WASM_RTOS_OS_INTERNAL_H

/*
 * Kernel internals shared by os.c and the guest ABI layer (abi.c). Not part
 * of the public API: embedders use os.h, programs use the ABI.
 */

#include "os.h"
#include "hw/hw_driver.h"

/*
 * The kernel's functions the devices call (hw/hw_host.h) are compiled under
 * the devices' names: the same function, no second copy, no call between.
 */
#define os_guest_begin hw_host_guest_begin
#define os_guest_end hw_host_guest_end
#define os_guest_check hw_host_guest_check
#define os_guest_read hw_host_guest_read
#define os_guest_write hw_host_guest_write
#define os_guest_buffer_open hw_host_buffer_open
#define os_guest_buffer_close hw_host_buffer_close
#define os_guest_buffer_drop hw_host_buffer_drop
#define os_guest_buffer_keep hw_host_buffer_keep
#define os_guest_kept_close hw_host_kept_close
#define os_signal_wait hw_host_signal_wait
#define os_signal_wake hw_host_signal_wake
#define os_wait_any_recheck hw_host_wait_any_recheck
#define os_transfer_wait_begin hw_host_transfer_wait_begin
#define os_transfer_wait_end hw_host_transfer_wait_end
#define os_time_ms64 hw_host_time_ms64
#define os_system_put32 hw_host_system_put32
#define os_system_put64 hw_host_system_put64
#define os_system_put hw_host_system_put
#define os_system_get hw_host_system_get
#define os_system_skip hw_host_system_skip
#define os_system_put_bytes hw_host_system_put_bytes
#define os_system_get32 hw_host_system_get32
#define os_system_get64 hw_host_system_get64
#define os_system_get_max hw_host_system_get_max
#define os_system_get_bytes hw_host_system_get_bytes
#define os_system_fail hw_host_system_fail
#define os_system_refuse hw_host_system_refuse
#define os_system_ok hw_host_system_ok
#define os_system_device hw_host_system_device
#define os_system_device_id hw_host_system_device_id

/* What a call may leave unused (the kernel's names of OS_HOST_FUNCTION). */
#define OS_ABI_UNUSED() \
    (void)runtime; \
    (void)os_host_user; \
    (void)os_host_memory

typedef enum OsObjectType
{
    OS_OBJECT_NONE = 0,
    OS_OBJECT_QUEUE,
    OS_OBJECT_MUTEX,
    OS_OBJECT_SEMAPHORE,
    OS_OBJECT_EVENT_GROUP,
    OS_OBJECT_TIMER,
    OS_OBJECT_TASK,
    /* An object of a driver (driver.h): the handle and the wait target name the driver. */
    OS_OBJECT_DRIVER
} OsObjectType;

/* One object a task can wait for together with others. */
struct HwDriver;

typedef struct OsWaitTarget
{
    OsObjectType type;
    uint32_t id;
    /* For OS_OBJECT_DRIVER. */
    const struct HwDriver* driver;
} OsWaitTarget;

/* ---- Calls from a guest import ------------------------------------------ */

/*
 * Bracket a guest call that may block. os_guest_begin records where the
 * import's i32 result lives; os_guest_end yields when the call started a
 * wait and makes the wakeup deliver the result in ABI form (the value on
 * success, a negative ABI error otherwise).
 */
OsStatus os_guest_begin(uint32_t* raw_return);
WmResult os_guest_end(void);

/* ---- The program's memory ------------------------------------------------ */

/*
 * The current task's memory is reached by address, as it may be paged:
 * ranges are checked (OS_STATUS_OUT_OF_BOUNDS when outside it, with
 * addresses near 4 GiB) and bytes copied in or out.
 */
OsStatus os_guest_check(uint32_t address, uint32_t length);
OsStatus os_guest_read(uint32_t address, void* out, uint32_t length);
OsStatus os_guest_write(uint32_t address, const void* data, uint32_t length);

/*
 * A range of the program's memory a port works on in place. data points
 * into the memory when the bytes lie in one piece; otherwise at a kernel
 * copy of them, which os_guest_buffer_close writes back when access has
 * OS_GUEST_OUT (a copy starts as the program's bytes, so what the port
 * left alone stays). A buffer is open while the port works and is closed
 * or dropped once. data is NULL for length 0.
 *
 * A pointer into a paged memory keeps its frame in RAM (pinned) until the
 * buffer is closed, so a page store never takes it away under the port.
 *
 * A port working in the background (the program waits meanwhile and keeps
 * its memory) needs the buffers past the call: os_guest_buffer_keep moves
 * a copy, or a pinned frame, into a list, and os_guest_kept_close writes
 * the copies back and lets the frames go when the transfer ends; after the
 * task has ended it only lets the copies go.
 */
#define OS_GUEST_IN HW_GUEST_IN
#define OS_GUEST_OUT HW_GUEST_OUT
typedef HwGuestBuffer OsGuestBuffer;
typedef HwGuestKept OsGuestKept;
OsStatus os_guest_buffer_open(OsGuestBuffer* buffer, uint32_t address, uint32_t length, uint8_t access);
OsStatus os_guest_buffer_close(OsGuestBuffer* buffer);
/* Let a buffer go without writing a copy back. */
void os_guest_buffer_drop(OsGuestBuffer* buffer);
void os_guest_buffer_keep(OsGuestBuffer* buffer, OsGuestKept** list);
OsStatus os_guest_kept_close(OsGuestKept** list);

/* A queue receive for a program: the item goes to address, now or when one arrives. */
OsStatus os_queue_receive_wait_guest(OsQueueHandle queue, uint32_t address, uint32_t timeout_ms);

/*
 * Waits for something outside the kernel's objects, named by a key: the
 * current task blocks until os_signal_wake(key) or the timeout. Like the
 * other waits, it starts in an import between os_guest_begin and
 * os_guest_end.
 */
OsStatus os_signal_wait(uint32_t key, uint32_t timeout_ms);
void os_signal_wake(uint32_t key);
#define OS_SIGNAL_KEY(type, id) (((uint32_t)(type) << 24) | ((id) & 0x00FFFFFFU))

/* Value the current task's last immediate wait produced (event bits). */
uint32_t os_guest_last_wait_value(void);
/* Kernel time in milliseconds, without the 32-bit wrap. */
uint64_t os_time_ms64(void);
/* Program output on fd 1 or 2, from the current task, to the output port. */
void os_output_write(uint32_t fd, const void* data, uint32_t size);

/*
 * The host function for an import table entry, made once and kept until
 * os_shutdown: signature as "i(iI*)" (i i32, I i64, f f32, F f64, * an
 * address; v for no result).
 */
WmResult os_host_function(const char* signature, OsHostImportFunction function, WmFunc** out);
/* Whether the length bytes at text (not terminated) are the string name. */
int os_name_is(const char* text, uint32_t length, const char* name);
/* The current task's WASI arguments; argv[0] is its name when it has none. */
uint32_t os_guest_args(const char* const** out_argv);
/* WASI proc_exit: the trap that ends the current task with the code. */
WmResult os_guest_exit(uint32_t code);

/* Per-task state owned by the ABI layer, released when the task ends. */
void** os_task_abi_state(OsTaskHandle task);

/* ---- Implemented by abi.c ------------------------------------------------ */

/*
 * The ABI call an import names, or *out NULL when it is not one. With the
 * program given, a kernel interface no driver offers, and a call newer than
 * this kernel, answer unsupported.
 */
WmResult os_abi_resolve(const WmModule* program, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, WmFunc** out);
void os_abi_release_task(OsTaskHandle task);
/* Paging ends with the tasks: the page store is forgotten. */
void os_page_shutdown(void);
/* Whether a page store is set. */
int os_page_store_on(void);
/*
 * A task's state image (a swapped-out task's snapshot without its memory)
 * in the page store's slots, which it reserves like frames: save writes
 * what the snapshot gives, load reads it back in order, drop lets the
 * slots go. Errors leave nothing behind.
 */
typedef struct OsPageImage
{
    uint32_t* slots;
    uint32_t count;
    uint32_t bytes;
} OsPageImage;
OsStatus os_page_image_save(OsPageImage* image, WmRuntime* runtime, WmInstance* instance, WmResult* out_result);
OsStatus os_page_image_load(const OsPageImage* image, WmRuntime* runtime, WmInstance* instance, WmResult* out_result);
void os_page_image_drop(OsPageImage* image);
/* Each swapped-out task's state, for a page store that leaves (snapshot.c). */
OsStatus os_swap_images_each(OsStatus (*fn)(OsPageImage* image, void* context), void* context);
/* The tasks with a frame or state on slots [first, first + count) end with error (snapshot.c). */
void os_tasks_end_on_slots(uint32_t first, uint32_t count, const char* error);
/* Frames the store has read in the background go to their programs, which go on. */
void os_page_poll(void);
/* A task waits for a frame read in the background, from the end of its slice; then it goes on. */
OsStatus os_page_wait_begin(OsTaskHandle task);
void os_page_wait_end(uint32_t task_id);
/*
 * The memory of another task, in one block, leaves RAM whole
 * (wm_memory_block_out): only a task between two slices (started, not
 * running, no transfer using its memory), and with cold_only one that waits
 * with no deadline or one OS_PAGE_COLD_MS away or more. 0 when none can.
 */
int os_task_block_out_one(int cold_only);
/*
 * One frame of the memory of a task that waits with no deadline, or one
 * OS_PAGE_CLEAN_COLD_MS away or more, written to the store ahead of need
 * (wm_memory_clean): 1 if one was, 0 when all such have their copies.
 */
int os_task_clean_one(void);
/*
 * What the scheduler does when nothing is ready and nothing is due within
 * OS_PAGE_CLEAN_QUIET_MS (decision 13): while RAM for frames is three
 * quarters full or more, one frame of a sleeping task to the store, so
 * that when room is needed its memory leaves RAM with no write.
 */
int os_page_clean_one(void);
/* A frame write takes a few milliseconds to an SD card (0.4 ms compressed on an ESP32): not when a task is due sooner. */
#ifndef OS_PAGE_CLEAN_QUIET_MS
#define OS_PAGE_CLEAN_QUIET_MS 20U
#endif
/*
 * Before a task's slice: its paged memory goes back into one block when RAM
 * has room for all of it (besides the frames of it already there), and at
 * most a quarter of its frames are only in the store (they are read back).
 */
void os_page_block_in(WmMemory* memory);

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

/* The current task waits for its background transfer (os.c). */
OsStatus os_transfer_wait_begin(void);
/* The transfer of task_id ended with status and value; wakes it (os.c). */
void os_transfer_wait_end(uint32_t task_id, OsStatus status, uint32_t value);
/* End every background transfer of task_id now, through the ports' finish. */
void os_transfer_finish_task(uint32_t task_id);

/* ---- Saving the whole system (system.c) ------------------------------------ */

/*
 * The stream os_system_save writes and os_system_restore reads: numbers
 * little-endian, texts as a length and bytes. The first failure sticks:
 * OS_STATUS_INVALID_ARGUMENT marks a stream that does not agree with itself.
 */
typedef HwSystemStream OsSystemStream;
void os_system_put32(OsSystemStream* stream, uint32_t value);
void os_system_put64(OsSystemStream* stream, uint64_t value);
/* A text, or none for NULL. */
void os_system_put_text(OsSystemStream* stream, const char* text);
/* Bytes as they are; their count is the reader's to know. */
void os_system_put(OsSystemStream* stream, const void* data, uint32_t size);
void os_system_get(OsSystemStream* stream, void* data, uint32_t size);
void os_system_skip(OsSystemStream* stream, uint32_t size);
/* A length, then the bytes. */
void os_system_put_bytes(OsSystemStream* stream, const void* data, uint32_t size);
uint32_t os_system_get32(OsSystemStream* stream);
uint64_t os_system_get64(OsSystemStream* stream);
/* A number at most max; a larger one breaks the stream. */
uint32_t os_system_get_max(OsSystemStream* stream, uint32_t max);
/* Bytes put with os_system_put_bytes into out (at most max; more breaks the stream); their count. */
uint32_t os_system_get_bytes(OsSystemStream* stream, void* out, uint32_t max);
/* A text in the heap, or NULL for none or a stream that failed. */
char* os_system_get_text(OsSystemStream* stream);
void os_system_fail(OsSystemStream* stream, OsStatus status);
/* A driver cannot save now: why, for the last error. */
void os_system_refuse(OsSystemStream* stream, OsStatus status, const char* why);
uint8_t os_system_ok(const OsSystemStream* stream);

/*
 * A driver's restore says what became of each device it saved: the device
 * saved as old_id is new_id now, or 0 when it is gone or cannot be set up
 * again (reason, or NULL for "a device it held is gone"). A task holding a
 * device that is gone is not restored.
 */
void os_system_device(OsSystemStream* stream, const struct HwDriver* driver, uint32_t old_id, uint32_t new_id,
                      const char* reason);
/* The id a saved device has now; 0 and why when it is gone. */
uint32_t os_system_device_id(OsSystemStream* stream, const struct HwDriver* driver, uint32_t old_id,
                             const char** out_reason);


/* A task's handles (abi.c). The interface of a device it holds whose driver cannot save it, or NULL. */
const char* os_abi_task_device(OsTaskHandle task);
void os_abi_save_task(OsTaskHandle task, OsSystemStream* stream);
/* Why the task cannot be restored (a device it held is gone), or NULL. */
const char* os_abi_restore_task(OsTaskHandle task, OsSystemStream* stream);
#if OS_HAS_FS
/* A task's open files (fs.c): path and position. */
void os_fs_save_task(uint32_t task_id, OsSystemStream* stream);
/* Opens them again when apply is set; why one cannot be (the task is lost), or NULL. */
const char* os_fs_restore_task(uint32_t task_id, OsSystemStream* stream, uint8_t apply);
#endif

/* ---- The host's RTOS (decision 15) ------------------------------------------
 * Semaphores, event groups, mutexes, software timers and task notifications
 * for native code, and helpers nothing outside used: the kernel's own and
 * its tests', not part of the public API. Programs reach the objects
 * through the ABI. */

uint32_t os_queue_get_id(OsQueueHandle queue);

OsQueueHandle os_queue_find_by_id(uint32_t queue_id);

OsStatus os_queue_send_wait(
    OsQueueHandle queue,
    const void* item,
    uint32_t timeout_ms
);

uint32_t os_queue_get_space(OsQueueHandle queue);

uint32_t os_queue_get_item_size(OsQueueHandle queue);

OsStatus os_mutex_set_name(OsMutexHandle mutex, const char* name);

OsStatus os_semaphore_set_name(OsSemaphoreHandle semaphore, const char* name);

OsStatus os_event_group_set_name(OsEventGroupHandle event_group, const char* name);

OsStatus os_mutex_create(OsMutexHandle* out_mutex);

void os_mutex_delete(OsMutexHandle mutex);

uint32_t os_mutex_get_id(OsMutexHandle mutex);

OsMutexHandle os_mutex_find_by_id(uint32_t mutex_id);

OsStatus os_mutex_lock(OsMutexHandle mutex, uint32_t timeout_ms);

OsStatus os_mutex_unlock(OsMutexHandle mutex);

OsTaskHandle os_mutex_get_owner(OsMutexHandle mutex);

OsStatus os_semaphore_create(
    OsSemaphoreHandle* out_semaphore,
    uint32_t max_count,
    uint32_t initial_count
);

void os_semaphore_delete(OsSemaphoreHandle semaphore);

uint32_t os_semaphore_get_id(OsSemaphoreHandle semaphore);

OsSemaphoreHandle os_semaphore_find_by_id(uint32_t semaphore_id);

OsStatus os_semaphore_take(OsSemaphoreHandle semaphore, uint32_t timeout_ms);

OsStatus os_semaphore_give(OsSemaphoreHandle semaphore);

uint32_t os_semaphore_get_count(OsSemaphoreHandle semaphore);

uint32_t os_semaphore_get_max_count(OsSemaphoreHandle semaphore);

OsStatus os_event_group_create(OsEventGroupHandle* out_event_group);

void os_event_group_delete(OsEventGroupHandle event_group);

uint32_t os_event_group_get_id(OsEventGroupHandle event_group);

OsEventGroupHandle os_event_group_find_by_id(uint32_t event_group_id);

uint32_t os_event_group_get_bits(OsEventGroupHandle event_group);

uint32_t os_event_group_set_bits(OsEventGroupHandle event_group, uint32_t bits);

uint32_t os_event_group_clear_bits(OsEventGroupHandle event_group, uint32_t bits);

OsStatus os_event_group_wait_bits(
    OsEventGroupHandle event_group,
    uint32_t bits_to_wait_for,
    uint8_t clear_on_exit,
    uint8_t wait_for_all,
    uint32_t timeout_ms
);

/*
 * Advance *previous_wake_time_ms by period_ms and block until that absolute
 * deadline when it is still in the future. The phase is advanced even when
 * the deadline was missed, matching FreeRTOS vTaskDelayUntil semantics.
 */
OsStatus os_task_delay_until(
    uint32_t* previous_wake_time_ms,
    uint32_t period_ms
);

OsStatus os_task_write_snapshot(OsTaskHandle task, OsSnapshotWrite write, void* context);

OsStatus os_task_read_snapshot(OsTaskHandle task, OsSnapshotRead read, void* context);

/* A background read or write of an added store ended (os_page_read_done and os_page_write_done are the first store's). */
void os_page_store_done(const OsPageStore* store, uint32_t slot, OsStatus status);

/*
 * Write the image of one loaded program that has none, and map it: what
 * the scheduler does when no task is ready and none is due within
 * OS_CODE_QUIET_MS. Writing takes a while (128 ms
 * for CoreMark's on an ESP32), so a host whose tasks are never all waiting
 * calls it where a pause suits it. OS_STATUS_OK: one written;
 * OS_STATUS_NO_READY_TASKS: none waits for one.
 */
OsStatus os_code_store_write_pending(void);

uint32_t os_task_get_last_wait_value(OsTaskHandle task);

OsStatus os_task_notify(
    OsTaskHandle task,
    uint32_t value,
    OsNotifyAction action
);

OsStatus os_task_notify_wait(
    uint32_t clear_on_entry,
    uint32_t clear_on_exit,
    uint32_t timeout_ms
);

OsStatus os_task_notify_take(
    uint8_t clear_count_on_exit,
    uint32_t timeout_ms
);

uint32_t os_task_get_notification_value(OsTaskHandle task);

uint8_t os_task_is_notification_pending(OsTaskHandle task);

/* Timers notify their target task and are deleted when that task exits. */
OsStatus os_timer_create(
    OsTimerHandle* out_timer,
    uint32_t period_ms,
    uint8_t auto_reload,
    OsTaskHandle target_task,
    uint32_t notify_value,
    OsNotifyAction notify_action
);

void os_timer_delete(OsTimerHandle timer);

uint32_t os_timer_get_id(OsTimerHandle timer);

OsTimerHandle os_timer_find_by_id(uint32_t timer_id);

OsStatus os_timer_start(OsTimerHandle timer);

OsStatus os_timer_stop(OsTimerHandle timer);

OsStatus os_timer_reset(OsTimerHandle timer);

OsStatus os_timer_change_period(OsTimerHandle timer, uint32_t period_ms);

uint8_t os_timer_is_active(OsTimerHandle timer);

uint32_t os_timer_get_period_ms(OsTimerHandle timer);

/*
 * Feed an absolute monotonic clock: time advances by the difference from the
 * previous call, wrapping at 32 bits. The first call only sets the baseline.
 */
OsStatus os_update_time_ms(uint32_t now_ms);

#endif
