/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    m68kemu.library — full containment memory model
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/bptr.h>
#include <string.h>
#include <stdio.h>

#include "m68kemu_intern.h"
#include "m68kemu_offsets.h"
#include "m68kemu_thunks.h"

struct M68KEmuContext *M68KEmu_CreateContext(struct M68KEmuLibBase *base)
{
    UBYTE *mem;
    struct M68KEmuContext *ctx;
    ULONG heap_size;

    mem = (UBYTE *)AllocMem(M68KEMU_MEM_SIZE, MEMF_CLEAR | MEMF_31BIT);
    if (!mem)
        return NULL;

    ctx = (struct M68KEmuContext *)AllocMem(sizeof(struct M68KEmuContext), MEMF_CLEAR);
    if (!ctx)
    {
        FreeMem(mem, M68KEMU_MEM_SIZE);
        return NULL;
    }

    ctx->mem      = mem;
    ctx->mem_size = M68KEMU_MEM_SIZE;
    ctx->emuBase  = base;

    ctx->stack_top    = M68KEMU_MEM_SIZE;
    ctx->stack_bottom = M68KEMU_MEM_SIZE - M68KEMU_STACK_SIZE;

    ctx->heap_start = M68KEMU_HEAP_BASE;
    ctx->heap_end   = ctx->stack_bottom;
    heap_size       = ctx->heap_end - ctx->heap_start;

    /* One big free block */
    ctx->heap_free = ctx->heap_start;
    m68k_write32(ctx, ctx->heap_start,     heap_size);
    m68k_write32(ctx, ctx->heap_start + 4, 0);

    return ctx;
}

void M68KEmu_DestroyContext(struct M68KEmuContext *ctx)
{
    if (!ctx) return;
#ifdef M68KEMU_WITH_RIGEL
    if (ctx->presenter.destroy)
        ctx->presenter.destroy(ctx);
    M68KEmu_RigelDestroy(ctx);
#endif
    if (ctx->mem) FreeMem(ctx->mem, ctx->mem_size);
    FreeMem(ctx, sizeof(struct M68KEmuContext));
}

/* First-fit allocation from one address-ordered free list. */
static ULONG heap_alloc_list(struct M68KEmuContext *ctx, ULONG *head, ULONG size)
{
    ULONG needed, prev, cur;

    size   = (size + 3) & ~3;
    needed = size + M68KEMU_HEAP_HDR;  /* header */

    prev = 0;
    cur  = *head;

    while (cur)
    {
        ULONG blk_size = m68k_read32(ctx, cur);
        ULONG blk_next = m68k_read32(ctx, cur + 4);

        if (blk_size >= needed)
        {
            ULONG remainder = blk_size - needed;

            if (remainder >= M68KEMU_HEAP_MIN_SPLIT)
            {
                /* Split: keep remainder as free, allocate from end */
                m68k_write32(ctx, cur, remainder);
                m68k_write32(ctx, cur + remainder, needed);
                return cur + remainder + M68KEMU_HEAP_HDR;
            }
            else
            {
                /* Use whole block */
                needed = blk_size;
                if (prev)
                    m68k_write32(ctx, prev + 4, blk_next);
                else
                    *head = blk_next;
                m68k_write32(ctx, cur, needed);
                return cur + M68KEMU_HEAP_HDR;
            }
        }
        prev = cur;
        cur  = blk_next;
    }
    return 0;
}

static void heap_free_list(struct M68KEmuContext *ctx, ULONG *head, ULONG blk)
{
    ULONG blk_size, prev, cur;

    blk_size = m68k_read32(ctx, blk);

    /* Insert in address order */
    prev = 0;
    cur  = *head;
    while (cur && cur < blk)
    {
        prev = cur;
        cur  = m68k_read32(ctx, cur + 4);
    }

    m68k_write32(ctx, blk,     blk_size);
    m68k_write32(ctx, blk + 4, cur);

    if (prev)
        m68k_write32(ctx, prev + 4, blk);
    else
        *head = blk;

    /* Coalesce forward */
    if (cur && (blk + blk_size) == cur)
    {
        ULONG cur_size = m68k_read32(ctx, cur);
        m68k_write32(ctx, blk,     blk_size + cur_size);
        m68k_write32(ctx, blk + 4, m68k_read32(ctx, cur + 4));
        blk_size += cur_size;
    }

    /* Coalesce backward */
    if (prev)
    {
        ULONG prev_size = m68k_read32(ctx, prev);
        if ((prev + prev_size) == blk)
        {
            m68k_write32(ctx, prev,     prev_size + blk_size);
            m68k_write32(ctx, prev + 4, m68k_read32(ctx, blk + 4));
        }
    }
}

