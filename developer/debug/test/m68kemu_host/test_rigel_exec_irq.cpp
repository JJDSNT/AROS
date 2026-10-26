/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Rigel milestone 3 acceptance test: the Exec interrupt API
 * (AddIntServer, RemIntServer, SetIntVector, Disable, Enable) dispatching
 * contained 68k code, and a Copper list running from Chip RAM.
 *
 * The 68k program is m68k/rigel_exec_irq.s, assembled by vasm into a Hunk
 * executable (see "make fixtures"). Rigel state is only observed through
 * its public API.
 */
#include <exec/memory.h>
#include "m68kemu_intern.h"
#include "m68kemu_offsets.h"
#include "m68kemu_thunks.h"
#include "m68k/rigel_exec_irq_hunk.h"

/* Rigel headers carry no C++ linkage guards */
extern "C" {
#include <rigel/rigel.h>
}

#include <csignal>
#include <cstdio>
#include <unistd.h>

/* Native exec services, bound to the same APIs as m68kemu_thunks.c */
static int g_native_disables;

static IPTR thunk_AllocMem(struct M68KEmuContext *ctx, void *cpu)
{
    return M68KEmu_HeapAlloc(ctx, THUNK_D(0), THUNK_D(1));
}

static IPTR thunk_Disable(struct M68KEmuContext *ctx, void *cpu)
{
    if (!M68KEmu_Disable(ctx))
        g_native_disables++;
    return 0;
}

static IPTR thunk_Enable(struct M68KEmuContext *ctx, void *cpu)
{
    if (!M68KEmu_Enable(ctx))
        g_native_disables--;
    return 0;
}

static IPTR thunk_SetIntVector(struct M68KEmuContext *ctx, void *cpu)
{
    return M68KEmu_SetIntVector(ctx, THUNK_D(0), THUNK_A(1));
}

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
    { 120, thunk_Disable },
    { 126, thunk_Enable },
    { 162, thunk_SetIntVector },
    { 168, thunk_AddIntServer },
    { 174, thunk_RemIntServer },
    { 198, thunk_AllocMem },
    { 0, nullptr }
};
const ULONG m68kemu_thunks_exec_count = 6;
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

/* A 22-byte m68k struct Interrupt with the given priority */
static ULONG make_interrupt(struct M68KEmuContext *ctx, BYTE pri)
{
    ULONG irq = M68KEmu_HeapAlloc(ctx, 22, MEMF_CLEAR);
    ctx->mem[irq + 8] = 2; /* NT_INTERRUPT */
    ctx->mem[irq + M68K_LN_PRI] = (UBYTE)pri;
    m68k_write32(ctx, irq + M68K_IS_CODE, 0x1000);
    return irq;
}

static void test_api()
{
    std::printf("Exec interrupt API\n");

    struct M68KEmuContext *ctx = new_context(false);
    ULONG low = make_interrupt(ctx, -10);
    ULONG mid = make_interrupt(ctx, 0);
    ULONG mid2 = make_interrupt(ctx, 0);
    ULONG high = make_interrupt(ctx, 20);

    M68KEmu_AddIntServer(ctx, 5, mid);
    M68KEmu_AddIntServer(ctx, 5, low);
    M68KEmu_AddIntServer(ctx, 5, high);
    M68KEmu_AddIntServer(ctx, 5, mid2);
    M68KEmu_AddIntServer(ctx, 5, mid2);
    const ULONG *chain = ctx->ints.servers[5];
    check(ctx->ints.num_servers[5] == 4 && chain[0] == high &&
          chain[1] == mid && chain[2] == mid2 && chain[3] == low,
          "servers ordered by ln_Pri, FIFO among equals, no duplicates");

    M68KEmu_RemIntServer(ctx, 5, mid);
    check(ctx->ints.num_servers[5] == 3 && chain[1] == mid2,
          "RemIntServer unlinks the node");

    check(M68KEmu_SetIntVector(ctx, 6, high) == 0 &&
          M68KEmu_SetIntVector(ctx, 6, low) == high,
          "SetIntVector returns the previous handler");

    check(!M68KEmu_Disable(ctx) && !M68KEmu_Enable(ctx),
          "Disable/Enable stay native without Rigel");
    M68KEmu_DestroyContext(ctx);
}

