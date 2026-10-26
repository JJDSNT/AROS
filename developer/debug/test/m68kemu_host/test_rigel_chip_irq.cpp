/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Rigel milestone 2 acceptance test: Chip RAM allocation, Hunk memory
 * attributes, and a Blitter interrupt delivered to a contained 68k
 * autovector handler, with native LVO calls before and after it.
 *
 * The 68k program is m68k/rigel_chip_irq.s, assembled by vasm into real
 * Hunk executables (see "make fixtures"): once with Kickstart 1.x
 * HUNK_RELOC32 only, once with vasm's defaults (short relocations and a
 * symbol table). Rigel state is only observed through its public API.
 */
#include <exec/memory.h>
#include "m68kemu_intern.h"
#include "m68kemu_offsets.h"
#include "m68kemu_thunks.h"
#include "m68k/rigel_chip_irq_hunk.h"
#include "m68k/rigel_chip_irq_short_hunk.h"

/* Rigel headers carry no C++ linkage guards */
extern "C" {
#include <rigel/rigel.h>
}

#include <csignal>
#include <cstdio>
#include <unistd.h>
#include <vector>

/* Native exec services, bound to the same heap API as m68kemu_thunks.c */
static int g_chip_allocs, g_frees_after_irq, g_frees, g_availmem_calls;

static IPTR thunk_AllocMem(struct M68KEmuContext *ctx, void *cpu)
{
    if (THUNK_D(1) & MEMF_CHIP)
        g_chip_allocs++;
    return M68KEmu_HeapAlloc(ctx, THUNK_D(0), THUNK_D(1));
}

static IPTR thunk_FreeMem(struct M68KEmuContext *ctx, void *cpu)
{
    const RigelContext *rigel = (const RigelContext *)ctx->rigel;

    /* Called after the interrupt: BLIT must be enabled-then-disabled and
       its request acknowledged. */
    g_frees++;
    if (!(rigel_get_intreq(rigel) & 0x0040) && rigel_get_ipl(rigel) == 0)
        g_frees_after_irq++;
    M68KEmu_HeapFree(ctx, THUNK_A(1), THUNK_D(0));
    return 0;
}

static IPTR thunk_AvailMem(struct M68KEmuContext *ctx, void *cpu)
{
    g_availmem_calls++;
    return M68KEmu_HeapAvail(ctx, THUNK_D(1));
}

static IPTR thunk_TypeOfMem(struct M68KEmuContext *ctx, void *cpu)
{
    return M68KEmu_HeapTypeOf(ctx, THUNK_A(1));
}

const struct M68KThunkEntry m68kemu_thunks_exec[] = {
    { 198, thunk_AllocMem },
    { 210, thunk_FreeMem },
    { 216, thunk_AvailMem },
    { 534, thunk_TypeOfMem },
    { 0, nullptr }
};
const ULONG m68kemu_thunks_exec_count = 4;
const struct M68KThunkEntry m68kemu_thunks_dos[] = { { 0, nullptr } };
const ULONG m68kemu_thunks_dos_count = 0;

extern "C" {
ULONG M68KEmu_OpenLibrary(struct M68KEmuContext *, const char *) { return 0; }
ULONG M68KEmu_LoadM68KLibrary(struct M68KEmuContext *, const char *) { return 0; }
}

#include "../../../../rom/m68kemu/m68kemu_memory.c"
#include "../../../../rom/m68kemu/Moira/Moira/Moira.cpp"
#include "../../../../rom/m68kemu/Moira/Moira/MoiraDebugger.cpp"
#include "../../../../rom/m68kemu/m68kemu_moira.cpp"
#include "../../../../rom/m68kemu/m68kemu_interrupts.c"

static int failures;