/*
 * With a split heap, MEMF_CHIP allocates only below chip_end, MEMF_FAST
 * only above it, and anything else prefers Fast and falls back to Chip
 * like AmigaOS. Without a split, the requirements are ignored.
 */
ULONG M68KEmu_HeapAlloc(struct M68KEmuContext *ctx, ULONG size, ULONG requirements)
{
    ULONG addr;

    if (!ctx->chip_end)
        addr = heap_alloc_list(ctx, &ctx->heap_free, size);
    else if (requirements & MEMF_CHIP)
        addr = heap_alloc_list(ctx, &ctx->chip_free, size);
    else
    {
        addr = heap_alloc_list(ctx, &ctx->heap_free, size);
        if (!addr && !(requirements & MEMF_FAST))
            addr = heap_alloc_list(ctx, &ctx->chip_free, size);
    }

    if (addr && (requirements & MEMF_CLEAR))
        memset(ctx->mem + addr, 0, (size + 3) & ~3);

    return addr;
}

void M68KEmu_HeapFree(struct M68KEmuContext *ctx, ULONG addr, ULONG size)
{
    ULONG blk;

    (void)size;
    if (!addr) return;

    blk = addr - M68KEMU_HEAP_HDR;
    if (ctx->chip_end && blk < ctx->chip_end)
        heap_free_list(ctx, &ctx->chip_free, blk);
    else
        heap_free_list(ctx, &ctx->heap_free, blk);
}

/* A split heap leaves the I/O space out of the Fast heap */
static BOOL heap_has_io_hole(struct M68KEmuContext *ctx)
{
    return ctx->chip_end && ctx->chip_end < M68KEMU_IO_START &&
           ctx->heap_end > M68KEMU_IO_END + M68KEMU_HEAP_MIN_SPLIT;
}

/*
 * Split a fresh heap at chip_end into a Chip and a Fast free list.
 * Must run before anything is allocated from the heap.
 */
BOOL M68KEmu_HeapSplitChip(struct M68KEmuContext *ctx, ULONG chip_end)
{
    ULONG heap_size = ctx->heap_end - ctx->heap_start;

    if (ctx->chip_end)
        return ctx->chip_end == chip_end;
    if (ctx->heap_free != ctx->heap_start ||
        m68k_read32(ctx, ctx->heap_start) != heap_size ||
        m68k_read32(ctx, ctx->heap_start + 4) != 0)
        return FALSE;
    if (chip_end & 3 ||
        chip_end < ctx->heap_start + M68KEMU_HEAP_MIN_SPLIT ||
        chip_end + M68KEMU_HEAP_MIN_SPLIT > ctx->heap_end)
        return FALSE;

    ctx->chip_end  = chip_end;
    ctx->chip_free = ctx->heap_start;
    m68k_write32(ctx, ctx->heap_start,     chip_end - ctx->heap_start);
    m68k_write32(ctx, ctx->heap_start + 4, 0);

    /* Fast heap: below and above the I/O space, which the chipset owns */
    ctx->heap_free = chip_end;
    if (heap_has_io_hole(ctx))
    {
        m68k_write32(ctx, chip_end,     M68KEMU_IO_START - chip_end);
        m68k_write32(ctx, chip_end + 4, M68KEMU_IO_END);
        m68k_write32(ctx, M68KEMU_IO_END,     ctx->heap_end - M68KEMU_IO_END);
        m68k_write32(ctx, M68KEMU_IO_END + 4, 0);
    }
    else
    {
        m68k_write32(ctx, chip_end,     ctx->heap_end - chip_end);
        m68k_write32(ctx, chip_end + 4, 0);
    }
    return TRUE;
}

