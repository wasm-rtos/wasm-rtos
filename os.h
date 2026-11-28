#ifndef WASM_RTOS_H
#define WASM_RTOS_H

#include <stdint.h>
#include <string.h>

#include "vm/source/wm.h"
#include "hw/hw_host.h"

/*
 * Build options (define to 0 to leave a feature out of the image):
 *   OS_HAS_PROCESSES  os_task_spawn() and the guest proc interface
 *   OS_HAS_FS         virtual file system behind the WASI file calls (fs.h)
 *   OS_FS_TAR         read-only tar image driver (needs OS_HAS_FS)
 *   OS_HAS_WASI       the WASI preview 1 calls programs built for it use
 * Off unless defined to 1:
 *   OS_HAS_MEMORY_ACCOUNTING  all interpreter memory per task in os_task_get_memory_usage
 */
#ifndef OS_HAS_PROCESSES
#define OS_HAS_PROCESSES 1
#endif
#ifndef OS_HAS_FS
#define OS_HAS_FS 1
#endif
#ifndef OS_FS_TAR
#define OS_FS_TAR OS_HAS_FS
#endif
#ifndef OS_HAS_WASI
#define OS_HAS_WASI 1
#endif
#ifndef OS_HAS_MEMORY_ACCOUNTING
#define OS_HAS_MEMORY_ACCOUNTING 0
#endif
/* A page store that keeps frames compressed in RAM (decision 12): not on AVR, which has no paging. */
#ifndef OS_HAS_COMPRESSED_STORE
#if defined(__AVR__)
#define OS_HAS_COMPRESSED_STORE 0
#else
#define OS_HAS_COMPRESSED_STORE 1
#endif
#endif
/* Code stores (decision 11): images are programs', and only where the interpreter predecodes (not on AVR). */
#ifndef OS_HAS_CODE_STORES
#if defined(__AVR__) || !OS_HAS_PROCESSES
#define OS_HAS_CODE_STORES 0
#else
#define OS_HAS_CODE_STORES 1
#endif
#endif
/* Finite delays, timeouts, and timer periods must not exceed INT32_MAX. */
#define OS_WAIT_FOREVER UINT32_MAX

typedef struct OsTask* OsTaskHandle;
typedef struct OsQueue* OsQueueHandle;
typedef struct OsMutex* OsMutexHandle;
typedef struct OsSemaphore* OsSemaphoreHandle;
typedef struct OsEventGroup* OsEventGroupHandle;
typedef struct OsTimer* OsTimerHandle;

/*
 * Host functions (the kernel's imports and os_host_import_register's) get
 * their arguments in 32-bit slots, i64 and f64 taking two, and leave their
 * result in the first. OS_HOST_FUNCTION declares one. Inside it,
 * OS_HOST_RETURNS(type) comes first and names the result slot raw_return;
 * OS_HOST_ARG reads the next argument (a pointer into the program's memory
 * arrives as a uint32_t address). OS_HOST_RETURN sets the result and returns.
 *
 * The program's memory is reached by address, never through a pointer to
 * all of it: it may be paged, in pieces of a frame (4 KiB by default). OS_HOST_CHECK_ADDR traps a
 * range outside the memory; after it, OS_HOST_LOAD32/64 read a value at an
 * address, OS_HOST_STORE8/32/64 write one and OS_HOST_COPY_FROM/TO move a
 * block (linear memory is little-endian, like every target the kernel runs
 * on). A port that works on a buffer in place opens an OsGuestBuffer
 * (os_internal.h). In bounds, an access can still fail when the memory is
 * paged (its frame could not come back from the page store, or into RAM):
 * the program's call then ends with that trap once the host function
 * returns, as it would had the program touched the frame itself.
 */
typedef WmHostFunction OsHostImportFunction;
/* As the devices declare them (hw/hw_host.h), with the names of the kernel. */
#define OS_HOST_FUNCTION(NAME) \
    WmResult NAME(WmRuntime* runtime, void* os_host_user, uint32_t* os_host_slot, WmMemory* os_host_memory)
#define OS_HOST_RETURNS(TYPE) TYPE* const raw_return = (TYPE*)(void*)os_host_slot
#define OS_HOST_ARG(TYPE, NAME) \
    TYPE NAME; \
    memcpy(&NAME, os_host_slot, sizeof(TYPE)); \
    os_host_slot += (sizeof(TYPE) + 3U) / 4U
#define OS_HOST_RETURN(VALUE) \
    do \
    { \
        *raw_return = (VALUE); \
        return NULL; \
    } while (0)
#define OS_HOST_CHECK_ADDR(ADDRESS, LENGTH) \
    do \
    { \
        if (!os_host_memory_in_bounds(os_host_memory, (uint32_t)(ADDRESS), (uint64_t)(LENGTH))) \
            return os_host_out_of_bounds; \
    } while (0)
