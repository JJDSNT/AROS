/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * Host-side proof that Moira, native LVO thunks, and Rigel operate in one
 * m68kemu context and observe the same Chip RAM.
 */
#include "m68kemu_intern.h"
#include "m68kemu_thunks.h"

#include <cstdio>

static int native_calls;

static IPTR thunk_native_probe(struct M68KEmuContext *, void *)
{
    native_calls++;
    return 42;
}

const struct M68KThunkEntry m68kemu_thunks_exec[] = {
    { 6, thunk_native_probe },
    { 0, nullptr }
};
const ULONG m68kemu_thunks_exec_count = 1;
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

static void emit_word(struct M68KEmuContext *ctx, ULONG &pc, UWORD value)
{
    m68k_write16(ctx, pc, value);
    pc += 2;
}

static void emit_long(struct M68KEmuContext *ctx, ULONG &pc, ULONG value)
{
    m68k_write32(ctx, pc, value);
    pc += 4;
}

static void emit_move_w_imm_abs(struct M68KEmuContext *ctx, ULONG &pc,
                                UWORD value, ULONG address)
{
    emit_word(ctx, pc, 0x33fc); /* MOVE.W #imm,(abs).L */
    emit_word(ctx, pc, value);
    emit_long(ctx, pc, address);
}

int main()
{
    constexpr ULONG source = 0x00020000u;
    constexpr ULONG destination = 0x00020002u;
    struct M68KEmuLibBase base = {};
    struct M68KEmuContext *ctx = M68KEmu_CreateContext(&base);

    if (ctx == nullptr || !M68KEmu_RigelInit(ctx)) {
        std::printf("FAIL: Rigel context initialization\n");
        M68KEmu_DestroyContext(ctx);
        return 1;
    }

    ULONG sysbase = M68KEmu_SetupLibBase(
        ctx, 0, "exec.library", 16,
        m68kemu_thunks_exec, m68kemu_thunks_exec_count,
        nullptr, 0
    );
    if (sysbase == 0) {
        std::printf("FAIL: fake native library setup\n");
        M68KEmu_DestroyContext(ctx);
        return 1;
    }

    m68k_write16(ctx, source, 0xcafe);
    m68k_write16(ctx, destination, 0x0000);

    ULONG code = M68KEmu_HeapAlloc(ctx, 256, 0);
    ULONG pc = code;

    emit_word(ctx, pc, 0x4eae); /* JSR -6(A6): native probe thunk */
    emit_word(ctx, pc, 0xfffa);

    /* Enable master and Blitter DMA, then copy one word A -> D. */
    emit_move_w_imm_abs(ctx, pc, 0x8240, 0x00dff096); /* DMACON */
    emit_move_w_imm_abs(ctx, pc, 0x09f0, 0x00dff040); /* BLTCON0 */
    emit_move_w_imm_abs(ctx, pc, 0x0000, 0x00dff042); /* BLTCON1 */
    emit_move_w_imm_abs(ctx, pc, 0xffff, 0x00dff044); /* BLTAFWM */
    emit_move_w_imm_abs(ctx, pc, 0xffff, 0x00dff046); /* BLTALWM */
    emit_move_w_imm_abs(ctx, pc, 0x0000, 0x00dff064); /* BLTAMOD */
    emit_move_w_imm_abs(ctx, pc, 0x0000, 0x00dff066); /* BLTDMOD */
    emit_move_w_imm_abs(ctx, pc, 0x0002, 0x00dff050); /* BLTAPTH */
    emit_move_w_imm_abs(ctx, pc, 0x0000, 0x00dff052); /* BLTAPTL */
    emit_move_w_imm_abs(ctx, pc, 0x0002, 0x00dff054); /* BLTDPTH */
    emit_move_w_imm_abs(ctx, pc, 0x0002, 0x00dff056); /* BLTDPTL */
    emit_move_w_imm_abs(ctx, pc, 0x0041, 0x00dff058); /* BLTSIZE: 1x1 */

    for (int i = 0; i < 8; ++i)
        emit_word(ctx, pc, 0x4e71); /* NOP: advance CPU/chipset time */
    emit_word(ctx, pc, 0x4e75);     /* RTS */

    ctx->entry_point = code;
    ctx->m68k_argptr = 0;
    ctx->m68k_argsize = 0;
    ctx->m68k_sysbase = sysbase;

    LONG result = M68KEmu_Execute(ctx);
    bool ok = result == 42 && native_calls == 1 &&
              m68k_read16(ctx, source) == 0xcafe &&
              m68k_read16(ctx, destination) == 0xcafe;

    std::printf("%s: native LVO + Rigel MMIO/timing/shared Chip RAM\n",
                ok ? "PASS" : "FAIL");
    M68KEmu_DestroyContext(ctx);
    return ok ? 0 : 1;
}