static ULONG heap_avail_list(struct M68KEmuContext *ctx, ULONG head, BOOL largest)
{
    ULONG total = 0, cur;

    for (cur = head; cur; cur = m68k_read32(ctx, cur + 4))
    {
        ULONG usable = m68k_read32(ctx, cur);
        usable = usable > M68KEMU_HEAP_HDR ? usable - M68KEMU_HEAP_HDR : 0;
        if (largest)
            total = usable > total ? usable : total;
        else
            total += usable;
    }
    return total;
}

/* AvailMem() semantics over the contained heap(s). */
ULONG M68KEmu_HeapAvail(struct M68KEmuContext *ctx, ULONG requirements)
{
    BOOL  largest = (requirements & MEMF_LARGEST) != 0;
    ULONG chip = 0, fast = 0;

    if (requirements & MEMF_TOTAL)
    {
        if (!ctx->chip_end)
            return ctx->heap_end - ctx->heap_start;
        chip = ctx->chip_end - ctx->heap_start;
        fast = ctx->heap_end - ctx->chip_end;
        if (heap_has_io_hole(ctx))
            fast -= M68KEMU_IO_END - M68KEMU_IO_START;
    }
    else
    {
        fast = heap_avail_list(ctx, ctx->heap_free, largest);
        if (ctx->chip_end)
            chip = heap_avail_list(ctx, ctx->chip_free, largest);
    }

    if (!ctx->chip_end)
        return fast;
    if (requirements & MEMF_CHIP)
        return chip;
    if (requirements & MEMF_FAST)
        return fast;
    if (largest)
        return chip > fast ? chip : fast;
    return chip + fast;
}

/* TypeOfMem() for a contained address. */
ULONG M68KEmu_HeapTypeOf(struct M68KEmuContext *ctx, ULONG addr)
{
    if (!ctx->chip_end)
        return MEMF_PUBLIC;
    return MEMF_PUBLIC | (addr < ctx->chip_end ? MEMF_CHIP : MEMF_FAST);
}

ULONG M68KEmu_LoadHunks(struct M68KEmuContext *ctx, BPTR segList)
{
    BPTR seg = segList;
    ULONG entry_point = 0;

    while (seg)
    {
        ULONG *baddr    = (ULONG *)BADDR(seg);
        ULONG alloc_size = *(baddr - 1);
        ULONG data_size  = alloc_size - sizeof(BPTR);
        UBYTE *src       = (UBYTE *)baddr + sizeof(BPTR);
        ULONG m68k_addr;

        m68k_addr = M68KEmu_HeapAlloc(ctx, data_size, 0);
        if (!m68k_addr)
            return 0;

        CopyMem(src, m68k_to_host(ctx, m68k_addr), data_size);

        if (!entry_point)
        {
            entry_point      = m68k_addr;
            ctx->entry_point = m68k_addr;
            ctx->seg_start   = m68k_addr;
        }

        seg = *((BPTR *)BADDR(seg));
    }

    return entry_point;
}

