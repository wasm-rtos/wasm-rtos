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
 *
 * Several stores (docs/decisions.md, 10) share one space of slots: each
 * store's slots follow those of the stores added before it, so the
 * interpreter sees a frame's slot as one number and only this file knows
 * which store holds it. A dirty frame goes to the store of the highest
 * priority with a free slot; stores of equal priority take turns. Each
 * store writes like a log from its own head. A store leaves gracefully
 * (its frames come back and go elsewhere) or is lost (the tasks with
 * frames or state on it end).
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
/* Stores at once. */
#ifndef OS_PAGE_MAX_STORES
#define OS_PAGE_MAX_STORES 4U
#endif
#define OS_PAGE_NO_SLOT 0xFFFFFFFFUL

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

/* A store: the host's description, and its slots, first among all the stores' slots. */
typedef struct OsPageStoreEntry
{
    /* The host's pointer, by which it names the store. */
    const OsPageStore* key;
    OsPageStore store;
    uint32_t priority;
    uint32_t first;
    /* A bit per slot: in use. */
    uint32_t* used;
    uint32_t used_count;
    /* Where the next frame written goes, or the first free slot after it. */
    uint32_t head;
    uint32_t stores;
    uint32_t loads;
    uint8_t present;
    /* Being removed: no frame goes to it. */
    uint8_t leaving;
} OsPageStoreEntry;

static struct
{
    OsPageStoreEntry entries[OS_PAGE_MAX_STORES];
    /* The first slot a store added next gets. */
    uint32_t next_first;
    /* Among stores of equal priority, the one to try first. */
    uint32_t turn;
    /* The RAM budget and region, from the store set first. */
    uint32_t ram_frames;
    /* That store writes in the background: the pool has room for frames on their way out. */
    uint8_t pool_for_writes;
    uint8_t on;
    uint32_t reserved;
    uint32_t stored;
    uint32_t stores;
    uint32_t loads;
    OsPageRead reads[OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES];
    /* Writes in the background now. */
    uint32_t writes;
    /* A slot whose background read failed: the next read of it is made at once. */
    uint32_t failed_slot;
    /* The frame pool, when the kernel took it from the heap. */
    void* pool;
} g_page;

static const char os_page_store_full[] = "exhaustion: the page store has no free slot";
static const char os_page_store_failed[] = "exhaustion: the page store could not move a frame";
/* Not exhaustion: dropping the code cache and trying again would not bring the frame back. */
static const char os_page_read_failed[] = "page store: a frame could not be read";
static const char os_page_store_gone[] = "page store gone";

/* The store a slot is on, and the slot within it; NULL when no store present has it. */
static OsPageStoreEntry* os_page_entry_of(uint32_t slot, uint32_t* out_local)
{
    uint32_t i = 0U;

    for (i = 0U; i < OS_PAGE_MAX_STORES; ++i)
    {
        OsPageStoreEntry* entry = &g_page.entries[i];
        if (entry->present && slot - entry->first < entry->store.slots)
        {
            *out_local = slot - entry->first;
            return entry;
        }
    }
    return NULL;
}

static OsPageStoreEntry* os_page_entry_named(const OsPageStore* store)
{
    uint32_t i = 0U;

    for (i = 0U; i < OS_PAGE_MAX_STORES && store != NULL; ++i)
    {
        if (g_page.entries[i].present && g_page.entries[i].key == store)
        {
            return &g_page.entries[i];
        }
    }
    return NULL;
}

