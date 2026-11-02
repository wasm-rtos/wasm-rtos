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
    OS_OBJECT_GPIO,
    OS_OBJECT_DISPLAY,
    OS_OBJECT_INPUT,
    OS_OBJECT_I2C,
    OS_OBJECT_SPI,
    OS_OBJECT_ADC,
    OS_OBJECT_PWM,
    OS_OBJECT_UART
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
/* An output whose levels are the pin's own, whatever the board inverts; it starts high (chip selects). */
OsStatus os_gpio_open_physical(const char* name, uint32_t length, uint32_t* out_id);
/* The platform's pin number behind a GPIO id. */
uint32_t os_gpio_pin_number(uint32_t id);
/*
 * A pin for a PWM output: it must have OS_GPIO_CAN_PWM (else
 * OS_STATUS_UNSUPPORTED); it is owned like any opened pin and starts as a
 * low output. *out_inverted says whether the program's levels are the
 * pin's inverted (board and program inversion together).
 */
OsStatus os_gpio_open_pwm(const char* name, uint32_t length, uint8_t active_low, uint32_t* out_id,
                          uint8_t* out_inverted);
/* Deliver edges reported by os_gpio_edge; wakes waiting programs. */
void os_gpio_poll(void);
void os_gpio_shutdown(void);
#endif

#if OS_HAS_DISPLAY
/* ---- Displays for programs (display.c) ------------------------------------ */

/* What display-get reports; unknown properties are OS_STATUS_UNSUPPORTED. */
#define OS_DISPLAY_PROP_WIDTH      1U
#define OS_DISPLAY_PROP_HEIGHT     2U
#define OS_DISPLAY_PROP_SHAPE      3U
#define OS_DISPLAY_PROP_FORMAT     4U
#define OS_DISPLAY_PROP_FORMATS    5U
#define OS_DISPLAY_PROP_DPI        6U
#define OS_DISPLAY_PROP_BRIGHTNESS 7U

/*
 * Displays are named by id (1-based). os_display_open: OS_STATUS_FILE_NOT_FOUND
 * for an unknown name, OS_STATUS_BUSY when another program owns it.
 * Rectangles must lie inside the display and data must hold exactly the
 * rectangle (OS_STATUS_INVALID_ARGUMENT); a format the display does not
 * accept is OS_STATUS_UNSUPPORTED.
 */
OsStatus os_display_open(const char* name, uint32_t length, uint32_t* out_id);
void os_display_close(uint32_t id);
OsStatus os_display_get(uint32_t id, uint32_t property, uint32_t* out_value);
OsStatus os_display_write(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          uint32_t format, const void* pixels, uint32_t size);
OsStatus os_display_fill(uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                         uint32_t format, uint32_t color);
OsStatus os_display_flush(uint32_t id);
OsStatus os_display_set_brightness(uint32_t id, uint32_t level);
void os_display_shutdown(void);
#endif

#if OS_HAS_INPUT
/* ---- Input devices for programs (input.c) --------------------------------- */

#define OS_INPUT_PROP_KIND           1U
#define OS_INPUT_PROP_WIDTH          2U
#define OS_INPUT_PROP_HEIGHT         3U
#define OS_INPUT_PROP_STEPS_PER_TURN 4U

/* One event as programs read it: 16 bytes, little-endian. */
#define OS_INPUT_EVENT_SIZE 16U
#define OS_INPUT_EVENT_KEY     1U
#define OS_INPUT_EVENT_ROTATE  2U
#define OS_INPUT_EVENT_TOUCH   3U
#define OS_INPUT_EVENT_TEXT    4U
#define OS_INPUT_EVENT_DROPPED 5U
/* Key event values. */
#define OS_INPUT_KEY_RELEASED   0
#define OS_INPUT_KEY_PRESSED    1
#define OS_INPUT_KEY_REPEATED   2
#define OS_INPUT_KEY_LONG_PRESS 3

/* Devices are named by id (1-based), like displays. */
OsStatus os_input_open(const char* name, uint32_t length, uint32_t* out_id);
void os_input_close(uint32_t id);
OsStatus os_input_get(uint32_t id, uint32_t property, uint32_t* out_value);
/* Move up to max_events events into out (OS_INPUT_EVENT_SIZE bytes each); returns how many. */
uint32_t os_input_read(uint32_t id, uint8_t* out, uint32_t max_events);
uint8_t os_input_ready(uint32_t id);
/* Long presses and repeats that are due; the next one, in ms from now (OS_WAIT_FOREVER: none). */
void os_input_update_time(void);
uint32_t os_input_next_deadline_ms(void);
void os_input_shutdown(void);
#endif