ULONG M68KEmu_SetupLibBase(struct M68KEmuContext *ctx, UWORD lib_id,
                            const char *name, UWORD num_vectors,
                            const struct M68KThunkEntry *thunks, ULONG num_thunks,
                            const struct M68KThunkEntry *gen_thunks, ULONG num_gen_thunks)
{
    ULONG vectors_size, total_size, region_offset, base_addr;
    UWORD i;

    if (ctx->num_libs >= M68KEMU_MAX_LIBS)
        return 0;

    vectors_size = (ULONG)num_vectors * M68K_JT_SLOT_SIZE;
    total_size   = vectors_size + 4;

    /* Find next free spot in LIBBASE_REGION */
    region_offset = M68KEMU_LIBBASE_REGION;
    for (i = 0; i < ctx->num_libs; i++)
    {
        ULONG lib_end = ctx->libs[i].m68k_addr + ctx->libs[i].pos_size;
        if (lib_end > region_offset)
            region_offset = (lib_end + 3) & ~3;
    }

    if (region_offset + total_size > M68KEMU_LIBBASE_REGION + M68KEMU_LIBBASE_SIZE)
        return 0;

    base_addr = region_offset + vectors_size;

    /* Fill ALL jump vector slots with A-line trap + NOP + NOP */
    for (i = 0; i < num_vectors; i++)
    {
        ULONG slot_addr = base_addr - ((ULONG)(i + 1) * M68K_JT_SLOT_SIZE);
        m68k_write16(ctx, slot_addr,     M68K_OP_ALINE);
        m68k_write16(ctx, slot_addr + 2, M68K_OP_NOP);
        m68k_write16(ctx, slot_addr + 4, M68K_OP_NOP);
    }

    ctx->libs[ctx->num_libs].m68k_addr   = base_addr;
    ctx->libs[ctx->num_libs].pos_size    = (lib_id == 0) ? M68K_SIZEOF_EXECBASE : 4;
    ctx->libs[ctx->num_libs].jt_start    = base_addr - vectors_size;
    ctx->libs[ctx->num_libs].lib_id      = lib_id;
    ctx->libs[ctx->num_libs].num_vectors = num_vectors;
    ctx->libs[ctx->num_libs].thunks      = thunks;
    ctx->libs[ctx->num_libs].num_thunks  = num_thunks;
    ctx->libs[ctx->num_libs].gen_thunks   = gen_thunks;
    ctx->libs[ctx->num_libs].num_gen_thunks = num_gen_thunks;
    if (name)
    {
        snprintf(ctx->libs[ctx->num_libs].name, sizeof(ctx->libs[ctx->num_libs].name), "%s", name);
    }
    ctx->num_libs++;

    /* Register in libmap for OpenLibrary lookups */
    if (name && ctx->num_libmap < M68KEMU_MAX_LIBMAP)
    {
        snprintf(ctx->libmap[ctx->num_libmap].name, sizeof(ctx->libmap[ctx->num_libmap].name), "%s", name);
        ctx->libmap[ctx->num_libmap].m68k_base = base_addr;
        ctx->num_libmap++;
    }

    return base_addr;
}

ULONG M68KEmu_FindLibBase(struct M68KEmuContext *ctx, const char *name)
{
    UWORD i;
    if (!name) return 0;
    for (i = 0; i < ctx->num_libmap; i++)
    {
        if (strcmp(ctx->libmap[i].name, name) == 0)
            return ctx->libmap[i].m68k_base;
    }
    return 0;
}

/* ── Hunk file loader (from raw memory buffer) ────────────────────── */

/* Hunk constants now in m68kemu_offsets.h */

static ULONG rd32(UBYTE **p)
{
    ULONG v = ((ULONG)(*p)[0] << 24) | ((ULONG)(*p)[1] << 16) |
              ((ULONG)(*p)[2] << 8)  |  (ULONG)(*p)[3];
    *p += 4;
    return v;
}

static UWORD rd16(UBYTE **p)
{
    UWORD v = (UWORD)(((*p)[0] << 8) | (*p)[1]);
    *p += 2;
    return v;
}

/* Add delta to the longword at offset in a loaded hunk, if it fits. */
static void reloc_hunk_long(struct M68KEmuContext *ctx, ULONG hunk_addr,
                            ULONG hunk_size, ULONG offset, ULONG delta)
{
    if (offset + 3 < hunk_size)
    {
        ULONG addr = hunk_addr + offset;
        m68k_write32(ctx, addr, m68k_read32(ctx, addr) + delta);
    }
}

