/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Rigel milestone 5 acceptance test: CIA access, CIA timer interrupts and
 * host input through the bridge.
 *
 * Runs m68k/rigel_input.s and, from the per-frame presenter callback,
 * injects keys, mouse movement and buttons through the bridge calls the
 * AROS window uses. The program checks what arrived and returns 0.
 */
#include <exec/memory.h>
#include "m68kemu_intern.h"
#include "m68kemu_offsets.h"
#include "m68kemu_thunks.h"
#include "m68k/rigel_input_hunk.h"

/* Rigel headers carry no C++ linkage guards (Rigel ISSUE-0012) */
extern "C" {
#include <rigel/rigel.h>
}

#include <csignal>
#include <cstdio>
#include <unistd.h>

static IPTR thunk_AddIntServer(struct M68KEmuContext *ctx, void *cpu)
{
    M68KEmu_AddIntServer(ctx, THUNK_D(0), THUNK_A(1));
    return 0;
}

static IPTR thunk_RemIntServer(struct M68KEmuContext *ctx, void *cpu)
{
    M68KEmu_RemIntServer(ctx, THUNK_D(0), THUNK_A(1));
    return 0;
}

const struct M68KThunkEntry m68kemu_thunks_exec[] = {
    { 168, thunk_AddIntServer },
    { 174, thunk_RemIntServer },
    { 0, nullptr }
};
const ULONG m68kemu_thunks_exec_count = 2;
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

/* Inject input the way the AROS window would, at fixed frames */
static ULONG g_frames;

static BOOL inject_present(struct M68KEmuContext *ctx,
                           const struct M68KEmuFrame *)
{
    switch (++g_frames)
    {
    case 3:     /* both reach the chipset, one frame apart */
        M68KEmu_RigelInputKey(ctx, 0x20);           /* A down */
        M68KEmu_RigelInputKey(ctx, 0x20 | 0x80);    /* A up */
        break;
    case 6:
        M68KEmu_RigelInputKey(ctx, 0x40);           /* Space down */
        M68KEmu_RigelInputKey(ctx, 0x40 | 0x80);    /* Space up */
        break;
    case 8:
        M68KEmu_RigelInputMouse(ctx, 5, -3);
        break;
    case 9:
        M68KEmu_RigelInputMouse(ctx, 2, 1);
        break;
    case 10:
        M68KEmu_RigelInputButtons(ctx, TRUE, TRUE);
        break;
    }
    return TRUE;
}

static void inject_destroy(struct M68KEmuContext *ctx)
{
    ctx->presenter.present = nullptr;
    ctx->presenter.destroy = nullptr;
}

static void test_io_space()
{
    std::printf("I/O space\n");

    struct M68KEmuLibBase base = {};
    struct M68KEmuContext *ctx = M68KEmu_CreateContext(&base);
    bool ok = ctx && M68KEmu_RigelInit(ctx);
    check(ok, "Rigel context");
    if (!ok)
    {
        M68KEmu_DestroyContext(ctx);
        return;
    }

    /* Exhaust the Fast heap in 64 KiB blocks: none may touch the I/O space */
    bool clear = true;
    ULONG blocks = 0, a;
    while ((a = M68KEmu_HeapAlloc(ctx, 65536, MEMF_FAST)) != 0)
    {
        blocks++;
        if (a + 65536 > M68KEMU_IO_START && a < M68KEMU_IO_END)
            clear = false;
    }
    check(clear && blocks > 300, "Fast heap never allocates $A00000-$DFFFFF");
    check(M68KEmu_HeapAvail(ctx, MEMF_FAST | MEMF_TOTAL) ==
          (M68KEMU_IO_START - M68KEMU_CHIP_RAM_SIZE) +
          (ctx->heap_end - M68KEMU_IO_END),
          "AvailMem(MEMF_TOTAL) leaves the I/O space out");
    M68KEmu_DestroyContext(ctx);
}

static void test_program()
{
    std::printf("CIA + input program\n");

    struct M68KEmuLibBase base = {};
    struct M68KEmuContext *ctx = M68KEmu_CreateContext(&base);
    if (!ctx || !M68KEmu_RigelInit(ctx))
    {
        check(false, "Rigel context");
        M68KEmu_DestroyContext(ctx);
        return;
    }
    ULONG sysbase = M68KEmu_SetupLibBase(ctx, 0, "exec.library", 128,
                                         m68kemu_thunks_exec,
                                         m68kemu_thunks_exec_count,
                                         nullptr, 0);
    ULONG entry = M68KEmu_LoadHunksFromMemory(
        ctx, (UBYTE *)rigel_input_hunk, (LONG)sizeof(rigel_input_hunk));
    check(sysbase && entry, "load Hunk executable");
    if (!sysbase || !entry)
    {
        M68KEmu_DestroyContext(ctx);
        return;
    }
    ctx->entry_point  = entry;
    ctx->m68k_sysbase = sysbase;
    ctx->presenter.present = inject_present;
    ctx->presenter.destroy = inject_destroy;

    LONG result = M68KEmu_Execute(ctx);
    if (result != 0)
        std::printf("  68k program failed at step %ld\n", (long)result);
    check(result == 0, "keys, mouse, buttons and CIA timer A as injected");
    check(ctx->key_count == 0, "key queue drained one key per frame");
    M68KEmu_DestroyContext(ctx);
}

int main()
{
    std::signal(SIGALRM, [](int) {
        static const char msg[] = "FAIL: timed out\n";
        (void)!write(2, msg, sizeof(msg) - 1);
        _exit(1);
    });
    alarm(60);

    test_io_space();
    test_program();

    std::printf("%s: Rigel milestone 5 (CIAs + input)\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