#define OS_HOST_LOAD32(ADDRESS) os_host_load32(runtime, os_host_memory, (uint32_t)(ADDRESS))
#define OS_HOST_LOAD64(ADDRESS) os_host_load64(runtime, os_host_memory, (uint32_t)(ADDRESS))
#define OS_HOST_STORE8(ADDRESS, VALUE) os_host_store8(runtime, os_host_memory, (uint32_t)(ADDRESS), (uint8_t)(VALUE))
#define OS_HOST_STORE32(ADDRESS, VALUE) os_host_store32(runtime, os_host_memory, (uint32_t)(ADDRESS), (uint32_t)(VALUE))
#define OS_HOST_STORE64(ADDRESS, VALUE) os_host_store64(runtime, os_host_memory, (uint32_t)(ADDRESS), (uint64_t)(VALUE))
#define OS_HOST_COPY_FROM(ADDRESS, OUT, LENGTH) \
    os_host_fault(runtime, wm_memory_read(os_host_memory, (uint32_t)(ADDRESS), (OUT), (uint32_t)(LENGTH)))
#define OS_HOST_COPY_TO(ADDRESS, DATA, LENGTH) \
    os_host_fault(runtime, wm_memory_write(os_host_memory, (uint32_t)(ADDRESS), (DATA), (uint32_t)(LENGTH)))
/* Little-endian values in kernel memory (a record copied out of the program). */
#define OS_HOST_READ32(POINTER) os_host_read32(POINTER)
#define OS_HOST_READ64(POINTER) os_host_read64(POINTER)
#define OS_HOST_WRITE8(POINTER, VALUE) (*(uint8_t*)(POINTER) = (uint8_t)(VALUE))
#define OS_HOST_WRITE32(POINTER, VALUE) os_host_write32((POINTER), (uint32_t)(VALUE))
#define OS_HOST_WRITE64(POINTER, VALUE) os_host_write64((POINTER), (uint64_t)(VALUE))
#define os_host_fault hw_host_fault
#define os_host_load32 hw_host_load32
#define os_host_load64 hw_host_load64
#define os_host_memory_in_bounds hw_host_memory_in_bounds
#define os_host_read32 hw_host_read32
#define os_host_read64 hw_host_read64
#define os_host_store32 hw_host_store32
#define os_host_store64 hw_host_store64
#define os_host_store8 hw_host_store8
#define os_host_write32 hw_host_write32
#define os_host_write64 hw_host_write64
#define os_host_out_of_bounds hw_host_out_of_bounds

typedef HwStatus OsStatus;
#define OS_STATUS_OK HW_STATUS_OK
#define OS_STATUS_ERROR HW_STATUS_ERROR
#define OS_STATUS_INVALID_ARGUMENT HW_STATUS_INVALID_ARGUMENT
#define OS_STATUS_OUT_OF_MEMORY HW_STATUS_OUT_OF_MEMORY
#define OS_STATUS_WASM_ERROR HW_STATUS_WASM_ERROR
#define OS_STATUS_TASK_DEAD HW_STATUS_TASK_DEAD
#define OS_STATUS_TASK_NOT_FOUND HW_STATUS_TASK_NOT_FOUND
#define OS_STATUS_NO_READY_TASKS HW_STATUS_NO_READY_TASKS
#define OS_STATUS_BUFFER_TOO_SMALL HW_STATUS_BUFFER_TOO_SMALL
#define OS_STATUS_UNSUPPORTED HW_STATUS_UNSUPPORTED
#define OS_STATUS_OUT_OF_BOUNDS HW_STATUS_OUT_OF_BOUNDS
#define OS_STATUS_QUEUE_FULL HW_STATUS_QUEUE_FULL
#define OS_STATUS_QUEUE_EMPTY HW_STATUS_QUEUE_EMPTY
#define OS_STATUS_QUEUE_NOT_FOUND HW_STATUS_QUEUE_NOT_FOUND
#define OS_STATUS_TIMEOUT HW_STATUS_TIMEOUT
#define OS_STATUS_BUSY HW_STATUS_BUSY
#define OS_STATUS_NOT_OWNER HW_STATUS_NOT_OWNER
#define OS_STATUS_MUTEX_NOT_FOUND HW_STATUS_MUTEX_NOT_FOUND
#define OS_STATUS_SEMAPHORE_FULL HW_STATUS_SEMAPHORE_FULL
#define OS_STATUS_SEMAPHORE_NOT_FOUND HW_STATUS_SEMAPHORE_NOT_FOUND
#define OS_STATUS_EVENT_GROUP_NOT_FOUND HW_STATUS_EVENT_GROUP_NOT_FOUND
#define OS_STATUS_TIMER_NOT_FOUND HW_STATUS_TIMER_NOT_FOUND
#define OS_STATUS_ABORTED HW_STATUS_ABORTED
#define OS_STATUS_NOT_WAITING HW_STATUS_NOT_WAITING
#define OS_STATUS_LIMIT_REACHED HW_STATUS_LIMIT_REACHED
#define OS_STATUS_FILE_NOT_FOUND HW_STATUS_FILE_NOT_FOUND
#define OS_STATUS_NOT_A_DIRECTORY HW_STATUS_NOT_A_DIRECTORY
#define OS_STATUS_IS_A_DIRECTORY HW_STATUS_IS_A_DIRECTORY
#define OS_STATUS_READ_ONLY HW_STATUS_READ_ONLY
#define OS_STATUS_END_OF_DIRECTORY HW_STATUS_END_OF_DIRECTORY
#define OS_STATUS_NACK HW_STATUS_NACK
#define OS_STATUS_BUS_ERROR HW_STATUS_BUS_ERROR
#define OS_STATUS_PENDING HW_STATUS_PENDING

