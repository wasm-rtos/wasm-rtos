/*
 * Code stores (docs/decisions.md, 11): programs' decoded code kept as
 * code images (wasm-rtos-vm decision 15) in storage the processor reads,
 * so it needs no RAM. A store holds images one after the other, each after
 * a header, aligned to its erase size. A module's image is looked for when
 * it is parsed, by its bytes' size and CRC-32, and mapped before its first
 * instance: a program's, shared by its processes, or a task's own. One
 * that has none gets it written when no task is ready.
 */
#include "os_kernel.h"

#include <string.h>

#if OS_HAS_CODE_STORES

/* "WRCI" */
#define OS_CODE_MAGIC 0x49435257U

typedef struct OsCodeHeader
{
    uint32_t magic;
    uint32_t program_size;
    uint32_t program_crc;
    uint32_t image_size;
} OsCodeHeader;

typedef struct OsCodeStoreSlot
{
    const OsCodeStore* store;
    uint32_t priority;
    /* Where the images end: from here on it is free. */
    uint32_t end;
    uint32_t images;
    uint32_t mapped;
} OsCodeStoreSlot;

static OsCodeStoreSlot g_code_stores[OS_CODE_MAX_STORES];
static uint32_t g_code_store_count;

static uint32_t os_code_align(const OsCodeStore* store, uint32_t offset)
{
    uint32_t unit = store->erase_size != 0U ? store->erase_size : 4U;
    return (offset + unit - 1U) / unit * unit;
}

static OsCodeStoreSlot* os_code_slot(const OsCodeStore* store)
{
    for (uint32_t i = 0U; i < g_code_store_count; ++i)
    {
        if (g_code_stores[i].store == store)
        {
            return &g_code_stores[i];
        }
    }
    return NULL;
}

/* The header at offset, or NULL where the images end. */
static const OsCodeHeader* os_code_header(const OsCodeStore* store, uint32_t offset)
{
    const OsCodeHeader* header;
    if (offset > store->size || store->size - offset < sizeof(OsCodeHeader) || (offset & 3U) != 0U)
    {
        return NULL;
    }
    header = (const OsCodeHeader*)(const void*)(store->base + offset);
    if (header->magic != OS_CODE_MAGIC || header->image_size > store->size - offset - sizeof(OsCodeHeader))
    {
        return NULL;
    }
    return header;
}

static void os_code_scan(OsCodeStoreSlot* slot)
{
    const OsCodeStore* store = slot->store;
    uint32_t offset = 0U;
    const OsCodeHeader* header;
    slot->images = 0U;
    while ((header = os_code_header(store, offset)) != NULL)
    {
        ++slot->images;
        offset = os_code_align(store, offset + (uint32_t)sizeof(OsCodeHeader) + header->image_size);
    }
    slot->end = offset;
}

