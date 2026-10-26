/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    m68kemu.library — contained Exec interrupt API.

    AddIntServer(), RemIntServer() and SetIntVector() keep their nodes in
    the context. With Rigel, each level 1-6 autovector runs a small 68k
    dispatcher that asks IntNext which server or handler to call, calls it
    with Exec's register conventions, and reports the result to IntDone:

    loop:   moveq   #level,d0
            jsr     IntNext(a6)     ; D0 = 0: nothing left
            tst.l   d0
            beq.s   done
            jsr     (a5)            ; server or handler
            move.w  sr,d1           ; Z: chain continues
            moveq   #level,d0
            jsr     IntDone(a6)
            bra.s   loop
    done:   rte
*/

#include <exec/types.h>
#include <string.h>

#include "m68kemu_intern.h"
#include "m68kemu_offsets.h"
#include "m68kemu_thunks.h"

#define CUSTOM_INTENA       0x09a
#define CUSTOM_INTREQ       0x09c
#define INTF_SETCLR_BIT     0x8000
#define INTF_INTEN_BIT      0x4000
#define INT_VERTB           5

/* Interrupts that Exec runs as server chains: PORTS, COPER, VERTB,
   EXTER, NMI. The others take one SetIntVector() handler. */
#define INT_SERVER_MASK     0xA038

#define AUTOVECTOR(level)   (0x60UL + 4UL * (level))
#define SR_Z                0x0004

/* INTREQ bits Paula maps to each IPL level */
static const UWORD int_level_mask[8] = {
    0x0000,
    0x0007, /* 1: TBE, DSKBLK, SOFTINT */
    0x0008, /* 2: PORTS */
    0x0070, /* 3: COPER, VERTB, BLIT */
    0x0780, /* 4: AUD0-3 */
    0x1800, /* 5: RBF, DSKSYNC */
    0x2000, /* 6: EXTER */
    0x0000
};

static void custom_write(struct M68KEmuContext *ctx, ULONG reg, UWORD value)
{
#ifdef M68KEMU_WITH_RIGEL
    if (ctx->rigel)
        (void)M68KEmu_RigelWrite(ctx, CUSTOM_BASE + reg, 2, value);
#else
    (void)ctx; (void)reg; (void)value;
#endif
}

static void int_state(struct M68KEmuContext *ctx, UWORD *intena, UWORD *intreq)
{
#ifdef M68KEMU_WITH_RIGEL
    M68KEmu_RigelIntState(ctx, intena, intreq);
#else
    (void)ctx;
    *intena = *intreq = 0;
#endif
}

/* ── Exec API ──────────────────────────────────────────────────────── */

void M68KEmu_AddIntServer(struct M68KEmuContext *ctx, ULONG intNum, ULONG irq)
{
    struct M68KIntState *s = &ctx->ints;
    UBYTE n, pos, i;
    BYTE pri;

    if (intNum >= M68KEMU_INT_COUNT || !irq || irq + M68K_IS_CODE + 4 > ctx->mem_size)
        return;
    n = s->num_servers[intNum];
    for (i = 0; i < n; i++)
        if (s->servers[intNum][i] == irq)
            return;
    if (n >= M68KEMU_INT_MAX_SERVERS)
        return;

    /* Highest ln_Pri first, FIFO among equals, as Enqueue() does */
    pri = (BYTE)ctx->mem[irq + M68K_LN_PRI];
    for (pos = 0; pos < n; pos++)
        if ((BYTE)ctx->mem[s->servers[intNum][pos] + M68K_LN_PRI] < pri)
            break;
    memmove(&s->servers[intNum][pos + 1], &s->servers[intNum][pos],
            (n - pos) * sizeof(ULONG));
    s->servers[intNum][pos] = irq;
    s->num_servers[intNum] = n + 1;

    /* Keep a chain that is being dispatched on its current node */
    for (i = 1; i < 7; i++)
        if (s->level[i].active == intNum + 1 && pos < s->level[i].next_server)
            s->level[i].next_server++;

    custom_write(ctx, CUSTOM_INTENA, INTF_SETCLR_BIT | (1 << intNum));
}