typedef enum OsTaskState
{
    OS_TASK_READY = 0,
    OS_TASK_RUNNING,
    OS_TASK_WAITING,
    OS_TASK_SUSPENDED,
    OS_TASK_DEAD
} OsTaskState;

typedef enum OsTaskExitReason
{
    OS_TASK_EXIT_NONE = 0,
    OS_TASK_EXIT_RETURNED,
    OS_TASK_EXIT_EXPLICIT,
    OS_TASK_EXIT_DELETED,
    OS_TASK_EXIT_WASM_ERROR
} OsTaskExitReason;

typedef enum OsTaskPriority
{
    /* Makes no predecoded code of its own: uses others', runs the rest in place (vm decision 21). */
    OS_TASK_PRIORITY_IDLE = 0,
    OS_TASK_PRIORITY_LOW = 1,
    OS_TASK_PRIORITY_NORMAL = 5,
    OS_TASK_PRIORITY_HIGH = 10,
    OS_TASK_PRIORITY_REALTIME = 20
} OsTaskPriority;

typedef enum OsValueType
{
    OS_VALUE_TYPE_I32 = 1,
    OS_VALUE_TYPE_I64 = 2,
    OS_VALUE_TYPE_F32 = 3,
    OS_VALUE_TYPE_F64 = 4
} OsValueType;

typedef enum OsNotifyAction
{
    OS_NOTIFY_NO_ACTION = 0,
    OS_NOTIFY_SET_BITS,
    OS_NOTIFY_INCREMENT,
    OS_NOTIFY_SET_VALUE_WITH_OVERWRITE,
    OS_NOTIFY_SET_VALUE_WITHOUT_OVERWRITE
} OsNotifyAction;

typedef uint32_t (*OsClockNowMsFunction)(void* context);
typedef void (*OsClockArmWakeupFunction)(void* context, uint32_t delay_ms);
typedef void (*OsClockCancelWakeupFunction)(void* context);

typedef struct OsClockPort
{
    OsClockNowMsFunction now_ms;
    OsClockArmWakeupFunction arm_wakeup;
    OsClockCancelWakeupFunction cancel_wakeup;
    void* context;
} OsClockPort;

typedef struct OsValue
{
    OsValueType type;
    union OsValueData
    {
        uint32_t i32;
        uint64_t i64;
        float f32;
        double f64;
    } value;
} OsValue;

OsStatus os_init(void);
void os_shutdown(void);

/*
 * Interpreter stack size. Whoever creates a task may pass stack_size; 0 takes
 * the size the program asks for in its "wasm-rtos.app" custom section, and
 * without one OS_DEFAULT_STACK_SIZE (8 KiB). A program overflowing its stack
 * traps with "stack overflow"; os_task_get_memory_usage reports the most it
 * has used (stack_peak_bytes).
 *
 * A program describes itself in a "wasm-rtos.app" custom section: records
 * of a key byte, a LEB128 length and that many bytes. Unknown keys are
 * skipped; a section that is not whole records fails the load, and so does
 * an ABI major version other than OS_ABI_VERSION_MAJOR. wasm-rtos.h has
 * macros that write the records (WR_APP_STACK_SIZE, WR_APP_NAME, ...).
 */
#define OS_ABI_VERSION_MAJOR 1U
#define OS_ABI_VERSION_MINOR 8U
#define OS_APP_KEY_ABI_VERSION 1U   /* u32: major << 16 | minor */
#define OS_APP_KEY_STACK_SIZE 2U    /* u32: interpreter stack bytes */
#define OS_APP_KEY_NAME 3U          /* UTF-8 */
#define OS_APP_KEY_ICON 4U          /* reserved: image */
#define OS_APP_KEY_FILE_TYPES 5U    /* reserved: UTF-8, ';'-separated extensions */
#define OS_APP_KEY_REQUIRES 6U      /* reserved: device requirements */

/*
 * entry_function_name may be NULL or "" to let the loader pick the entry
 * point: _start (WASI command), then app_main, then main. Every function
 * import must resolve when the task is created; otherwise creation fails
 * with OS_STATUS_WASM_ERROR and os_get_last_error_result() names the first
 * missing import.
 */
OsStatus os_task_create(
    OsTaskHandle* out_task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    const char* task_name,
    uint32_t stack_size,
    uint32_t priority
);

OsStatus os_task_create_with_args(
    OsTaskHandle* out_task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    OsValue* entry_args,
    uint32_t entry_arg_count,
    const char* task_name,
    uint32_t stack_size,
    uint32_t priority
);

OsStatus os_queue_create(
    OsQueueHandle* out_queue,
    uint32_t item_size,
    uint32_t item_count
);

void os_queue_delete(OsQueueHandle queue);
OsStatus os_queue_send(OsQueueHandle queue, const void* item);
OsStatus os_queue_receive(OsQueueHandle queue, void* out_item);
/*
 * A receive that can complete immediately accepts any writable C buffer.
 * If it must block, out_item must point into the current WASM task's linear
 * memory so the kernel can retain a stable offset instead of a native pointer.
 * Native buffers return OS_STATUS_UNSUPPORTED rather than being retained.
 */