#if OS_HAS_I2C
/* ---- I2C for programs (bus.c) ---------------------------------------------- */

/*
 * Devices are named by id (1-based). os_i2c_open: OS_STATUS_FILE_NOT_FOUND
 * for an unknown bus, OS_STATUS_BUSY when another program has the address
 * open, OS_STATUS_UNSUPPORTED without a port or buses.
 */
OsStatus os_i2c_open(const char* bus_name, uint32_t length, uint32_t address, uint32_t* out_id);
void os_i2c_close(uint32_t id);
OsStatus os_i2c_transaction(uint32_t id, const OsI2cOp* ops, uint32_t count);
void os_i2c_shutdown(void);
#endif

#if OS_HAS_SPI
/* ---- SPI for programs (bus.c) ---------------------------------------------- */

/*
 * A cs of length 0 takes the whole bus: the program drives chip select
 * itself. Otherwise the kernel claims the cs pin and drives it around every
 * transaction, and the bus is shared. OS_STATUS_BUSY when that conflicts.
 */
OsStatus os_spi_open(const char* bus_name, uint32_t bus_length, const char* cs, uint32_t cs_length,
                     uint32_t mode, uint32_t frequency_hz, uint32_t* out_id);
void os_spi_close(uint32_t id);
OsStatus os_spi_transaction(uint32_t id, const OsSpiOp* ops, uint32_t count);
void os_spi_shutdown(void);
#endif

#if OS_HAS_ADC
/* ---- Analog inputs for programs (adc.c) ------------------------------------ */
#define OS_ADC_PROP_RESOLUTION 1U
#define OS_ADC_PROP_MAX        2U
#define OS_ADC_PROP_FULL_SCALE 3U
OsStatus os_adc_open(const char* name, uint32_t length, uint32_t* out_id);
OsStatus os_adc_read(uint32_t id, uint32_t* out_raw);
OsStatus os_adc_read_mv(uint32_t id, uint32_t* out_mv);
OsStatus os_adc_get(uint32_t id, uint32_t property, uint32_t* out_value);
void os_adc_shutdown(void);
#endif

#if OS_HAS_PWM
/* ---- PWM outputs for programs (pwm.c) -------------------------------------- */
#define OS_PWM_PROP_FREQUENCY 1U
#define OS_PWM_PROP_DUTY      2U
#define OS_PWM_ACTIVE_LOW     0x01U
/* Duty as the program sees it: 0 off .. 65535 fully on. */
OsStatus os_pwm_open(const char* name, uint32_t length, uint32_t frequency_hz, uint32_t flags, uint32_t* out_id);
OsStatus os_pwm_set_duty(uint32_t id, uint32_t duty);
OsStatus os_pwm_set_frequency(uint32_t id, uint32_t frequency_hz);
OsStatus os_pwm_get(uint32_t id, uint32_t property, uint32_t* out_value);
void os_pwm_close(uint32_t id);
void os_pwm_shutdown(void);
#endif

#if OS_HAS_UART
/* ---- Serial ports for programs (uart.c) ------------------------------------ */
#define OS_UART_PROP_BAUD      1U
#define OS_UART_PROP_AVAILABLE 2U
#define OS_UART_PROP_ERRORS    3U
OsStatus os_uart_open(const char* name, uint32_t length, uint32_t baud, uint32_t format, uint32_t* out_id);
void os_uart_close(uint32_t id);
uint32_t os_uart_read(uint32_t id, uint8_t* out, uint32_t max);
OsStatus os_uart_write(uint32_t id, const uint8_t* data, uint32_t size, uint32_t* out_written);
/* 0 when the port can take bytes again; starts a wait otherwise (OS_STATUS_PENDING). */
OsStatus os_uart_wait_writable(uint32_t id, uint32_t timeout_ms);
OsStatus os_uart_get(uint32_t id, uint32_t property, uint32_t* out_value);
uint8_t os_uart_ready(uint32_t id);
/* Deliver what ports reported from interrupt handlers. */
void os_uart_poll(void);
void os_uart_shutdown(void);
#endif

#if OS_HAS_I2C || OS_HAS_SPI
void os_bus_shutdown(void);
/* Deliver background transfers that have ended. */
void os_bus_poll(void);
/* End the background transfer task_id waits for, at once (the port's finish). */
void os_bus_finish_task(uint32_t task_id);
/* The current task waits for its background transfer (os.c). */
OsStatus os_bus_wait_begin(void);
/* The transfer of task_id ended with status; wakes it (os.c). */
void os_bus_wait_end(uint32_t task_id, OsStatus status);
#endif

#endif
