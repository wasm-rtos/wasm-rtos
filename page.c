/*
 * Paging: programs' memory in 4 KiB frames with RAM a cache of it, and a
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
 */

#include "os_internal.h"

#include <stdlib.h>
#include <string.h>

/* Background reads at once; a read when all are busy is made at once instead. */
#ifndef OS_PAGE_MAX_READS
#define OS_PAGE_MAX_READS 8U
#endif

/* A frame the store reads in the background, for a waiting program. */
typedef struct OsPageRead
{
    WmMemory* memory;
    uint32_t index;
    uint32_t slot;
    uint32_t task_id;
    uint8_t busy;
    /* Set by os_page_read_done, from anywhere. */
    volatile uint8_t done;
    volatile OsStatus status;
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
    /* The CLOCK hand: a frame's place counting through all memories. */
    uint32_t hand;
    OsPageRead reads[OS_PAGE_MAX_READS];
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

/* Store one frame that was not touched lately: 0 when there is none to take. */
static int os_page_take_one(void)
{
    uint32_t total = 0U;
    uint32_t step = 0U;
    WmMemory* memory = NULL;

    for (memory = wm_memory_next(NULL); memory != NULL; memory = wm_memory_next(memory))
    {
        total += wm_memory_frames(memory);
    }
    /* Two rounds clear every mark, so a third finds a frame if any may go. */
    for (step = 0U; total != 0U && step < 3U * total; ++step)
    {
        uint32_t at = g_page.hand++ % total;
        for (memory = wm_memory_next(NULL); memory != NULL; memory = wm_memory_next(memory))
        {
            uint32_t frames = wm_memory_frames(memory);
            if (at < frames)
            {
                if (wm_memory_frame_look(memory, at) == 0 && wm_memory_frame_take(memory, at) == NULL)
                {
                    return 1;
                }
                break;
            }
            at -= frames;
        }
    }
    return 0;
}

static int os_page_room(void* context, int again)
{
    (void)context;
    if (again)
    {
        return os_page_take_one();
    }
    /* Pinned frames may keep RAM over its budget for a while. */
    while (g_page.store.ram_frames != 0U && wm_pager_resident() >= g_page.store.ram_frames)
    {
        if (!os_page_take_one())
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

static WmResult os_page_write(void* context, uint32_t* slot, const uint8_t* frame)
{
    uint32_t old = *slot;
    uint32_t index = g_page.head;
    uint32_t step = 0U;
    (void)context;

    /* The frame's old slot is free from now on, unless this write fails. */
    if (old != 0xFFFFFFFFUL)
    {
        os_page_free_slot(old);
    }
    while (step < g_page.store.slots && os_page_slot_used(index))
    {
        index = index + 1U == g_page.store.slots ? 0U : index + 1U;
        ++step;
    }
    if (step == g_page.store.slots || g_page.store.write(g_page.store.context, index, frame) != OS_STATUS_OK)
    {
        if (old != 0xFFFFFFFFUL)
        {
            os_page_use_slot(old);
        }
        return step == g_page.store.slots ? os_page_store_full : os_page_store_failed;
    }
    os_page_use_slot(index);
    ++g_page.stores;
    g_page.head = index + 1U == g_page.store.slots ? 0U : index + 1U;
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

    for (i = 0U; i < OS_PAGE_MAX_READS; ++i)
    {
        if (g_page.reads[i].busy && g_page.reads[i].slot == slot)
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
        read = g_page.reads[i].busy ? NULL : &g_page.reads[i];
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
    read->done = 0U;
    read->busy = 1U;
    status = g_page.store.read_start(g_page.store.context, slot, frame);
    if (status == OS_STATUS_PENDING)
    {
        return wm_page_wait;
    }
    read->busy = 0U;
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
    status = read->done ? read->status : g_page.store.finish(g_page.store.context, slot);
    read->busy = 0U;
    if (status == OS_STATUS_OK)
    {
        ++g_page.loads;
    }
    os_page_wait_end(read->task_id);
    return status == OS_STATUS_OK ? NULL : os_page_store_failed;
}

void os_page_read_done(uint32_t slot, OsStatus status)
{
    OsPageRead* read = os_page_read_of(slot);

    if (read != NULL)
    {
        read->status = status;
        read->done = 1U;
    }
}

void os_page_poll(void)
{
    uint32_t i = 0U;

    for (i = 0U; i < OS_PAGE_MAX_READS; ++i)
    {
        OsPageRead* read = &g_page.reads[i];
        if (read->busy && read->done)
        {
            OsStatus status = read->status;
            read->busy = 0U;
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

static WmPager os_pager = {NULL, os_page_reserve, os_page_room, os_page_write, os_page_read, os_page_drop, NULL, NULL};

OsStatus os_page_store_set(const OsPageStore* store)
{
    uint32_t* used = NULL;

    if (os_get_task_count() != 0U)
    {
        return OS_STATUS_BUSY;
    }
    if (store != NULL && (store->write == NULL || store->read == NULL || store->slots == 0U ||
                          (store->read_start == NULL) != (store->finish == NULL)))
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
        os_pager.finish = store->read_start != NULL ? os_page_finish : NULL;
        wm_set_pager(&os_pager);
    }
    return OS_STATUS_OK;
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
