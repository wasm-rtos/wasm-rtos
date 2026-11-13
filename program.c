/*
 * Programs processes run, loaded once and shared (docs/decisions.md, 9).
 *
 * A program is its bytes, which the kernel copied or a file system image
 * holds, and its module. Processes of the same bytes hold the same program:
 * it is found by size and contents, compared byte by byte, so a file that
 * changed is a new program and the processes of the old one keep running
 * it. The module is parsed when the first process of the program needs an
 * instance and freed when none has one (all swapped out or ended); its
 * decoded code is the code cache's, shared like the module. The program
 * goes with its last process.
 */

#include "os_kernel.h"

#if OS_HAS_PROCESSES

static OsProgram* g_programs;

static OsProgram* os_program_retain(OsProgram* program)
{
    ++program->references;
    return program;
}

OsProgram* os_program_find(const uint8_t* bytes, uint32_t size)
{
    OsProgram* program = NULL;

    for (program = g_programs; program != NULL; program = program->next)
    {
        if (program->size == size && memcmp(program->bytes, bytes, size) == 0)
        {
            return os_program_retain(program);
        }
    }
    return NULL;
}

static OsStatus os_program_add(OsProgram* fields, OsProgram** out_program)
{
    OsProgram* program = (OsProgram*)malloc(sizeof(OsProgram));

    if (program == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    *program = *fields;
    program->references = 1U;
    program->module_users = 0U;
    program->module = NULL;
    program->next = g_programs;
    g_programs = program;
    *out_program = program;
    return OS_STATUS_OK;
}

OsStatus os_program_adopt(uint8_t* owned_bytes, uint32_t size, OsProgram** out_program)
{
    OsProgram fields;
    OsStatus status = OS_STATUS_OK;

    *out_program = os_program_find(owned_bytes, size);
    if (*out_program != NULL)
    {
        free(owned_bytes);
        return OS_STATUS_OK;
    }
    memset(&fields, 0, sizeof(fields));
    fields.bytes = owned_bytes;
    fields.size = size;
    fields.owned_bytes = owned_bytes;
    status = os_program_add(&fields, out_program);
    if (status != OS_STATUS_OK)
    {
        free(owned_bytes);
    }
    return status;
}

OsStatus os_program_from_bytes(const uint8_t* bytes, uint32_t size, OsProgram** out_program)
{
    uint8_t* copy = NULL;

    *out_program = os_program_find(bytes, size);
    if (*out_program != NULL)
    {
        return OS_STATUS_OK;
    }
    copy = (uint8_t*)malloc(size);
    if (copy == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memcpy(copy, bytes, size);
    return os_program_adopt(copy, size, out_program);
}

#if OS_HAS_FS
OsStatus os_program_from_path(const char* path, OsProgram** out_program)
{
    OsProgram fields;
    OsProgram* program = NULL;
    OsFsStat stat;
    OsStatus status = os_fs_stat(path, &stat);

    *out_program = NULL;
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    /* Before anything is read whole: a loaded program of the same contents. */
    for (program = g_programs; program != NULL; program = program->next)
    {
        if (program->size == stat.size && os_fs_file_matches(path, program->bytes, program->size))
        {
            *out_program = os_program_retain(program);
            return OS_STATUS_OK;
        }
    }
    memset(&fields, 0, sizeof(fields));
    status = os_fs_image_open(path, &fields.file);
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    fields.bytes = fields.file.bytes;
    fields.size = fields.file.size;
    status = os_program_add(&fields, out_program);
    if (status != OS_STATUS_OK)
    {
        os_fs_image_close(&fields.file);
    }
    return status;
}
#endif

void os_program_release(OsProgram* program)
{
    OsProgram** link = &g_programs;

    if (program == NULL || --program->references > 0U)
    {
        return;
    }
    while (*link != NULL && *link != program)
    {
        link = &(*link)->next;
    }
    if (*link != NULL)
    {
        *link = program->next;
    }
    wm_module_free(program->module);
    free(program->owned_bytes);
#if OS_HAS_FS
    os_fs_image_close(&program->file);
#endif
    free(program);
}

WmResult os_program_module_take(OsProgram* program, WmModule** out_module)
{
    WmResult result = NULL;

    if (program->module == NULL)
    {
        result = wm_module_load(&program->module, program->bytes, program->size);
        if (result != NULL)
        {
            program->module = NULL;
            return result;
        }
    }
    ++program->module_users;
    *out_module = program->module;
    return NULL;
}

void os_program_module_give(OsProgram* program)
{
    if (program->module_users > 0U && --program->module_users == 0U)
    {
        wm_module_free(program->module);
        program->module = NULL;
    }
}

uint32_t os_get_program_count(void)
{
    const OsProgram* program = NULL;
    uint32_t count = 0U;

    for (program = g_programs; program != NULL; program = program->next)
    {
        ++count;
    }
    return count;
}

#else

uint32_t os_get_program_count(void)
{
    return 0U;
}

#endif