OsStatus os_queue_receive_wait(
    OsQueueHandle queue,
    void* out_item,
    uint32_t timeout_ms
);
uint32_t os_queue_get_count(OsQueueHandle queue);
/*
 * A name lets programs open the object (the guest sync interface). Objects
 * the host creates stay the host's: programs never delete them.
 */
OsStatus os_queue_set_name(OsQueueHandle queue, const char* name);

OsStatus os_host_import_register(
    const char* module_name,
    const char* import_name,
    const char* signature,
    OsHostImportFunction function
);

void os_host_import_clear_all(void);

/*
 * Host imports that block: pass the import's result slot (raw_return) to
 * os_host_wait_begin before calling a waiting function such as
 * os_queue_receive_wait or os_task_delay_ms from inside the import, and return os_host_wait_end()
 * from it. When the call started a wait the task blocks, and the slot gets
 * the wait's status (or value) when it wakes.
 */
OsStatus os_host_wait_begin(uint32_t* result_slot);
WmResult os_host_wait_end(void);

/*
 * Loaders (decision 16): other ways of making a program out of its module,
 * joined as drivers are (wasm-rtos-dylink's os_dylink_add is one). A task's
 * module is offered to each in the order added, the kernel's own (one
 * module, nothing linked) last. The three steps are the interpreter's:
 *
 *   build  the program's instances, running none of their code: NULL, or
 *          os_not_mine for a module that is not this loader's (the next is
 *          asked), or an error. host resolves the kernel's functions
 *          (host_context is its context). memory is NULL, or, back from
 *          swap, the program's memory kept apart with its bytes: build on
 *          it (wm_instantiate_with_memory) and write nothing into it.
 *          process is the loader's own, given back to start and free.
 *   start  what build left to run: start functions, relocations,
 *          constructors. Called once, as the kernel starts any program;
 *          not back from swap, where the state already holds it.
 *   free   everything build made, the instance included.
 *
 * A loader that builds the same instances from the same module, and keeps
 * its state in WebAssembly's objects, has its tasks saved and read back as
 * any (os_task_save_snapshot, os_system_save): the instance's snapshot
 * holds every instance linked to it (wasm-rtos-vm decision 23), and swaps
 * out as any (decision 17) when its program's instance owns its memory; a
 * program that imports a memory the loader made answers
 * OS_STATUS_UNSUPPORTED to os_task_swap_out.
 */
extern const char os_not_mine[];

typedef struct OsLoader
{
    WmResult (*build)(void* context, WmModule* program, WmMemory* memory, WmResolver host, void* host_context,
                      void** out_process, WmInstance** out_instance);
    WmResult (*start)(void* process, WmRuntime* runtime);
    void (*free)(void* process);
    void* context;
} OsLoader;

/* Up to OS_MAX_LOADERS (4); the loader must outlive os_shutdown(). */
OsStatus os_loader_add(const OsLoader* loader);

OsStatus os_task_delete(OsTaskHandle task);

#if OS_HAS_PROCESSES
/*
 * Start a WebAssembly program as a new process, like an executable loader.
 * The module bytes and argv are copied, so the caller may release them
 * afterwards. The entry point is detected (_start, app_main, main); a WASI
 * program receives argv through args_get. priority 0 inherits the calling
 * task's base priority, or OS_TASK_PRIORITY_NORMAL when called from the host.
 * stack_size 0 lets the program choose (see "Interpreter stack size"); guest
 * spawn calls always pass 0.
 *
 * A process spawned from inside a task becomes that task's child. A dead
 * child is reclaimed after its parent collects it (the guest proc wait
 * call), or as soon as it dies when its parent is gone. A process spawned
 * by the host has no parent and stays until the host deletes it, exactly
 * like a task from os_task_create. At most
 * OS_MAX_SPAWNED_TASKS spawned processes exist at once; further spawns
 * return OS_STATUS_LIMIT_REACHED.
 *
 * Processes of the same program share it: its bytes and its parsed module
 * (with the decoded code) are loaded once, and each process has its own
 * instance, memory and stack. A program is found by its contents, compared
 * byte by byte, so os_task_spawn copies the bytes only when no loaded
 * program has them, and the program goes with its last process.
 *
 * Programs start processes through the guest ABI's proc interface
 * (docs/abi.md), which gives them handles to their children.
 */
OsStatus os_task_spawn(
    OsTaskHandle* out_task,
    const uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* const* argv,
    uint32_t argc,
    uint32_t priority,
    uint32_t stack_size
);
uint32_t os_task_get_parent_id(OsTaskHandle task);
/* Programs loaded for processes, each once however many processes run it. */
uint32_t os_get_program_count(void);
#if OS_HAS_FS
/*
 * Start the program stored at an absolute file system path. A file whose
 * contents a running process already has is that process's program: it is
 * compared in pieces, not read whole, and a file that changed is a new
 * program. A file whose driver maps it into memory (the tar driver does)
 * runs in place without a copy, and its mount stays busy until the last
 * process of it is destroyed; any other file is read into RAM first. With
 * argc 0 the program gets argv = { path }.
 * Errors: OS_STATUS_FILE_NOT_FOUND, OS_STATUS_IS_A_DIRECTORY,
 * OS_STATUS_NOT_A_DIRECTORY, plus everything os_task_spawn returns.
 */