/* Every frame may come to need a slot of its own: the slots of the stores that take frames. */
static uint32_t os_page_limit(void)
{
    uint32_t i = 0U;
    uint32_t slots = 0U;

    for (i = 0U; i < OS_PAGE_MAX_STORES; ++i)
    {
        if (g_page.entries[i].present && !g_page.entries[i].leaving)
        {
            slots += g_page.entries[i].store.slots;
        }
    }
    return slots;
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
 * passed it (CLOCK, over the frames in RAM only), going at most rounds
 * times round: 0 when none may go. One round finds only a frame that was
 * cold already; two clear every mark, so a third finds one if any may go.
 */
static int os_page_take_one(uint32_t rounds)
{
    WmMemory* memory = NULL;
    uint32_t index = 0U;
    uint32_t step = 0U;
    uint32_t steps = rounds * wm_pager_resident();

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

/*
 * A memory may stay in one block, reached at full speed, while the RAM
 * budget has room for all of its frames. When RAM is short later, a block
 * leaves RAM whole (os_page_block_out_one) and its memory goes on paged.
 */
static int os_page_block(void* context, uint32_t frames)
{
    (void)context;
    return g_page.ram_frames == 0U ||
           (uint64_t)wm_pager_resident() - g_page.writes + frames <= g_page.ram_frames;
}

void os_page_block_in(WmMemory* memory)
{
    uint32_t frames = 0U;
    uint32_t stored = 0U;
    if (memory == NULL || !g_page.on || wm_memory_block_frames(memory) != 0U ||
        (frames = wm_memory_frames(memory)) == 0U)
    {
        return;
    }
    /* Room for all of it besides the frames of it that are in RAM already. */
    if (g_page.ram_frames != 0U &&
        (uint64_t)wm_pager_resident() - wm_memory_resident(memory) / wm_frame_size() + frames > g_page.ram_frames)
    {
        return;
    }
    stored = wm_memory_stored(memory) / wm_frame_size();
    if (stored * 4U > frames)
    {
        return;
    }
    (void)wm_memory_block_in(memory);
}

static int os_page_finish_a_write(void);

/*
 * Room for one frame, the coldest memory first: the block of a task that
 * waits and will not wake soon, whole (a block keeps no marks of its own
 * touches, but the kernel knows when the task wakes); then a frame by
 * CLOCK; then the block of any task between two slices.
 */
static int os_page_make_room(void)
{
    return os_task_block_out_one(1) || os_page_take_one(3U) || os_task_block_out_one(0);
}

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
        return os_page_make_room() && (g_page.writes == 0U || os_page_finish_a_write());
    }
    /* Pinned frames may keep RAM over its budget for a while; frames being written are on their way out. */
    while (g_page.ram_frames != 0U && wm_pager_resident() - g_page.writes >= g_page.ram_frames)
    {
        if (g_page.writes == OS_PAGE_MAX_WRITES)
        {
            (void)os_page_finish_a_write();
        }
        else if (!os_page_make_room())
        {
            break;
        }
    }
    return 1;
}

static int os_page_local_used(const OsPageStoreEntry* entry, uint32_t local)
{
    return (entry->used[local / 32U] & (1UL << (local % 32U))) != 0U;
}

static void os_page_use_slot(uint32_t slot)
{
    uint32_t local = 0U;
    OsPageStoreEntry* entry = os_page_entry_of(slot, &local);

    if (entry != NULL)
    {
        entry->used[local / 32U] |= 1UL << (local % 32U);
        ++entry->used_count;
        ++g_page.stored;
    }
}

/* A slot of a store that is gone is forgotten already. */
static void os_page_free_slot(uint32_t slot)
{
    uint32_t local = 0U;
    OsPageStoreEntry* entry = os_page_entry_of(slot, &local);

    if (entry != NULL && os_page_local_used(entry, local))
    {
        entry->used[local / 32U] &= ~(1UL << (local % 32U));
        --entry->used_count;
        --g_page.stored;
    }
}

/* The store the next frame goes to: the highest priority with a free slot, equal ones in turn. */
static OsPageStoreEntry* os_page_entry_for_write(void)
{
    OsPageStoreEntry* best = NULL;
    uint32_t best_at = 0U;
    uint32_t n = 0U;

    for (n = 0U; n < OS_PAGE_MAX_STORES; ++n)
    {
        uint32_t i = (g_page.turn + n) % OS_PAGE_MAX_STORES;
        OsPageStoreEntry* entry = &g_page.entries[i];
        if (entry->present && !entry->leaving && entry->used_count < entry->store.slots &&
            (best == NULL || entry->priority > best->priority))
        {
            best = entry;
            best_at = i;
        }
    }
    if (best != NULL)
    {
        g_page.turn = (best_at + 1U) % OS_PAGE_MAX_STORES;
    }
    return best;
}

/*
 * A slot for a dirty frame: in the store it should go to, the next free one
 * after the last written there, and the slot it had is free from then on.
 * Undone with os_page_slot_back when the write fails.
 */