void M68KEmu_RemIntServer(struct M68KEmuContext *ctx, ULONG intNum, ULONG irq)
{
    struct M68KIntState *s = &ctx->ints;
    UBYTE n, pos, i;

    if (intNum >= M68KEMU_INT_COUNT)
        return;
    n = s->num_servers[intNum];
    for (pos = 0; pos < n; pos++)
        if (s->servers[intNum][pos] == irq)
            break;
    if (pos == n)
        return;

    memmove(&s->servers[intNum][pos], &s->servers[intNum][pos + 1],
            (n - pos - 1) * sizeof(ULONG));
    s->num_servers[intNum] = n - 1;

    for (i = 1; i < 7; i++)
        if (s->level[i].active == intNum + 1 && pos < s->level[i].next_server)
            s->level[i].next_server--;

    if (n == 1)
        custom_write(ctx, CUSTOM_INTENA, 1 << intNum);
}

ULONG M68KEmu_SetIntVector(struct M68KEmuContext *ctx, ULONG intNum, ULONG irq)
{
    ULONG old;

    if (intNum >= M68KEMU_INT_COUNT)
        return 0;
    old = ctx->ints.vectors[intNum];
    ctx->ints.vectors[intNum] = irq;
    return old;
}

/*
 * Disable()/Enable() nest on the contained INTENA master bit. They return
 * FALSE without Rigel, where the caller keeps the native behavior.
 */
BOOL M68KEmu_Disable(struct M68KEmuContext *ctx)
{
#ifdef M68KEMU_WITH_RIGEL
    if (ctx->rigel)
    {
        if (ctx->ints.disable_depth++ == 0)
            custom_write(ctx, CUSTOM_INTENA, INTF_INTEN_BIT);
        return TRUE;
    }
#endif
    (void)ctx;
    return FALSE;
}

BOOL M68KEmu_Enable(struct M68KEmuContext *ctx)
{
#ifdef M68KEMU_WITH_RIGEL
    if (ctx->rigel)
    {
        if (ctx->ints.disable_depth && --ctx->ints.disable_depth == 0)
            custom_write(ctx, CUSTOM_INTENA, INTF_SETCLR_BIT | INTF_INTEN_BIT);
        return TRUE;
    }
#endif
    (void)ctx;
    return FALSE;
}

/* ── Dispatcher thunks ─────────────────────────────────────────────── */

static BOOL int_call(struct M68KEmuContext *ctx, void *cpu, ULONG irq, ULONG d1)
{
    ULONG code = m68k_read32(ctx, irq + M68K_IS_CODE);

    if (!code)
        return FALSE;
    M68KEmu_SetA(cpu, 0, CUSTOM_BASE);
    M68KEmu_SetA(cpu, 1, m68k_read32(ctx, irq + M68K_IS_DATA));
    M68KEmu_SetA(cpu, 5, code);
    M68KEmu_SetA(cpu, 6, ctx->m68k_sysbase);
    M68KEmu_SetD(cpu, 1, d1);
    return TRUE;
}

/* -6: IntNext(D0=level) -> D0 = 1 with A0/A1/A5/A6/D1 set up, 0 when done */
static IPTR int_next(struct M68KEmuContext *ctx, void *cpu)
{
    struct M68KIntState *s = &ctx->ints;
    struct M68KIntLevelState *st = &s->level[THUNK_D(0) & 7];
    UWORD mask = int_level_mask[THUNK_D(0) & 7];
    int guard;

    for (guard = 0; guard < 64; guard++)
    {
        if (!st->active)
        {
            UWORD intena, intreq, pending;
            UBYTE b;

            int_state(ctx, &intena, &intreq);
            if (!(intena & INTF_INTEN_BIT))
                return 0;
            pending = intena & intreq & mask;
            if (!pending)
                return 0;
            for (b = 0; !(pending & (1 << b)); b++)
                ;

            if (INT_SERVER_MASK & (1 << b))
            {
                /* Acknowledge first so a new request during the chain
                   is not lost */
                custom_write(ctx, CUSTOM_INTREQ, 1 << b);
                st->active = b + 1;
                st->next_server = 0;
            }
            else if (s->vectors[b] &&
                     int_call(ctx, cpu, s->vectors[b], intena & intreq))
            {
                /* The handler acknowledges INTREQ itself */
                st->active = b + 1;
                return 1;
            }
            else
            {
                custom_write(ctx, CUSTOM_INTREQ, 1 << b);
                continue;
            }
        }

        while (st->next_server < s->num_servers[st->active - 1])
        {
            ULONG irq = s->servers[st->active - 1][st->next_server++];
            if (int_call(ctx, cpu, irq, 0))
                return 1;
        }
        st->active = 0;
    }
    return 0;
}

