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

#define M68KEMU_RIGEL_CHIP_RAM_SIZE M68KEMU_CHIP_RAM_SIZE

/* Contained default autovector handlers, in the unused user-vector area
   after the LINEA RTE and Supervisor() trampoline (0x100-0x10B). */
#define M68KEMU_RIGEL_IRQ_STUBS     0x000110UL
#define M68KEMU_RIGEL_IRQ_STUB_SIZE 10UL
#define M68KEMU_AUTOVECTOR(level)   (0x60UL + 4UL * (level))
#define M68KEMU_CUSTOM_INTREQ       0x00dff09cUL

/* INTREQ bits that Paula maps to IPL levels 1-6 */
static const UWORD rigel_level_intreq[7] = {
    0x0000,
    0x0007, /* 1: TBE, DSKBLK, SOFT */
    0x0008, /* 2: PORTS */
    0x0070, /* 3: COPER, VERTB, BLIT */
    0x0780, /* 4: AUD0-3 */
    0x1800, /* 5: RBF, DSKSYNC */
    0x2000  /* 6: EXTER */
};

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

    /* DMA clients need MEMF_CHIP to land inside Rigel's Chip RAM window */
    if (!M68KEmu_HeapSplitChip(ctx, M68KEMU_RIGEL_CHIP_RAM_SIZE))
        return FALSE;

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

/*
 * Give every unset level 1-6 autovector a handler that acknowledges that
 * level's INTREQ bits and returns, so an enabled interrupt without a 68k
 * handler is dropped instead of jumping to address 0. Level 7 (NMI) just
 * returns. Vectors the program installs later replace these.
 */
void M68KEmu_RigelInstallVectors(struct M68KEmuContext *ctx, ULONG rte_addr)
{
    ULONG level;

    if (!ctx || !ctx->rigel)
        return;

    for (level = 1; level <= 6; level++)
    {
        ULONG stub = M68KEMU_RIGEL_IRQ_STUBS +
                     (level - 1) * M68KEMU_RIGEL_IRQ_STUB_SIZE;

        m68k_write16(ctx, stub,     0x33fc);   /* MOVE.W #mask,(abs).L */
        m68k_write16(ctx, stub + 2, rigel_level_intreq[level]);
        m68k_write32(ctx, stub + 4, M68KEMU_CUSTOM_INTREQ);
        m68k_write16(ctx, stub + 8, 0x4e73);   /* RTE */

        if (!m68k_read32(ctx, M68KEMU_AUTOVECTOR(level)))
            m68k_write32(ctx, M68KEMU_AUTOVECTOR(level), stub);
    }
    if (!m68k_read32(ctx, M68KEMU_AUTOVECTOR(7)))
        m68k_write32(ctx, M68KEMU_AUTOVECTOR(7), rte_addr);
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
