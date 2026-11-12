#include "os_kernel.h"

#if OS_HAS_MEMORY_ACCOUNTING

/* Where interpreter allocations are charged now (NULL: nowhere). */
OsMemoryAccount* g_os_memory_account;

static void* os_vm_allocate(void* pointer, size_t size)
{
    OsMemoryHeader* header = pointer != NULL ? (OsMemoryHeader*)pointer - 1 : NULL;
    OsMemoryAccount* account = header != NULL ? header->info.account : g_os_memory_account;
    size_t old_size = header != NULL ? header->info.size : 0U;
    OsMemoryHeader* moved = NULL;

    if (size == 0U)
    {
        if (account != NULL)
        {
            account->bytes -= old_size;
        }
        free(header);
        return NULL;
    }
    moved = (OsMemoryHeader*)realloc(header, sizeof(OsMemoryHeader) + size);
    if (moved == NULL)
    {
        return NULL;
    }
    moved->info.size = size;
    moved->info.account = account;
    if (account != NULL)
    {
        account->bytes = account->bytes - old_size + size;
        if (account->bytes > account->peak)
        {
            account->peak = account->bytes;
        }
    }
    return moved + 1;
}

/* Predecoded records are a cache every task shares and none owns (wm_set_code_allocator): charged to none. */
static void* os_vm_code_allocate(void* pointer, size_t size)
{
    OsMemoryAccount* account = g_os_memory_account;
    void* result;

    g_os_memory_account = NULL;
    result = os_vm_allocate(pointer, size);
    g_os_memory_account = account;
    return result;
}
#endif

static OsHostFunctionNode* g_os_host_functions;

const char os_host_out_of_bounds[] = "trap: out of bounds memory access";
/* WASI proc_exit ends a program with this trap. */
const char os_exit_trap[] = "trap: exit";

int os_name_is(const char* text, uint32_t length, const char* name)
{
    return strlen(name) == length && memcmp(text, name, length) == 0;
}

static uint8_t os_signature_type(char letter)
{
    switch (letter)
    {
        case 'i': case '*': return WM_I32;
        case 'I': return WM_I64;
        case 'f': return WM_F32;
        case 'F': return WM_F64;
        default: return 0U;
    }
}

WmResult os_host_function(const char* signature, OsHostImportFunction function, WmFunc** out)
{
    OsHostFunctionNode* node = NULL;
    uint8_t params[16];
    uint8_t results[1];
    uint32_t param_count = 0U;
    uint32_t result_count = 0U;
    const char* at = signature;

    *out = NULL;
    for (node = g_os_host_functions; node != NULL; node = node->next)
    {
        if (node->function == function && strcmp(node->signature, signature) == 0)
        {
            *out = node->func;
            return NULL;
        }
    }

    /* "r(ppp)": one result letter or v, then the parameters. */
    if (*at != 'v')
    {
        results[0] = os_signature_type(*at);
        if (results[0] == 0U)
        {
            return "invalid host function signature";
        }
        result_count = 1U;
    }
    if (*++at != '(')
    {
        return "invalid host function signature";
    }
    for (++at; *at != ')'; ++at)
    {
        if (param_count == sizeof(params) || os_signature_type(*at) == 0U)
        {
            return "invalid host function signature";
        }
        params[param_count++] = os_signature_type(*at);
    }

    node = (OsHostFunctionNode*)calloc(1U, sizeof(OsHostFunctionNode));
    if (node == NULL)
    {
        return "out of memory";
    }
    {
#if OS_HAS_MEMORY_ACCOUNTING
        /* Shared by every task: charged to none. */
        OsMemoryAccount* account = g_os_memory_account;
        g_os_memory_account = NULL;
#endif
        node->func = wm_host_function_new(params, param_count, results, result_count, function, NULL);
#if OS_HAS_MEMORY_ACCOUNTING
        g_os_memory_account = account;
#endif
    }
    if (node->func == NULL)
    {
        free(node);
        return "out of memory";
    }
    node->signature = (char*)malloc(strlen(signature) + 1U);
    if (node->signature == NULL)
    {
        wm_host_function_free(node->func);
        free(node);
        return "out of memory";
    }
    strcpy(node->signature, signature);
    node->function = function;
    node->next = g_os_host_functions;
    g_os_host_functions = node;
    *out = node->func;
    return NULL;
}

static void os_host_functions_free(void)
{
    while (g_os_host_functions != NULL)
    {
        OsHostFunctionNode* next = g_os_host_functions->next;
        wm_host_function_free(g_os_host_functions->func);
        free(g_os_host_functions->signature);
        free(g_os_host_functions);
        g_os_host_functions = next;
    }
}

OsState g_os;

uint32_t os_guest_args(const char* const** out_argv)
{
    static const char* default_argv[1];
    OsTaskHandle task = g_os.current_task;

#if OS_HAS_PROCESSES
    if (task != NULL && task->argv != NULL && task->argc > 0U)
    {
        *out_argv = (const char* const*)task->argv;
        return task->argc;
    }
#endif
    default_argv[0] = task != NULL ? task->name : "";
    *out_argv = default_argv;
    return 1U;
}

WmResult os_guest_exit(uint32_t code)
{
    if (g_os.current_task != NULL)
    {
        g_os.current_task->wasi_exit_code = code;
    }
    return os_exit_trap;
}
static char* os_duplicate_string(const char* value);
static void os_host_import_free_list(void);
static int os_host_import_exists(const char* module_name, const char* import_name);
#if OS_HAS_DYLINK
static void os_wasm_library_free_all(void);
static WmResult os_dylink_find(void* context, const char* name, WmModule** out);
static WmResult os_resolve_host_import(
    const WmModule* program,
    const char* module,
    uint32_t module_length,
    const char* name,
    uint32_t name_length,
    WmFunc** out
);
static WmResult os_dylink_host_import(
    void* context,
    const char* module,
    uint32_t module_length,
    const char* name,
    uint32_t name_length,
    uint8_t kind,
    WmExtern* out
);
#endif
static OsStatus os_wasm_result_to_status(WmResult result);
static OsStatus os_check_module_imports(OsTaskHandle task, const struct OsImportLink* link);
static const char* os_detect_entry_function(const WmInstance* instance);
static OsStatus os_validate_entry_function(OsTaskHandle task);
static OsStatus os_task_memory(OsTaskHandle task, WmMemory** out_memory);
static uint32_t os_task_linear_memory_size(OsTaskHandle task);
static uint32_t os_task_resident_memory_size(OsTaskHandle task);

OsStatus os_init(void)
{
    os_shutdown();

    g_os.next_task_id = 1U;
    g_os.next_queue_id = 1U;
    g_os.next_mutex_id = 1U;
    g_os.next_semaphore_id = 1U;
    g_os.next_event_group_id = 1U;
    g_os.next_timer_id = 1U;
#if OS_HAS_DYLINK
    g_os.next_wasm_library_id = 1U;
#endif
    g_os.next_wait_sequence = 1U;
#if OS_HAS_PROCESSES
    g_os.spawned_task_count = 0U;
#endif
#if OS_HAS_MEMORY_ACCOUNTING
    wm_set_allocator(os_vm_allocate);
    wm_set_code_allocator(os_vm_code_allocate);
#endif
    g_os.initialized = 1U;
    os_clear_last_error();

    return OS_STATUS_OK;
}