OsStatus os_task_spawn_path(
    OsTaskHandle* out_task,
    const char* path,
    const char* const* argv,
    uint32_t argc,
    uint32_t priority,
    uint32_t stack_size
);
#endif
#endif
OsStatus os_task_delay_ms(uint32_t delay_ms);
/*
 * Wake a task blocked by a delay or synchronization wait. Blocking imports
 * that return a status resume with OS_STATUS_ABORTED.
 */
OsStatus os_task_abort_wait(OsTaskHandle task);
/*
 * Called from a host import: end the calling task's slice and keep it READY.
 * OS_STATUS_TASK_NOT_FOUND when no task is running.
 */
OsStatus os_task_yield(void);
/*
 * Safe from a host import or between slices (an interrupt handler's deferred
 * work): the running task stops at its next check, or else the next slice
 * stops at once. One request ends one slice.
 */
void os_request_preempt(void);
/*
 * A suspended task gets no slices until os_task_resume. Suspending a waiting
 * task cancels its wait: when resumed it continues at once, a delay ends
 * early and a blocking import returns OS_STATUS_BUSY. Resuming a task that is
 * not suspended changes nothing. Both return OS_STATUS_TASK_DEAD for a dead
 * task.
 */
OsStatus os_task_suspend(OsTaskHandle task);
OsStatus os_task_resume(OsTaskHandle task);

/*
 * A snapshot is a program's state between two slices: its position, stack,
 * linear memory and globals, without host addresses, so it loads into a task
 * running the same module on this or another device with the same kernel
 * version, whatever the pointer size. Load it into a new task created from
 * the same module; it continues where the snapshot was taken.
 *
 * Kernel state is not part of it: waits, timers, handles and open files stay
 * with the task they belong to. So saving needs a task that is neither
 * waiting nor the target of a timer (OS_STATUS_BUSY), and loading needs one
 * that has not run yet (OS_STATUS_BUSY). A load that fails leaves the task
 * as it was created. A loader's task (os_loader_add) is saved with every
 * instance linked to its program's, and loads into a task its loader built
 * the same way.
 *
 * The stream calls write or read the snapshot in pieces through a callback;
 * a callback error stops the call and is returned as it is.
 */
typedef OsStatus (*OsSnapshotWrite)(void* context, const void* data, uint32_t size);
typedef OsStatus (*OsSnapshotRead)(void* context, void* data, uint32_t size);

OsStatus os_task_get_snapshot_size(
    OsTaskHandle task,
    uint32_t* out_size
);

OsStatus os_task_save_snapshot(
    OsTaskHandle task,
    uint8_t* buffer,
    uint32_t buffer_size,
    uint32_t* out_size
);

OsStatus os_task_load_snapshot(
    OsTaskHandle task,
    const uint8_t* buffer,
    uint32_t buffer_size
);

/*
 * The whole system, for a device that loses its RAM in deep sleep or when
 * switched off: every task (its program's state, what it waits for, its
 * notifications, handles, open files, children) and every queue, mutex,
 * semaphore, event group and timer with what it holds, and every device a
 * task holds (each driver saves its own, by name), written to a stream
 * between two slices (not from inside a task). Modules are not written: a
 * process started by path is opened from it again, one the kernel copied
 * is in the stream, and for a task the host created the kernel asks find
 * by task name; a module of other bytes is refused.
 *
 * Saving returns OS_STATUS_BUSY while a transfer or page is on its way (try
 * after the next slice) and OS_STATUS_UNSUPPORTED while a task holds a
 * display that loses its picture in the sleep (OsDisplayInfo.keeps_image);
 * the last error says which.
 *
 * Restore into a kernel set up as at start (os_init, clock, page store,
 * drivers, host imports, loaders) that has no task or object yet. The
 * clock goes on from where it was plus slept_ms, and what fell due during
 * the sleep is delivered at once. A stream that is cut, of another format
 * or that does not agree with itself restores nothing: an error, and the
 * kernel is as before. A task whose module was not found or changed, whose
 * open file is gone or changed, or whose device the firmware no longer
 * registers under its name or cannot set up, ends as a task that traps
 * (OS_TASK_EXIT_WASM_ERROR, its error text "restore: <why>"); every other
 * task goes on. The host finds its tasks and objects by their ids, which
 * are those they had.
 */
typedef OsStatus (*OsModuleFind)(void* context, const char* task_name, const uint8_t** out_bytes,
                                 uint32_t* out_size);
OsStatus os_system_save(OsSnapshotWrite write, void* context);
OsStatus os_system_restore(OsSnapshotRead read, void* context, OsModuleFind find, void* find_context,
                           uint32_t slept_ms);

/*
 * Paging moves single frames of programs' memory (4 KiB unless the
 * interpreter is built with another WM_FRAME_SIZE), so programs may
 * together have more memory than the board has RAM,
 * and a program may have more than RAM itself. With a page store, every
 * program's memory is paged: a frame comes into RAM when the program (or a
 * host call) touches it, and when RAM is full the kernel stores the frame
 * touched least recently (CLOCK) in one of the store's slots and brings it
 * back when it is touched again.
 *
 * Memory is promised only as far as it can be kept: creating a task or
 * growing its memory fails unless every frame of every program could have
 * a slot, so a program never runs out of memory it was given. A frame keeps
 * its slot once stored: one that came back and was only read leaves RAM
 * again without a write.
 *
 * The store has slots numbered from 0, each as large as a frame:
 * wm_frame_size() bytes (WM_FRAME_SIZE, 4096 unless built otherwise; with
 * 0, memory is never paged and there is no page store). ram_frames is how
 * many frames the kernel keeps in RAM (0: as many as the heap holds). Calls
 * happen from the scheduler or a host call, one at a time. Set the store
 * before creating tasks.
 */