OsStatus os_code_store_add(const OsCodeStore* store, uint32_t priority)
{
    OsCodeStoreSlot* slot;
    if (store == NULL || store->base == NULL || store->write == NULL || store->size < sizeof(OsCodeHeader) ||
        ((uintptr_t)store->base & 3U) != 0U || (store->erase_size != 0U && store->erase == NULL) ||
        (store->erase_size & 3U) != 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (os_code_slot(store) != NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    if (g_code_store_count == OS_CODE_MAX_STORES)
    {
        return OS_STATUS_OUT_OF_MEMORY;
    }
    slot = &g_code_stores[g_code_store_count];
    memset(slot, 0, sizeof(*slot));
    slot->store = store;
    slot->priority = priority;
    os_code_scan(slot);
    /* Nothing is mapped yet: a store mostly full of images, some perhaps an older firmware's, starts afresh. */
    if (store->size - slot->end < store->size / 4U)
    {
        uint32_t whole = store->erase_size != 0U ? store->size / store->erase_size * store->erase_size : 0U;
        if (store->erase != NULL && store->erase(store->context, 0U, whole) != OS_STATUS_OK)
        {
            return OS_STATUS_ERROR;
        }
        if (store->erase == NULL)
        {
            /* RAM: a header no more is enough. */
            static const uint32_t zero = 0U;
            if (store->write(store->context, 0U, &zero, sizeof(zero)) != OS_STATUS_OK)
            {
                return OS_STATUS_ERROR;
            }
        }
        slot->end = 0U;
        slot->images = 0U;
    }
    ++g_code_store_count;
    return OS_STATUS_OK;
}

OsStatus os_code_store_priority(const OsCodeStore* store, uint32_t priority)
{
    OsCodeStoreSlot* slot = os_code_slot(store);
    if (slot == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    slot->priority = priority;
    return OS_STATUS_OK;
}

OsStatus os_code_store_get_stats(const OsCodeStore* store, OsCodeStoreStats* out_stats)
{
    const OsCodeStoreSlot* slot = os_code_slot(store);
    if (slot == NULL || out_stats == NULL)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    out_stats->priority = slot->priority;
    out_stats->size = store->size;
    out_stats->used = slot->end;
    out_stats->images = slot->images;
    out_stats->mapped = slot->mapped;
    return OS_STATUS_OK;
}

/* CRC-32 (the polynomial of zlib), a bit at a time: once a module's bytes, and only where it is looked for. */
static uint32_t os_code_crc(const uint8_t* bytes, uint32_t size, OsCodeImage* image)
{
    if (!image->crc_known)
    {
        uint32_t crc = 0xFFFFFFFFU;
        for (uint32_t i = 0U; i < size; ++i)
        {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
            }
        }
        image->crc = ~crc;
        image->crc_known = 1U;
    }
    return image->crc;
}

/* Stores by priority, the highest first. */
static uint32_t os_code_by_priority(OsCodeStoreSlot** out)
{
    for (uint32_t i = 0U; i < g_code_store_count; ++i)
    {
        uint32_t at = i;
        while (at > 0U && out[at - 1U]->priority < g_code_stores[i].priority)
        {
            out[at] = out[at - 1U];
            --at;
        }
        out[at] = &g_code_stores[i];
    }
    return g_code_store_count;
}

/* Map the module's image from the stores, by priority: 1 if one maps. */
static int os_code_find(WmModule* module, const uint8_t* bytes, uint32_t size, OsCodeImage* image)
{
    OsCodeStoreSlot* order[OS_CODE_MAX_STORES];
    uint32_t count = os_code_by_priority(order);
    uint32_t crc = os_code_crc(bytes, size, image);
    for (uint32_t i = 0U; i < count; ++i)
    {
        const OsCodeStore* store = order[i]->store;
        uint32_t offset = 0U;
        const OsCodeHeader* header;
        while ((header = os_code_header(store, offset)) != NULL)
        {
            /* The interpreter refuses an image of another build or module: the search goes on. */
            if (header->program_size == size && header->program_crc == crc &&
                wm_code_image_map(module, store->base + offset + sizeof(OsCodeHeader), header->image_size) == NULL)
            {
                image->state = OS_CODE_IMAGE_MAPPED;
                ++order[i]->mapped;
                return 1;
            }
            offset = os_code_align(store, offset + (uint32_t)sizeof(OsCodeHeader) + header->image_size);
        }
    }
    return 0;
}

void os_code_module_loaded(WmModule* module, const uint8_t* bytes, uint32_t size, OsCodeImage* image)
{
    if (module == NULL || g_code_store_count == 0U || image->state == OS_CODE_IMAGE_NOT)
    {
        return;
    }
    if (!os_code_find(module, bytes, size, image))
    {
        image->state = OS_CODE_IMAGE_WANTED;
    }
}

/* An image streamed into a store at a running offset, or only counted (store NULL). */
typedef struct OsCodeWriter
{
    const OsCodeStore* store;
    uint32_t offset;
    uint32_t size;
} OsCodeWriter;

static WmResult os_code_write(void* context, const void* data, uint32_t size)
{
    OsCodeWriter* writer = (OsCodeWriter*)context;
    if (writer->store != NULL &&
        writer->store->write(writer->store->context, writer->offset + writer->size, data, size) != OS_STATUS_OK)
    {
        return "exhaustion: the code store could not be written";
    }
    writer->size += size;
    return NULL;
}

/* The module's image written to the store with the highest priority that has room, and mapped. */
static void os_code_write_image(WmModule* module, const uint8_t* bytes, uint32_t size, OsCodeImage* image)
{
    OsCodeStoreSlot* order[OS_CODE_MAX_STORES];
    uint32_t count = os_code_by_priority(order);
    OsCodeWriter counter = {NULL, 0U, 0U};
    /* A module of the same bytes may have written it since. */
    if (os_code_find(module, bytes, size, image))
    {
        return;
    }
    if (wm_code_image_write(module, os_code_write, &counter) != NULL)
    {
        image->state = OS_CODE_IMAGE_NOT;
        return;
    }
    for (uint32_t i = 0U; i < count; ++i)
    {
        OsCodeStoreSlot* slot = order[i];
        const OsCodeStore* store = slot->store;
        uint32_t need = os_code_align(store, (uint32_t)sizeof(OsCodeHeader) + counter.size);
        OsCodeWriter writer = {store, slot->end + (uint32_t)sizeof(OsCodeHeader), 0U};
        OsCodeHeader header = {OS_CODE_MAGIC, size, os_code_crc(bytes, size, image), counter.size};
        if (store->size - slot->end < need)
        {
            continue;
        }
        if (store->erase != NULL && store->erase(store->context, slot->end, need) != OS_STATUS_OK)
        {
            continue;
        }
        /* The image first, then the header that makes it count. */
        if (wm_code_image_write(module, os_code_write, &writer) != NULL || writer.size != counter.size ||
            store->write(store->context, slot->end, &header, sizeof(header)) != OS_STATUS_OK)
        {
            continue;
        }
        image->state = wm_code_image_map(module, store->base + slot->end + sizeof(OsCodeHeader), counter.size) == NULL
                           ? OS_CODE_IMAGE_MAPPED
                           : OS_CODE_IMAGE_NOT;
        slot->end += need;
        ++slot->images;
        slot->mapped += image->state == OS_CODE_IMAGE_MAPPED;
        return;
    }
    image->state = OS_CODE_IMAGE_NOT;
}

/* Only a module that runs predecoded has records to keep, and only once its first instance is made. */
static int os_code_wants(const WmModule* module, const OsCodeImage* image)
{
    return module != NULL && image->state == OS_CODE_IMAGE_WANTED && wm_module_predecoded(module) == NULL;
}

OsStatus os_code_store_write_pending(void)
{
    OsProgram* program;
    OsTaskHandle task;
    for (program = os_programs(); program != NULL; program = program->next)
    {
        if (os_code_wants(program->module, &program->code))
        {
            os_code_write_image(program->module, program->bytes, program->size, &program->code);
            return OS_STATUS_OK;
        }
    }
    /* Tasks the host made from its own bytes parse their own modules (decision 9). */
    for (task = g_os.task_list; task != NULL; task = task->next)
    {
        if (!task->module_shared && os_code_wants(task->wasm_module, &task->code))
        {
            os_code_write_image(task->wasm_module, task->wasm_bytes, task->wasm_size, &task->code);
            return OS_STATUS_OK;
        }
    }
    return OS_STATUS_NO_READY_TASKS;
}

void os_code_shutdown(void)
{
    memset(g_code_stores, 0, sizeof(g_code_stores));
    g_code_store_count = 0U;
}

#endif