static bool is_black(rigel_u32 px)
{
    return (px & 0x00ffffffu) == 0 || (px & 0xffffff00u) == 0;
}

static void test_program()
{
    std::printf("Exec interrupt + Copper program\n");

    struct M68KEmuContext *ctx = new_context(true);
    check(ctx != nullptr, "Rigel context");
    if (!ctx)
        return;

    ULONG sysbase = M68KEmu_SetupLibBase(ctx, 0, "exec.library", 128,
                                         m68kemu_thunks_exec,
                                         m68kemu_thunks_exec_count,
                                         nullptr, 0);
    ULONG entry = M68KEmu_LoadHunksFromMemory(
        ctx, (UBYTE *)rigel_exec_irq_hunk, (LONG)sizeof(rigel_exec_irq_hunk));
    check(sysbase && entry, "load Hunk executable");
    if (!sysbase || !entry)
    {
        M68KEmu_DestroyContext(ctx);
        return;
    }
    ctx->entry_point  = entry;
    ctx->m68k_sysbase = sysbase;

    LONG result = M68KEmu_Execute(ctx);
    RigelContext *rigel = (RigelContext *)ctx->rigel;

    if (result != 0)
        std::printf("  68k program failed at step %ld\n", (long)result);
    check(result == 0, "68k program completes every step");
    check(g_native_disables == 0, "Disable/Enable never reached the host");
    check(ctx->ints.num_servers[4] == 0 && ctx->ints.num_servers[5] == 0 &&
          ctx->ints.vectors[6] == 0, "servers removed, handler restored");
    check((rigel_get_intena(rigel) & 0x4070) == 0x4000,
          "INTENA: master on, COPER/VERTB/BLIT off");

    /* Let the Copper draw one more complete frame */
    rigel_step(rigel, (rigel_cycle_t)rigel_get_frame_cycles(rigel) * 2);

    rigel_frame_t frame = {};
    bool have_frame = rigel_get_frame(rigel, &frame) && frame.pixels &&
                      frame.format == RIGEL_PIXEL_RGBA8888 && frame.height > 2;
    check(have_frame, "Rigel has a completed RGBA frame");
    if (have_frame)
    {
        const UBYTE *rows = (const UBYTE *)frame.pixels;
        rigel_u32 x = frame.width / 2;
        rigel_u32 transitions = 0, first_red = 0;

        for (rigel_u32 y = 1; y < frame.height; y++)
        {
            rigel_u32 above = ((const rigel_u32 *)(rows + (y - 1) * frame.pitch))[x];
            rigel_u32 here  = ((const rigel_u32 *)(rows + y * frame.pitch))[x];
            if (is_black(above) != is_black(here))
            {
                transitions++;
                first_red = y;
            }
        }
        rigel_u32 top = ((const rigel_u32 *)rows)[x];
        rigel_u32 bottom = ((const rigel_u32 *)(rows + (frame.height - 1) * frame.pitch))[x];

        if (transitions != 1)
            std::printf("  frame %ux%u: %u colour transitions\n",
                        frame.width, frame.height, transitions);
        rigel_denise_video_desc_t desc = {};
        bool have_desc = rigel_denise_get_video_desc(rigel, &desc);

        check(is_black(top) && !is_black(bottom) && transitions == 1,
              "frame black above the Copper WAIT, red below");
        check(have_desc && first_red + desc.visible_y_start == 0x80,
              "colour changes on the WAIT line ($80)");
        /* Rigel labels this RGBA8888, but pixels are host u32 0x00RRGGBB;
           the AROS presenter relies on it */
        check(top == 0x00000000u && bottom == 0x00ff0000u,
              "pixels are host-endian 0x00RRGGBB");
        check((frame.flags & RIGEL_FRAME_COPPER_ACTIVE) != 0,
              "frame reports Copper activity");
    }
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

    test_api();
    test_program();

    std::printf("%s: Rigel milestone 3 (Exec interrupts + Copper)\n",
                failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
