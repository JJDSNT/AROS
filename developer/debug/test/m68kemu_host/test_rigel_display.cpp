/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Rigel milestone 4 acceptance test: the frame pump that feeds a display.
 *
 * Runs m68k/rigel_display.s (a one-bitplane checkerboard set up by the
 * Copper, COLOR01 cycled by a VERTB server) with a presenter that records
 * frames instead of drawing them, then checks what a window would have
 * shown. Set M68KEMU_DUMP=file.ppm to write the last recorded frame.
 */
#include <exec/memory.h>
#include "m68kemu_intern.h"
#include "m68kemu_offsets.h"
#include "m68kemu_thunks.h"
#include "m68k/rigel_display_hunk.h"

/* Rigel headers carry no C++ linkage guards (Rigel ISSUE-0012) */
extern "C" {
#include <rigel/rigel.h>
}

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

static IPTR thunk_AllocMem(struct M68KEmuContext *ctx, void *cpu)
{
    return M68KEmu_HeapAlloc(ctx, THUNK_D(0), THUNK_D(1));
}

static IPTR thunk_FreeMem(struct M68KEmuContext *ctx, void *cpu)
{
    M68KEmu_HeapFree(ctx, THUNK_A(1), THUNK_D(0));
    return 0;
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
    { 168, thunk_AddIntServer },
    { 174, thunk_RemIntServer },
    { 198, thunk_AllocMem },
    { 210, thunk_FreeMem },
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

/* ── Recording presenter ──────────────────────────────────────────── */

struct Recorder
{
    ULONG stop_after;               /* 0 = never ask to stop */
    ULONG presented;
    ULONG destroyed;
    std::vector<ULONG> numbers;
    ULONG width, height, frame_us;
    std::vector<std::vector<ULONG>> frames;   /* packed rows */
};

static BOOL record_present(struct M68KEmuContext *ctx,
                           const struct M68KEmuFrame *frame)
{
    Recorder *r = (Recorder *)ctx->presenter.data;

    r->presented++;
    r->numbers.push_back(frame->number);
    r->width = frame->width;
    r->height = frame->height;
    r->frame_us = frame->frame_us;
    r->frames.emplace_back((size_t)frame->width * frame->height);
    for (ULONG y = 0; y < frame->height; y++)
        std::memcpy(&r->frames.back()[(size_t)y * frame->width],
                    (const UBYTE *)frame->pixels + (size_t)y * frame->pitch,
                    frame->width * sizeof(ULONG));
    return !(r->stop_after && r->presented >= r->stop_after);
}

static void record_destroy(struct M68KEmuContext *ctx)
{
    Recorder *r = (Recorder *)ctx->presenter.data;

    r->destroyed++;
    ctx->presenter.present = nullptr;
    ctx->presenter.destroy = nullptr;
    ctx->presenter.data = nullptr;
}

static void dump_ppm(const Recorder &r, const char *path)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", r.width, r.height);
    for (ULONG px : r.frames.back())
    {
        unsigned char rgb[3] = { (unsigned char)(px >> 16),
                                 (unsigned char)(px >> 8),
                                 (unsigned char)px };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

/* Run the display program for `frames` frames with a recorder attached */
static LONG run_display(Recorder &r, const char *frames)
{
    struct M68KEmuLibBase base = {};
    struct M68KEmuContext *ctx = M68KEmu_CreateContext(&base);

    if (!ctx || !M68KEmu_RigelInit(ctx))
    {
        M68KEmu_DestroyContext(ctx);
        return -100;
    }
    ULONG sysbase = M68KEmu_SetupLibBase(ctx, 0, "exec.library", 128,
                                         m68kemu_thunks_exec,
                                         m68kemu_thunks_exec_count,
                                         nullptr, 0);
    ULONG entry = M68KEmu_LoadHunksFromMemory(
        ctx, (UBYTE *)rigel_display_hunk, (LONG)sizeof(rigel_display_hunk));
    ULONG args = M68KEmu_HeapAlloc(ctx, 16, MEMF_CLEAR);
    if (!sysbase || !entry || !args)
    {
        M68KEmu_DestroyContext(ctx);
        return -101;
    }
    std::memcpy(ctx->mem + args, frames, std::strlen(frames));

    ctx->entry_point  = entry;
    ctx->m68k_sysbase = sysbase;
    ctx->m68k_argptr  = args;
    ctx->m68k_argsize = (ULONG)std::strlen(frames);
    ctx->presenter.present = record_present;
    ctx->presenter.destroy = record_destroy;
    ctx->presenter.data    = &r;

    LONG result = M68KEmu_Execute(ctx);
    M68KEmu_DestroyContext(ctx);
    return result;
}

/* rigel_display.s: COLOR00 $0026, COLOR01 cycled through this table */
static const UWORD colours[16] = {
    0x0f00, 0x0f40, 0x0f80, 0x0fc0, 0x0ff0, 0x0af0, 0x04f0, 0x00f4,
    0x00fa, 0x00ff, 0x00af, 0x004f, 0x040f, 0x0a0f, 0x0f0f, 0x0f08
};

static ULONG rgb(UWORD c)
{
    return ((c >> 8 & 15) * 0x11u) << 16 | ((c >> 4 & 15) * 0x11u) << 8 |
           (c & 15) * 0x11u;
}

static bool in_table(ULONG px)
{
    for (UWORD c : colours)
        if (rgb(c) == px)
            return true;
    return false;
}

static void test_frames()
{
    std::printf("Display program, 12 frames\n");

    Recorder r = {};
    LONG result = run_display(r, "12\n");
    check(result == 0, "program runs its 12 frames and returns 0");
    check(r.presented == 12 && r.destroyed == 1,
          "one presented frame per VERTB, presenter released once");

    bool ordered = r.numbers.size() == 12;
    for (size_t i = 1; ordered && i < r.numbers.size(); i++)
        ordered = r.numbers[i] == r.numbers[i - 1] + 1;
    check(ordered, "frames presented in order, none skipped");
    check(r.frame_us >= 19900 && r.frame_us <= 20100,
          "frame duration is one PAL frame (20 ms)");

    if (const char *path = std::getenv("M68KEMU_DUMP"))
        dump_ppm(r, path);
    if (r.frames.empty() || r.width < 320 || r.height < 256)
    {
        check(false, "frame holds a 320x256 playfield");
        return;
    }

    /* Find the playfield: on the first cell row, cell 0 is COLOR00 and
       cell 1 is the first COLOR01 pixel. */
    const std::vector<ULONG> &last = r.frames.back();
    const ULONG background = rgb(0x0026);
    ULONG x1 = 0;
    while (x1 < r.width && last[8 * r.width + x1] == background)
        x1++;
    ULONG x0 = x1 >= 16 ? x1 - 16 : 0;
    ULONG bright = x1 < r.width ? last[8 * r.width + x1] : 0;
    check(x1 < r.width && x0 + 320 <= r.width && in_table(bright),
          "playfield found, COLOR01 from the server's table");

    bool board = true;
    for (ULONG cy = 0; cy < 16 && board; cy++)
        for (ULONG cx = 0; cx < 20 && board; cx++)
        {
            ULONG px = last[(cy * 16 + 8) * r.width + x0 + cx * 16 + 8];
            board = px == (((cx + cy) & 1) ? bright : background);
        }
    check(board, "16x16 checkerboard over 320x256 in two colours");

    /* COLOR01 changes every frame once the server runs */
    bool cycling = true;
    for (size_t i = 3; i < r.frames.size() && cycling; i++)
    {
        ULONG now = r.frames[i][8 * r.width + x1];
        ULONG before = r.frames[i - 1][8 * r.width + x1];
        cycling = in_table(now) && now != before;
    }
    check(cycling, "COLOR01 changes between consecutive frames");
}

static void test_stop()
{
    std::printf("Presenter asks to stop\n");

    Recorder r = {};
    r.stop_after = 3;
    LONG result = run_display(r, "50\n");
    check(result == M68KEMU_EXIT_STOPPED && r.presented == 3 && r.destroyed == 1,
          "program ends after 3 of 50 frames with RETURN_WARN");
}

int main()
{
    std::signal(SIGALRM, [](int) {
        static const char msg[] = "FAIL: timed out\n";
        (void)!write(2, msg, sizeof(msg) - 1);
        _exit(1);
    });
    alarm(120);

    test_frames();
    test_stop();

    std::printf("%s: Rigel milestone 4 (display)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
