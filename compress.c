/*
 * A page store that keeps frames compressed in RAM (docs/decisions.md, 12),
 * as zram and macOS's compressed memory do. Frames are compressed with
 * WKdm (Wilson and Kaplan's algorithm for memory words, the one macOS
 * uses): each 32-bit word is zero, the same as one of 16 recent words, the
 * same but for its low 10 bits, or new. A frame of one word repeated is
 * that word. A frame that compresses to more than three quarters of itself
 * does not fit, nor one when the store is full: the kernel gives it to the
 * next store by priority (an SD card, say).
 *
 * The region the host gives holds this state, a table of slots, a link per
 * chunk and the chunks: a compressed frame is a chain of chunks of
 * OS_COMPRESS_CHUNK bytes, so the region never needs a free run of any
 * size, and a scratch buffer of a frame and an eighth.
 */
#include "os_kernel.h"

#include <string.h>

#if OS_HAS_COMPRESSED_STORE

#define OS_COMPRESS_CHUNK 128U
#define OS_COMPRESS_NONE 0xFFFFU
/* A slot's size: what a frame of one word repeated is marked with. */
#define OS_COMPRESS_SAME 0xFFFFU

typedef struct OsCompressSlot
{
    uint16_t first;
    /* Compressed bytes; 0: empty; OS_COMPRESS_SAME: the word is in the first chunk. */
    uint16_t size;
} OsCompressSlot;

typedef struct OsCompressState
{
    uint32_t frame;
    uint32_t chunks;
    uint32_t free_head;
    uint32_t free_count;
    OsCompressSlot* slots;
    uint16_t* next;
    uint8_t* data;
    uint8_t* scratch;
} OsCompressState;

/* ---- WKdm, bit by bit. */

typedef struct OsBits
{
    uint8_t* p;
    uint8_t* end;
    uint64_t acc;
    uint32_t n;
} OsBits;

static int os_bits_put(OsBits* b, uint32_t value, uint32_t count)
{
    b->acc |= (uint64_t)value << b->n;
    b->n += count;
    while (b->n >= 8U)
    {
        if (b->p == b->end)
        {
            return 0;
        }
        *b->p++ = (uint8_t)b->acc;
        b->acc >>= 8;
        b->n -= 8U;
    }
    return 1;
}

static uint32_t os_bits_get(OsBits* b, uint32_t count)
{
    uint32_t value;
    while (b->n < count)
    {
        b->acc |= (uint64_t)*b->p++ << b->n;
        b->n += 8U;
    }
    value = (uint32_t)(b->acc & ((1ULL << count) - 1U));
    b->acc >>= count;
    b->n -= count;
    return value;
}

static uint32_t os_wk_hash(uint32_t word)
{
    return ((word >> 10) * 2654435761U) >> 28;
}

/* Bytes written to out (at most limit), or 0 when it does not fit. */
static uint32_t os_wk_compress(const uint8_t* frame, uint32_t words, uint8_t* out, uint32_t limit)
{
    uint32_t dict[16] = {0};
    OsBits bits = {out, out + limit, 0U, 0U};
    for (uint32_t i = 0U; i < words; ++i)
    {
        uint32_t word;
        uint32_t h;
        int ok;
        memcpy(&word, frame + i * 4U, 4U);
        if (word == 0U)
        {
            ok = os_bits_put(&bits, 0U, 2U);
        }
        else
        {
            h = os_wk_hash(word);
            if (dict[h] == word)
            {
                ok = os_bits_put(&bits, 1U | (h << 2), 6U);
            }
            else if ((dict[h] >> 10) == (word >> 10))
            {
                ok = os_bits_put(&bits, 2U | (h << 2) | ((word & 1023U) << 6), 16U);
                dict[h] = word;
            }
            else
            {
                ok = os_bits_put(&bits, 3U, 2U) && os_bits_put(&bits, word, 32U);
                dict[h] = word;
            }
        }
        if (!ok)
        {
            return 0U;
        }
    }
    if (bits.n != 0U && !os_bits_put(&bits, 0U, 8U - bits.n))
    {
        return 0U;
    }
    return (uint32_t)(bits.p - out);
}

