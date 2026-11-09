/*
 * The WASI preview 1 types and values the kernel uses, as programs see
 * them: wasm32, little-endian.
 */
#ifndef WASM_RTOS_WASI_H
#define WASM_RTOS_WASI_H

#include <stdint.h>

typedef uint32_t __wasi_size_t;
typedef uint16_t __wasi_errno_t;
typedef uint32_t __wasi_fd_t;
typedef uint64_t __wasi_filesize_t;
typedef int64_t __wasi_filedelta_t;
typedef uint64_t __wasi_dircookie_t;
typedef uint64_t __wasi_rights_t;
typedef uint16_t __wasi_fdflags_t;
typedef uint16_t __wasi_oflags_t;
typedef uint32_t __wasi_lookupflags_t;

#define __WASI_ERRNO_SUCCESS (UINT16_C(0))
#define __WASI_ERRNO_BADF (UINT16_C(8))
#define __WASI_ERRNO_FAULT (UINT16_C(21))
#define __WASI_ERRNO_INVAL (UINT16_C(28))
#define __WASI_ERRNO_IO (UINT16_C(29))
#define __WASI_ERRNO_ISDIR (UINT16_C(31))
#define __WASI_ERRNO_MFILE (UINT16_C(33))
#define __WASI_ERRNO_NAMETOOLONG (UINT16_C(37))
#define __WASI_ERRNO_NOENT (UINT16_C(44))
#define __WASI_ERRNO_NOMEM (UINT16_C(48))
#define __WASI_ERRNO_NOSYS (UINT16_C(52))
#define __WASI_ERRNO_NOTDIR (UINT16_C(54))
#define __WASI_ERRNO_ROFS (UINT16_C(69))
#define __WASI_ERRNO_SPIPE (UINT16_C(70))

#define __WASI_CLOCKID_THREAD_CPUTIME_ID (UINT32_C(3))
#define __WASI_EVENTTYPE_CLOCK (UINT8_C(0))
#define __WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME (UINT16_C(1))

#define __WASI_FILETYPE_CHARACTER_DEVICE (UINT8_C(2))
#define __WASI_FILETYPE_DIRECTORY (UINT8_C(3))
#define __WASI_FILETYPE_REGULAR_FILE (UINT8_C(4))
#define __WASI_PREOPENTYPE_DIR (UINT8_C(0))

#define __WASI_FDFLAGS_APPEND (UINT16_C(1))
#define __WASI_FDFLAGS_DSYNC (UINT16_C(2))
#define __WASI_FDFLAGS_RSYNC (UINT16_C(8))
#define __WASI_FDFLAGS_SYNC (UINT16_C(16))

#define __WASI_OFLAGS_CREAT (UINT16_C(1))
#define __WASI_OFLAGS_DIRECTORY (UINT16_C(2))
#define __WASI_OFLAGS_EXCL (UINT16_C(4))
#define __WASI_OFLAGS_TRUNC (UINT16_C(8))

#define __WASI_WHENCE_SET (UINT8_C(0))
#define __WASI_WHENCE_CUR (UINT8_C(1))
#define __WASI_WHENCE_END (UINT8_C(2))

#define __WASI_RIGHTS_FD_READ (UINT64_C(2))
#define __WASI_RIGHTS_FD_SEEK (UINT64_C(4))
#define __WASI_RIGHTS_FD_TELL (UINT64_C(32))
#define __WASI_RIGHTS_FD_WRITE (UINT64_C(64))
#define __WASI_RIGHTS_FD_ADVISE (UINT64_C(128))
#define __WASI_RIGHTS_FD_ALLOCATE (UINT64_C(256))
#define __WASI_RIGHTS_PATH_OPEN (UINT64_C(8192))
#define __WASI_RIGHTS_FD_READDIR (UINT64_C(16384))
#define __WASI_RIGHTS_PATH_FILESTAT_GET (UINT64_C(262144))
#define __WASI_RIGHTS_FD_FILESTAT_GET (UINT64_C(2097152))
#define __WASI_RIGHTS_FD_FILESTAT_SET_SIZE (UINT64_C(4194304))

#endif