ULONG M68KEmu_LoadHunksFromMemory(struct M68KEmuContext *ctx, UBYTE *data, LONG size)
{
    UBYTE *p = data;
    UBYTE *end = data + size;
    ULONG magic, num_hunks, first, last;
    ULONG *hunk_sizes = NULL;
    ULONG *hunk_addrs = NULL;
    ULONG *hunk_reqs  = NULL;
    ULONG i, entry_point = 0;

    if (p + 4 > end) return 0;
    magic = rd32(&p);
    if (magic != HUNK_HEADER) return 0;

    /* Skip resident library names */
    while (p + 4 <= end)
    {
        ULONG n = rd32(&p);
        if (n == 0) break;
        p += n * 4;
    }

    if (p + 12 > end) return 0;
    num_hunks = rd32(&p);
    first     = rd32(&p);  /* unused but must be consumed */
    last      = rd32(&p);  /* unused but must be consumed */
    (void)first; (void)last;

    if (num_hunks == 0 || num_hunks > 100) return 0;

    /* Read hunk sizes (in longwords) */
    hunk_sizes = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    hunk_addrs = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    hunk_reqs  = (ULONG *)AllocMem(num_hunks * sizeof(ULONG), MEMF_CLEAR);
    if (!hunk_sizes || !hunk_addrs || !hunk_reqs) goto fail;

    /* Hunk sizes in longwords; the top two bits carry memory attributes */
    for (i = 0; i < num_hunks; i++)
    {
        ULONG raw, memf;

        if (p + 4 > end) goto fail;
        raw  = rd32(&p);
        memf = raw & HUNK_MEMF_MASK;
        hunk_sizes[i] = (raw & HUNK_TYPE_MASK) * 4;

        if (memf == HUNK_MEMF_MASK)
        {
            if (p + 4 > end) goto fail;
            hunk_reqs[i] = rd32(&p) & (MEMF_CHIP | MEMF_FAST);
        }
        else if (memf == HUNK_MEMF_CHIP)
            hunk_reqs[i] = MEMF_CHIP;
        else if (memf == HUNK_MEMF_FAST)
            hunk_reqs[i] = MEMF_FAST;
    }

    /* Allocate space for each hunk in containment memory. Cleared so that
       BSS and the tail of short CODE/DATA hunks read as zero. */
    for (i = 0; i < num_hunks; i++)
    {
        ULONG addr = M68KEmu_HeapAlloc(ctx, hunk_sizes[i],
                                       hunk_reqs[i] | MEMF_CLEAR);
        if (!addr) goto fail;
        hunk_addrs[i] = addr;
        if (i == 0) entry_point = addr;
    }

    /* Parse hunk contents */
    i = 0;
    while (p + 4 <= end)
    {
        ULONG type = rd32(&p) & HUNK_TYPE_MASK;

        if (type == HUNK_CODE || type == HUNK_DATA)
        {
            if (p + 4 > end) goto fail;
            ULONG longs = rd32(&p);
            ULONG bytes = longs * 4;
            if (p + bytes > end || i >= num_hunks ||
                bytes > hunk_sizes[i]) goto fail;

            /* Copy data into containment space */
            memcpy(m68k_to_host(ctx, hunk_addrs[i]), p, bytes);
            p += bytes;
        }
        else if (type == HUNK_BSS)
        {
            if (p + 4 > end) goto fail;
            rd32(&p); /* skip size, already zeroed */
        }
        else if (type == HUNK_RELOC32)
        {
            while (p + 4 <= end)
            {
                ULONG count = rd32(&p);
                if (count == 0) break;
                if (p + 4 > end) goto fail;
                ULONG ref_hunk = rd32(&p);
                if (ref_hunk >= num_hunks) goto fail;

                ULONG ref_base = hunk_addrs[ref_hunk];
                ULONG j;
                for (j = 0; j < count; j++)
                {
                    if (p + 4 > end) goto fail;
                    ULONG offset = rd32(&p);
                    if (i < num_hunks)
                        reloc_hunk_long(ctx, hunk_addrs[i], hunk_sizes[i],
                                        offset, ref_base);
                }
            }
        }
        else if (type == HUNK_RELOC32SHORT || type == HUNK_DREL32 ||
                 type == HUNK_RELRELOC32)
        {
            /* Word-sized count, hunk and offsets; padded to a longword.
               RELRELOC32 fields are PC-relative, so they move by the
               distance between the target hunk and this one. */
            UBYTE *start = p;

            if (i >= num_hunks) goto fail;
            for (;;)
            {
                UWORD count, ref_hunk, j;
                ULONG delta;

                if (p + 2 > end) goto fail;
                count = rd16(&p);
                if (count == 0) break;
                if (p + 2 > end) goto fail;
                ref_hunk = rd16(&p);
                if (ref_hunk >= num_hunks) goto fail;

                delta = hunk_addrs[ref_hunk];
                if (type == HUNK_RELRELOC32)
                    delta -= hunk_addrs[i];

                if (p + 2UL * count > end) goto fail;
                for (j = 0; j < count; j++)
                    reloc_hunk_long(ctx, hunk_addrs[i], hunk_sizes[i],
                                    rd16(&p), delta);
            }
            if ((p - start) & 2)
                p += 2;
        }
        else if (type == HUNK_NAME || type == HUNK_DEBUG)
        {
            ULONG longs;

            if (p + 4 > end) goto fail;
            longs = rd32(&p);
            if (longs > (ULONG)(end - p) / 4) goto fail;
            p += longs * 4;
        }
        else if (type == HUNK_SYMBOL)
        {
            /* (name length in longs, name, value) until a zero length */
            for (;;)
            {
                ULONG longs;

                if (p + 4 > end) goto fail;
                longs = rd32(&p) & 0x00FFFFFF;
                if (longs == 0) break;
                if (longs + 1 > (ULONG)(end - p) / 4) goto fail;
                p += (longs + 1) * 4;
            }
        }
        else if (type == HUNK_END)
        {
            i++;
        }
        else
        {
            /* Unknown hunk type — skip */
            break;
        }
    }

    ctx->entry_point = entry_point;
    ctx->seg_start   = entry_point;

    FreeMem(hunk_sizes, num_hunks * sizeof(ULONG));
    FreeMem(hunk_addrs, num_hunks * sizeof(ULONG));
    FreeMem(hunk_reqs,  num_hunks * sizeof(ULONG));
    return entry_point;

fail:
    if (hunk_addrs) {
        for (i = 0; i < num_hunks; i++) {
            if (hunk_addrs[i])
                M68KEmu_HeapFree(ctx, hunk_addrs[i], hunk_sizes[i]);
        }
        FreeMem(hunk_addrs, num_hunks * sizeof(ULONG));
    }
    if (hunk_sizes) FreeMem(hunk_sizes, num_hunks * sizeof(ULONG));
    if (hunk_reqs)  FreeMem(hunk_reqs,  num_hunks * sizeof(ULONG));
    return 0;
}