void os_shutdown(void)
{
    OsTaskHandle task = g_os.task_list;

    if (g_os.clock_wakeup_armed && g_os.clock_port.cancel_wakeup != NULL)
    {
        g_os.clock_port.cancel_wakeup(g_os.clock_port.context);
    }

    while (task != NULL)
    {
        OsTaskHandle next = task->next;
        os_task_free(task);
        task = next;
    }

#if OS_HAS_DYLINK
    os_wasm_library_free_all();
#endif

    os_queue_free_all();
    os_mutex_free_all();
    os_semaphore_free_all();
    os_event_group_free_all();
    os_timer_free_all();
    os_host_import_free_list();
    os_host_functions_free();
#if OS_HAS_FS
    os_fs_shutdown();
#endif
    os_drivers_shutdown();
    os_page_shutdown();

    memset(&g_os, 0, sizeof(g_os));
}

OsStatus os_host_import_register(
    const char* module_name,
    const char* import_name,
    const char* signature,
    OsHostImportFunction function
)
{
    OsHostImportNode* node = NULL;

    if (module_name == NULL || module_name[0] == '\0' ||
        import_name == NULL || import_name[0] == '\0' ||
        signature == NULL || signature[0] == '\0' ||
        function == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (!g_os.initialized)
    {
        return OS_STATUS_ERROR;
    }

    if (os_host_import_exists(module_name, import_name))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    node = (OsHostImportNode*)calloc(1U, sizeof(OsHostImportNode));
    if (node == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    node->module_name = os_duplicate_string(module_name);
    node->import_name = os_duplicate_string(import_name);
    node->signature = os_duplicate_string(signature);
    node->function = function;

    if (node->module_name == NULL || node->import_name == NULL || node->signature == NULL)
    {
        free(node->module_name);
        free(node->import_name);
        free(node->signature);
        free(node);
        return OS_STATUS_OUT_OF_MEMORY;
    }

    node->next = g_os.host_import_list;
    g_os.host_import_list = node;

    return OS_STATUS_OK;
}

void os_host_import_clear_all(void)
{
    os_host_import_free_list();
}
#if OS_HAS_DYLINK
OsStatus os_wasm_library_register(
    OsWasmLibraryHandle* out_library,
    const char* module_name,
    uint8_t* wasm_bytes,
    uint32_t wasm_size
)
{
    OsWasmLibraryHandle library = NULL;

    if (out_library == NULL || module_name == NULL || module_name[0] == '\0' ||
        wasm_bytes == NULL || wasm_size == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    *out_library = NULL;
    if (!g_os.initialized)
    {
        OsStatus status = os_init();
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }
    if (os_wasm_library_find(module_name) != NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    library = (OsWasmLibraryHandle)calloc(1U, sizeof(struct OsWasmLibrary));
    if (library == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    library->module_name = os_duplicate_string(module_name);
    if (library->module_name == NULL)
    {
        free(library);
        return OS_STATUS_OUT_OF_MEMORY;
    }
    library->id = g_os.next_wasm_library_id++;
    library->wasm_bytes = wasm_bytes;
    library->wasm_size = wasm_size;
    library->next = g_os.wasm_library_list;
    g_os.wasm_library_list = library;
    ++g_os.wasm_library_count;
    *out_library = library;
    return OS_STATUS_OK;
}

OsWasmLibraryHandle os_wasm_library_find(const char* module_name)
{
    OsWasmLibraryHandle library = NULL;
    if (module_name == NULL)
    {
        return NULL;
    }
    for (library = g_os.wasm_library_list;
         library != NULL;
         library = library->next)
    {
        if (strcmp(library->module_name, module_name) == 0)
        {
            return library;
        }
    }
    return NULL;
}

uint32_t os_wasm_library_get_id(OsWasmLibraryHandle library)
{
    return library != NULL ? library->id : 0U;
}

const char* os_wasm_library_get_name(OsWasmLibraryHandle library)
{
    return library != NULL ? library->module_name : NULL;
}

uint32_t os_wasm_library_get_count(void)
{
    return g_os.wasm_library_count;
}

static WmResult os_dylink_find(void* context, const char* name, WmModule** out)
{
    OsWasmLibraryHandle library = os_wasm_library_find(name);

    (void)context;
    *out = NULL;
    if (library == NULL)
    {
        return "unlinkable: dylink.0 dependency is missing";
    }
    /* Each process loads its own module; only the bytes are shared. */
    return wm_module_load(out, library->wasm_bytes, library->wasm_size);
}

static WmResult os_dylink_host_import(
    void* context,
    const char* module,
    uint32_t module_length,
    const char* name,
    uint32_t name_length,
    uint8_t kind,
    WmExtern* out
)
{
    (void)context;
    out->kind = kind;
    out->as.func = NULL;
    return os_resolve_host_import(NULL, module, module_length, name, name_length, &out->as.func);
}
#endif

static uint8_t os_read_leb_u32(const uint8_t** cursor, const uint8_t* end, uint32_t* out_value)
{
    uint32_t value = 0U;
    uint32_t shift = 0U;

    while (*cursor < end && shift < 35U)
    {
        uint8_t byte = *(*cursor)++;
        value |= (uint32_t)(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0U)
        {
            *out_value = value;
            return 1U;
        }
        shift += 7U;
    }
    return 0U;
}

static uint32_t os_read_le_u32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

/*
 * Find the custom section "wasm-rtos.app": records of a key byte, a LEB128
 * length and that many bytes. Unknown keys are skipped. A module the parser
 * cannot walk is left to the interpreter to reject; a malformed section is an error.
 */
OsStatus os_read_app_metadata(const uint8_t* bytes, uint32_t size, OsAppMetadata* out_metadata)
{
    static const char name[] = "wasm-rtos.app";
    const uint8_t* cursor = bytes + 8U;
    const uint8_t* end = bytes + size;

    memset(out_metadata, 0, sizeof(*out_metadata));
    if (size < 8U)
    {
        return OS_STATUS_OK;
    }

    while (cursor < end)
    {
        uint8_t id = *cursor++;
        uint32_t section_size = 0U;
        const uint8_t* section_end = NULL;
        uint32_t name_length = 0U;

        if (!os_read_leb_u32(&cursor, end, &section_size) || section_size > (uint32_t)(end - cursor))
        {
            return OS_STATUS_OK;
        }
        section_end = cursor + section_size;
        if (id == 0U && os_read_leb_u32(&cursor, section_end, &name_length) &&
            name_length == sizeof(name) - 1U && name_length <= (uint32_t)(section_end - cursor) &&
            memcmp(cursor, name, name_length) == 0)
        {
            cursor += name_length;
            while (cursor < section_end)
            {
                uint8_t key = *cursor++;
                uint32_t length = 0U;

                if (!os_read_leb_u32(&cursor, section_end, &length) || length > (uint32_t)(section_end - cursor))
                {
                    return OS_STATUS_INVALID_ARGUMENT;
                }
                if (key == OS_APP_KEY_ABI_VERSION || key == OS_APP_KEY_STACK_SIZE)
                {
                    if (length != 4U)
                    {
                        return OS_STATUS_INVALID_ARGUMENT;
                    }
                    if (key == OS_APP_KEY_ABI_VERSION)
                    {
                        out_metadata->abi_version = os_read_le_u32(cursor);
                    }
                    else
                    {
                        out_metadata->stack_size = os_read_le_u32(cursor);
                    }
                }
                else if (key == OS_APP_KEY_NAME)
                {
                    out_metadata->name = cursor;
                    out_metadata->name_length = length;
                }
                cursor += length;
            }
        }
        cursor = section_end;
    }
    return OS_STATUS_OK;
}


OsStatus os_task_get_memory_usage(OsTaskHandle task, OsTaskMemoryUsage* out_usage)
{
    if (task == NULL || out_usage == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    memset(out_usage, 0, sizeof(*out_usage));
    if (task->wasm_runtime != NULL)
    {
        /* The interpreter keeps two guard slots around the stack. */
        (void)wm_runtime_stack(task->wasm_runtime, &out_usage->stack_bytes);
        out_usage->stack_bytes += 2U * (uint32_t)sizeof(uint32_t);
        out_usage->linear_memory_bytes = os_task_linear_memory_size(task);
        out_usage->resident_memory_bytes = os_task_resident_memory_size(task);
        out_usage->stored_memory_bytes = task->wasm_instance != NULL && wm_instance_memory(task->wasm_instance) != NULL
            ? wm_memory_stored(wm_instance_memory(task->wasm_instance)) : 0U;
        out_usage->block_memory_bytes = task->wasm_instance != NULL && wm_instance_memory(task->wasm_instance) != NULL
            ? wm_memory_block_frames(wm_instance_memory(task->wasm_instance)) * wm_frame_size() : 0U;
        out_usage->code_bytes = wm_module_code_bytes(task->wasm_module);
    }
    out_usage->stack_peak_bytes = os_task_stack_peak(task);
#if OS_HAS_MEMORY_ACCOUNTING
    out_usage->interpreter_bytes = (uint32_t)task->memory_account.bytes;
    out_usage->interpreter_peak_bytes = (uint32_t)task->memory_account.peak;
#endif
#if OS_HAS_PROCESSES
    if (task->owned_wasm_bytes != NULL)
    {
        out_usage->image_bytes = task->wasm_size;
    }
#if OS_HAS_FS
    if (task->image.copy != NULL)
    {
        out_usage->image_bytes = task->image.size;
    }
#endif
#endif
    out_usage->swap_bytes = task->swapped ? task->swap_image.bytes : 0U;
    if (task->swapped_memory != NULL)
    {
        (void)wm_memory_data(task->swapped_memory, &out_usage->linear_memory_bytes);
        out_usage->resident_memory_bytes = wm_memory_resident(task->swapped_memory);
        out_usage->stored_memory_bytes = wm_memory_stored(task->swapped_memory);
    }
    out_usage->kernel_bytes = (uint32_t)sizeof(struct OsTask);
    out_usage->total_bytes = out_usage->image_bytes + out_usage->kernel_bytes +
        (out_usage->interpreter_bytes != 0U
            ? out_usage->interpreter_bytes
            : out_usage->stack_bytes + out_usage->resident_memory_bytes + out_usage->code_bytes);
    return OS_STATUS_OK;
}

/* The status for what the interpreter said about a memory access. */
static OsStatus os_memory_status(WmResult result)
{
    if (result == NULL)
    {
        return OS_STATUS_OK;
    }
    return strncmp(result, "exhaustion", 10U) == 0 ? OS_STATUS_OUT_OF_MEMORY : OS_STATUS_OUT_OF_BOUNDS;
}

OsStatus os_task_read_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    uint8_t* out_buffer,
    uint32_t byte_count
)
{
    WmMemory* memory = NULL;
    OsStatus status = OS_STATUS_OK;

    if (byte_count > 0U && out_buffer == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_task_memory(task, &memory);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (memory == NULL)
    {
        return byte_count == 0U && wasm_address == 0U ? OS_STATUS_OK : OS_STATUS_OUT_OF_BOUNDS;
    }
    return os_memory_status(wm_memory_read(memory, wasm_address, out_buffer, byte_count));
}

OsStatus os_task_write_memory(
    OsTaskHandle task,
    uint32_t wasm_address,
    const uint8_t* buffer,
    uint32_t byte_count
)
{
    WmMemory* memory = NULL;
    OsStatus status = OS_STATUS_OK;

    if (byte_count > 0U && buffer == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_task_memory(task, &memory);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (memory == NULL)
    {
        return byte_count == 0U && wasm_address == 0U ? OS_STATUS_OK : OS_STATUS_OUT_OF_BOUNDS;
    }
    return os_memory_status(wm_memory_write(memory, wasm_address, buffer, byte_count));
}

const char* os_get_last_error_phase(void)
{
    return g_os.last_error.phase != NULL ? g_os.last_error.phase : "none";
}

const char* os_get_last_error_result(void)
{
    return g_os.last_error.result;
}

static void os_text_append(char* text, size_t* used, const char* part)
{
    while (part != NULL && *part != '\0' && *used + 1U < OS_TASK_ERROR_MAX)
    {
        text[(*used)++] = *part++;
    }
    text[*used] = '\0';
}

/*
 * Keep why a task failed: the phase and result of the last error and, when
 * the interpreter keeps them, the functions it was in, innermost first.
 */
void os_task_record_error(OsTaskHandle task)
{
    char text[OS_TASK_ERROR_MAX];
    size_t used = 0U;
    char* copy = NULL;

    text[0] = '\0';
    os_text_append(text, &used, g_os.last_error.phase);
    os_text_append(text, &used, ": ");
    os_text_append(text, &used, g_os.last_error.result != NULL ? g_os.last_error.result : "failed");
    if (task->wasm_runtime != NULL)
    {
        /* The functions the trap was in, innermost first, when the interpreter keeps them. */
        WmFunc* functions[OS_TRACE_FRAMES];
        uint32_t count = wm_trace(task->wasm_runtime, functions, OS_TRACE_FRAMES);
        const char* separator = " in ";

        for (uint32_t index = 0U; index < count && index < OS_TRACE_FRAMES; ++index)
        {
            uint32_t length = 0U;
            const char* name = wm_func_name(functions[index], &length);
            char copy[48];

            if (name == NULL || length == 0U)
            {
                name = "?";
                length = 1U;
            }
            if (length >= sizeof(copy))
            {
                length = sizeof(copy) - 1U;
            }
            memcpy(copy, name, length);
            copy[length] = '\0';
            os_text_append(text, &used, separator);
            os_text_append(text, &used, copy);
            separator = " < ";
        }
        if (count > OS_TRACE_FRAMES)
        {
            os_text_append(text, &used, " < ...");
        }
    }
    copy = (char*)malloc(used + 1U);
    if (copy != NULL)
    {
        memcpy(copy, text, used + 1U);
    }
    free(task->error_text);
    task->error_text = copy;
}

const char* os_task_get_error(OsTaskHandle task)
{
    return task != NULL && os_task_is_in_list(task) ? task->error_text : NULL;
}

const char* os_get_last_error_task_name(void)
{
    return g_os.last_error.has_task_name ? g_os.last_error.task_name : NULL;
}

OsStatus os_get_last_error_status(void)
{
    return g_os.last_error.status;
}

static char* os_duplicate_string(const char* value)
{
    size_t length = 0U;
    char* copy = NULL;

    if (value == NULL)
    {
        return NULL;
    }

    length = strlen(value);
    copy = (char*)malloc(length + 1U);
    if (copy == NULL)
    {
        return NULL;
    }

    memcpy(copy, value, length + 1U);
    return copy;
}

static void os_host_import_free_list(void)
{
    OsHostImportNode* node = g_os.host_import_list;

    while (node != NULL)
    {
        OsHostImportNode* next = node->next;
        free(node->module_name);
        free(node->import_name);
        free(node->signature);
        free(node);
        node = next;
    }

    g_os.host_import_list = NULL;
}
#if OS_HAS_DYLINK
static void os_wasm_library_free_all(void)
{
    OsWasmLibraryHandle library = g_os.wasm_library_list;
    while (library != NULL)
    {
        OsWasmLibraryHandle next = library->next;
        free(library->module_name);
        free(library);
        library = next;
    }
    g_os.wasm_library_list = NULL;
    g_os.wasm_library_count = 0U;
}
#endif

static int os_host_import_exists(const char* module_name, const char* import_name)
{
    OsHostImportNode* node = NULL;

    for (node = g_os.host_import_list; node != NULL; node = node->next)
    {
        if (strcmp(node->module_name, module_name) == 0 &&
            strcmp(node->import_name, import_name) == 0)
        {
            return 1;
        }
    }

    return 0;
}

/*
 * The host function an import names: the guest ABI first, then the file
 * system's WASI calls, the basic WASI calls, and what the embedder
 * registered. *out stays NULL when nothing answers.
 */
static WmResult os_resolve_host_import(
    const WmModule* program,
    const char* module,
    uint32_t module_length,
    const char* name,
    uint32_t name_length,
    WmFunc** out
)
{
    OsHostImportNode* node = NULL;
    WmResult result = os_abi_resolve(program, module, module_length, name, name_length, out);

    if (result != NULL || *out != NULL)
    {
        return result;
    }
#if OS_HAS_WASI
#if OS_HAS_FS
    result = os_fs_resolve_wasi(module, module_length, name, name_length, out);
    if (result != NULL || *out != NULL)
    {
        return result;
    }
#endif
    result = os_abi_resolve_wasi(module, module_length, name, name_length, out);
    if (result != NULL || *out != NULL)
    {
        return result;
    }
#endif
    for (node = g_os.host_import_list; node != NULL; node = node->next)
    {
        if ((strcmp(node->module_name, "*") == 0 || os_name_is(module, module_length, node->module_name)) &&
            os_name_is(name, name_length, node->import_name))
        {
            return os_host_function(node->signature, node->function, out);
        }
    }
    return NULL;
}

static WmResult os_resolve_import(
    void* context,
    const char* module,
    uint32_t module_length,
    const char* name,
    uint32_t name_length,
    uint8_t kind,
    WmExtern* out
)
{
    OsImportLink* link = (OsImportLink*)context;
    WmResult result = NULL;

    out->kind = kind;
    out->as.func = NULL;
    if (kind != WM_EXTERN_FUNC)
    {
        return "unlinkable: programs import only functions";
    }
    result = os_resolve_host_import(link->program, module, module_length, name, name_length, &out->as.func);
    if (result == NULL && out->as.func == NULL && link->missing++ == 0U)
    {
        link->module = module;
        link->module_length = module_length;
        link->name = name;
        link->name_length = name_length;
    }
    return result;
}

static OsStatus os_find_entry_function(OsTaskHandle task, const char* entry_function_name);

/*
 * Out of memory while making a program: the code cache's records (only a
 * speed-up, made again when called) go, and it gets one more try.
 */
static uint8_t os_retry_without_code_cache(WmResult result)
{
    if (result == NULL || strncmp(result, "exhaustion", 10) != 0 || wm_code_cache_bytes() == 0U)
    {
        return 0U;
    }
    wm_code_cache_trim(0U);
    return 1U;
}

OsStatus os_task_create_wasm(
    OsTaskHandle task,
    uint8_t* wasm_bytes,
    uint32_t wasm_size,
    const char* entry_function_name,
    uint32_t stack_size
)
{
    WmResult result = NULL;
    OsImportLink link;
    WmMemory* kept = NULL;
    uint8_t* stack = NULL;
    uint32_t stack_bytes = 0U;

    /* Frames: one per 64 bytes of stack, enough for calls that keep a few values each. */
    task->wasm_runtime = wm_runtime_new(stack_size, stack_size / 64U < 32U ? 32U : stack_size / 64U);
    if (task->wasm_runtime == NULL && wm_code_cache_bytes() != 0U)
    {
        wm_code_cache_trim(0U);
        task->wasm_runtime = wm_runtime_new(stack_size, stack_size / 64U < 32U ? 32U : stack_size / 64U);
    }
    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    stack = wm_runtime_stack(task->wasm_runtime, &stack_bytes);
    memset(stack, OS_STACK_PAINT, stack_bytes);
    task->wasm_stack_base = stack;

    result = wm_module_load(&task->wasm_module, wasm_bytes, wasm_size);
    if (os_retry_without_code_cache(result))
    {
        result = wm_module_load(&task->wasm_module, wasm_bytes, wasm_size);
    }
    if (result != NULL)
    {
        OsStatus status = os_wasm_result_to_status(result);
        task->wasm_module = NULL;
        os_set_last_error(task, "parse_module", result, status);
        return status;
    }

#if OS_HAS_DYLINK
    if (wm_module_is_dylink(task->wasm_module))
    {
        /*
         * A PIE program: load it and its dylink.0 dependencies into the
         * task's own runtime. Every process gets private library memory,
         * globals and table, like shared objects in an ELF process.
         */
        WmDylinkOptions options;

        memset(&options, 0, sizeof(options));
        options.context = task;
        options.find = os_dylink_find;
        options.host = os_dylink_host_import;
        options.stack_size = OS_DYLINK_LINEAR_STACK_SIZE;
        result = wm_dylink_load(&task->wasm_dylink, task->wasm_module, task->name, task->wasm_runtime, &options);
        if (result != NULL)
        {
            OsStatus status = os_wasm_result_to_status(result);
            os_set_last_error(task, "dylink_load", result, status);
            return status;
        }
        task->wasm_instance = wm_dylink_instance(task->wasm_dylink);
        /* Look only in the program: a library may export the same name. */
        return os_find_entry_function(task, entry_function_name);
    }
#endif

    memset(&link, 0, sizeof(link));
    link.program = task->wasm_module;
    /* Back from swap: the instance takes the memory kept apart, and the program goes on from its state. */
    kept = task->swapped_memory;
    if (kept != NULL)
    {
        result = wm_instantiate_with_memory(&task->wasm_instance, task->wasm_module, kept, os_resolve_import, &link);
        if (task->wasm_instance == NULL && os_retry_without_code_cache(result))
        {
            result = wm_instantiate_with_memory(&task->wasm_instance, task->wasm_module, kept, os_resolve_import, &link);
        }
        if (task->wasm_instance != NULL && wm_instance_memory(task->wasm_instance) == kept)
        {
            task->swapped_memory = NULL;
        }
    }
    else
    {
        result = wm_instantiate(&task->wasm_instance, task->wasm_module, NULL, os_resolve_import, &link);
        if (task->wasm_instance == NULL && os_retry_without_code_cache(result))
        {
            result = wm_instantiate(&task->wasm_instance, task->wasm_module, NULL, os_resolve_import, &link);
        }
    }
    if (result != NULL)
    {
        OsStatus status = os_wasm_result_to_status(result);
        task->wasm_instance = NULL;
        os_set_last_error(task, "load_module", result, status);
        return status;
    }

    {
        OsStatus status = os_check_module_imports(task, &link);
        if (status != OS_STATUS_OK)
        {
            return status;
        }
    }

    result = kept != NULL ? NULL : wm_instance_start(task->wasm_instance, task->wasm_runtime);
    if (result != NULL)
    {
        OsStatus status = os_wasm_result_to_status(result);
        os_set_last_error(task, "start", result, status);
        return status;
    }

    return os_find_entry_function(task, entry_function_name);
}

static OsStatus os_find_entry_function(OsTaskHandle task, const char* entry_function_name)
{
    WmExtern entry;
    WmResult result = NULL;

    if (entry_function_name == NULL || entry_function_name[0] == '\0')
    {
        entry_function_name = os_detect_entry_function(task->wasm_instance);
        if (entry_function_name == NULL)
        {
            os_set_last_error(task, "find_function",
                              "no entry point: export _start, app_main or main",
                              OS_STATUS_WASM_ERROR);
            return OS_STATUS_WASM_ERROR;
        }
        task->entry_function_name = entry_function_name;
    }

    result = wm_instance_export(task->wasm_instance, entry_function_name, (uint32_t)strlen(entry_function_name), &entry);
    if (result == NULL && entry.kind != WM_EXTERN_FUNC)
    {
        result = "function lookup failed";
    }
    if (result != NULL)
    {
        OsStatus status = os_wasm_result_to_status(result);
        os_set_last_error(task, "find_function", result, status);
        return status;
    }
    task->wasm_entry_function = entry.as.func;

    return os_validate_entry_function(task);
}

static OsValueType os_value_type(uint8_t wasm_type)
{
    switch (wasm_type)
    {
        case WM_I32: return OS_VALUE_TYPE_I32;
        case WM_I64: return OS_VALUE_TYPE_I64;
        case WM_F32: return OS_VALUE_TYPE_F32;
        case WM_F64: return OS_VALUE_TYPE_F64;
        default: return (OsValueType)-1;
    }
}

static OsStatus os_validate_entry_function(OsTaskHandle task)
{
    const uint8_t* params = NULL;
    const uint8_t* results = NULL;
    uint32_t argument_count = 0U;
    uint32_t return_count = 0U;
    uint32_t result_slots = 0U;

    if (task == NULL || task->wasm_entry_function == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    wm_func_type(task->wasm_entry_function, &params, &argument_count, &results, &return_count);

    if (argument_count != task->entry_arg_count)
    {
        os_set_last_error(task, "validate_entry_signature", "entry argument count mismatch", OS_STATUS_INVALID_ARGUMENT);
        return OS_STATUS_INVALID_ARGUMENT;
    }

    for (uint32_t argument_index = 0U; argument_index < argument_count; ++argument_index)
    {
        OsValueType wasm_type = os_value_type(params[argument_index]);

        if (wasm_type == (OsValueType)-1)
        {
            os_set_last_error(task, "validate_entry_signature", "unsupported entry argument type", OS_STATUS_UNSUPPORTED);
            return OS_STATUS_UNSUPPORTED;
        }
        if (wasm_type != task->entry_args[argument_index].type)
        {
            os_set_last_error(task, "validate_entry_signature", "entry argument type mismatch", OS_STATUS_INVALID_ARGUMENT);
            return OS_STATUS_INVALID_ARGUMENT;
        }
    }

    os_task_free_return_values(task);
    free(task->wasm_results);
    task->wasm_results = NULL;
    if (return_count == 0U)
    {
        return OS_STATUS_OK;
    }

    for (uint32_t return_index = 0U; return_index < return_count; ++return_index)
    {
        if (os_value_type(results[return_index]) == (OsValueType)-1)
        {
            os_set_last_error(task, "validate_entry_signature", "unsupported entry return type", OS_STATUS_WASM_ERROR);
            return OS_STATUS_WASM_ERROR;
        }
        result_slots += wm_type_slots(results[return_index]);
    }

    task->entry_return_values = (OsValue*)calloc(return_count, sizeof(OsValue));
    task->wasm_results = (uint32_t*)calloc(result_slots, sizeof(uint32_t));
    if (task->entry_return_values == NULL || task->wasm_results == NULL)
    {
        os_task_free_return_values(task);
        free(task->wasm_results);
        task->wasm_results = NULL;
        return OS_STATUS_OUT_OF_MEMORY;
    }

    task->entry_return_count = return_count;
    task->entry_return_code = 0U;
    for (uint32_t return_index = 0U; return_index < return_count; ++return_index)
    {
        task->entry_return_values[return_index].type = os_value_type(results[return_index]);
    }

    return OS_STATUS_OK;
}

/* A value's slots: one for 32-bit types, two for 64-bit ones. */
static uint32_t os_value_slots(const OsValue* value)
{
    return value->type == OS_VALUE_TYPE_I64 || value->type == OS_VALUE_TYPE_F64 ? 2U : 1U;
}

OsStatus os_capture_entry_return_value(OsTaskHandle task)
{
    uint32_t slot = 0U;

    if (task == NULL || task->wasm_entry_function == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (task->entry_return_count == 0U)
    {
        task->entry_return_code = 0U;
        return OS_STATUS_OK;
    }

    for (uint32_t return_index = 0U; return_index < task->entry_return_count; ++return_index)
    {
        OsValue* return_value = &task->entry_return_values[return_index];
        uint32_t slots = os_value_slots(return_value);

        memcpy(&return_value->value, task->wasm_results + slot, slots * sizeof(uint32_t));
        slot += slots;
    }

    if (task->entry_return_values[0].type == OS_VALUE_TYPE_I32)
    {
        task->entry_return_code = task->entry_return_values[0].value.i32;
    }
    else
    {
        task->entry_return_code = 0U;
    }

    return OS_STATUS_OK;
}

OsStatus os_call_entry_function(OsTaskHandle task, WmResult* out_result)
{
    uint32_t* arguments = NULL;
    uint32_t slot = 0U;

    if (task == NULL || task->wasm_entry_function == NULL || out_result == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (task->entry_arg_count > 0U)
    {
        arguments = (uint32_t*)calloc((size_t)task->entry_arg_count * 2U, sizeof(uint32_t));
        if (arguments == NULL)
        {
            *out_result = "entry argument allocation failed";
            return OS_STATUS_OUT_OF_MEMORY;
        }

        for (uint32_t argument_index = 0U; argument_index < task->entry_arg_count; ++argument_index)
        {
            const OsValue* argument = &task->entry_args[argument_index];
            uint32_t slots = os_value_slots(argument);

            memcpy(arguments + slot, &argument->value, slots * sizeof(uint32_t));
            slot += slots;
        }
    }

    *out_result = wm_call(task->wasm_runtime, task->wasm_entry_function, arguments, task->wasm_results);
    free(arguments);
    return OS_STATUS_OK;
}

/* The size of the task's linear memory, 0 without one. */
static uint32_t os_task_linear_memory_size(OsTaskHandle task)
{
    WmMemory* memory = task->wasm_instance != NULL ? wm_instance_memory(task->wasm_instance) : NULL;
    uint32_t size = 0U;

    if (memory != NULL)
    {
        (void)wm_memory_data(memory, &size);
    }
    return size;
}

/* The part of the task's linear memory in RAM, 0 without one. */
static uint32_t os_task_resident_memory_size(OsTaskHandle task)
{
    WmMemory* memory = task->wasm_instance != NULL ? wm_instance_memory(task->wasm_instance) : NULL;

    return memory != NULL ? wm_memory_resident(memory) : 0U;
}

/* The task's linear memory to reach now; NULL (and OS_STATUS_OK) when the program has none. */
static OsStatus os_task_memory(OsTaskHandle task, WmMemory** out_memory)
{
    if (task == NULL || out_memory == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    *out_memory = NULL;

    if (!os_task_is_in_list(task))
    {
        return OS_STATUS_TASK_NOT_FOUND;
    }

    /* A swapped-out task keeps its memory, paged: reaching it brings its frames back. */
    if (task->swapped)
    {
        *out_memory = task->swapped_memory;
        return task->swapped_memory != NULL ? OS_STATUS_OK : OS_STATUS_BUSY;
    }

    if (task->wasm_runtime == NULL)
    {
        return OS_STATUS_TASK_DEAD;
    }

    *out_memory = task->wasm_instance != NULL ? wm_instance_memory(task->wasm_instance) : NULL;
    return OS_STATUS_OK;
}

/* The wasm address of a host pointer into the task's one-block memory; a paged memory has none. */
OsStatus os_task_get_memory_address(
    OsTaskHandle task,
    const void* memory,
    uint32_t byte_count,
    uint32_t* out_wasm_address
)
{
    WmMemory* wasm_memory = NULL;
    uint8_t* data = NULL;
    uint32_t memory_size = 0U;
    uintptr_t memory_address = (uintptr_t)memory;
    uintptr_t offset = 0U;
    OsStatus status = OS_STATUS_OK;

    if (task == NULL || memory == NULL || out_wasm_address == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    *out_wasm_address = 0U;

    status = os_task_memory(task, &wasm_memory);
    if (status != OS_STATUS_OK)
    {
        return status == OS_STATUS_TASK_DEAD ? OS_STATUS_OUT_OF_BOUNDS : status;
    }
    data = wasm_memory != NULL ? wm_memory_data(wasm_memory, &memory_size) : NULL;
    if (data == NULL || memory_address < (uintptr_t)data)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    offset = memory_address - (uintptr_t)data;
    if (offset > (uintptr_t)memory_size ||
        byte_count > memory_size - (uint32_t)offset)
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }

    *out_wasm_address = (uint32_t)offset;
    return OS_STATUS_OK;
}

void os_task_cleanup_wasm(OsTaskHandle task)
{
    if (task == NULL)
    {
        return;
    }

    task->wasm_started = 0U;
    task->wasm_needs_resume = 0U;
    task->wasm_entry_function = NULL;

#if OS_HAS_DYLINK
    if (task->wasm_dylink != NULL)
    {
        wm_dylink_free(task->wasm_dylink);
        task->wasm_dylink = NULL;
        task->wasm_instance = NULL;
    }
#endif
    wm_instance_free(task->wasm_instance);
    task->wasm_instance = NULL;
    wm_module_free(task->wasm_module);
    task->wasm_module = NULL;
    if (task->wasm_runtime != NULL)
    {
        (void)os_task_stack_peak(task);
        wm_runtime_free(task->wasm_runtime);
        task->wasm_runtime = NULL;
    }
    task->wasm_stack_base = NULL;
    free(task->wasm_results);
    task->wasm_results = NULL;
}

/* Reject a program whose function imports are not all answered, before any of its code runs. */
static OsStatus os_check_module_imports(OsTaskHandle task, const OsImportLink* link)
{
    char* detail = g_os.last_error.detail;
    const uint32_t detail_size = (uint32_t)sizeof(g_os.last_error.detail);
    uint32_t length = 0U;
    const char* parts[3];
    uint32_t part_lengths[3];
    uint32_t part_index = 0U;

    if (link->missing == 0U)
    {
        return OS_STATUS_OK;
    }

    parts[0] = link->module;
    part_lengths[0] = link->module_length;
    parts[1] = ".";
    part_lengths[1] = 1U;
    parts[2] = link->name;
    part_lengths[2] = link->name_length;
    for (const char* prefix = "missing import "; *prefix != '\0' && length + 1U < detail_size; ++prefix)
    {
        detail[length++] = *prefix;
    }
    for (part_index = 0U; part_index < 3U; ++part_index)
    {
        for (uint32_t at = 0U; at < part_lengths[part_index] && length + 1U < detail_size; ++at)
        {
            detail[length++] = parts[part_index][at];
        }
    }
    detail[length] = '\0';

    if (link->missing > 1U)
    {
        /* Append " (+N more)" when it fits. */
        char suffix[24];
        uint32_t suffix_length = 0U;
        uint32_t remaining = link->missing - 1U;
        char digits[11];
        uint32_t digit_count = 0U;

        do
        {
            digits[digit_count++] = (char)('0' + (remaining % 10U));
            remaining /= 10U;
        } while (remaining != 0U && digit_count < (uint32_t)sizeof(digits));

        suffix[suffix_length++] = ' ';
        suffix[suffix_length++] = '(';
        suffix[suffix_length++] = '+';
        while (digit_count > 0U)
        {
            suffix[suffix_length++] = digits[--digit_count];
        }
        for (const char* text = " more)"; *text != '\0'; ++text)
        {
            suffix[suffix_length++] = *text;
        }
        if (length + suffix_length < detail_size)
        {
            memcpy(detail + length, suffix, suffix_length);
            detail[length + suffix_length] = '\0';
        }
    }

    os_set_last_error(task, "check_imports", detail, OS_STATUS_WASM_ERROR);
    return OS_STATUS_WASM_ERROR;
}

/*
 * Pick the entry point the way a loader picks an executable's: a WASI command
 * (_start) first, then the wasm-rtos task convention, then a bare main.
 */
static const char* os_detect_entry_function(const WmInstance* instance)
{
    static const char* const candidates[] = { "_start", "app_main", "main" };
    uint32_t candidate_index = 0U;

    if (instance == NULL)
    {
        return NULL;
    }

    for (candidate_index = 0U;
         candidate_index < (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
         ++candidate_index)
    {
        WmExtern entry;
        const char* name = candidates[candidate_index];
        if (wm_instance_export(instance, name, (uint32_t)strlen(name), &entry) == NULL &&
            entry.kind == WM_EXTERN_FUNC)
        {
            return name;
        }
    }

    return NULL;
}

static OsStatus os_wasm_result_to_status(WmResult result)
{
    if (result == NULL)
    {
        return OS_STATUS_OK;
    }

    return OS_STATUS_WASM_ERROR;
}

OsStatus os_wasm_snapshot_result_to_status(WmResult result)
{
    if (result == NULL)
    {
        return OS_STATUS_OK;
    }

    if (strncmp(result, "unsupported:", 12U) == 0)
    {
        return OS_STATUS_UNSUPPORTED;
    }

    if (result == wm_snapshot_invalid)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    if (strncmp(result, "exhaustion:", 11U) == 0)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }

    return OS_STATUS_WASM_ERROR;
}

uint8_t os_time_reached(uint32_t now, uint32_t target)
{
    return ((int32_t)(now - target) >= 0) ? 1U : 0U;
}

void os_clear_last_error(void)
{
    g_os.last_error.status = OS_STATUS_OK;
    g_os.last_error.result = NULL;
    g_os.last_error.phase = NULL;
    g_os.last_error.task_name[0] = '\0';
    g_os.last_error.has_task_name = 0U;
}

void os_set_last_error(OsTaskHandle task, const char* phase, WmResult result, OsStatus status)
{
    g_os.last_error.status = status;
    g_os.last_error.result = result;
    g_os.last_error.phase = phase != NULL ? phase : "none";
    g_os.last_error.task_name[0] = '\0';
    g_os.last_error.has_task_name = 0U;

    if (task != NULL && task->name[0] != '\0')
    {
        strncpy(g_os.last_error.task_name, task->name, OS_TASK_NAME_MAX - 1U);
        g_os.last_error.task_name[OS_TASK_NAME_MAX - 1U] = '\0';
        g_os.last_error.has_task_name = 1U;
    }
}

/* ---------------------------------------------------------------------------
 * Support for the guest ABI layer (abi.c): guest calls, object names and
 * references, counting timers and waiting for any of several objects.
 * ------------------------------------------------------------------------ */

/*
 * An import that may block names its result slot first. Some waits clear the
 * previous wait state when they start, so the slot is attached again once
 * the import knows whether the task blocks.
 */
OsStatus os_import_wait_begin(uint32_t* raw_return)
{
    OsStatus status = os_prepare_wait_return(g_os.current_task, raw_return);

    g_os.import_return_slot = status == OS_STATUS_OK ? raw_return : NULL;
    return status;
}

WmResult os_import_wait_end(uint8_t abi)
{
    OsTaskHandle task = g_os.current_task;
    uint32_t* slot = g_os.import_return_slot;

    g_os.import_return_slot = NULL;
    if (task != NULL && task->state == OS_TASK_WAITING && task->wait_type != OS_TASK_WAIT_NONE && slot != NULL &&
        os_prepare_wait_return(task, slot) == OS_STATUS_OK)
    {
        if (abi)
        {
            /* The wakeup writes the value or a negative ABI error. */
            task->wait_return_kind = OS_WAIT_RETURN_ABI;
        }
        /* Fuel is 0: the program stops when the import returns. */
        return NULL;
    }
    os_clear_wait_return(task);
    return NULL;
}

OsStatus os_guest_begin(uint32_t* raw_return)
{
    return os_import_wait_begin(raw_return);
}

WmResult os_guest_end(void)
{
    return os_import_wait_end(1U);
}

OsStatus os_guest_check(uint32_t address, uint32_t length)
{
    WmMemory* memory = NULL;
    OsStatus status = os_task_memory(g_os.current_task, &memory);

    if (status != OS_STATUS_OK)
    {
        return status;
    }
    return os_host_memory_in_bounds(memory, address, length) ? OS_STATUS_OK : OS_STATUS_OUT_OF_BOUNDS;
}

/*
 * What an access to the current task's memory in bounds came to: a failure
 * there (a paged memory whose frame could not come back) also ends the
 * program's call with that trap once the host function returns, as if the
 * program had touched the frame itself.
 */
static OsStatus os_guest_fault(WmResult result)
{
    if (result != NULL && g_os.current_task != NULL && g_os.current_task->wasm_runtime != NULL)
    {
        (void)wm_trap(g_os.current_task->wasm_runtime, result);
    }
    return os_memory_status(result);
}

OsStatus os_guest_read(uint32_t address, void* out, uint32_t length)
{
    WmMemory* memory = NULL;
    OsStatus status = os_task_memory(g_os.current_task, &memory);

    if (status != OS_STATUS_OK || memory == NULL)
    {
        return status != OS_STATUS_OK || length != 0U || address != 0U ? OS_STATUS_OUT_OF_BOUNDS : OS_STATUS_OK;
    }
    if (!os_host_memory_in_bounds(memory, address, length))
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }
    return os_guest_fault(wm_memory_read(memory, address, out, length));
}

OsStatus os_guest_write(uint32_t address, const void* data, uint32_t length)
{
    WmMemory* memory = NULL;
    OsStatus status = os_task_memory(g_os.current_task, &memory);

    if (status != OS_STATUS_OK || memory == NULL)
    {
        return status != OS_STATUS_OK || length != 0U || address != 0U ? OS_STATUS_OUT_OF_BOUNDS : OS_STATUS_OK;
    }
    if (!os_host_memory_in_bounds(memory, address, length))
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }
    return os_guest_fault(wm_memory_write(memory, address, data, length));
}

/* A task's frames of a range go back to the page store's choosing, if the task is still there. */
static void os_guest_unpin(uint32_t task_id, uint32_t address, uint32_t length)
{
    OsTaskHandle task = os_task_find_by_id(task_id);
    WmMemory* memory = NULL;

    if (task != NULL && os_task_memory(task, &memory) == OS_STATUS_OK && memory != NULL)
    {
        (void)wm_memory_pin(memory, address, length, -1);
    }
}

static OsGuestKept* os_guest_copy_of(const OsGuestBuffer* buffer)
{
    return (OsGuestKept*)(void*)(buffer->data - offsetof(OsGuestKept, data));
}

/* Write a copy back into its task's memory, if the task is still there. */
static OsStatus os_guest_copy_write_back(const OsGuestKept* copy)
{
    OsTaskHandle task = (copy->access & OS_GUEST_OUT) != 0U && !copy->pinned ? os_task_find_by_id(copy->task_id) : NULL;

    if (copy->pinned)
    {
        os_guest_unpin(copy->task_id, copy->address, copy->length);
    }
    if (task == NULL)
    {
        return OS_STATUS_OK;
    }
    /* During the task's own call a failure traps it; a background transfer's end only reports it. */
    return task == g_os.current_task ? os_guest_write(copy->address, copy->data, copy->length)
                                     : os_task_write_memory(task, copy->address, copy->data, copy->length);
}

OsStatus os_guest_buffer_open(OsGuestBuffer* buffer, uint32_t address, uint32_t length, uint8_t access)
{
    WmMemory* memory = NULL;
    OsGuestKept* copy = NULL;
    OsStatus status = os_task_memory(g_os.current_task, &memory);

    memset(buffer, 0, sizeof(*buffer));
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (!os_host_memory_in_bounds(memory, address, length))
    {
        return OS_STATUS_OUT_OF_BOUNDS;
    }
    buffer->address = address;
    buffer->length = length;
    buffer->task_id = g_os.current_task->id;
    buffer->access = access;
    if (length == 0U)
    {
        return OS_STATUS_OK;
    }
    status = os_guest_fault(wm_memory_pointer(memory, address, length, &buffer->data));
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    if (buffer->data != NULL)
    {
        /*
         * A paged memory's frame stays in RAM while the port has a pointer
         * into it. It is in RAM now, so pinning makes no frame and takes none.
         */
        if (wm_memory_data(memory, NULL) == NULL)
        {
            if (wm_memory_pin(memory, address, length, 1) != NULL)
            {
                buffer->data = NULL;
                return OS_STATUS_OUT_OF_MEMORY;
            }
            buffer->pinned = 1U;
        }
        return OS_STATUS_OK;
    }
    /* Split across pages: the port works on a copy. */
    copy = (OsGuestKept*)malloc(offsetof(OsGuestKept, data) + length);
    if (copy == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    copy->next = NULL;
    copy->address = address;
    copy->length = length;
    copy->task_id = buffer->task_id;
    copy->access = access;
    copy->pinned = 0U;
    status = os_guest_fault(wm_memory_read(memory, address, copy->data, length));
    if (status != OS_STATUS_OK)
    {
        free(copy);
        return status;
    }
    buffer->data = copy->data;
    buffer->copied = 1U;
    return OS_STATUS_OK;
}

OsStatus os_guest_buffer_close(OsGuestBuffer* buffer)
{
    OsStatus status = buffer->copied ? os_guest_copy_write_back(os_guest_copy_of(buffer)) : OS_STATUS_OK;

    os_guest_buffer_drop(buffer);
    return status;
}

void os_guest_buffer_drop(OsGuestBuffer* buffer)
{
    if (buffer->copied)
    {
        free(os_guest_copy_of(buffer));
    }
    if (buffer->pinned)
    {
        os_guest_unpin(buffer->task_id, buffer->address, buffer->length);
    }
    buffer->data = NULL;
    buffer->copied = 0U;
    buffer->pinned = 0U;
    buffer->length = 0U;
}

void os_guest_buffer_keep(OsGuestBuffer* buffer, OsGuestKept** list)
{
    OsGuestKept* kept = buffer->copied ? os_guest_copy_of(buffer) : NULL;

    if (buffer->pinned)
    {
        /* Only the range, to unpin when the transfer ends; without memory for it the frames stay pinned. */
        kept = (OsGuestKept*)malloc(offsetof(OsGuestKept, data));
        if (kept != NULL)
        {
            kept->address = buffer->address;
            kept->length = buffer->length;
            kept->task_id = buffer->task_id;
            kept->access = 0U;
            kept->pinned = 1U;
        }
    }
    if (kept != NULL)
    {
        kept->next = *list;
        *list = kept;
    }
    buffer->data = NULL;
    buffer->copied = 0U;
    buffer->pinned = 0U;
    buffer->length = 0U;
}

OsStatus os_guest_kept_close(OsGuestKept** list)
{
    OsStatus status = OS_STATUS_OK;

    while (*list != NULL)
    {
        OsGuestKept* copy = *list;
        OsStatus one = os_guest_copy_write_back(copy);
        if (one != OS_STATUS_OK)
        {
            status = one;
        }
        *list = copy->next;
        free(copy);
    }
    return status;
}

void** os_task_abi_state(OsTaskHandle task)
{
    return task != NULL ? &task->abi_state : NULL;
}

uint8_t os_object_fields(OsObjectType type, uint32_t id, OsObjectFields* out)
{
    switch (type)
    {
        case OS_OBJECT_QUEUE:
        {
            OsQueueHandle object = os_queue_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_MUTEX:
        {
            OsMutexHandle object = os_mutex_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_SEMAPHORE:
        {
            OsSemaphoreHandle object = os_semaphore_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_EVENT_GROUP:
        {
            OsEventGroupHandle object = os_event_group_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        case OS_OBJECT_TIMER:
        {
            OsTimerHandle object = os_timer_find_by_id(id);
            if (object == NULL) return 0U;
            OS_OBJECT_FIELDS_OF(object, out);
            return 1U;
        }
        default:
            return 0U;
    }
}

uint8_t os_object_name_matches(const char* object_name, const char* name, uint32_t name_length)
{
    return object_name != NULL && strlen(object_name) == name_length &&
           memcmp(object_name, name, name_length) == 0;
}

OsStatus os_object_adopt(OsObjectType type, uint32_t id, const char* name, uint32_t name_length)
{
    OsObjectFields fields;

    if (!os_object_fields(type, id, &fields))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (name_length > 0U)
    {
        char* copy = (char*)malloc((size_t)name_length + 1U);
        if (copy == NULL)
        {
            return OS_STATUS_OUT_OF_MEMORY;
        }
        memcpy(copy, name, name_length);
        copy[name_length] = '\0';
        free(*fields.name);
        *fields.name = copy;
    }
    *fields.guest_owned = 1U;
    *fields.guest_refs = 1U;
    return OS_STATUS_OK;
}

uint32_t os_object_find_by_name(OsObjectType type, const char* name, uint32_t name_length)
{
    if (name == NULL || name_length == 0U)
    {
        return 0U;
    }
    switch (type)
    {
        case OS_OBJECT_QUEUE: OS_FIND_NAMED(g_os.queue_list, OsQueueHandle);
        case OS_OBJECT_MUTEX: OS_FIND_NAMED(g_os.mutex_list, OsMutexHandle);
        case OS_OBJECT_SEMAPHORE: OS_FIND_NAMED(g_os.semaphore_list, OsSemaphoreHandle);
        case OS_OBJECT_EVENT_GROUP: OS_FIND_NAMED(g_os.event_group_list, OsEventGroupHandle);
        case OS_OBJECT_TIMER: OS_FIND_NAMED(g_os.timer_list, OsTimerHandle);
        default: return 0U;
    }
}

void os_object_retain(OsObjectType type, uint32_t id)
{
    OsObjectFields fields;

    if (os_object_fields(type, id, &fields) && *fields.guest_owned)
    {
        ++*fields.guest_refs;
    }
}

void os_object_release(OsObjectType type, uint32_t id)
{
    OsObjectFields fields;

    if (!os_object_fields(type, id, &fields) || !*fields.guest_owned)
    {
        return;
    }
    if (*fields.guest_refs > 1U)
    {
        --*fields.guest_refs;
        return;
    }
    switch (type)
    {
        case OS_OBJECT_QUEUE: os_queue_delete(os_queue_find_by_id(id)); break;
        case OS_OBJECT_MUTEX: os_mutex_delete(os_mutex_find_by_id(id)); break;
        case OS_OBJECT_SEMAPHORE: os_semaphore_delete(os_semaphore_find_by_id(id)); break;
        case OS_OBJECT_EVENT_GROUP: os_event_group_delete(os_event_group_find_by_id(id)); break;
        case OS_OBJECT_TIMER: os_timer_delete(os_timer_find_by_id(id)); break;
        default: break;
    }
}

uint32_t os_guest_last_wait_value(void)
{
    return g_os.current_task != NULL ? g_os.current_task->last_wait_value : 0U;
}

uint64_t os_time_ms64(void)
{
    return g_os.tick_ms_extended;
}
