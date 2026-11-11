/*
 * Paging: programs' memory in 4 KiB frames with RAM a cache of it, and a
 * page store the platform gives for the rest. The interpreter moves frames
 * through the pager here; the kernel decides which frame leaves RAM (CLOCK:
 * the first one not touched since the hand last passed it) and keeps the
 * promise that every frame a program was given has a slot to go to.
 *
 * A frame keeps its slot once stored, so a frame that came back and was
 * only read leaves RAM again without a write; only dirty frames are written.
 */

#include "os_internal.h"

#include <stdlib.h>
#include <string.h>

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
    /* The CLOCK hand: a frame's place counting through all memories. */
    uint32_t hand;
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

static WmResult os_page_write(void* context, uint32_t* slot, const uint8_t* frame)
{
    uint32_t index = *slot;
    (void)context;

    /* A frame stored before goes over its own slot. */
    if (index != 0xFFFFFFFFUL)
    {
        if (g_page.store.write(g_page.store.context, index, frame) != OS_STATUS_OK)
        {
            return os_page_store_failed;
        }
        ++g_page.stores;
        return NULL;
    }
    index = 0U;
    while (index < g_page.store.slots && (g_page.used[index / 32U] & (1UL << (index % 32U))) != 0U)
    {
        ++index;
    }
    if (index == g_page.store.slots)
    {
        return os_page_store_full;
    }
    if (g_page.store.write(g_page.store.context, index, frame) != OS_STATUS_OK)
    {
        return os_page_store_failed;
    }
    g_page.used[index / 32U] |= 1UL << (index % 32U);
    ++g_page.stored;
    ++g_page.stores;
    *slot = index;
    return NULL;
}

static void os_page_free_slot(uint32_t slot)
{
    g_page.used[slot / 32U] &= ~(1UL << (slot % 32U));
    --g_page.stored;
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

static const WmPager os_pager = {NULL, os_page_reserve, os_page_room, os_page_write, os_page_read, os_page_drop};

OsStatus os_page_store_set(const OsPageStore* store)
{
    uint32_t* used = NULL;

    if (os_get_task_count() != 0U)
    {
        return OS_STATUS_BUSY;
    }
    if (store != NULL && (store->write == NULL || store->read == NULL || store->slots == 0U))
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
