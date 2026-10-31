#include "fs.h"

#if OS_HAS_FS

#include "wasm3/source/m3_env.h"
#include "wasm3/source/extra/wasi_core.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OS_FS_MAX_MOUNTS
#define OS_FS_MAX_MOUNTS 8U
#endif

#ifndef OS_FS_MAX_FILES
#define OS_FS_MAX_FILES 16U
#endif

/* WASI fd 0-2 are stdio and fd 3 is the preopened root; opened files follow. */
#define OS_FS_ROOT_FD 3U
#define OS_FS_FIRST_FILE_FD 4U

typedef struct OsFsMount
{
    char path[OS_FS_PATH_MAX];
    uint32_t path_length;
    const OsFsDriver* driver;
    void* context;
} OsFsMount;

typedef struct OsFsOpenFile
{
    uint8_t used;
    uint8_t is_directory;
    const OsFsDriver* driver;
    void* context;
    void* handle;
    uint64_t position;
    uint64_t size;
    char* path;
} OsFsOpenFile;

typedef struct OsFsProcess
{
    uint32_t task_id;
    OsFsOpenFile files[OS_FS_MAX_FILES];
    struct OsFsProcess* next;
} OsFsProcess;

static OsFsMount g_mounts[OS_FS_MAX_MOUNTS];
static uint32_t g_mount_count;
static OsFsProcess* g_processes;

/* ---------------------------------------------------------------------------
 * Paths and mounts
 * ------------------------------------------------------------------------ */

/*
 * Join base and path (path may be absolute) and normalize the result:
 * collapse repeated '/', drop "." and resolve ".." without leaving "/".
 */
static OsStatus os_fs_normalize(
    const char* base,
    const char* path,
    uint32_t path_length,
    char* out,
    uint32_t out_size
)
{
    char joined[OS_FS_PATH_MAX * 2U];
    uint32_t joined_length = 0U;
    uint32_t out_length = 0U;
    uint32_t index = 0U;

    if (path_length > 0U && path[0] == '/')
    {
        base = "/";
    }

    for (const char* cursor = base; *cursor != '\0'; ++cursor)
    {
        if (joined_length + 1U >= sizeof(joined))
        {
            return OS_STATUS_BUFFER_TOO_SMALL;
        }
        joined[joined_length++] = *cursor;
    }
    if (joined_length + 1U >= sizeof(joined))
    {
        return OS_STATUS_BUFFER_TOO_SMALL;
    }
    joined[joined_length++] = '/';
    for (index = 0U; index < path_length; ++index)
    {
        if (path[index] == '\0')
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
        if (joined_length + 1U >= sizeof(joined))
        {
            return OS_STATUS_BUFFER_TOO_SMALL;
        }
        joined[joined_length++] = path[index];
    }

    out[out_length++] = '/';
    index = 0U;
    while (index < joined_length)
    {
        uint32_t start = 0U;
        uint32_t length = 0U;

        while (index < joined_length && joined[index] == '/')
        {
            ++index;
        }
        start = index;
        while (index < joined_length && joined[index] != '/')
        {
            ++index;
        }
        length = index - start;

        if (length == 0U || (length == 1U && joined[start] == '.'))
        {
            continue;
        }
        if (length == 2U && joined[start] == '.' && joined[start + 1U] == '.')
        {
            while (out_length > 1U && out[out_length - 1U] != '/')
            {
                --out_length;
            }
            if (out_length > 1U)
            {
                --out_length;
            }
            continue;
        }

        if (out_length > 1U)
        {
            if (out_length + 1U >= out_size)
            {
                return OS_STATUS_BUFFER_TOO_SMALL;
            }
            out[out_length++] = '/';
        }
        if (out_length + length >= out_size)
        {
            return OS_STATUS_BUFFER_TOO_SMALL;
        }
        memcpy(out + out_length, joined + start, length);
        out_length += length;
    }

    out[out_length] = '\0';
    return OS_STATUS_OK;
}

/* Longest mount covering an absolute normalized path; *relative is inside it. */
static OsFsMount* os_fs_find_mount(const char* path, const char** relative)
{
    OsFsMount* best = NULL;
    uint32_t index = 0U;

    for (index = 0U; index < g_mount_count; ++index)
    {
        OsFsMount* mount = &g_mounts[index];
        uint32_t length = mount->path_length;
        uint8_t covers = 0U;

        if (length == 1U)
        {
            covers = 1U;
        }
        else if (strncmp(path, mount->path, length) == 0 &&
                 (path[length] == '\0' || path[length] == '/'))
        {
            covers = 1U;
        }

        if (covers && (best == NULL || length > best->path_length))
        {
            best = mount;
        }
    }

    if (best != NULL && relative != NULL)
    {
        const char* rest = path + (best->path_length == 1U ? 0U : best->path_length);
        while (*rest == '/')
        {
            ++rest;
        }
        *relative = rest;
    }
    return best;
}

/* True when a mount point lies strictly below directory path. */
static uint8_t os_fs_has_mount_below(const char* path)
{
    uint32_t path_length = (uint32_t)strlen(path);
    uint32_t index = 0U;

    for (index = 0U; index < g_mount_count; ++index)
    {
        const char* mount_path = g_mounts[index].path;
        if (path_length == 1U)
        {
            if (g_mounts[index].path_length > 1U)
            {
                return 1U;
            }
        }
        else if (strncmp(mount_path, path, path_length) == 0 && mount_path[path_length] == '/')
        {
            return 1U;
        }
    }
    return 0U;
}

/* Name of the mount point that is a direct child of directory path, by order. */
static const char* os_fs_mount_child(const char* path, uint32_t child_index)
{
    uint32_t path_length = (uint32_t)strlen(path);
    uint32_t index = 0U;
    uint32_t found = 0U;

    for (index = 0U; index < g_mount_count; ++index)
    {
        const char* mount_path = g_mounts[index].path;
        const char* rest = NULL;

        if (g_mounts[index].path_length <= 1U)
        {
            continue;
        }
        if (path_length == 1U)
        {
            rest = mount_path + 1;
        }
        else if (strncmp(mount_path, path, path_length) == 0 && mount_path[path_length] == '/')
        {
            rest = mount_path + path_length + 1U;
        }

        if (rest != NULL && strchr(rest, '/') == NULL)
        {
            if (found == child_index)
            {
                return rest;
            }
            ++found;
        }
    }
    return NULL;
}