static void os_wk_decompress(const uint8_t* in, uint32_t words, uint8_t* frame)
{
    uint32_t dict[16] = {0};
    OsBits bits = {(uint8_t*)(uintptr_t)in, NULL, 0U, 0U};
    for (uint32_t i = 0U; i < words; ++i)
    {
        uint32_t tag = os_bits_get(&bits, 2U);
        uint32_t word = 0U;
        if (tag == 3U)
        {
            word = os_bits_get(&bits, 32U);
            dict[os_wk_hash(word)] = word;
        }
        else if (tag != 0U)
        {
            uint32_t h = os_bits_get(&bits, 4U);
            word = dict[h];
            if (tag == 2U)
            {
                word = (word & ~1023U) | os_bits_get(&bits, 10U);
                dict[h] = word;
            }
        }
        memcpy(frame + i * 4U, &word, 4U);
    }
}

/* ---- The chunks. */

static void os_compress_free_chain(OsCompressState* state, uint32_t first)
{
    while (first != OS_COMPRESS_NONE)
    {
        uint32_t next = state->next[first];
        state->next[first] = (uint16_t)state->free_head;
        state->free_head = first;
        ++state->free_count;
        first = next;
    }
}

static void os_compress_drop(void* context, uint32_t slot)
{
    OsCompressState* state = (OsCompressState*)context;
    OsCompressSlot* entry = &state->slots[slot];
    if (entry->size != 0U)
    {
        os_compress_free_chain(state, entry->first);
        entry->size = 0U;
        entry->first = OS_COMPRESS_NONE;
    }
}

/* size bytes of data into a new chain of chunks: its first, or OS_COMPRESS_NONE when there are too few. */
static uint32_t os_compress_store_chain(OsCompressState* state, const uint8_t* data, uint32_t size)
{
    uint32_t count = (size + OS_COMPRESS_CHUNK - 1U) / OS_COMPRESS_CHUNK;
    uint32_t first = OS_COMPRESS_NONE;
    uint32_t last = OS_COMPRESS_NONE;
    if (count > state->free_count)
    {
        return OS_COMPRESS_NONE;
    }
    for (uint32_t i = 0U; i < count; ++i)
    {
        uint32_t chunk = state->free_head;
        uint32_t part = size - i * OS_COMPRESS_CHUNK < OS_COMPRESS_CHUNK ? size - i * OS_COMPRESS_CHUNK : OS_COMPRESS_CHUNK;
        state->free_head = state->next[chunk];
        --state->free_count;
        memcpy(state->data + (size_t)chunk * OS_COMPRESS_CHUNK, data + i * OS_COMPRESS_CHUNK, part);
        state->next[chunk] = OS_COMPRESS_NONE;
        if (last == OS_COMPRESS_NONE)
        {
            first = chunk;
        }
        else
        {
            state->next[last] = (uint16_t)chunk;
        }
        last = chunk;
    }
    return first;
}

static OsStatus os_compress_write(void* context, uint32_t slot, const void* frame)
{
    OsCompressState* state = (OsCompressState*)context;
    const uint8_t* bytes = (const uint8_t*)frame;
    uint32_t words = state->frame / 4U;
    uint32_t first_word;
    uint32_t size;
    uint32_t chain;
    int same = 1;
    if (slot >= state->chunks)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    os_compress_drop(state, slot);
    memcpy(&first_word, bytes, 4U);
    for (uint32_t i = 1U; i < words && same; ++i)
    {
        same = memcmp(bytes + i * 4U, &first_word, 4U) == 0;
    }
    if (same)
    {
        chain = os_compress_store_chain(state, (const uint8_t*)&first_word, 4U);
        size = OS_COMPRESS_SAME;
    }
    else
    {
        /* Worth keeping only at three quarters or less. */
        size = os_wk_compress(bytes, words, state->scratch, state->frame / 4U * 3U);
        if (size == 0U)
        {
            return OS_STATUS_BUFFER_TOO_SMALL;
        }
        chain = os_compress_store_chain(state, state->scratch, size);
    }
    if (chain == OS_COMPRESS_NONE)
    {
        return OS_STATUS_BUFFER_TOO_SMALL;
    }
    state->slots[slot].first = (uint16_t)chain;
    state->slots[slot].size = (uint16_t)size;
    return OS_STATUS_OK;
}

