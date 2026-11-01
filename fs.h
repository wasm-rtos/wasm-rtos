#ifndef WASM_RTOS_FS_H
#define WASM_RTOS_FS_H

#include <stdint.h>

#include "os.h"

#if OS_HAS_FS

/*
 * Virtual file system. Programs see one tree through standard WASI calls
 * (path_open, fd_read, fd_seek, fd_readdir, path_filestat_get, ...); the
 * kernel routes every path to the file system mounted on its longest
 * matching prefix. Directories that only contain mount points ("/" with
 * "/bin" and "/apps" mounted) exist implicitly.
 *
 * This first version is read only: calls that would create, change or
 * remove files fail with the WASI error ROFS.
 */

#define OS_FS_PATH_MAX 256U
#define OS_FS_NAME_MAX 100U

typedef enum OsFsType
{
    OS_FS_TYPE_FILE = 1,
    OS_FS_TYPE_DIRECTORY = 2
} OsFsType;

typedef struct OsFsStat
{
    OsFsType type;
    uint64_t size;
} OsFsStat;

typedef struct OsFsDirEntry
{
    OsFsType type;
    char name[OS_FS_NAME_MAX + 1U];
} OsFsDirEntry;

/*
 * A file system driver. Paths are relative to the mount point, use '/' as
 * separator, have no leading or trailing '/', and "" names the mount root.
 * Status codes: OS_STATUS_FILE_NOT_FOUND, OS_STATUS_NOT_A_DIRECTORY,
 * OS_STATUS_IS_A_DIRECTORY, OS_STATUS_END_OF_DIRECTORY.
 */
typedef struct OsFsDriver
{
    OsStatus (*stat)(void* context, const char* path, OsFsStat* out_stat);
    OsStatus (*open)(void* context, const char* path, void** out_handle);
    OsStatus (*read)(
        void* context,
        void* handle,
        uint64_t offset,
        void* buffer,
        uint32_t length,
        uint32_t* out_read
    );
    void (*close)(void* context, void* handle);
    /* Entry number index of directory path, in a stable order. */
    OsStatus (*read_dir)(void* context, const char* path, uint32_t index, OsFsDirEntry* out_entry);
    /* Optional: the file contents in addressable memory, or NULL. */
    const uint8_t* (*map)(void* context, void* handle, uint64_t* out_size);
} OsFsDriver;

/* path is absolute ("/" or "/apps"); the driver and context must outlive the mount. */
OsStatus os_fs_mount(const char* path, const OsFsDriver* driver, void* context);
OsStatus os_fs_unmount(const char* path);
OsStatus os_fs_stat(const char* path, OsFsStat* out_stat);
OsStatus os_fs_read_dir(const char* path, uint32_t index, OsFsDirEntry* out_entry);

#if OS_FS_TAR
/*
 * Read-only file system over a ustar archive (`tar -cf image.tar ...`).
 * File data is served in place, so an image in memory-mapped flash needs
 * no copy. Entries whose name starts with "._" (macOS metadata) are
 * ignored; set COPYFILE_DISABLE=1 when creating the archive on macOS to
 * leave them out entirely.
 */
typedef struct OsFsTar
{
    const uint8_t* image;
    uint32_t size;
} OsFsTar;

/* Validates every header; the image must stay valid while it is mounted. */
OsStatus os_fs_tar_init(OsFsTar* tar, const uint8_t* image, uint32_t size);
extern const OsFsDriver os_fs_tar_driver;
#endif

/*
 * A program image opened by path. When the driver maps the file, bytes point
 * into the file system itself (no copy) and the mount stays busy until the
 * image is closed; otherwise the file is read into a heap copy.
 */
typedef struct OsFsImage
{
    const uint8_t* bytes;
    uint32_t size;
    const OsFsDriver* driver;
    void* context;
    void* handle;
    uint8_t* copy;
} OsFsImage;

OsStatus os_fs_image_open(const char* path, OsFsImage* out_image);
void os_fs_image_close(OsFsImage* image);

/* Kernel internal. */
M3Result os_fs_link_wasi(IM3Module module);
void os_fs_release_task(uint32_t task_id);
void os_fs_shutdown(void);

#endif /* OS_HAS_FS */

#endif