OsStatus os_fs_mount(const char* path, const OsFsDriver* driver, void* context)
{
    char normalized[OS_FS_PATH_MAX];
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;

    if (path == NULL || path[0] != '/' || driver == NULL || driver->stat == NULL ||
        driver->open == NULL || driver->read == NULL || driver->read_dir == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    status = os_fs_normalize("/", path, (uint32_t)strlen(path), normalized, sizeof(normalized));
    if (status != OS_STATUS_OK)
    {
        return status;
    }

    for (index = 0U; index < g_mount_count; ++index)
    {
        if (strcmp(g_mounts[index].path, normalized) == 0)
        {
            return OS_STATUS_BUSY;
        }
    }
    if (g_mount_count >= OS_FS_MAX_MOUNTS)
    {
        return OS_STATUS_LIMIT_REACHED;
    }

    memcpy(g_mounts[g_mount_count].path, normalized, strlen(normalized) + 1U);
    g_mounts[g_mount_count].path_length = (uint32_t)strlen(normalized);
    g_mounts[g_mount_count].driver = driver;
    g_mounts[g_mount_count].context = context;
    ++g_mount_count;
    return OS_STATUS_OK;
}

OsStatus os_fs_unmount(const char* path)
{
    char normalized[OS_FS_PATH_MAX];
    uint32_t index = 0U;

    if (path == NULL ||
        os_fs_normalize("/", path, (uint32_t)strlen(path), normalized, sizeof(normalized)) != OS_STATUS_OK)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    for (index = 0U; index < g_mount_count; ++index)
    {
        if (strcmp(g_mounts[index].path, normalized) == 0)
        {
            OsFsProcess* process = NULL;
            for (process = g_processes; process != NULL; process = process->next)
            {
                uint32_t file_index = 0U;
                for (file_index = 0U; file_index < OS_FS_MAX_FILES; ++file_index)
                {
                    if (process->files[file_index].used &&
                        process->files[file_index].context == g_mounts[index].context &&
                        process->files[file_index].driver == g_mounts[index].driver)
                    {
                        return OS_STATUS_BUSY;
                    }
                }
            }

            memmove(&g_mounts[index], &g_mounts[index + 1U],
                    (g_mount_count - index - 1U) * sizeof(g_mounts[0]));
            --g_mount_count;
            return OS_STATUS_OK;
        }
    }
    return OS_STATUS_FILE_NOT_FOUND;
}

static OsStatus os_fs_stat_normalized(const char* path, OsFsStat* out_stat)
{
    const char* relative = NULL;
    OsFsMount* mount = os_fs_find_mount(path, &relative);
    OsStatus status = OS_STATUS_FILE_NOT_FOUND;

    if (mount != NULL)
    {
        status = mount->driver->stat(mount->context, relative, out_stat);
        if (status == OS_STATUS_OK)
        {
            return OS_STATUS_OK;
        }
    }

    if ((path[0] == '/' && path[1] == '\0') || os_fs_has_mount_below(path))
    {
        out_stat->type = OS_FS_TYPE_DIRECTORY;
        out_stat->size = 0U;
        return OS_STATUS_OK;
    }
    return status;
}

OsStatus os_fs_stat(const char* path, OsFsStat* out_stat)
{
    char normalized[OS_FS_PATH_MAX];
    OsStatus status = OS_STATUS_OK;

    if (path == NULL || out_stat == NULL || path[0] != '/')
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_fs_normalize("/", path, (uint32_t)strlen(path), normalized, sizeof(normalized));
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    return os_fs_stat_normalized(normalized, out_stat);
}

/*
 * Driver entries come first, then mount points directly below the
 * directory that the driver does not already list.
 */
static OsStatus os_fs_read_dir_normalized(const char* path, uint32_t index, OsFsDirEntry* out_entry)
{
    const char* relative = NULL;
    OsFsMount* mount = os_fs_find_mount(path, &relative);
    uint32_t driver_count = 0U;
    uint32_t mount_index = 0U;
    OsFsStat stat;

    if (os_fs_stat_normalized(path, &stat) != OS_STATUS_OK)
    {
        return OS_STATUS_FILE_NOT_FOUND;
    }
    if (stat.type != OS_FS_TYPE_DIRECTORY)
    {
        return OS_STATUS_NOT_A_DIRECTORY;
    }

    if (mount != NULL)
    {
        OsFsStat driver_stat;
        if (mount->driver->stat(mount->context, relative, &driver_stat) == OS_STATUS_OK &&
            driver_stat.type == OS_FS_TYPE_DIRECTORY)
        {
            OsStatus status = mount->driver->read_dir(mount->context, relative, index, out_entry);
            if (status != OS_STATUS_END_OF_DIRECTORY)
            {
                return status;
            }
            while (mount->driver->read_dir(mount->context, relative, driver_count, out_entry) == OS_STATUS_OK)
            {
                ++driver_count;
            }
        }
    }

    for (mount_index = 0U;; ++mount_index)
    {
        const char* name = os_fs_mount_child(path, mount_index);
        uint8_t listed = 0U;

        if (name == NULL)
        {
            return OS_STATUS_END_OF_DIRECTORY;
        }

        if (mount != NULL)
        {
            uint32_t driver_index = 0U;
            OsFsDirEntry entry;
            for (driver_index = 0U; driver_index < driver_count; ++driver_index)
            {
                if (mount->driver->read_dir(mount->context, relative, driver_index, &entry) == OS_STATUS_OK &&
                    strcmp(entry.name, name) == 0)
                {
                    listed = 1U;
                    break;
                }
            }
        }
        if (listed)
        {
            continue;
        }

        if (driver_count == index)
        {
            size_t length = strlen(name);
            if (length > OS_FS_NAME_MAX)
            {
                length = OS_FS_NAME_MAX;
            }
            memcpy(out_entry->name, name, length);
            out_entry->name[length] = '\0';
            out_entry->type = OS_FS_TYPE_DIRECTORY;
            return OS_STATUS_OK;
        }
        ++driver_count;
    }
}

OsStatus os_fs_read_dir(const char* path, uint32_t index, OsFsDirEntry* out_entry)
{
    char normalized[OS_FS_PATH_MAX];
    OsStatus status = OS_STATUS_OK;

    if (path == NULL || out_entry == NULL || path[0] != '/')
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    status = os_fs_normalize("/", path, (uint32_t)strlen(path), normalized, sizeof(normalized));
    if (status != OS_STATUS_OK)
    {
        return status;
    }
    return os_fs_read_dir_normalized(normalized, index, out_entry);
}

/* ---------------------------------------------------------------------------
 * Per-process open files
 * ------------------------------------------------------------------------ */

static OsFsProcess* os_fs_find_process(uint32_t task_id, uint8_t create)
{
    OsFsProcess* process = NULL;

    for (process = g_processes; process != NULL; process = process->next)
    {
        if (process->task_id == task_id)
        {
            return process;
        }
    }
    if (!create)
    {
        return NULL;
    }

    /* Created on the first open, so programs that never open files cost nothing. */
    process = (OsFsProcess*)calloc(1U, sizeof(OsFsProcess));
    if (process == NULL)
    {
        return NULL;
    }
    process->task_id = task_id;
    process->next = g_processes;
    g_processes = process;
    return process;
}

static void os_fs_close_file(OsFsOpenFile* file)
{
    if (!file->used)
    {
        return;
    }
    if (!file->is_directory && file->driver->close != NULL)
    {
        file->driver->close(file->context, file->handle);
    }
    free(file->path);
    memset(file, 0, sizeof(*file));
}

void os_fs_release_task(uint32_t task_id)
{
    OsFsProcess** link = &g_processes;

    while (*link != NULL)
    {
        OsFsProcess* process = *link;
        if (process->task_id == task_id)
        {
            uint32_t index = 0U;
            for (index = 0U; index < OS_FS_MAX_FILES; ++index)
            {
                os_fs_close_file(&process->files[index]);
            }
            *link = process->next;
            free(process);
            return;
        }
        link = &process->next;
    }
}

void os_fs_shutdown(void)
{
    while (g_processes != NULL)
    {
        os_fs_release_task(g_processes->task_id);
    }
    g_mount_count = 0U;
}

static uint32_t os_fs_current_task_id(void)
{
    OsTaskHandle task = os_task_get_current();
    return task != NULL ? os_task_get_id(task) : 0U;
}

static OsFsOpenFile* os_fs_file_for_fd(uint32_t fd)
{
    OsFsProcess* process = NULL;

    if (fd < OS_FS_FIRST_FILE_FD || fd >= OS_FS_FIRST_FILE_FD + OS_FS_MAX_FILES)
    {
        return NULL;
    }
    process = os_fs_find_process(os_fs_current_task_id(), 0U);
    if (process == NULL || !process->files[fd - OS_FS_FIRST_FILE_FD].used)
    {
        return NULL;
    }
    return &process->files[fd - OS_FS_FIRST_FILE_FD];
}

/* Directory path of a directory fd (the preopened root or an opened directory). */
static const char* os_fs_directory_for_fd(uint32_t fd)
{
    OsFsOpenFile* file = NULL;

    if (fd == OS_FS_ROOT_FD)
    {
        return "/";
    }
    file = os_fs_file_for_fd(fd);
    return file != NULL && file->is_directory ? file->path : NULL;
}

static __wasi_errno_t os_fs_errno(OsStatus status)
{
    switch (status)
    {
        case OS_STATUS_OK: return __WASI_ERRNO_SUCCESS;
        case OS_STATUS_FILE_NOT_FOUND: return __WASI_ERRNO_NOENT;
        case OS_STATUS_NOT_A_DIRECTORY: return __WASI_ERRNO_NOTDIR;
        case OS_STATUS_IS_A_DIRECTORY: return __WASI_ERRNO_ISDIR;
        case OS_STATUS_READ_ONLY: return __WASI_ERRNO_ROFS;
        case OS_STATUS_BUFFER_TOO_SMALL: return __WASI_ERRNO_NAMETOOLONG;
        case OS_STATUS_OUT_OF_MEMORY: return __WASI_ERRNO_NOMEM;
        case OS_STATUS_LIMIT_REACHED: return __WASI_ERRNO_MFILE;
        case OS_STATUS_INVALID_ARGUMENT: return __WASI_ERRNO_INVAL;
        default: return __WASI_ERRNO_IO;
    }
}

static uint64_t os_fs_inode(const char* text)
{
    /* FNV-1a; never 0 so programs do not skip the entry. */
    uint64_t hash = 1469598103934665603ULL;
    for (; *text != '\0'; ++text)
    {
        hash ^= (uint8_t)*text;
        hash *= 1099511628211ULL;
    }
    return hash != 0U ? hash : 1U;
}

/* ---------------------------------------------------------------------------
 * WASI file calls
 * ------------------------------------------------------------------------ */

#define OS_FS_FILE_RIGHTS \
    (__WASI_RIGHTS_FD_READ | __WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL | \
     __WASI_RIGHTS_FD_FILESTAT_GET | __WASI_RIGHTS_FD_ADVISE)
#define OS_FS_DIRECTORY_RIGHTS \
    (__WASI_RIGHTS_PATH_OPEN | __WASI_RIGHTS_FD_READDIR | \
     __WASI_RIGHTS_PATH_FILESTAT_GET | __WASI_RIGHTS_FD_FILESTAT_GET)
#define OS_FS_WRITE_RIGHTS \
    (__WASI_RIGHTS_FD_WRITE | __WASI_RIGHTS_FD_ALLOCATE | __WASI_RIGHTS_FD_FILESTAT_SET_SIZE)

typedef struct OsFsIovec
{
    uint32_t buf;
    uint32_t buf_len;
} OsFsIovec;

static OsStatus os_fs_read_into(OsFsOpenFile* file, uint64_t offset, void* buffer, uint32_t length, uint32_t* out_read)
{
    *out_read = 0U;
    if (offset >= file->size || length == 0U)
    {
        return OS_STATUS_OK;
    }
    if ((uint64_t)length > file->size - offset)
    {
        length = (uint32_t)(file->size - offset);
    }
    return file->driver->read(file->context, file->handle, offset, buffer, length, out_read);
}

static m3ApiRawFunction(os_fs_wasi_fd_prestat_get)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(uint8_t*, buf)
    (void)_ctx;

    m3ApiCheckMem(buf, 8);
    if (fd != OS_FS_ROOT_FD)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }
    m3ApiWriteMem32(buf, __WASI_PREOPENTYPE_DIR);
    m3ApiWriteMem32(buf + 4, 1);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_fd_prestat_dir_name)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(char*, path)
    m3ApiGetArg(__wasi_size_t, path_len)
    (void)_ctx;

    if (fd != OS_FS_ROOT_FD)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }
    if (path_len < 1U)
    {
        m3ApiReturn(__WASI_ERRNO_INVAL);
    }
    m3ApiCheckMem(path, 1);
    path[0] = '/';
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_path_open)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, dirfd)
    m3ApiGetArg(__wasi_lookupflags_t, dirflags)
    m3ApiGetArgMem(const char*, path)
    m3ApiGetArg(__wasi_size_t, path_len)
    m3ApiGetArg(__wasi_oflags_t, oflags)
    m3ApiGetArg(__wasi_rights_t, rights_base)
    m3ApiGetArg(__wasi_rights_t, rights_inheriting)
    m3ApiGetArg(__wasi_fdflags_t, fdflags)
    m3ApiGetArgMem(__wasi_fd_t*, out_fd)
    const char* directory = os_fs_directory_for_fd(dirfd);
    char normalized[OS_FS_PATH_MAX];
    OsFsProcess* process = NULL;
    OsFsOpenFile* file = NULL;
    const char* relative = NULL;
    OsFsMount* mount = NULL;
    OsFsStat stat;
    OsStatus status = OS_STATUS_OK;
    uint32_t index = 0U;
    (void)_ctx;
    (void)dirflags;
    (void)rights_inheriting;

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(out_fd, sizeof(__wasi_fd_t));

    if (directory == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }
    if ((oflags & (__WASI_OFLAGS_CREAT | __WASI_OFLAGS_EXCL | __WASI_OFLAGS_TRUNC)) != 0U ||
        (fdflags & (__WASI_FDFLAGS_APPEND | __WASI_FDFLAGS_DSYNC | __WASI_FDFLAGS_RSYNC | __WASI_FDFLAGS_SYNC)) != 0U ||
        (rights_base & OS_FS_WRITE_RIGHTS) != 0U)
    {
        m3ApiReturn(__WASI_ERRNO_ROFS);
    }

    status = os_fs_normalize(directory, path, path_len, normalized, sizeof(normalized));
    if (status == OS_STATUS_OK)
    {
        status = os_fs_stat_normalized(normalized, &stat);
    }
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(os_fs_errno(status));
    }
    if ((oflags & __WASI_OFLAGS_DIRECTORY) != 0U && stat.type != OS_FS_TYPE_DIRECTORY)
    {
        m3ApiReturn(__WASI_ERRNO_NOTDIR);
    }

    process = os_fs_find_process(os_fs_current_task_id(), 1U);
    if (process == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_NOMEM);
    }
    for (index = 0U; index < OS_FS_MAX_FILES; ++index)
    {
        if (!process->files[index].used)
        {
            file = &process->files[index];
            break;
        }
    }
    if (file == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_MFILE);
    }

    if (stat.type == OS_FS_TYPE_DIRECTORY)
    {
        size_t length = strlen(normalized) + 1U;
        file->path = (char*)malloc(length);
        if (file->path == NULL)
        {
            m3ApiReturn(__WASI_ERRNO_NOMEM);
        }
        memcpy(file->path, normalized, length);
        file->is_directory = 1U;
    }
    else
    {
        mount = os_fs_find_mount(normalized, &relative);
        status = mount != NULL
            ? mount->driver->open(mount->context, relative, &file->handle)
            : OS_STATUS_FILE_NOT_FOUND;
        if (status != OS_STATUS_OK)
        {
            m3ApiReturn(os_fs_errno(status));
        }
        file->driver = mount->driver;
        file->context = mount->context;
        file->size = stat.size;
    }

    file->used = 1U;
    file->position = 0U;
    m3ApiWriteMem32(out_fd, OS_FS_FIRST_FILE_FD + index);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_fd_read)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(OsFsIovec*, iovs)
    m3ApiGetArg(__wasi_size_t, iovs_len)
    m3ApiGetArgMem(__wasi_size_t*, nread)
    OsFsOpenFile* file = os_fs_file_for_fd(fd);
    uint32_t total = 0U;
    uint32_t index = 0U;
    (void)_ctx;

    m3ApiCheckMem(iovs, iovs_len * sizeof(OsFsIovec));
    m3ApiCheckMem(nread, sizeof(__wasi_size_t));

    if (fd == 0U)
    {
        /* No console input yet: stdin reads as end of file. */
        m3ApiWriteMem32(nread, 0);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
    if (file == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }
    if (file->is_directory)
    {
        m3ApiReturn(__WASI_ERRNO_ISDIR);
    }

    for (index = 0U; index < iovs_len; ++index)
    {
        void* address = m3ApiOffsetToPtr(m3ApiReadMem32(&iovs[index].buf));
        uint32_t length = m3ApiReadMem32(&iovs[index].buf_len);
        uint32_t got = 0U;
        OsStatus status = OS_STATUS_OK;

        if (length == 0U)
        {
            continue;
        }
        m3ApiCheckMem(address, length);
        status = os_fs_read_into(file, file->position, address, length, &got);
        if (status != OS_STATUS_OK)
        {
            m3ApiReturn(os_fs_errno(status));
        }
        file->position += got;
        total += got;
        if (got < length)
        {
            break;
        }
    }

    m3ApiWriteMem32(nread, total);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_fd_pread)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(OsFsIovec*, iovs)
    m3ApiGetArg(__wasi_size_t, iovs_len)
    m3ApiGetArg(__wasi_filesize_t, offset)
    m3ApiGetArgMem(__wasi_size_t*, nread)
    OsFsOpenFile* file = os_fs_file_for_fd(fd);
    uint32_t total = 0U;
    uint32_t index = 0U;
    (void)_ctx;

    m3ApiCheckMem(iovs, iovs_len * sizeof(OsFsIovec));
    m3ApiCheckMem(nread, sizeof(__wasi_size_t));

    if (file == NULL)
    {
        m3ApiReturn(fd < OS_FS_FIRST_FILE_FD ? __WASI_ERRNO_SPIPE : __WASI_ERRNO_BADF);
    }
    if (file->is_directory)
    {
        m3ApiReturn(__WASI_ERRNO_ISDIR);
    }

    for (index = 0U; index < iovs_len; ++index)
    {
        void* address = m3ApiOffsetToPtr(m3ApiReadMem32(&iovs[index].buf));
        uint32_t length = m3ApiReadMem32(&iovs[index].buf_len);
        uint32_t got = 0U;
        OsStatus status = OS_STATUS_OK;

        if (length == 0U)
        {
            continue;
        }
        m3ApiCheckMem(address, length);
        status = os_fs_read_into(file, offset + total, address, length, &got);
        if (status != OS_STATUS_OK)
        {
            m3ApiReturn(os_fs_errno(status));
        }
        total += got;
        if (got < length)
        {
            break;
        }
    }

    m3ApiWriteMem32(nread, total);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