static uint32_t os_page_next_slot(uint32_t old)
{
    OsPageStoreEntry* entry = NULL;
    uint32_t index = 0U;

    if (old != OS_PAGE_NO_SLOT)
    {
        os_page_free_slot(old);
    }
    entry = os_page_entry_for_write();
    if (entry == NULL)
    {
        if (old != OS_PAGE_NO_SLOT)
        {
            os_page_use_slot(old);
        }
        return OS_PAGE_NO_SLOT;
    }
    index = entry->head;
    while (os_page_local_used(entry, index))
    {
        index = index + 1U == entry->store.slots ? 0U : index + 1U;
    }
    entry->head = index + 1U == entry->store.slots ? 0U : index + 1U;
    os_page_use_slot(entry->first + index);
    return entry->first + index;
}

static void os_page_slot_back(uint32_t old, uint32_t index)
{
    os_page_free_slot(index);
    if (old != OS_PAGE_NO_SLOT)
    {
        os_page_use_slot(old);
    }
}

/* Write or read a frame at once, in the store its slot is on. */
static OsStatus os_page_store_write(uint32_t slot, const void* frame)
{
    uint32_t local = 0U;
    OsPageStoreEntry* entry = os_page_entry_of(slot, &local);
    OsStatus status = entry != NULL ? entry->store.write(entry->store.context, local, frame) : OS_STATUS_ERROR;

    if (status == OS_STATUS_OK)
    {
        ++entry->stores;
        ++g_page.stores;
    }
    return status;
}

static OsStatus os_page_store_read(uint32_t slot, void* frame)
{
    uint32_t local = 0U;
    OsPageStoreEntry* entry = os_page_entry_of(slot, &local);
    OsStatus status = entry != NULL ? entry->store.read(entry->store.context, local, frame) : OS_STATUS_ERROR;

    if (status == OS_STATUS_OK)
    {
        ++entry->loads;
        ++g_page.loads;
    }
    return status;
}

/* Why a frame could not be read: its store is gone, or it failed. */
static WmResult os_page_read_error(uint32_t slot)
{
    uint32_t local = 0U;
    return os_page_entry_of(slot, &local) == NULL ? os_page_store_gone : os_page_read_failed;
}

static WmResult os_page_write(void* context, uint32_t* slot, const uint8_t* frame)
{
    uint32_t old = *slot;
    uint32_t index = os_page_next_slot(old);
    (void)context;

    if (index == OS_PAGE_NO_SLOT)
    {
        return os_page_store_full;
    }
    if (os_page_store_write(index, frame) != OS_STATUS_OK)
    {
        os_page_slot_back(old, index);
        return os_page_store_failed;
    }
    *slot = index;
    return NULL;
}

static WmResult os_page_read(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    return os_page_store_read(slot, frame) == OS_STATUS_OK ? NULL : os_page_read_error(slot);
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
    uint32_t local = 0U;
    OsPageStoreEntry* entry = os_page_entry_of(slot, &local);
    uint32_t i = 0U;
    (void)context;

    for (i = 0U; i < OS_PAGE_MAX_READS && read == NULL; ++i)
    {
        read = OS_PAGE_FLAG_GET(g_page.reads[i].busy) ? NULL : &g_page.reads[i];
    }
    /* A store that reads at once, no room to note it, or it failed in the background just now: read it at once. */
    if (entry == NULL || entry->store.read_start == NULL || read == NULL || slot == g_page.failed_slot)
    {
        g_page.failed_slot = OS_PAGE_NO_SLOT;
        return os_page_read(NULL, slot, frame);
    }
    read->memory = memory;
    read->index = index;
    read->slot = slot;
    read->task_id = os_task_get_id(os_task_get_current());
    OS_PAGE_FLAG_PUT(read->done, 0U);
    read->writing = 0U;
    OS_PAGE_FLAG_PUT(read->busy, 1U);
    status = entry->store.read_start(entry->store.context, local, frame);
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
        return os_page_read_failed;
    }
    ++entry->loads;
    ++g_page.loads;
    return NULL;
}

/* The store's finish for a read or write in flight, by its slot among all stores. */
static OsStatus os_page_store_finish(uint32_t slot)
{
    uint32_t local = 0U;
    OsPageStoreEntry* entry = os_page_entry_of(slot, &local);
    return entry != NULL && entry->store.finish != NULL ? entry->store.finish(entry->store.context, local)
                                                        : OS_STATUS_ERROR;
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
    status = OS_PAGE_FLAG_GET(read->done) ? read->status : os_page_store_finish(slot);
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
    return status == OS_STATUS_OK ? NULL : os_page_read_error(slot);
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
            os_page_write_ended(write, OS_PAGE_FLAG_GET(write->done) ? write->status : os_page_store_finish(write->slot));
            return 1;
        }
    }
    return 0;
}

