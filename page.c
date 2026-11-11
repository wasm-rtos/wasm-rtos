/*
 * Paging: programs' memory in frames (4 KiB by default) with RAM a cache of it, and a
 * page store the platform gives for the rest. The interpreter moves frames
 * through the pager here; the kernel decides which frame leaves RAM (CLOCK:
 * the first one not touched since the hand last passed it) and keeps the
 * promise that every frame a program was given has a slot to go to.
 *
 * A frame keeps its slot once stored, so a frame that came back and was
 * only read leaves RAM again without a write; only dirty frames are written.
 * Those go like a log: each to the next free slot after the last one
 * written, freeing the slot it had, so the store sees writes one after
 * another (what SD cards and flash do fastest) instead of all over it.
 *
 * A store that reads in the background lets a program that touched a
 * stored frame wait for it while the others run: the read is noted here,
 * the store says when it ends (os_page_read_done, from anywhere), and the
 * scheduler hands the frame over and wakes the program (os_page_poll).
 * A store that writes in the background takes dirty frames on their way
 * out the same way: nobody waits, and a frame's RAM goes when its write
 * ends (os_page_write_done). Frames being written do not count against
 * the RAM budget, but at most OS_PAGE_MAX_WRITES are.
 */

#include "os_internal.h"

#include <stdlib.h>
#include <string.h>

/* Reads and writes in the background at once; one more is made at once instead. */
#ifndef OS_PAGE_MAX_READS
#define OS_PAGE_MAX_READS 8U
#endif
#ifndef OS_PAGE_MAX_WRITES
#define OS_PAGE_MAX_WRITES 4U
#endif

/*
 * Flags the store's side may read or set from another thread or core: a
 * read or write ends with its status written first and done set after it
 * (release), and the kernel takes done before the status (acquire).
 */
#if defined(__STDC_NO_ATOMICS__)
typedef volatile uint8_t OsPageFlag;
#define OS_PAGE_FLAG_GET(flag) (flag)
#define OS_PAGE_FLAG_PUT(flag, value) ((flag) = (uint8_t)(value))
#else
#include <stdatomic.h>
typedef atomic_uchar OsPageFlag;
#define OS_PAGE_FLAG_GET(flag) atomic_load_explicit(&(flag), memory_order_acquire)
#define OS_PAGE_FLAG_PUT(flag, value) atomic_store_explicit(&(flag), (unsigned char)(value), memory_order_release)
#endif

/* A frame the store reads (for a waiting program) or writes (on its way out) in the background. */
typedef struct OsPageRead
{
    WmMemory* memory;
    uint32_t index;
    uint32_t slot;
    uint32_t task_id;
    OsPageFlag busy;
    uint8_t writing;
    /* Set by os_page_read_done or os_page_write_done, from anywhere, after status. */
    OsPageFlag done;
    OsStatus status;
} OsPageRead;

static struct
{
    OsPageStore store;
    uint8_t on;
    /* A bit per slot: in use. */
    uint32_t* used;
    uint32_t reserved;
    uint32_t stored;
    uint32_t stores;
    uint32_t loads;
    /* Where the next frame written goes, or the first free slot after it. */
    uint32_t head;
    OsPageRead reads[OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES];
    /* Writes in the background now. */
    uint32_t writes;
    /* A slot whose background read failed: the next read of it is made at once. */
    uint32_t failed_slot;
} g_page;

static const char os_page_store_full[] = "exhaustion: the page store has no free slot";
static const char os_page_store_failed[] = "exhaustion: the page store could not move a frame";

/* Every frame may come to need a slot of its own. */
static uint32_t os_page_limit(void)
{
    return g_page.store.slots;
}

static int os_page_reserve(void* context, int32_t frames)
{
    (void)context;
    if (frames > 0 && (uint64_t)g_page.reserved + (uint32_t)frames > os_page_limit())
    {
        return 0;
    }
    g_page.reserved += (uint32_t)frames;
    return 1;
}

/*
 * Take one frame out of RAM that was not touched since the hand last
 * passed it (CLOCK, over the frames in RAM only): 0 when none may go.
 */
static int os_page_take_one(void)
{
    WmMemory* memory = NULL;
    uint32_t index = 0U;
    uint32_t step = 0U;
    /* Two rounds clear every mark, so a third finds a frame if any may go. */
    uint32_t steps = 3U * wm_pager_resident();

    for (step = 0U; step < steps && wm_pager_hand(&memory, &index); ++step)
    {
        if (wm_memory_frame_look(memory, index) == 0)
        {
            WmResult result = wm_memory_frame_take(memory, index);
            if (result == NULL || result == wm_page_wait)
            {
                return 1;
            }
        }
    }
    return 0;
}