/* stdout and stderr go to the host C library; files are read only. */
static m3ApiRawFunction(os_fs_wasi_fd_write)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(OsFsIovec*, iovs)
    m3ApiGetArg(__wasi_size_t, iovs_len)
    m3ApiGetArgMem(__wasi_size_t*, nwritten)
    FILE* stream = fd == 1U ? stdout : fd == 2U ? stderr : NULL;
    uint32_t total = 0U;
    uint32_t index = 0U;
    (void)_ctx;

    m3ApiCheckMem(iovs, iovs_len * sizeof(OsFsIovec));
    m3ApiCheckMem(nwritten, sizeof(__wasi_size_t));

    if (stream == NULL)
    {
        m3ApiReturn(os_fs_file_for_fd(fd) != NULL || fd == OS_FS_ROOT_FD ? __WASI_ERRNO_ROFS : __WASI_ERRNO_BADF);
    }

    for (index = 0U; index < iovs_len; ++index)
    {
        void* address = m3ApiOffsetToPtr(m3ApiReadMem32(&iovs[index].buf));
        uint32_t length = m3ApiReadMem32(&iovs[index].buf_len);
        if (length == 0U)
        {
            continue;
        }
        m3ApiCheckMem(address, length);
        total += (uint32_t)fwrite(address, 1U, length, stream);
    }
    fflush(stream);

    m3ApiWriteMem32(nwritten, total);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static __wasi_errno_t os_fs_seek(uint32_t fd, int64_t offset, uint32_t whence, uint64_t* out_position)
{
    OsFsOpenFile* file = os_fs_file_for_fd(fd);
    int64_t base = 0;

    if (file == NULL)
    {
        return fd < OS_FS_FIRST_FILE_FD ? __WASI_ERRNO_SPIPE : __WASI_ERRNO_BADF;
    }
    if (file->is_directory)
    {
        return __WASI_ERRNO_ISDIR;
    }

    if (whence == __WASI_WHENCE_SET)
    {
        base = 0;
    }
    else if (whence == __WASI_WHENCE_CUR)
    {
        base = (int64_t)file->position;
    }
    else if (whence == __WASI_WHENCE_END)
    {
        base = (int64_t)file->size;
    }
    else
    {
        return __WASI_ERRNO_INVAL;
    }

    if ((offset < 0 && base + offset < 0))
    {
        return __WASI_ERRNO_INVAL;
    }
    file->position = (uint64_t)(base + offset);
    *out_position = file->position;
    return __WASI_ERRNO_SUCCESS;
}