static void check(bool ok, const char *what)
{
    std::printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static struct M68KEmuContext *new_context(bool rigel)
{
    struct M68KEmuLibBase base = {};
    struct M68KEmuContext *ctx = M68KEmu_CreateContext(&base);

    if (ctx && rigel && !M68KEmu_RigelInit(ctx))
    {
        M68KEmu_DestroyContext(ctx);
        return nullptr;
    }
    return ctx;
}

static void put32(std::vector<UBYTE> &v, ULONG x)
{
    v.push_back((UBYTE)(x >> 24));
    v.push_back((UBYTE)(x >> 16));
    v.push_back((UBYTE)(x >> 8));
    v.push_back((UBYTE)x);
}

/* Two-hunk image: hunk 0 holds a pointer relocated to hunk 1, whose size
   word carries the given attribute bits (and the extended long if any). */
static ULONG load_attr_probe(struct M68KEmuContext *ctx, ULONG size_bits,
                             ULONG ext_attrs)
{
    std::vector<UBYTE> h;

    put32(h, HUNK_HEADER);
    put32(h, 0);
    put32(h, 2); put32(h, 0); put32(h, 1);
    put32(h, 1);
    put32(h, 4 | size_bits);
    if ((size_bits & HUNK_MEMF_MASK) == HUNK_MEMF_MASK)
        put32(h, ext_attrs);
    put32(h, HUNK_CODE); put32(h, 1); put32(h, 0);
    put32(h, HUNK_RELOC32); put32(h, 1); put32(h, 1); put32(h, 0); put32(h, 0);
    put32(h, HUNK_END);
    put32(h, HUNK_BSS); put32(h, 4);
    put32(h, HUNK_END);

    ULONG entry = M68KEmu_LoadHunksFromMemory(ctx, h.data(), (LONG)h.size());
    return entry ? m68k_read32(ctx, entry) : 0;
}

static void test_heap_split()
{
    std::printf("Chip/Fast heap\n");

    struct M68KEmuContext *legacy = new_context(false);
    ULONG a = M68KEmu_HeapAlloc(legacy, 64, MEMF_CHIP);
    check(a >= M68KEMU_CHIP_RAM_SIZE, "legacy heap ignores MEMF_CHIP");
    check(load_attr_probe(legacy, HUNK_MEMF_CHIP, 0) >= M68KEMU_CHIP_RAM_SIZE,
          "legacy loader ignores HUNKF_CHIP");
    M68KEmu_DestroyContext(legacy);

    struct M68KEmuContext *ctx = new_context(true);
    check(ctx != nullptr, "Rigel context splits the heap");
    if (!ctx)
        return;

    ULONG chip = M68KEmu_HeapAlloc(ctx, 64, MEMF_CHIP);
    ULONG fast = M68KEmu_HeapAlloc(ctx, 64, MEMF_FAST);
    ULONG any  = M68KEmu_HeapAlloc(ctx, 64, MEMF_ANY);
    check(chip >= M68KEMU_HEAP_BASE && chip + 64 <= M68KEMU_CHIP_RAM_SIZE,
          "MEMF_CHIP inside Chip window");
    check(fast >= M68KEMU_CHIP_RAM_SIZE, "MEMF_FAST above Chip window");
    check(any >= M68KEMU_CHIP_RAM_SIZE, "MEMF_ANY prefers Fast");
    check((M68KEmu_HeapTypeOf(ctx, chip) & MEMF_CHIP) &&
          (M68KEmu_HeapTypeOf(ctx, fast) & MEMF_FAST), "TypeOfMem");

    ULONG chip_free = M68KEmu_HeapAvail(ctx, MEMF_CHIP);
    M68KEmu_HeapFree(ctx, chip, 64);
    check(M68KEmu_HeapAvail(ctx, MEMF_CHIP) > chip_free &&
          M68KEmu_HeapAlloc(ctx, 64, MEMF_CHIP) == chip,
          "Chip block returns to the Chip list");

    m68k_write32(ctx, chip, 0xdeadbeef);
    M68KEmu_HeapFree(ctx, chip, 64);
    ULONG cleared = M68KEmu_HeapAlloc(ctx, 64, MEMF_CHIP | MEMF_CLEAR);
    check(cleared == chip && m68k_read32(ctx, cleared) == 0, "MEMF_CLEAR");

    ULONG big = M68KEmu_HeapAlloc(ctx, M68KEMU_CHIP_RAM_SIZE, MEMF_CHIP);
    check(big == 0, "oversized MEMF_CHIP fails instead of using Fast");

    check(load_attr_probe(ctx, HUNK_MEMF_CHIP, 0) < M68KEMU_CHIP_RAM_SIZE,
          "HUNKF_CHIP hunk in Chip");
    check(load_attr_probe(ctx, HUNK_MEMF_FAST, 0) >= M68KEMU_CHIP_RAM_SIZE,
          "HUNKF_FAST hunk in Fast");
    check(load_attr_probe(ctx, HUNK_MEMF_MASK, MEMF_CHIP) < M68KEMU_CHIP_RAM_SIZE,
          "extended attribute long (MEMF_CHIP) in Chip");
    M68KEmu_DestroyContext(ctx);
}

static void put16(std::vector<UBYTE> &v, UWORD x)
{
    v.push_back((UBYTE)(x >> 8));
    v.push_back((UBYTE)x);
}

/*
 * Hunk 0 (code, 4 longs):
 *   longs 0 = 0x10 and 3 = 0x30: relocated to hunk 1 by a short block with
 *     an odd word count, so it is padded to a longword
 *   long 1 = 0x20: relocated PC-relative to hunk 1 by HUNK_RELRELOC32
 *   long 2 = 0: relocated to hunk 1 by HUNK_RELOC32, giving hunk 1's address
 * HUNK_SYMBOL and HUNK_DEBUG must be skipped and the BSS hunk after them
 * still loaded.
 */
static bool load_short_reloc_probe(ULONG reloc_type, ULONG *entry_out,
                                   ULONG *hunk1_out, ULONG *rel_out,
                                   ULONG *abs_out, ULONG *abs2_out)
{
    std::vector<UBYTE> h;
    struct M68KEmuContext *ctx = new_context(false);

    put32(h, HUNK_HEADER);
    put32(h, 0);
    put32(h, 2); put32(h, 0); put32(h, 1);
    put32(h, 4);
    put32(h, 4);
    put32(h, HUNK_CODE); put32(h, 4);
    put32(h, 0x10); put32(h, 0x20); put32(h, 0); put32(h, 0x30);
    put32(h, HUNK_RELOC32);
    put32(h, 1); put32(h, 1); put32(h, 8); put32(h, 0);
    put32(h, reloc_type);
    put16(h, 2); put16(h, 1); put16(h, 0); put16(h, 12);  /* longs 0, 3 */
    put16(h, 0);                                          /* 5 words */
    put16(h, 0);                                          /* padding */
    put32(h, HUNK_RELRELOC32);
    put16(h, 1); put16(h, 1); put16(h, 4); put16(h, 0);  /* 4: no pad */
    put32(h, HUNK_SYMBOL);
    put32(h, 1); put32(h, 0x73796d00); put32(h, 0);       /* "sym" = 0 */
    put32(h, 0);
    put32(h, HUNK_DEBUG); put32(h, 2); put32(h, 0); put32(h, 0);
    put32(h, HUNK_END);
    put32(h, HUNK_BSS); put32(h, 4);
    put32(h, HUNK_END);

    ULONG entry = M68KEmu_LoadHunksFromMemory(ctx, h.data(), (LONG)h.size());
    *entry_out = entry;
    *abs_out = entry ? m68k_read32(ctx, entry) : 0;
    *rel_out = entry ? m68k_read32(ctx, entry + 4) : 0;
    *hunk1_out = entry ? m68k_read32(ctx, entry + 8) : 0;
    *abs2_out = entry ? m68k_read32(ctx, entry + 12) : 0;
    M68KEmu_DestroyContext(ctx);
    return entry != 0;
}

static void test_short_relocs()
{
    static const struct { ULONG type; const char *name; } kinds[] = {
        { HUNK_RELOC32SHORT, "HUNK_RELOC32SHORT" },
        { HUNK_DREL32,       "HUNK_DREL32" },
    };

    std::printf("Short Hunk relocations\n");
    for (const auto &k : kinds)
    {
        ULONG entry, hunk1, rel, abs, abs2;
        char what[80];
        bool loaded = load_short_reloc_probe(k.type, &entry, &hunk1, &rel, &abs,
                                             &abs2);

        std::snprintf(what, sizeof(what), "%s + RELRELOC32 + SYMBOL/DEBUG", k.name);
        check(loaded && hunk1 != entry && hunk1 + 0x10 == abs &&
              hunk1 + 0x30 == abs2 &&
              rel == 0x20 + (hunk1 - entry), what);
    }
}

static void test_blit_interrupt(const char *name, const unsigned char *image,
                                size_t size)
{
    std::printf("Blitter interrupt program (%s)\n", name);
    g_chip_allocs = g_frees_after_irq = g_frees = g_availmem_calls = 0;

    struct M68KEmuContext *ctx = new_context(true);
    check(ctx != nullptr, "Rigel context");
    if (!ctx)
        return;

    ULONG sysbase = M68KEmu_SetupLibBase(ctx, 0, "exec.library", 128,
                                         m68kemu_thunks_exec,
                                         m68kemu_thunks_exec_count,
                                         nullptr, 0);
    ULONG entry = M68KEmu_LoadHunksFromMemory(
        ctx, (UBYTE *)image, (LONG)size);
    check(sysbase && entry, "load Hunk executable");
    if (!sysbase || !entry)
    {
        M68KEmu_DestroyContext(ctx);
        return;
    }

    ctx->entry_point  = entry;
    ctx->m68k_sysbase = sysbase;

    LONG result = M68KEmu_Execute(ctx);
    const RigelContext *rigel = (const RigelContext *)ctx->rigel;

    if (result != 0)
        std::printf("  68k program failed at step %ld\n", (long)result);
    check(result == 0, "68k program completes every step");
    check(g_chip_allocs == 2, "two MEMF_CHIP AllocMem calls");
    check(g_frees == 2 && g_frees_after_irq == 2,
          "FreeMem LVOs after the interrupt was acknowledged");
    check(g_availmem_calls == 1, "AvailMem LVO after the interrupt");
    check(!(rigel_get_intena(rigel) & 0x0040) &&
          !(rigel_get_intreq(rigel) & 0x0040) && rigel_get_ipl(rigel) == 0,
          "Rigel left with BLIT disabled and acknowledged");
    check(m68k_read32(ctx, 0x6c) ==
              M68KEMU_INT_STUBS + 2 * M68KEMU_INT_STUB_SIZE,
          "level 3 Exec dispatcher restored");
    M68KEmu_DestroyContext(ctx);
}

int main()
{
    /* A lost acknowledge would re-enter the handler forever */
    std::signal(SIGALRM, [](int) {
        static const char msg[] = "FAIL: timed out\n";
        (void)!write(2, msg, sizeof(msg) - 1);
        _exit(1);
    });
    alarm(20);

    test_heap_split();
    test_short_relocs();
    test_blit_interrupt("HUNK_RELOC32", rigel_chip_irq_hunk,
                        sizeof(rigel_chip_irq_hunk));
    test_blit_interrupt("HUNK_DREL32 + HUNK_SYMBOL", rigel_chip_irq_short_hunk,
                        sizeof(rigel_chip_irq_short_hunk));

    std::printf("%s: Rigel milestone 2 (Chip RAM + interrupts)\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