static OsStatus os_compress_read(void* context, uint32_t slot, void* frame)
{
    OsCompressState* state = (OsCompressState*)context;
    const OsCompressSlot* entry;
    uint32_t chunk;
    uint32_t at = 0U;
    if (slot >= state->chunks || state->slots[slot].size == 0U)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    entry = &state->slots[slot];
    if (entry->size == OS_COMPRESS_SAME)
    {
        uint32_t word;
        memcpy(&word, state->data + (size_t)entry->first * OS_COMPRESS_CHUNK, 4U);
        for (uint32_t i = 0U; i < state->frame / 4U; ++i)
        {
            memcpy((uint8_t*)frame + i * 4U, &word, 4U);
        }
        return OS_STATUS_OK;
    }
    /* The chain in a row first, as the decoder reads one stream. */
    for (chunk = entry->first; chunk != OS_COMPRESS_NONE && at < entry->size; chunk = state->next[chunk])
    {
        uint32_t part = entry->size - at < OS_COMPRESS_CHUNK ? entry->size - at : OS_COMPRESS_CHUNK;
        memcpy(state->scratch + at, state->data + (size_t)chunk * OS_COMPRESS_CHUNK, part);
        at += part;
    }
    /* The decoder may read a few bytes past the last it needs. */
    memset(state->scratch + at, 0, 8U);
    os_wk_decompress(state->scratch, state->frame / 4U, (uint8_t*)frame);
    return OS_STATUS_OK;
}

OsStatus os_page_store_compressed(OsPageStore* out_store, void* region, uint32_t bytes)
{
    OsCompressState* state = (OsCompressState*)region;
    uint32_t frame = wm_frame_size();
    uint32_t fixed = (uint32_t)sizeof(OsCompressState) + frame / 4U * 3U + 8U;
    uint32_t per_chunk = OS_COMPRESS_CHUNK + (uint32_t)sizeof(OsCompressSlot) + (uint32_t)sizeof(uint16_t);
    uint32_t chunks;
    uint8_t* at;
    if (out_store == NULL || region == NULL || ((uintptr_t)region & 3U) != 0U || frame == 0U || bytes < fixed + per_chunk)
    {
        return OS_STATUS_INVALID_ARGUMENT;
    }
    chunks = (bytes - fixed) / per_chunk;
    if (chunks > OS_COMPRESS_NONE - 1U)
    {
        chunks = OS_COMPRESS_NONE - 1U;
    }
    memset(state, 0, sizeof(*state));
    state->frame = frame;
    state->chunks = chunks;
    at = (uint8_t*)region + sizeof(OsCompressState);
    state->slots = (OsCompressSlot*)(void*)at;
    at += (size_t)chunks * sizeof(OsCompressSlot);
    state->data = at;
    at += (size_t)chunks * OS_COMPRESS_CHUNK;
    state->next = (uint16_t*)(void*)at;
    at += (size_t)chunks * sizeof(uint16_t);
    state->scratch = at;
    for (uint32_t i = 0U; i < chunks; ++i)
    {
        state->slots[i].size = 0U;
        state->slots[i].first = OS_COMPRESS_NONE;
        state->next[i] = (uint16_t)(i + 1U < chunks ? i + 1U : OS_COMPRESS_NONE);
    }
    state->free_head = 0U;
    state->free_count = chunks;
    memset(out_store, 0, sizeof(*out_store));
    out_store->context = state;
    /* As many slots as chunks: a frame of one word repeated takes one. */
    out_store->slots = chunks;
    out_store->write = os_compress_write;
    out_store->read = os_compress_read;
    out_store->drop = os_compress_drop;
    return OS_STATUS_OK;
}

#endif