static m3ApiRawFunction(os_fs_wasi_fd_seek)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArg(__wasi_filedelta_t, offset)
    m3ApiGetArg(uint32_t, whence)
    m3ApiGetArgMem(__wasi_filesize_t*, result)
    uint64_t position = 0U;
    __wasi_errno_t error = 0;
    (void)_ctx;

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));
    error = os_fs_seek(fd, offset, whence, &position);
    if (error == __WASI_ERRNO_SUCCESS)
    {
        m3ApiWriteMem64(result, position);
    }
    m3ApiReturn(error);
}

/* wasi_unstable numbers whence as CUR, END, SET. */
static m3ApiRawFunction(os_fs_wasi_unstable_fd_seek)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArg(__wasi_filedelta_t, offset)
    m3ApiGetArg(uint32_t, whence)
    m3ApiGetArgMem(__wasi_filesize_t*, result)
    static const uint32_t whence_map[3] = { __WASI_WHENCE_CUR, __WASI_WHENCE_END, __WASI_WHENCE_SET };
    uint64_t position = 0U;
    __wasi_errno_t error = 0;
    (void)_ctx;

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));
    if (whence > 2U)
    {
        m3ApiReturn(__WASI_ERRNO_INVAL);
    }
    error = os_fs_seek(fd, offset, whence_map[whence], &position);
    if (error == __WASI_ERRNO_SUCCESS)
    {
        m3ApiWriteMem64(result, position);
    }
    m3ApiReturn(error);
}