static int os_page_finish_a_write(void);

static int os_page_room(void* context, int again)
{
    (void)context;
    if (again)
    {
        /* No heap for a frame: a write ends and its frame's RAM goes, or one more frame leaves now. */
        if (os_page_finish_a_write())
        {
            return 1;
        }
        return os_page_take_one() && (g_page.writes == 0U || os_page_finish_a_write());
    }
    /* Pinned frames may keep RAM over its budget for a while; frames being written are on their way out. */
    while (g_page.store.ram_frames != 0U && wm_pager_resident() - g_page.writes >= g_page.store.ram_frames)
    {
        if (g_page.writes == OS_PAGE_MAX_WRITES)
        {
            (void)os_page_finish_a_write();
        }
        else if (!os_page_take_one())
        {
            break;
        }
    }
    return 1;
}

static int os_page_slot_used(uint32_t slot)
{
    return (g_page.used[slot / 32U] & (1UL << (slot % 32U))) != 0U;
}

static void os_page_use_slot(uint32_t slot)
{
    g_page.used[slot / 32U] |= 1UL << (slot % 32U);
    ++g_page.stored;
}

static void os_page_free_slot(uint32_t slot)
{
    g_page.used[slot / 32U] &= ~(1UL << (slot % 32U));
    --g_page.stored;
}

/*
 * A slot for a dirty frame: the next free one after the last written, and
 * the slot it had is free from then on. Undone with os_page_slot_back when
 * the write fails.
 */
static uint32_t os_page_next_slot(uint32_t old)
{
    uint32_t index = g_page.head;
    uint32_t step = 0U;

    if (old != 0xFFFFFFFFUL)
    {
        os_page_free_slot(old);
    }
    while (step < g_page.store.slots && os_page_slot_used(index))
    {
        index = index + 1U == g_page.store.slots ? 0U : index + 1U;
        ++step;
    }
    if (step == g_page.store.slots)
    {
        if (old != 0xFFFFFFFFUL)
        {
            os_page_use_slot(old);
        }
        return 0xFFFFFFFFUL;
    }
    os_page_use_slot(index);
    g_page.head = index + 1U == g_page.store.slots ? 0U : index + 1U;
    return index;
}

static void os_page_slot_back(uint32_t old, uint32_t index)
{
    os_page_free_slot(index);
    if (old != 0xFFFFFFFFUL)
    {
        os_page_use_slot(old);
    }
}

static WmResult os_page_write(void* context, uint32_t* slot, const uint8_t* frame)
{
    uint32_t old = *slot;
    uint32_t index = os_page_next_slot(old);
    (void)context;

    if (index == 0xFFFFFFFFUL)
    {
        return os_page_store_full;
    }
    if (g_page.store.write(g_page.store.context, index, frame) != OS_STATUS_OK)
    {
        os_page_slot_back(old, index);
        return os_page_store_failed;
    }
    ++g_page.stores;
    *slot = index;
    return NULL;
}

static WmResult os_page_read(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    if (g_page.store.read(g_page.store.context, slot, frame) != OS_STATUS_OK)
    {
        return os_page_store_failed;
    }
    ++g_page.loads;
    return NULL;
}

static void os_page_drop(void* context, uint32_t slot)
{
    (void)context;
    os_page_free_slot(slot);
}

static OsPageRead* os_page_read_of(uint32_t slot)
{
    uint32_t i = 0U;

    for (i = 0U; i < OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES; ++i)
    {
        if (OS_PAGE_FLAG_GET(g_page.reads[i].busy) && g_page.reads[i].slot == slot)
        {
            return &g_page.reads[i];
        }
    }
    return NULL;
}