static WmResult os_page_write_start(void* context, uint32_t* slot, const uint8_t* frame, WmMemory* memory, uint32_t index)
{
    OsPageRead* write = NULL;
    OsPageStoreEntry* entry = NULL;
    uint32_t local = 0U;
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
    if (chosen == OS_PAGE_NO_SLOT)
    {
        return os_page_store_full;
    }
    entry = os_page_entry_of(chosen, &local);
    /* A store that writes at once, or no room in the pool for frames on their way out: written now. */
    if (entry->store.write_start == NULL || !g_page.pool_for_writes)
    {
        if (os_page_store_write(chosen, frame) != OS_STATUS_OK)
        {
            os_page_slot_back(old, chosen);
            return os_page_store_failed;
        }
        *slot = chosen;
        return NULL;
    }
    write->memory = memory;
    write->index = index;
    write->slot = chosen;
    write->task_id = 0U;
    OS_PAGE_FLAG_PUT(write->done, 0U);
    write->writing = 1U;
    OS_PAGE_FLAG_PUT(write->busy, 1U);
    status = entry->store.write_start(entry->store.context, local, frame);
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

void os_page_store_done(const OsPageStore* store, uint32_t slot, OsStatus status)
{
    uint32_t i = 0U;

    /* From anywhere: the entries are only read here, and a store that ends is never removed mid-flight. */
    for (i = 0U; i < OS_PAGE_MAX_STORES; ++i)
    {
        if (g_page.entries[i].present && g_page.entries[i].key == store)
        {
            OsPageRead* read = os_page_read_of(g_page.entries[i].first + slot);
            if (read != NULL)
            {
                read->status = status;
                OS_PAGE_FLAG_PUT(read->done, 1U);
            }
            return;
        }
    }
}

/* The store set with os_page_store_set is the first entry. */
void os_page_write_done(uint32_t slot, OsStatus status)
{
    os_page_store_done(g_page.entries[0].key, slot, status);
}

void os_page_read_done(uint32_t slot, OsStatus status)
{
    os_page_store_done(g_page.entries[0].key, slot, status);
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
            wm_memory_frame_loaded(read->memory, read->index, status == OS_STATUS_OK ? NULL : os_page_read_error(read->slot));
            os_page_wait_end(read->task_id);
        }
    }
}

static WmPager os_pager = {NULL, os_page_reserve, os_page_room, os_page_write, os_page_read, os_page_drop, NULL, NULL, NULL, os_page_block};

uint32_t os_page_ram_bytes(const OsPageStore* store)
{
    if (store == NULL || store->ram_frames == 0U)
    {
        return 0U;
    }
    return (store->ram_frames + (store->write_start != NULL ? OS_PAGE_MAX_WRITES : 0U)) * wm_frame_size();
}

/* Background reads and writes when some store has them. */
static void os_page_set_background(void)
{
    uint32_t i = 0U;
    uint8_t reads = 0U;
    uint8_t writes = 0U;

    for (i = 0U; i < OS_PAGE_MAX_STORES; ++i)
    {
        const OsPageStoreEntry* entry = &g_page.entries[i];
        reads |= (uint8_t)(entry->present && entry->store.read_start != NULL);
        writes |= (uint8_t)(entry->present && entry->store.write_start != NULL);
    }
    os_pager.read_start = reads ? os_page_read_start : NULL;
    os_pager.write_start = writes ? os_page_write_start : NULL;
    os_pager.finish = reads || writes ? os_page_finish : NULL;
}

static OsStatus os_page_check(const OsPageStore* store)
{
    if (store == NULL || store->write == NULL || store->read == NULL || store->slots == 0U ||
        ((store->read_start != NULL || store->write_start != NULL) && store->finish == NULL))
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    return OS_STATUS_OK;
}

/* A store in a free entry, its slots after all the others'. */
static OsStatus os_page_entry_add(const OsPageStore* store, uint32_t priority)
{
    uint32_t i = 0U;
    uint32_t* used = NULL;

    for (i = 0U; i < OS_PAGE_MAX_STORES && g_page.entries[i].present; ++i)
    {
    }
    if (i == OS_PAGE_MAX_STORES || (uint64_t)g_page.next_first + store->slots > OS_PAGE_NO_SLOT)
    {
        return OS_STATUS_LIMIT_REACHED;
    }
    used = (uint32_t*)calloc((store->slots + 31U) / 32U, sizeof(uint32_t));
    if (used == NULL)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memset(&g_page.entries[i], 0, sizeof(g_page.entries[i]));
    g_page.entries[i].key = store;
    g_page.entries[i].store = *store;
    g_page.entries[i].priority = priority;
    g_page.entries[i].first = g_page.next_first;
    g_page.entries[i].used = used;
    g_page.entries[i].present = 1U;
    g_page.next_first += store->slots;
    os_page_set_background();
    return OS_STATUS_OK;
}