static m3ApiRawFunction(os_fs_wasi_fd_tell)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(__wasi_filesize_t*, result)
    OsFsOpenFile* file = os_fs_file_for_fd(fd);
    (void)_ctx;

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));
    if (file == NULL)
    {
        m3ApiReturn(fd < OS_FS_FIRST_FILE_FD ? __WASI_ERRNO_SPIPE : __WASI_ERRNO_BADF);
    }
    m3ApiWriteMem64(result, file->position);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_fd_close)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    OsFsOpenFile* file = os_fs_file_for_fd(fd);
    (void)runtime;
    (void)_ctx;
    (void)_mem;

    if (fd <= OS_FS_ROOT_FD)
    {
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
    if (file == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }
    os_fs_close_file(file);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_fd_fdstat_get)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(uint8_t*, buf)
    OsFsOpenFile* file = os_fs_file_for_fd(fd);
    uint8_t filetype = 0U;
    uint64_t rights = 0U;
    uint64_t inheriting = 0U;
    (void)_ctx;

    m3ApiCheckMem(buf, 24);
    if (fd <= 2U)
    {
        filetype = __WASI_FILETYPE_CHARACTER_DEVICE;
        rights = fd == 0U ? __WASI_RIGHTS_FD_READ : __WASI_RIGHTS_FD_WRITE;
    }
    else if (fd == OS_FS_ROOT_FD || (file != NULL && file->is_directory))
    {
        filetype = __WASI_FILETYPE_DIRECTORY;
        rights = OS_FS_DIRECTORY_RIGHTS;
        inheriting = OS_FS_DIRECTORY_RIGHTS | OS_FS_FILE_RIGHTS;
    }
    else if (file != NULL)
    {
        filetype = __WASI_FILETYPE_REGULAR_FILE;
        rights = OS_FS_FILE_RIGHTS;
    }
    else
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }

    memset(buf, 0, 24);
    m3ApiWriteMem8(buf, filetype);
    m3ApiWriteMem64(buf + 8, rights);
    m3ApiWriteMem64(buf + 16, inheriting);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static void os_fs_write_filestat(uint8_t* buf, const char* path, const OsFsStat* stat)
{
    memset(buf, 0, 64);
    m3ApiWriteMem64(buf + 8, os_fs_inode(path));
    m3ApiWriteMem8(buf + 16, stat->type == OS_FS_TYPE_DIRECTORY
        ? __WASI_FILETYPE_DIRECTORY
        : __WASI_FILETYPE_REGULAR_FILE);
    m3ApiWriteMem64(buf + 24, 1U);
    m3ApiWriteMem64(buf + 32, stat->size);
}