static WmResult os_page_read_start(void* context, uint32_t slot, uint8_t* frame, WmMemory* memory, uint32_t index)
{
    OsPageRead* read = NULL;
    OsStatus status = OS_STATUS_OK;
    uint32_t i = 0U;
    (void)context;

    for (i = 0U; i < OS_PAGE_MAX_READS && read == NULL; ++i)
    {
        read = OS_PAGE_FLAG_GET(g_page.reads[i].busy) ? NULL : &g_page.reads[i];
    }
    /* No room to note it, or it failed in the background just now: read it at once. */
    if (read == NULL || slot == g_page.failed_slot)
    {
        g_page.failed_slot = 0xFFFFFFFFUL;
        return os_page_read(NULL, slot, frame);
    }
    read->memory = memory;
    read->index = index;
    read->slot = slot;
    read->task_id = os_task_get_id(os_task_get_current());
    OS_PAGE_FLAG_PUT(read->done, 0U);
    read->writing = 0U;
    OS_PAGE_FLAG_PUT(read->busy, 1U);
    status = g_page.store.read_start(g_page.store.context, slot, frame);
    if (status == OS_STATUS_PENDING)
    {
        return wm_page_wait;
    }
    OS_PAGE_FLAG_PUT(read->busy, 0U);
    /* The store cannot start another now: read it at once. */
    if (status == OS_STATUS_BUSY)
    {
        return os_page_read(NULL, slot, frame);
    }
    if (status != OS_STATUS_OK)
    {
        return os_page_store_failed;
    }
    ++g_page.loads;
    return NULL;
}

/* Someone needs a frame still being read: the read ends now, and its program may go on. */
static WmResult os_page_finish(void* context, uint32_t slot)
{
    OsPageRead* read = os_page_read_of(slot);
    OsStatus status = OS_STATUS_OK;
    (void)context;

    if (read == NULL)
    {
        return NULL;
    }
    status = OS_PAGE_FLAG_GET(read->done) ? read->status : g_page.store.finish(g_page.store.context, slot);
    OS_PAGE_FLAG_PUT(read->busy, 0U);
    if (read->writing)
    {
        /* The memory frees or keeps the frame itself. */
        --g_page.writes;
        g_page.stores += status == OS_STATUS_OK ? 1U : 0U;
        return status == OS_STATUS_OK ? NULL : os_page_store_failed;
    }
    if (status == OS_STATUS_OK)
    {
        ++g_page.loads;
    }
    os_page_wait_end(read->task_id);
    return status == OS_STATUS_OK ? NULL : os_page_store_failed;
}

/* A write in the background ended (status): the frame's RAM goes, or it stays, dirty. */
static void os_page_write_ended(OsPageRead* write, OsStatus status)
{
    OS_PAGE_FLAG_PUT(write->busy, 0U);
    --g_page.writes;
    g_page.stores += status == OS_STATUS_OK ? 1U : 0U;
    wm_memory_frame_stored(write->memory, write->index, status == OS_STATUS_OK ? NULL : os_page_store_failed);
}

/* Wait for one write in flight to end: 0 when none is. */
static int os_page_finish_a_write(void)
{
    uint32_t i = 0U;

    for (i = 0U; i < OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES; ++i)
    {
        OsPageRead* write = &g_page.reads[i];
        if (OS_PAGE_FLAG_GET(write->busy) && write->writing)
        {
            os_page_write_ended(write, OS_PAGE_FLAG_GET(write->done) ? write->status : g_page.store.finish(g_page.store.context, write->slot));
            return 1;
        }
    }
    return 0;
}

static WmResult os_page_write_start(void* context, uint32_t* slot, const uint8_t* frame, WmMemory* memory, uint32_t index)
{
    OsPageRead* write = NULL;
    uint32_t old = *slot;
    uint32_t chosen = 0U;
    OsStatus status = OS_STATUS_OK;
    uint32_t i = 0U;

    for (i = 0U; i < OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES && write == NULL; ++i)
    {
        write = OS_PAGE_FLAG_GET(g_page.reads[i].busy) ? NULL : &g_page.reads[i];
    }
    if (write == NULL || g_page.writes == OS_PAGE_MAX_WRITES)
    {
        return os_page_write(context, slot, frame);
    }
    chosen = os_page_next_slot(old);
    if (chosen == 0xFFFFFFFFUL)
    {
        return os_page_store_full;
    }
    write->memory = memory;
    write->index = index;
    write->slot = chosen;
    write->task_id = 0U;
    OS_PAGE_FLAG_PUT(write->done, 0U);
    write->writing = 1U;
    OS_PAGE_FLAG_PUT(write->busy, 1U);
    status = g_page.store.write_start(g_page.store.context, chosen, frame);
    if (status == OS_STATUS_PENDING)
    {
        ++g_page.writes;
        *slot = chosen;
        return wm_page_wait;
    }
    OS_PAGE_FLAG_PUT(write->busy, 0U);
    os_page_slot_back(old, chosen);
    /* The store cannot start another now: write it at once. */
    return status == OS_STATUS_BUSY ? os_page_write(context, slot, frame) : os_page_store_failed;
}

void os_page_write_done(uint32_t slot, OsStatus status)
{
    os_page_read_done(slot, status);
}

