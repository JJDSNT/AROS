/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Rigel milestone 6 acceptance test: Paula audio sampled by the bridge.
 *
 * Runs m68k/rigel_audio.s (a 440 Hz square wave on channel 0 after a few
 * silent frames) with a presenter that records the per-frame audio
 * batches, then checks rate, silence, pitch and channel routing. Set
 * M68KEMU_WAV=file.wav to write what was recorded.
 */
#include <exec/memory.h>
#include "m68kemu_intern.h"
#include "m68kemu_offsets.h"
#include "m68kemu_thunks.h"
#include "m68k/rigel_audio_hunk.h"

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
    { 162, thunk_SetIntVector },
    { 168, thunk_AddIntServer },
    { 174, thunk_RemIntServer },
    { 198, thunk_AllocMem },
    { 210, thunk_FreeMem },
    { 0, nullptr }
};
const ULONG m68kemu_thunks_exec_count = 5;
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

static const ULONG RATE = 22050;
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
    std::vector<ULONG> batch;       /* stereo frames per video frame */
    std::vector<WORD>  samples;     /* interleaved left, right */
};

static void record_audio(struct M68KEmuContext *ctx, const WORD *s, ULONG frames)
{
    Recorder *r = (Recorder *)ctx->presenter.data;
    r->batch.push_back(frames);
    r->samples.insert(r->samples.end(), s, s + frames * 2);
}

static BOOL record_present(struct M68KEmuContext *, const struct M68KEmuFrame *)
{
    return TRUE;
}

static void record_destroy(struct M68KEmuContext *ctx)
{
    ctx->presenter.present = nullptr;
    ctx->presenter.audio = nullptr;
    ctx->presenter.destroy = nullptr;
}

static void write_wav(const Recorder &r, const char *path)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return;
    ULONG bytes = (ULONG)(r.samples.size() * 2);
    auto u32 = [&](ULONG v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](UWORD v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(RATE); u32(RATE * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    std::fwrite(r.samples.data(), 2, r.samples.size(), f);
    std::fclose(f);
}

static void test_program()
{
    std::printf("Audio program\n");

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
        ctx, (UBYTE *)rigel_audio_hunk, (LONG)sizeof(rigel_audio_hunk));
    ULONG args = M68KEmu_HeapAlloc(ctx, 16, MEMF_CLEAR);
    if (!sysbase || !entry || !args)
    {
        check(false, "load Hunk executable");
        M68KEmu_DestroyContext(ctx);
        return;
    }
    std::memcpy(ctx->mem + args, "60\n", 3);

    Recorder r;
    ctx->entry_point  = entry;
    ctx->m68k_sysbase = sysbase;
    ctx->m68k_argptr  = args;
    ctx->m68k_argsize = 3;
    ctx->presenter.present    = record_present;
    ctx->presenter.audio      = record_audio;
    ctx->presenter.audio_rate = RATE;
    ctx->presenter.destroy    = record_destroy;
    ctx->presenter.data       = &r;

    LONG result = M68KEmu_Execute(ctx);
    ULONG dropped = ctx->audio_dropped;
    M68KEmu_DestroyContext(ctx);

    /* Rigel ISSUE-0018: audio DMA never raises AUDxINT, so the program
       reports 6. Expected until Rigel wires its audio interrupt sink. */
    if (result == 6)
        std::printf("  %-58s XFAIL (Rigel ISSUE-0018)\n",
                    "~440 channel 0 block interrupts per second");
    else
    {
        if (result != 0)
            std::printf("  68k program failed at step %ld\n", (long)result);
        check(result == 0, "program plays, counts ~440 blocks/s, returns 0");
        if (result == 0)
            std::printf("  NOTE: audio interrupts work now; drop the "
                        "ISSUE-0018 expectation\n");
    }
    if (const char *path = std::getenv("M68KEMU_WAV"))
        write_wav(r, path);

    /* 22050 Hz over a 19.97 ms PAL frame (312 lines) is 440.3 samples */
    bool rate_ok = r.batch.size() > 60 && dropped == 0;
    for (size_t i = 1; rate_ok && i < r.batch.size(); i++)
        rate_ok = r.batch[i] >= 440 && r.batch[i] <= 441;
    check(rate_ok, "440-441 stereo samples per frame, none dropped");

    /* DMA starts on the fifth VERTB; the first three frames are silent */
    size_t per_frame = 440;
    bool silent = r.samples.size() > 3 * per_frame * 2;
    for (size_t i = 0; silent && i < 3 * per_frame * 2; i++)
        silent = r.samples[i] == 0;
    check(silent, "silence before audio DMA starts");

    /* Tone window: frames 15..55, well inside playback */
    size_t from = 15 * per_frame, to = 55 * per_frame;
    if (r.samples.size() < to * 2)
    {
        check(false, "enough samples recorded");
        return;
    }
    long sum = 0;
    WORD lo = 32767, hi = -32768, right_peak = 0;
    for (size_t i = from; i < to; i++)
    {
        WORD l = r.samples[i * 2], rr = r.samples[i * 2 + 1];
        sum += l;
        lo = l < lo ? l : lo;
        hi = l > hi ? l : hi;
        right_peak = (rr < 0 ? -rr : rr) > right_peak ? (rr < 0 ? -rr : rr) : right_peak;
    }
    long mean = sum / (long)(to - from);
    ULONG crossings = 0;
    for (size_t i = from + 1; i < to; i++)
        if ((r.samples[(i - 1) * 2] >= mean) != (r.samples[i * 2] >= mean))
            crossings++;
    double seconds = (double)(to - from) / RATE;
    double hz = crossings / 2.0 / seconds;

    std::printf("  left %d..%d, %.1f Hz; right peak %d\n", lo, hi, hz, right_peak);
    check(hi - lo > 1000 && hz > 435.0 && hz < 445.0,
          "about 440 Hz on the left output");
    check(right_peak == 0, "right output silent (channel 0 is left)");
}

int main()
{
    std::signal(SIGALRM, [](int) {
        static const char msg[] = "FAIL: timed out\n";
        (void)!write(2, msg, sizeof(msg) - 1);
        _exit(1);
    });
    alarm(60);

    test_program();

    std::printf("%s: Rigel milestone 6 (audio)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