static m3ApiRawFunction(os_fs_wasi_fd_filestat_get)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(uint8_t*, buf)
    OsFsOpenFile* file = os_fs_file_for_fd(fd);
    OsFsStat stat;
    (void)_ctx;

    m3ApiCheckMem(buf, 64);
    if (fd <= 2U)
    {
        memset(buf, 0, 64);
        m3ApiWriteMem8(buf + 16, __WASI_FILETYPE_CHARACTER_DEVICE);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
    if (fd == OS_FS_ROOT_FD)
    {
        stat.type = OS_FS_TYPE_DIRECTORY;
        stat.size = 0U;
        os_fs_write_filestat(buf, "/", &stat);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
    if (file == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }
    stat.type = file->is_directory ? OS_FS_TYPE_DIRECTORY : OS_FS_TYPE_FILE;
    stat.size = file->size;
    os_fs_write_filestat(buf, file->path != NULL ? file->path : "", &stat);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_path_filestat_get)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, dirfd)
    m3ApiGetArg(__wasi_lookupflags_t, flags)
    m3ApiGetArgMem(const char*, path)
    m3ApiGetArg(__wasi_size_t, path_len)
    m3ApiGetArgMem(uint8_t*, buf)
    const char* directory = os_fs_directory_for_fd(dirfd);
    char normalized[OS_FS_PATH_MAX];
    OsFsStat stat;
    OsStatus status = OS_STATUS_OK;
    (void)_ctx;
    (void)flags;

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(buf, 64);
    if (directory == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }

    status = os_fs_normalize(directory, path, path_len, normalized, sizeof(normalized));
    if (status == OS_STATUS_OK)
    {
        status = os_fs_stat_normalized(normalized, &stat);
    }
    if (status != OS_STATUS_OK)
    {
        m3ApiReturn(os_fs_errno(status));
    }
    os_fs_write_filestat(buf, normalized, &stat);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