/* -12: IntDone(D0=level, D1=SR after the call) */
static IPTR int_done(struct M68KEmuContext *ctx, void *cpu)
{
    struct M68KIntState *s = &ctx->ints;
    struct M68KIntLevelState *st = &s->level[THUNK_D(0) & 7];

    UBYTE b;

    if (!st->active)
        return 0;
    b = st->active - 1;
    if (!(INT_SERVER_MASK & (1 << b)))
        st->active = 0;
    else if (b != INT_VERTB && !(THUNK_D(1) & SR_Z))
        st->next_server = s->num_servers[b];
    return 0;
}

static const struct M68KThunkEntry int_thunks[] = {
    { 6,  int_next },
    { 12, int_done },
    { 0, NULL }
};

/*
 * Set up the dispatchers and point every unset level 1-6 autovector at
 * them; level 7 (NMI) just returns. Programs that write an autovector
 * themselves replace the dispatcher for that level.
 */
BOOL M68KEmu_IntInstall(struct M68KEmuContext *ctx, ULONG rte_addr)
{
    ULONG level, base;

    if (!ctx->ints.dispatch_base)
    {
        ctx->ints.dispatch_base = M68KEmu_SetupLibBase(ctx, ctx->num_libs, NULL,
                                                       2, int_thunks, 2, NULL, 0);
        if (!ctx->ints.dispatch_base)
            return FALSE;
    }
    base = ctx->ints.dispatch_base;

    for (level = 1; level <= 6; level++)
    {
        ULONG p = M68KEMU_INT_STUBS + (level - 1) * M68KEMU_INT_STUB_SIZE;
        UWORD moveq = (UWORD)(0x7000 | level);

        m68k_write32(ctx, p +  0, 0x48E7C0C6); /* movem.l d0-d1/a0-a1/a5-a6,-(sp) */
        m68k_write16(ctx, p +  4, 0x2C7C);     /* movea.l #base,a6 */
        m68k_write32(ctx, p +  6, base);
        m68k_write16(ctx, p + 10, moveq);      /* loop: moveq #level,d0 */
        m68k_write32(ctx, p + 12, 0x4EAEFFFA); /* jsr -6(a6)  IntNext */
        m68k_write16(ctx, p + 16, 0x4A80);     /* tst.l d0 */
        m68k_write16(ctx, p + 18, 0x6712);     /* beq.s done */
        m68k_write16(ctx, p + 20, 0x4E95);     /* jsr (a5) */
        m68k_write16(ctx, p + 22, 0x40C1);     /* move.w sr,d1 */
        m68k_write16(ctx, p + 24, 0x2C7C);     /* movea.l #base,a6 */
        m68k_write32(ctx, p + 26, base);
        m68k_write16(ctx, p + 30, moveq);      /* moveq #level,d0 */
        m68k_write32(ctx, p + 32, 0x4EAEFFF4); /* jsr -12(a6) IntDone */
        m68k_write16(ctx, p + 36, 0x60E4);     /* bra.s loop */
        m68k_write32(ctx, p + 38, 0x4CDF6303); /* done: movem.l (sp)+,... */
        m68k_write16(ctx, p + 42, 0x4E73);     /* rte */

        if (!m68k_read32(ctx, AUTOVECTOR(level)))
            m68k_write32(ctx, AUTOVECTOR(level), p);
    }
    if (!m68k_read32(ctx, AUTOVECTOR(7)))
        m68k_write32(ctx, AUTOVECTOR(7), rte_addr);

    /* A running AmigaOS has the interrupt master enable set */
    if (!ctx->ints.disable_depth)
        custom_write(ctx, CUSTOM_INTENA, INTF_SETCLR_BIT | INTF_INTEN_BIT);
    return TRUE;
}