/* Auto-open libraries: scan containment memory for the C startup pattern:
   N zero longwords (base pointers) followed immediately by N
   consecutive NUL-terminated ".library" name strings. */
void M68KEmu_AutoOpenLibs(struct M68KEmuContext *ctx)
{
    UBYTE *base = ctx->mem;
    ULONG sz = ctx->mem_size;
    ULONG off = M68KEMU_HEAP_BASE;

    while (off + 10 < sz)
    {
        if (base[off] != '.' || memcmp(base + off, ".library", 8) != 0 ||
            base[off + 8] != 0)
        {
            off++;
            continue;
        }

        /* Found ".library\0" — scan back for name start */
        ULONG name_start = off;
        while (name_start > 0 && base[name_start - 1] >= 0x20 &&
               base[name_start - 1] < 0x7f)
            name_start--;

        /* Collect consecutive library name strings */
        ULONG names[16];
        ULONG count = 0;
        ULONG p = name_start;
        while (p < sz && count < 16)
        {
            ULONG q = p;
            while (q < sz && base[q] >= 0x20 && base[q] < 0x7f) q++;
            if (q >= p + 9 && q < sz && base[q] == 0 &&
                memcmp(base + q - 8, ".library", 8) == 0)
            {
                names[count++] = p;
                p = q + 1;
            }
            else
                break;
        }

        if (count == 0) { off++; continue; }

        /* Check if count*4 bytes before first name are all zero */
        if (names[0] < count * 4 + 4) { off = p; continue; }
        ULONG table_start = names[0] - count * 4;

        BOOL all_zero = TRUE;
        ULONG j;
        for (j = 0; j < count * 4; j++)
            if (base[table_start + j] != 0) { all_zero = FALSE; break; }

        if (!all_zero) { off = p; continue; }

        /* Fill base pointer slots */
        for (j = 0; j < count; j++)
        {
            const char *lname = (const char *)(base + names[j]);
            ULONG fakebase = M68KEmu_OpenLibrary(ctx, lname);
            if (fakebase)
                m68k_write32(ctx, table_start + j * 4, fakebase);
        }

        off = p;
    }
}