/* cookie is the index of the next entry; a partial last entry fills the buffer. */
static m3ApiRawFunction(os_fs_wasi_fd_readdir)
{
    m3ApiReturnType(uint32_t)
    m3ApiGetArg(__wasi_fd_t, fd)
    m3ApiGetArgMem(uint8_t*, buf)
    m3ApiGetArg(__wasi_size_t, buf_len)
    m3ApiGetArg(__wasi_dircookie_t, cookie)
    m3ApiGetArgMem(__wasi_size_t*, bufused)
    const char* directory = os_fs_directory_for_fd(fd);
    uint32_t used = 0U;
    uint64_t index = cookie;
    (void)_ctx;

    m3ApiCheckMem(buf, buf_len);
    m3ApiCheckMem(bufused, sizeof(__wasi_size_t));
    if (directory == NULL)
    {
        m3ApiReturn(__WASI_ERRNO_BADF);
    }

    while (used < buf_len && index <= UINT32_MAX)
    {
        OsFsDirEntry entry;
        uint8_t header[24];
        uint32_t name_length = 0U;
        uint32_t chunk = 0U;
        char child[OS_FS_PATH_MAX];
        OsStatus status = os_fs_read_dir_normalized(directory, (uint32_t)index, &entry);

        if (status == OS_STATUS_END_OF_DIRECTORY)
        {
            break;
        }
        if (status != OS_STATUS_OK)
        {
            m3ApiReturn(os_fs_errno(status));
        }

        name_length = (uint32_t)strlen(entry.name);
        if (os_fs_normalize(directory, entry.name, name_length, child, sizeof(child)) != OS_STATUS_OK)
        {
            child[0] = '\0';
        }
        memset(header, 0, sizeof(header));
        m3ApiWriteMem64(header, index + 1U);
        m3ApiWriteMem64(header + 8, os_fs_inode(child));
        m3ApiWriteMem32(header + 16, name_length);
        m3ApiWriteMem8(header + 20, entry.type == OS_FS_TYPE_DIRECTORY
            ? __WASI_FILETYPE_DIRECTORY
            : __WASI_FILETYPE_REGULAR_FILE);

        chunk = buf_len - used < sizeof(header) ? buf_len - used : (uint32_t)sizeof(header);
        memcpy(buf + used, header, chunk);
        used += chunk;
        chunk = buf_len - used < name_length ? buf_len - used : name_length;
        memcpy(buf + used, entry.name, chunk);
        used += chunk;
        ++index;
    }

    m3ApiWriteMem32(bufused, used);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

static m3ApiRawFunction(os_fs_wasi_read_only)
{
    m3ApiReturnType(uint32_t)
    (void)runtime;
    (void)_ctx;
    (void)_sp;
    (void)_mem;
    m3ApiReturn(__WASI_ERRNO_ROFS);
}

static m3ApiRawFunction(os_fs_wasi_not_supported)
{
    m3ApiReturnType(uint32_t)
    (void)runtime;
    (void)_ctx;
    (void)_sp;
    (void)_mem;
    m3ApiReturn(__WASI_ERRNO_NOSYS);
}

static m3ApiRawFunction(os_fs_wasi_no_effect)
{
    m3ApiReturnType(uint32_t)
    (void)runtime;
    (void)_ctx;
    (void)_sp;
    (void)_mem;
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

typedef struct OsFsWasiImport
{
    const char* name;
    const char* signature;
    M3RawCall function;
} OsFsWasiImport;

/* Same layout in wasi_unstable and wasi_snapshot_preview1. */
static const OsFsWasiImport g_wasi_common[] =
{
    { "fd_prestat_get", "i(i*)", os_fs_wasi_fd_prestat_get },
    { "fd_prestat_dir_name", "i(i*i)", os_fs_wasi_fd_prestat_dir_name },
    { "path_open", "i(ii*iiIIi*)", os_fs_wasi_path_open },
    { "fd_read", "i(i*i*)", os_fs_wasi_fd_read },
    { "fd_pread", "i(i*iI*)", os_fs_wasi_fd_pread },
    { "fd_write", "i(i*i*)", os_fs_wasi_fd_write },
    { "fd_tell", "i(i*)", os_fs_wasi_fd_tell },
    { "fd_close", "i(i)", os_fs_wasi_fd_close },
    { "fd_fdstat_get", "i(i*)", os_fs_wasi_fd_fdstat_get },
    { "fd_readdir", "i(i*iI*)", os_fs_wasi_fd_readdir },
    { "fd_advise", "i(iIIi)", os_fs_wasi_no_effect },
    { "fd_datasync", "i(i)", os_fs_wasi_no_effect },
    { "fd_sync", "i(i)", os_fs_wasi_no_effect },
    { "fd_fdstat_set_flags", "i(ii)", os_fs_wasi_read_only },
    { "fd_fdstat_set_rights", "i(iII)", os_fs_wasi_not_supported },
    { "fd_allocate", "i(iII)", os_fs_wasi_read_only },
    { "fd_filestat_set_size", "i(iI)", os_fs_wasi_read_only },
    { "fd_filestat_set_times", "i(iIIi)", os_fs_wasi_read_only },
    { "fd_pwrite", "i(i*iI*)", os_fs_wasi_read_only },
    { "fd_renumber", "i(ii)", os_fs_wasi_not_supported },
    { "path_create_directory", "i(i*i)", os_fs_wasi_read_only },
    { "path_filestat_set_times", "i(ii*iIIi)", os_fs_wasi_read_only },
    { "path_link", "i(ii*ii*i)", os_fs_wasi_read_only },
    { "path_readlink", "i(i*i*i*)", os_fs_wasi_not_supported },
    { "path_remove_directory", "i(i*i)", os_fs_wasi_read_only },
    { "path_rename", "i(i*ii*i)", os_fs_wasi_read_only },
    { "path_symlink", "i(*ii*i)", os_fs_wasi_read_only },
    { "path_unlink_file", "i(i*i)", os_fs_wasi_read_only }
};

static const OsFsWasiImport g_wasi_preview1[] =
{
    { "fd_seek", "i(iIi*)", os_fs_wasi_fd_seek },
    { "fd_filestat_get", "i(i*)", os_fs_wasi_fd_filestat_get },
    { "path_filestat_get", "i(ii*i*)", os_fs_wasi_path_filestat_get }
};

/* wasi_unstable lays out filestat differently; its stat calls are not offered. */
static const OsFsWasiImport g_wasi_unstable[] =
{
    { "fd_seek", "i(iIi*)", os_fs_wasi_unstable_fd_seek },
    { "fd_filestat_get", "i(i*)", os_fs_wasi_not_supported },
    { "path_filestat_get", "i(ii*i*)", os_fs_wasi_not_supported }
};

static M3Result os_fs_link_table(IM3Module module, const char* namespace_name, const OsFsWasiImport* table, uint32_t count)
{
    uint32_t index = 0U;

    for (index = 0U; index < count; ++index)
    {
        M3Result result = m3_LinkRawFunction(
            module,
            namespace_name,
            table[index].name,
            table[index].signature,
            table[index].function
        );
        if (result != m3Err_none && result != m3Err_functionLookupFailed)
        {
            return result;
        }
    }
    return m3Err_none;
}

/*
 * Replaces the file calls bound by m3_LinkWASI (which reach the host file
 * system) with calls into the virtual file system. Must run after it.
 */
M3Result os_fs_link_wasi(IM3Module module)
{
    static const char* const namespaces[2] = { "wasi_unstable", "wasi_snapshot_preview1" };
    uint32_t index = 0U;
    M3Result result = m3Err_none;

    for (index = 0U; index < 2U; ++index)
    {
        result = os_fs_link_table(module, namespaces[index], g_wasi_common,
                                  (uint32_t)(sizeof(g_wasi_common) / sizeof(g_wasi_common[0])));
        if (result != m3Err_none)
        {
            return result;
        }
    }

    result = os_fs_link_table(module, "wasi_snapshot_preview1", g_wasi_preview1,
                              (uint32_t)(sizeof(g_wasi_preview1) / sizeof(g_wasi_preview1[0])));
    if (result != m3Err_none)
    {
        return result;
    }
    return os_fs_link_table(module, "wasi_unstable", g_wasi_unstable,
                            (uint32_t)(sizeof(g_wasi_unstable) / sizeof(g_wasi_unstable[0])));
}

/* ---------------------------------------------------------------------------
 * tar driver
 * ------------------------------------------------------------------------ */

#if OS_FS_TAR

#define OS_TAR_BLOCK 512U

static uint64_t os_tar_octal(const uint8_t* field, uint32_t length)
{
    uint64_t value = 0U;
    uint32_t index = 0U;

    while (index < length && (field[index] == ' ' || field[index] == '\0'))
    {
        ++index;
    }
    for (; index < length && field[index] >= '0' && field[index] <= '7'; ++index)
    {
        value = (value << 3U) | (uint64_t)(field[index] - '0');
    }
    return value;
}

static uint8_t os_tar_is_zero_block(const uint8_t* block)
{
    uint32_t index = 0U;
    for (index = 0U; index < OS_TAR_BLOCK; ++index)
    {
        if (block[index] != 0U)
        {
            return 0U;
        }
    }
    return 1U;
}

static uint8_t os_tar_checksum_ok(const uint8_t* header)
{
    uint64_t expected = os_tar_octal(header + 148, 8U);
    uint64_t sum = 0U;
    uint32_t index = 0U;

    for (index = 0U; index < OS_TAR_BLOCK; ++index)
    {
        sum += (index >= 148U && index < 156U) ? (uint64_t)' ' : header[index];
    }
    return sum == expected;
}

typedef struct OsTarEntry
{
    const uint8_t* header;
    const uint8_t* data;
    uint64_t size;
    OsFsType type;
    /* ustar prefix (155) + '/' + name (100) + NUL */
    char name[155U + 1U + 100U + 1U];
} OsTarEntry;

/* Entry at *offset, advancing *offset; returns 0 at the end of the archive. */
static uint8_t os_tar_next(const OsFsTar* tar, uint32_t* offset, OsTarEntry* entry)
{
    while (*offset + OS_TAR_BLOCK <= tar->size)
    {
        const uint8_t* header = tar->image + *offset;
        uint64_t size = 0U;
        uint64_t data_blocks = 0U;
        uint8_t typeflag = 0U;
        uint32_t length = 0U;
        const char* name = NULL;
        const char* last = NULL;

        if (os_tar_is_zero_block(header))
        {
            return 0U;
        }

        size = os_tar_octal(header + 124, 12U);
        typeflag = header[156];
        data_blocks = (size + OS_TAR_BLOCK - 1U) / OS_TAR_BLOCK;
        *offset += OS_TAR_BLOCK + (uint32_t)(data_blocks * OS_TAR_BLOCK);

        if (typeflag != '0' && typeflag != '\0' && typeflag != '5')
        {
            continue;
        }

        /* ustar keeps long paths as prefix "/" name. */
        if (memcmp(header + 257, "ustar", 5) == 0 && header[345] != '\0')
        {
            uint32_t prefix_length = (uint32_t)strnlen((const char*)header + 345, 155U);
            memcpy(entry->name, header + 345, prefix_length);
            entry->name[prefix_length] = '/';
            length = prefix_length + 1U;
        }
        {
            uint32_t name_length = (uint32_t)strnlen((const char*)header, 100U);
            memcpy(entry->name + length, header, name_length);
            length += name_length;
        }
        entry->name[length] = '\0';

        name = entry->name;
        while (name[0] == '.' && name[1] == '/')
        {
            name += 2;
        }
        while (name[0] == '/')
        {
            ++name;
        }
        memmove(entry->name, name, strlen(name) + 1U);
        length = (uint32_t)strlen(entry->name);
        while (length > 0U && entry->name[length - 1U] == '/')
        {
            entry->name[--length] = '\0';
        }
        if (length == 0U || (length == 1U && entry->name[0] == '.'))
        {
            continue;
        }

        last = strrchr(entry->name, '/');
        last = last != NULL ? last + 1 : entry->name;
        if (last[0] == '.' && last[1] == '_')
        {
            continue;
        }

        entry->header = header;
        entry->data = header + OS_TAR_BLOCK;
        entry->size = size;
        entry->type = typeflag == '5' ? OS_FS_TYPE_DIRECTORY : OS_FS_TYPE_FILE;
        return 1U;
    }
    return 0U;
}

OsStatus os_fs_tar_init(OsFsTar* tar, const uint8_t* image, uint32_t size)
{
    uint32_t offset = 0U;

    if (tar == NULL || image == NULL || size < OS_TAR_BLOCK || size % OS_TAR_BLOCK != 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }

    while (offset + OS_TAR_BLOCK <= size)
    {
        const uint8_t* header = image + offset;
        uint64_t entry_size = 0U;
        uint64_t end = 0U;

        if (os_tar_is_zero_block(header))
        {
            break;
        }
        if (!os_tar_checksum_ok(header))
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
        entry_size = os_tar_octal(header + 124, 12U);
        end = (uint64_t)offset + OS_TAR_BLOCK + (entry_size + OS_TAR_BLOCK - 1U) / OS_TAR_BLOCK * OS_TAR_BLOCK;
        if (end > size)
        {
            return OS_STATUS_INVALID_ARGUMENT;
        }
        offset = (uint32_t)end;
    }

    tar->image = image;
    tar->size = size;
    return OS_STATUS_OK;
}

static OsStatus os_tar_stat(void* context, const char* path, OsFsStat* out_stat)
{
    const OsFsTar* tar = (const OsFsTar*)context;
    uint32_t path_length = (uint32_t)strlen(path);
    uint32_t offset = 0U;
    uint8_t implied_directory = 0U;
    OsTarEntry entry;

    if (path_length == 0U)
    {
        out_stat->type = OS_FS_TYPE_DIRECTORY;
        out_stat->size = 0U;
        return OS_STATUS_OK;
    }

    while (os_tar_next(tar, &offset, &entry))
    {
        if (strcmp(entry.name, path) == 0)
        {
            out_stat->type = entry.type;
            out_stat->size = entry.type == OS_FS_TYPE_FILE ? entry.size : 0U;
            return OS_STATUS_OK;
        }
        if (strncmp(entry.name, path, path_length) == 0 && entry.name[path_length] == '/')
        {
            implied_directory = 1U;
        }
    }

    if (implied_directory)
    {
        out_stat->type = OS_FS_TYPE_DIRECTORY;
        out_stat->size = 0U;
        return OS_STATUS_OK;
    }
    return OS_STATUS_FILE_NOT_FOUND;
}

static OsStatus os_tar_open(void* context, const char* path, void** out_handle)
{
    const OsFsTar* tar = (const OsFsTar*)context;
    uint32_t offset = 0U;
    OsTarEntry entry;

    while (os_tar_next(tar, &offset, &entry))
    {
        if (strcmp(entry.name, path) == 0)
        {
            if (entry.type != OS_FS_TYPE_FILE)
            {
                return OS_STATUS_IS_A_DIRECTORY;
            }
            *out_handle = (void*)entry.header;
            return OS_STATUS_OK;
        }
    }
    return OS_STATUS_FILE_NOT_FOUND;
}

static OsStatus os_tar_read(
    void* context,
    void* handle,
    uint64_t offset,
    void* buffer,
    uint32_t length,
    uint32_t* out_read
)
{
    const uint8_t* header = (const uint8_t*)handle;
    uint64_t size = os_tar_octal(header + 124, 12U);
    (void)context;

    *out_read = 0U;
    if (offset >= size)
    {
        return OS_STATUS_OK;
    }
    if ((uint64_t)length > size - offset)
    {
        length = (uint32_t)(size - offset);
    }
    memcpy(buffer, header + OS_TAR_BLOCK + offset, length);
    *out_read = length;
    return OS_STATUS_OK;
}

static const uint8_t* os_tar_map(void* context, void* handle, uint64_t* out_size)
{
    const uint8_t* header = (const uint8_t*)handle;
    (void)context;
    *out_size = os_tar_octal(header + 124, 12U);
    return header + OS_TAR_BLOCK;
}

/*
 * Children of path in archive order. A child appears once even when it is
 * only implied by deeper entries or listed more than once.
 */
static OsStatus os_tar_read_dir(void* context, const char* path, uint32_t index, OsFsDirEntry* out_entry)
{
    const OsFsTar* tar = (const OsFsTar*)context;
    uint32_t path_length = (uint32_t)strlen(path);
    uint32_t offset = 0U;
    uint32_t found = 0U;
    OsTarEntry entry;

    while (os_tar_next(tar, &offset, &entry))
    {
        const char* rest = NULL;
        const char* slash = NULL;
        uint32_t child_length = 0U;
        uint8_t duplicate = 0U;
        uint32_t earlier = 0U;
        OsTarEntry previous;

        if (path_length == 0U)
        {
            rest = entry.name;
        }
        else if (strncmp(entry.name, path, path_length) == 0 && entry.name[path_length] == '/')
        {
            rest = entry.name + path_length + 1U;
        }
        if (rest == NULL || rest[0] == '\0')
        {
            continue;
        }

        slash = strchr(rest, '/');
        child_length = slash != NULL ? (uint32_t)(slash - rest) : (uint32_t)strlen(rest);
        if (child_length > OS_FS_NAME_MAX)
        {
            continue;
        }

        /* Skip a child already produced by an earlier entry. */
        while (os_tar_next(tar, &earlier, &previous) && previous.header != entry.header)
        {
            const char* previous_rest = NULL;
            if (path_length == 0U)
            {
                previous_rest = previous.name;
            }
            else if (strncmp(previous.name, path, path_length) == 0 && previous.name[path_length] == '/')
            {
                previous_rest = previous.name + path_length + 1U;
            }
            if (previous_rest != NULL && strncmp(previous_rest, rest, child_length) == 0 &&
                (previous_rest[child_length] == '\0' || previous_rest[child_length] == '/'))
            {
                duplicate = 1U;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        if (found == index)
        {
            memcpy(out_entry->name, rest, child_length);
            out_entry->name[child_length] = '\0';
            out_entry->type = (slash != NULL || entry.type == OS_FS_TYPE_DIRECTORY)
                ? OS_FS_TYPE_DIRECTORY
                : OS_FS_TYPE_FILE;
            return OS_STATUS_OK;
        }
        ++found;
    }
    return OS_STATUS_END_OF_DIRECTORY;
}

const OsFsDriver os_fs_tar_driver =
{
    os_tar_stat,
    os_tar_open,
    os_tar_read,
    NULL,
    os_tar_read_dir,
    os_tar_map
};

#endif /* OS_FS_TAR */

#endif /* OS_HAS_FS */