static void os_page_entry_forget(OsPageStoreEntry* entry)
{
    g_page.stored -= entry->used_count;
    free(entry->used);
    memset(entry, 0, sizeof(*entry));
    os_page_set_background();
}

OsStatus os_page_store_set(const OsPageStore* store)
{
    OsStatus status = OS_STATUS_OK;

    if (os_get_task_count() != 0U)
    {
        return OS_STATUS_BUSY;
    }
    if (store != NULL && os_page_check(store) != OS_STATUS_OK)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    os_page_shutdown();
    if (store == NULL)
    {
        return OS_STATUS_OK;
    }
    /* The frames in RAM, one region set aside now: they never cut up the heap (vm decision 6). */
    if (store->ram_frames != 0U && store->ram == NULL)
    {
        g_page.pool = malloc(os_page_ram_bytes(store));
        if (g_page.pool == NULL)
        {
            return OS_STATUS_OUT_OF_MEMORY;
        }
    }
    status = os_page_entry_add(store, 0U);
    if (status != OS_STATUS_OK)
    {
        free(g_page.pool);
        g_page.pool = NULL;
        return status;
    }
    if (store->ram_frames != 0U)
    {
        wm_set_frame_pool(store->ram != NULL ? (uint8_t*)store->ram : (uint8_t*)g_page.pool,
                          os_page_ram_bytes(store) / wm_frame_size());
    }
    g_page.ram_frames = store->ram_frames;
    g_page.pool_for_writes = (uint8_t)(store->ram_frames == 0U || store->write_start != NULL);
    g_page.on = 1U;
    g_page.failed_slot = OS_PAGE_NO_SLOT;
    wm_set_pager(&os_pager);
    return OS_STATUS_OK;
}