typedef struct OsPageStore
{
    void* context;
    uint32_t slots;
    uint32_t ram_frames;
    OsStatus (*write)(void* context, uint32_t slot, const void* frame);
    OsStatus (*read)(void* context, uint32_t slot, void* frame);
    /*
     * Optional, for a store that reads in the background (DMA, another
     * thread): start reading slot into frame and return OS_STATUS_PENDING,
     * then call os_page_read_done when the read ends, also from an
     * interrupt or another thread; finish waits for a read in flight to end
     * and returns its status. Once finish has returned for a read, its
     * os_page_read_done must have come already or never come: a late one
     * could meet a new read of the same slot. Meanwhile only the program that touched the
     * frame waits, and the others run. OS_STATUS_OK: read at once;
     * OS_STATUS_BUSY: it cannot start another read now, and the kernel
     * reads the frame at once with read. Only predecoded code waits: code
     * run in place (WM_PREDECODE off, or a build too large to predecode)
     * reads frames at once.
     */
    OsStatus (*read_start)(void* context, uint32_t slot, void* frame);
    OsStatus (*finish)(void* context, uint32_t slot);
    /*
     * Optional, the same for writes: start writing frame to slot and return
     * OS_STATUS_PENDING, then call os_page_write_done when the write ends.
     * The frame leaves RAM only then, and nobody waits for it meanwhile;
     * finish waits for such a write too (it needs finish). OS_STATUS_OK:
     * written at once; OS_STATUS_BUSY: the kernel writes it at once.
     */
    OsStatus (*write_start)(void* context, uint32_t slot, const void* frame);
    /*
     * Optional: the region the frames in RAM come from (external RAM, for
     * one), os_page_ram_bytes(store) bytes. NULL: the kernel takes it from
     * the heap when the store is set. With a RAM budget (ram_frames not 0)
     * frames come only from this pool, never cut up the heap, and a
     * program's memory in one block is its frames in a row there.
     */
    void* ram;
    /*
     * Optional: slot holds nothing anyone needs any more (its frame's memory
     * went, or the frame was written elsewhere), for a store that keeps its
     * frames in memory of its own (os_page_store_compressed). Not called for
     * a slot taken again at once.
     */
    void (*drop)(void* context, uint32_t slot);
} OsPageStore;

/* A background read or write the store started has ended. */
void os_page_read_done(uint32_t slot, OsStatus status);
void os_page_write_done(uint32_t slot, OsStatus status);

/*
 * NULL turns paging off. OS_STATUS_BUSY while tasks exist. The store set
 * gives the RAM budget (ram_frames, ram) and is the only one; more can be
 * added after it.
 */
OsStatus os_page_store_set(const OsPageStore* store);

/*
 * Several stores (up to OS_PAGE_MAX_STORES, 4), as swap areas are: a dirty
 * frame goes to the store of the highest priority with a free slot, and
 * stores of equal priority take turns. Every store is named by the pointer
 * it was added with. An added store gives slots only (ram_frames 0, ram
 * NULL). Stores may be added, re-prioritized, removed and lost while tasks
 * run, between slices.
 *
 * os_page_store_priority: frames written from then on follow the new
 * order; frames already stored stay where they are.
 *
 * os_page_store_remove: the store leaves gracefully. Its frames come back
 * into RAM and go to the other stores when they leave again, and swapped
 * tasks' states on it are copied to them. OS_STATUS_BUSY, and nothing moved,
 * when the other stores do not have the slots for every frame promised.
 *
 * os_page_store_lost: the store is gone (a card pulled out, a cable
 * unplugged) and its frames with it. Every task with a frame or its state
 * on it ends as one that traps, with the error "page store gone"; every
 * other task goes on.
 *
 * A read that fails ends the task that needed the frame with "page store:
 * a frame could not be read".
 */
OsStatus os_page_store_add(const OsPageStore* store, uint32_t priority);
OsStatus os_page_store_priority(const OsPageStore* store, uint32_t priority);
OsStatus os_page_store_remove(const OsPageStore* store);
OsStatus os_page_store_lost(const OsPageStore* store);

/* One store: its priority, slots, slots in use, frames written to it and read from it. */
typedef struct OsPageStoreStats
{
    uint32_t priority;
    uint32_t slots;
    uint32_t used_slots;
    uint32_t stores;
    uint32_t loads;
} OsPageStoreStats;
OsStatus os_page_get_store_stats(const OsPageStore* store, OsPageStoreStats* out_stats);
/*
 * The bytes of the frame pool for a store: ram_frames frames, and a few
 * more for frames on their way out when it writes in the background (they
 * do not count against the budget until their write ends). 0 without a RAM
 * budget.
 */
uint32_t os_page_ram_bytes(const OsPageStore* store);

