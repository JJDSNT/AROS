/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Optional Rigel chipset bridge. Rigel remains CPU- and AROS-independent;
    this file owns all policy connecting it to m68kemu.library.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include <rigel/rigel.h>

#include "m68kemu_intern.h"

#define M68KEMU_RIGEL_CHIP_RAM_SIZE (512UL * 1024UL)

static void *rigel_alloc(size_t size, void *opaque)
{
    (void)opaque;
    return AllocVec((ULONG)size, MEMF_CLEAR);
}

static void rigel_free(void *ptr, void *opaque)
{
    (void)opaque;
    FreeVec(ptr);
}

static rigel_u16 rigel_chip_read16(void *opaque, rigel_u32 addr)
{
    struct M68KEmuContext *ctx = (struct M68KEmuContext *)opaque;

    if (addr + 1u >= M68KEMU_RIGEL_CHIP_RAM_SIZE)
        return 0xffffu;
    return (rigel_u16)m68k_read16(ctx, (ULONG)addr);
}

static void rigel_chip_write16(void *opaque, rigel_u32 addr, rigel_u16 value)
{
    struct M68KEmuContext *ctx = (struct M68KEmuContext *)opaque;

    if (addr + 1u >= M68KEMU_RIGEL_CHIP_RAM_SIZE)
        return;
    m68k_write16(ctx, (ULONG)addr, (UWORD)value);
}

BOOL M68KEmu_RigelInit(struct M68KEmuContext *ctx)
{
    rigel_config_t config = { 0 };

    if (!ctx)
        return FALSE;
    if (ctx->rigel)
        return TRUE;

    config.alloc_fn = rigel_alloc;
    config.free_fn = rigel_free;
    config.chip_ram_size = M68KEMU_RIGEL_CHIP_RAM_SIZE;
    config.chip_ram.opaque = ctx;
    config.chip_ram.read16 = rigel_chip_read16;
    config.chip_ram.write16 = rigel_chip_write16;
    config.video_std = RIGEL_VIDEO_PAL;
    config.chipset_model = RIGEL_CHIPSET_OCS;

    ctx->rigel = (APTR)rigel_create(&config);
    return ctx->rigel != NULL;
}

void M68KEmu_RigelDestroy(struct M68KEmuContext *ctx)
{
    if (!ctx || !ctx->rigel)
        return;
    rigel_destroy((RigelContext *)ctx->rigel);
    ctx->rigel = NULL;
}

BOOL M68KEmu_RigelRead(struct M68KEmuContext *ctx, ULONG addr,
                       UBYTE size, ULONG *value)
{
    rigel_u32 result = 0;
    rigel_mmio_result_t status;

    if (!ctx || !ctx->rigel || !value)
        return FALSE;

    status = rigel_mmio_read((RigelContext *)ctx->rigel,
                             (rigel_u32)addr, (rigel_u8)size, &result);
    if (status == RIGEL_MMIO_UNMAPPED)
        return FALSE;

    *value = status == RIGEL_MMIO_HANDLED ? (ULONG)result : 0;
    return TRUE;
}

BOOL M68KEmu_RigelWrite(struct M68KEmuContext *ctx, ULONG addr,
                        UBYTE size, ULONG value)
{
    rigel_mmio_result_t status;

    if (!ctx || !ctx->rigel)
        return FALSE;

    status = rigel_mmio_write((RigelContext *)ctx->rigel,
                              (rigel_u32)addr, (rigel_u8)size,
                              (rigel_u32)value);
    return status != RIGEL_MMIO_UNMAPPED;
}

void M68KEmu_RigelStepCPU(struct M68KEmuContext *ctx,
                          unsigned long long cycles)
{
    if (ctx && ctx->rigel)
        (void)rigel_step_cpu_cycles((RigelContext *)ctx->rigel,
                                    (rigel_u64)cycles);
}

UBYTE M68KEmu_RigelIPL(const struct M68KEmuContext *ctx)
{
    if (!ctx || !ctx->rigel)
        return 0;
    return (UBYTE)rigel_get_ipl((const RigelContext *)ctx->rigel);
}