void os_page_read_done(uint32_t slot, OsStatus status)
{
    OsPageRead* read = os_page_read_of(slot);

    if (read != NULL)
    {
        read->status = status;
        OS_PAGE_FLAG_PUT(read->done, 1U);
    }
}

void os_page_poll(void)
{
    uint32_t i = 0U;

    for (i = 0U; i < OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES; ++i)
    {
        OsPageRead* read = &g_page.reads[i];
        if (OS_PAGE_FLAG_GET(read->busy) && OS_PAGE_FLAG_GET(read->done) && read->writing)
        {
            os_page_write_ended(read, read->status);
        }
        else if (OS_PAGE_FLAG_GET(read->busy) && OS_PAGE_FLAG_GET(read->done))
        {
            OsStatus status = read->status;
            OS_PAGE_FLAG_PUT(read->busy, 0U);
            if (status == OS_STATUS_OK)
            {
                ++g_page.loads;
            }
            else
            {
                g_page.failed_slot = read->slot;
            }
            wm_memory_frame_loaded(read->memory, read->index, status == OS_STATUS_OK ? NULL : os_page_store_failed);
            os_page_wait_end(read->task_id);
        }
    }
}

static WmPager os_pager = {NULL, os_page_reserve, os_page_room, os_page_write, os_page_read, os_page_drop, NULL, NULL, NULL};

OsStatus os_page_store_set(const OsPageStore* store)
{
    uint32_t* used = NULL;

    if (os_get_task_count() != 0U)
    {
        return OS_STATUS_BUSY;
    }
    if (store != NULL && (store->write == NULL || store->read == NULL || store->slots == 0U ||
                          ((store->read_start != NULL || store->write_start != NULL) && store->finish == NULL)))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (store != NULL)
    {
        used = (uint32_t*)calloc((store->slots + 31U) / 32U, sizeof(uint32_t));
        if (used == NULL)
        {
            return OS_STATUS_OUT_OF_MEMORY;
        }
    }
    os_page_shutdown();
    if (store != NULL)
    {
        g_page.store = *store;
        g_page.used = used;
        g_page.on = 1U;
        g_page.failed_slot = 0xFFFFFFFFUL;
        /* Background reads only when the store has both halves of them. */
        os_pager.read_start = store->read_start != NULL ? os_page_read_start : NULL;
        os_pager.write_start = store->write_start != NULL ? os_page_write_start : NULL;
        os_pager.finish = store->finish != NULL ? os_page_finish : NULL;
        wm_set_pager(&os_pager);
    }
    return OS_STATUS_OK;
}

int os_page_store_on(void)
{
    return g_page.on;
}

/* ---- State images: a snapshot in slots, a frame's size at a time ---- */

/* A slot holds a frame: WM_FRAME_SIZE bytes, as the interpreter is built. */
#define OS_PAGE_SLOT_BYTES wm_frame_size()

typedef struct OsPageImageStream
{
    OsPageImage* image;
    const OsPageImage* source;
    uint8_t* buffer;
    uint32_t used;
    uint32_t next;
    OsStatus status;
} OsPageImageStream;