/* What paging holds now, across all programs. */
typedef struct OsPageStats
{
    uint32_t reserved_frames; /* frames promised to programs */
    uint32_t limit_frames;    /* how many can be promised: the slots */
    uint32_t ram_frames;      /* frames in RAM */
    uint32_t stored_frames;   /* slots in use (a frame in RAM may keep one, holding a copy) */
    uint32_t stores;          /* frames written to the store so far */
    uint32_t loads;           /* frames read back so far */
} OsPageStats;

OsStatus os_page_get_stats(OsPageStats* out_stats);

#if OS_HAS_COMPRESSED_STORE
/*
 * A page store that keeps frames compressed in RAM, in region (bytes long,
 * 4-byte aligned), as zram does (docs/decisions.md, 12): set up into
 * out_store, to add with os_page_store_add at a priority above slower
 * stores. A frame that compresses to more than three quarters of itself,
 * or finds the region full, goes to the next store. A frame takes about
 * half of itself on average (WKdm); one of a single word repeated, 128
 * bytes.
 */
OsStatus os_page_store_compressed(OsPageStore* out_store, void* region, uint32_t bytes);
#endif

#if OS_HAS_CODE_STORES
/*
 * Code stores (docs/decisions.md, 11): storage the processor reads as
 * memory (internal flash mapped as data, PSRAM) where programs' decoded
 * code is kept as code images, so it needs no RAM: a function without
 * records in RAM runs from its program's image. Added at start, before
 * programs load, and never removed. A store found more than three
 * quarters full is erased then; images are written again as programs
 * load. base stays valid and readable all the time; erase (NULL for RAM)
 * sets size bytes at offset, a multiple of erase_size, to be written
 * again; write writes data at offset, into bytes erased before.
 */
typedef struct OsCodeStore
{
    void* context;
    const uint8_t* base;
    uint32_t size;
    uint32_t erase_size;
    OsStatus (*erase)(void* context, uint32_t offset, uint32_t size);
    OsStatus (*write)(void* context, uint32_t offset, const void* data, uint32_t size);
} OsCodeStore;

#ifndef OS_CODE_MAX_STORES
#define OS_CODE_MAX_STORES 2U
#endif
/*
 * The scheduler writes an image only when no task is ready and none is due
 * sooner than this: writing one takes a while (128 ms for CoreMark's on an
 * ESP32), and a task waking meanwhile would wake late.
 */
#ifndef OS_CODE_QUIET_MS
#define OS_CODE_QUIET_MS 500U
#endif

/* A new image goes to the store with the highest priority that has room. */
OsStatus os_code_store_add(const OsCodeStore* store, uint32_t priority);
OsStatus os_code_store_priority(const OsCodeStore* store, uint32_t priority);
typedef struct OsCodeStoreStats
{
    uint32_t priority;
    uint32_t size;
    uint32_t used;   /* bytes of images and their headers */
    uint32_t images; /* images in it, of this firmware or an older one */
    uint32_t mapped; /* programs mapped from it since the store was added */
} OsCodeStoreStats;
OsStatus os_code_store_get_stats(const OsCodeStore* store, OsCodeStoreStats* out_stats);
#endif

/*
 * Where programs' stdout (fd 1) and stderr (fd 2) go. write gets the id of
 * the task that wrote, so a platform can keep a log per program, send it to
 * a UART, or both. Without a port, output goes to the C library's stdout
 * and stderr. Like the other ports, it is cleared by os_init.
 */
typedef struct OsOutputPort
{
    void* context;
    void (*write)(void* context, uint32_t task_id, uint32_t fd, const void* data, uint32_t size);
} OsOutputPort;

/* NULL restores the default. */
OsStatus os_output_port_set(const OsOutputPort* port);

/*
 * Swapping a task out frees all its RAM but the control block, at once,
 * through the page store: its state (stack, calls, globals, tables) is
 * written to the store's slots and its memory's frames go there as they
 * would one by one; the memory itself stays apart, paged. The task stays
 * where it is: it keeps waiting, its timers and handles keep working, and a
 * queue item its wait delivers goes straight into its memory. It comes back
 * from that state with the same memory, whose frames return as they are
 * touched. Any task but the one running can be swapped out, waiting or not;
 * one that has not started frees its state and starts afresh.
 * OS_STATUS_UNSUPPORTED without a page store or for linked tasks; errors
 * leave the task as it was.
 */
OsStatus os_task_swap_out(OsTaskHandle task);

/*
 * Bring a swapped-out task back now. The scheduler does this by itself before
 * the task's next slice; if that fails, the task ends with
 * OS_TASK_EXIT_WASM_ERROR and the error phase "swap_in".
 */
OsStatus os_task_swap_in(OsTaskHandle task);

uint8_t os_task_is_swapped(OsTaskHandle task);

/*
 * Sets the base priority; the scheduler always runs the highest priority
 * READY task and rotates tasks of equal priority. A mutex owner may run
 * higher while it inherits a waiter's priority (os_task_get_priority).
 */
OsStatus os_task_set_priority(
    OsTaskHandle task,
    uint32_t priority
);