OsStatus os_page_store_add(const OsPageStore* store, uint32_t priority)
{
    if (!g_page.on)
    {
        return OS_STATUS_UNSUPPORTED;
    }
    if (os_page_check(store) != OS_STATUS_OK || store->ram_frames != 0U || store->ram != NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (os_page_entry_named(store) != NULL)
    {
        return OS_STATUS_BUSY;
    }
    return os_page_entry_add(store, priority);
}

OsStatus os_page_store_priority(const OsPageStore* store, uint32_t priority)
{
    OsPageStoreEntry* entry = os_page_entry_named(store);

    if (entry == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    entry->priority = priority;
    return OS_STATUS_OK;
}

/* Every read and write in the background ends now. */
static void os_page_finish_all(void)
{
    uint32_t i = 0U;

    while (os_page_finish_a_write())
    {
    }
    for (i = 0U; i < OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES; ++i)
    {
        if (OS_PAGE_FLAG_GET(g_page.reads[i].busy))
        {
            (void)os_page_finish(NULL, g_page.reads[i].slot);
        }
    }
}

/* A swapped-out task's state, its slots on the store that leaves copied to others. */
static OsStatus os_page_image_move(OsPageImage* image, void* context)
{
    const OsPageStoreEntry* leaving = (const OsPageStoreEntry*)context;
    uint8_t* buffer = NULL;
    uint32_t i = 0U;
    OsStatus status = OS_STATUS_OK;

    for (i = 0U; i < image->count && status == OS_STATUS_OK; ++i)
    {
        uint32_t old = image->slots[i];
        uint32_t slot = 0U;
        if (old - leaving->first >= leaving->store.slots)
        {
            continue;
        }
        if (buffer == NULL && (buffer = (uint8_t*)malloc(wm_frame_size())) == NULL)
        {
            return OS_STATUS_OUT_OF_MEMORY;
        }
        status = os_page_store_read(old, buffer);
        if (status != OS_STATUS_OK)
        {
            break;
        }
        slot = os_page_next_slot(old);
        if (slot == OS_PAGE_NO_SLOT)
        {
            status = OS_STATUS_LIMIT_REACHED;
            break;
        }
        status = os_page_store_write(slot, buffer);
        if (status != OS_STATUS_OK)
        {
            os_page_slot_back(old, slot);
            break;
        }
        image->slots[i] = slot;
    }
    free(buffer);
    return status;
}

OsStatus os_page_store_remove(const OsPageStore* store)
{
    OsPageStoreEntry* entry = os_page_entry_named(store);
    WmMemory* memory = NULL;
    OsStatus status = OS_STATUS_OK;

    if (entry == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    /* The other stores must hold every frame promised. */
    entry->leaving = 1U;
    if (g_page.reserved > os_page_limit())
    {
        entry->leaving = 0U;
        return OS_STATUS_BUSY;
    }
    os_page_finish_all();
    /* Each frame on it comes into RAM, dirty and without a slot: it goes to another store when it leaves. */
    for (memory = wm_memory_next(NULL); memory != NULL && status == OS_STATUS_OK; memory = wm_memory_next(memory))
    {
        uint32_t index = 0U;
        uint32_t frames = wm_memory_frames(memory);
        for (index = 0U; index < frames && status == OS_STATUS_OK; ++index)
        {
            if (wm_memory_frame_slot(memory, index) - entry->first < entry->store.slots &&
                wm_memory_frame_unstore(memory, index) != NULL)
            {
                status = OS_STATUS_ERROR;
            }
        }
    }
    if (status == OS_STATUS_OK)
    {
        status = os_swap_images_each(os_page_image_move, entry);
    }
    if (status != OS_STATUS_OK || entry->used_count != 0U)
    {
        /* What moved stays moved; the store stays, and takes frames again. */
        entry->leaving = 0U;
        return status != OS_STATUS_OK ? status : OS_STATUS_ERROR;
    }
    os_page_entry_forget(entry);
    return OS_STATUS_OK;
}

OsStatus os_page_store_lost(const OsPageStore* store)
{
    OsPageStoreEntry* entry = os_page_entry_named(store);
    uint32_t first = 0U;
    uint32_t count = 0U;
    uint32_t i = 0U;

    if (entry == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    first = entry->first;
    count = entry->store.slots;
    /* Reads and writes on their way to it end as failed: nobody waits for a store that is gone. */
    for (i = 0U; i < OS_PAGE_MAX_READS + OS_PAGE_MAX_WRITES; ++i)
    {
        OsPageRead* read = &g_page.reads[i];
        if (OS_PAGE_FLAG_GET(read->busy) && read->slot - first < count)
        {
            read->status = OS_STATUS_ERROR;
            OS_PAGE_FLAG_PUT(read->done, 1U);
        }
    }
    os_page_entry_forget(entry);
    os_page_poll();
    /* Every task with a frame or its state on it ends; the others go on. */
    os_tasks_end_on_slots(first, count, os_page_store_gone);
    return OS_STATUS_OK;
}

OsStatus os_page_get_store_stats(const OsPageStore* store, OsPageStoreStats* out_stats)
{
    const OsPageStoreEntry* entry = os_page_entry_named(store);

    if (entry == NULL || out_stats == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    out_stats->priority = entry->priority;
    out_stats->slots = entry->store.slots;
    out_stats->used_slots = entry->used_count;
    out_stats->stores = entry->stores;
    out_stats->loads = entry->loads;
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
    slot = os_page_next_slot(OS_PAGE_NO_SLOT);
    if (slot == OS_PAGE_NO_SLOT)
    {
        os_page_reserve(NULL, -1);
        return OS_STATUS_OUT_OF_MEMORY;
    }
    memset(stream->buffer + stream->used, 0, OS_PAGE_SLOT_BYTES - stream->used);
    if (os_page_store_write(slot, stream->buffer) != OS_STATUS_OK)
    {
        os_page_free_slot(slot);
        os_page_reserve(NULL, -1);
        return OS_STATUS_ERROR;
    }
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
            stream->status = os_page_store_read(stream->source->slots[stream->next++], stream->buffer);
            stream->used = 0U;
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
    uint32_t i = 0U;

    if (g_page.on)
    {
        wm_set_pager(NULL);
        wm_set_frame_pool(NULL, 0U);
    }
    for (i = 0U; i < OS_PAGE_MAX_STORES; ++i)
    {
        free(g_page.entries[i].used);
    }
    free(g_page.pool);
    memset(&g_page, 0, sizeof(g_page));
}