/* The buffer, full or the last part padded, into a slot reserved for it. */
static OsStatus os_page_image_flush(OsPageImageStream* stream)
{
    OsPageImage* image = stream->image;
    uint32_t* slots = NULL;
    uint32_t slot = 0U;

    if (!os_page_reserve(NULL, 1))
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    slots = (uint32_t*)realloc(image->slots, ((size_t)image->count + 1U) * sizeof(uint32_t));
    if (slots == NULL)
    {
        os_page_reserve(NULL, -1);
        return OS_STATUS_OUT_OF_MEMORY;
    }
    image->slots = slots;
    slot = os_page_next_slot(0xFFFFFFFFUL);
    if (slot == 0xFFFFFFFFUL)
    {
        os_page_reserve(NULL, -1);
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memset(stream->buffer + stream->used, 0, OS_PAGE_SLOT_BYTES - stream->used);
    if (g_page.store.write(g_page.store.context, slot, stream->buffer) != OS_STATUS_OK)
    {
        os_page_free_slot(slot);
        os_page_reserve(NULL, -1);
        return OS_STATUS_ERROR;
    }
    ++g_page.stores;
    image->slots[image->count++] = slot;
    stream->used = 0U;
    return OS_STATUS_OK;
}

static WmResult os_page_image_put(void* context, const void* data, uint32_t size)
{
    OsPageImageStream* stream = (OsPageImageStream*)context;
    const uint8_t* bytes = (const uint8_t*)data;

    while (size > 0U && stream->status == OS_STATUS_OK)
    {
        uint32_t n = OS_PAGE_SLOT_BYTES - stream->used < size ? OS_PAGE_SLOT_BYTES - stream->used : size;
        memcpy(stream->buffer + stream->used, bytes, n);
        stream->used += n;
        stream->image->bytes += n;
        bytes += n;
        size -= n;
        if (stream->used == OS_PAGE_SLOT_BYTES)
        {
            stream->status = os_page_image_flush(stream);
        }
    }
    return stream->status == OS_STATUS_OK ? NULL : os_page_store_failed;
}

static WmResult os_page_image_get(void* context, void* data, uint32_t size)
{
    OsPageImageStream* stream = (OsPageImageStream*)context;
    uint8_t* bytes = (uint8_t*)data;

    while (size > 0U && stream->status == OS_STATUS_OK)
    {
        uint32_t n = 0U;
        if (stream->used == OS_PAGE_SLOT_BYTES || stream->next == 0U)
        {
            if (stream->next == stream->source->count)
            {
                stream->status = OS_STATUS_ERROR;
                break;
            }
            stream->status = g_page.store.read(g_page.store.context, stream->source->slots[stream->next++], stream->buffer);
            stream->used = 0U;
            ++g_page.loads;
            continue;
        }
        n = OS_PAGE_SLOT_BYTES - stream->used < size ? OS_PAGE_SLOT_BYTES - stream->used : size;
        memcpy(bytes, stream->buffer + stream->used, n);
        stream->used += n;
        bytes += n;
        size -= n;
    }
    return stream->status == OS_STATUS_OK ? NULL : os_page_store_failed;
}

OsStatus os_page_image_save(OsPageImage* image, WmRuntime* runtime, WmInstance* instance, WmResult* out_result)
{
    OsPageImageStream stream;

    memset(image, 0, sizeof(*image));
    memset(&stream, 0, sizeof(stream));
    stream.image = image;
    stream.buffer = (uint8_t*)malloc(OS_PAGE_SLOT_BYTES);
    if (!g_page.on || stream.buffer == NULL)
    {
        free(stream.buffer);
        return g_page.on ? OS_STATUS_OUT_OF_MEMORY : OS_STATUS_UNSUPPORTED;
    }
    *out_result = wm_snapshot_write_state(runtime, instance, os_page_image_put, &stream);
    if (*out_result == NULL && stream.used > 0U)
    {
        stream.status = os_page_image_flush(&stream);
    }
    free(stream.buffer);
    if (*out_result != NULL || stream.status != OS_STATUS_OK)
    {
        os_page_image_drop(image);
        return stream.status != OS_STATUS_OK ? stream.status : OS_STATUS_WASM_ERROR;
    }
    return OS_STATUS_OK;
}

OsStatus os_page_image_load(const OsPageImage* image, WmRuntime* runtime, WmInstance* instance, WmResult* out_result)
{
    OsPageImageStream stream;

    memset(&stream, 0, sizeof(stream));
    stream.source = image;
    stream.buffer = (uint8_t*)malloc(OS_PAGE_SLOT_BYTES);
    if (stream.buffer == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    *out_result = wm_snapshot_read(runtime, instance, os_page_image_get, &stream);
    free(stream.buffer);
    if (stream.status != OS_STATUS_OK)
    {
        return stream.status;
    }
    return *out_result == NULL ? OS_STATUS_OK : OS_STATUS_WASM_ERROR;
}

void os_page_image_drop(OsPageImage* image)
{
    uint32_t i = 0U;

    for (i = 0U; i < image->count && g_page.on; ++i)
    {
        os_page_free_slot(image->slots[i]);
    }
    if (image->count > 0U && g_page.on)
    {
        os_page_reserve(NULL, -(int32_t)image->count);
    }
    free(image->slots);
    memset(image, 0, sizeof(*image));
}

OsStatus os_page_get_stats(OsPageStats* out_stats)
{
    if (out_stats == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (!g_page.on)
    {
        return OS_STATUS_OK;
    }
    out_stats->reserved_frames = g_page.reserved;
    out_stats->limit_frames = os_page_limit();
    out_stats->ram_frames = wm_pager_resident();
    out_stats->stored_frames = g_page.stored;
    out_stats->stores = g_page.stores;
    out_stats->loads = g_page.loads;
    return OS_STATUS_OK;
}

void os_page_shutdown(void)
{
    if (g_page.on)
    {
        wm_set_pager(NULL);
    }
    free(g_page.used);
    memset(&g_page, 0, sizeof(g_page));
}