uint32_t os_task_get_priority(OsTaskHandle task);
uint32_t os_task_get_base_priority(OsTaskHandle task);
uint32_t os_task_get_id(OsTaskHandle task);
OsTaskHandle os_task_find_by_id(uint32_t task_id);
uint32_t os_task_get_id_list(uint32_t* out_task_ids, uint32_t max_task_ids);
/* The name a program gives itself in its wasm-rtos.app section, or NULL. */
const char* os_task_get_app_name(OsTaskHandle task);
OsTaskState os_task_get_state(OsTaskHandle task);
OsTaskExitReason os_task_get_exit_reason(OsTaskHandle task);
uint32_t os_task_get_exit_code(OsTaskHandle task);
OsStatus os_task_get_return_value(OsTaskHandle task, OsValue* out_value);
uint32_t os_task_get_return_value_count(OsTaskHandle task);
OsStatus os_task_get_return_values(
    OsTaskHandle task,
    OsValue* out_values,
    uint32_t max_values,
    uint32_t* out_value_count
);
/*
 * RAM a task uses, in bytes. stack, linear memory and code are measured
 * exactly in every build. interpreter_bytes is everything the interpreter
 * allocated for the task (stack, memory, module tables, names, types and
 * the like) and needs OS_HAS_MEMORY_ACCOUNTING=1; otherwise it is 0 and
 * total_bytes leaves that metadata out. Predecoded code is a cache every
 * task shares: made at a function's first call, dropped when memory is
 * short; code_bytes is the part of it the task's module holds now, counted
 * in no task's interpreter_bytes.
 */
typedef struct OsTaskMemoryUsage
{
    uint32_t stack_bytes;            /* interpreter value stack */
    uint32_t stack_peak_bytes;       /* the most of it the task has used, also after it ends */
    uint32_t linear_memory_bytes;    /* the program's linear memory, with spare room from memory.grow */
    uint32_t resident_memory_bytes;  /* the part of it in RAM now: all of it, or the pages touched when paged */
    uint32_t stored_memory_bytes;    /* the part of it only a page store holds */
    uint32_t block_memory_bytes;     /* with a page store: all of it while it is one block, reached at full speed; else 0 */
    uint32_t code_bytes;             /* its module's predecoded code made so far (a cache tasks share, not in interpreter_bytes) */
    uint32_t interpreter_bytes;      /* all interpreter allocations, see above */
    uint32_t interpreter_peak_bytes; /* the most interpreter_bytes has been */
    uint32_t image_bytes;            /* kernel copy of the module; 0 when it runs in place or the host owns it */
    uint32_t swap_bytes;             /* size of its state in the page store while swapped out, else 0 */
    uint32_t kernel_bytes;           /* task control block */
    uint32_t total_bytes;
} OsTaskMemoryUsage;

OsStatus os_task_get_memory_usage(OsTaskHandle task, OsTaskMemoryUsage* out_usage);

/*
 * Copy to or from a task's linear memory at a wasm address. The whole range
 * must lie inside the memory (OS_STATUS_OUT_OF_BOUNDS), including addresses
 * near 4 GiB. A swapped-out task's memory is reached too (its frames come
 * back); one that never started has none (OS_STATUS_BUSY).
 */
OsStatus os_task_read_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    uint8_t* out_buffer,
    uint32_t byte_count
);
OsStatus os_task_write_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    const uint8_t* buffer,
    uint32_t byte_count
);
const char* os_task_get_name(OsTaskHandle task);
OsTaskHandle os_task_get_current(void);
uint32_t os_task_get_run_count(OsTaskHandle task);
/*
 * Interpreter work units the task has spent, over all its slices; 0 in a
 * build without fuel. Deterministic, unlike time, so the same program uses
 * the same fuel on every device and in the browser.
 */
uint64_t os_task_get_fuel_used(OsTaskHandle task);
OsStatus os_task_get_last_wait_status(OsTaskHandle task);

/*
 * Time is the kernel's only platform dependency, and the kernel contains no
 * platform code: a board, the browser or a test either installs a clock port
 * or drives time itself with os_tick().
 *
 * now_ms is a monotonic platform clock. arm_wakeup and cancel_wakeup must
 * either both be supplied for tickless operation or both be NULL for polling.
 * A hardware ISR should only defer work; call os_clock_wakeup from a safe
 * platform context after the interrupt or event-loop callback is delivered.
 */
OsStatus os_clock_port_set(const OsClockPort* port);
void os_clock_port_clear(void);
OsStatus os_clock_poll(void);
OsStatus os_clock_wakeup(void);
uint8_t os_clock_is_tickless(void);

const char* os_get_last_error_phase(void);
const char* os_get_last_error_result(void);
const char* os_get_last_error_task_name(void);
/*
 * Why a task ended with OS_TASK_EXIT_WASM_ERROR, kept until it is deleted:
 * "phase: result", and the functions it was in when the interpreter keeps them,
 * innermost first ("call: [trap] unreachable executed in parse < main").
 * NULL for tasks that did not fail.
 */
const char* os_task_get_error(OsTaskHandle task);
OsStatus os_get_last_error_status(void);

/* Advance kernel time by milliseconds. */
void os_tick(uint32_t milliseconds);
uint32_t os_get_tick_ms(void);
uint32_t os_get_next_wakeup_ms(void);

OsStatus os_schedule(void);
uint32_t os_get_task_count(void);
uint32_t os_get_ready_task_count(void);
uint32_t os_get_waiting_task_count(void);
uint32_t os_get_timer_count(void);

#endif
