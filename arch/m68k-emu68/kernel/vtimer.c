/*
 * Emu68 virtual platform timer.
 *
 * Emu68 exposes a generic MMIO timer and delivers it through the standard
 * m68k level-5 autovector.  AROS translates that platform interrupt into
 * Exec's vertical-blank heartbeat; no Amiga chipset registers are involved.
 */

#include <aros/kernel.h>
#include <exec/types.h>
#include <hardware/intbits.h>

#include "cpu_m68k.h"
#include <kernel_base.h>
#include <proto/kernel.h>
#include <kernel_intr.h>
#include <kernel_interrupts.h>

#include "exec_platform.h"

#define EMU68_VTIMER_BASE        0xf0000000UL
#define EMU68_VTIMER_ID          0x45365431UL
#define EMU68_VTIMER_INTERVAL    0x14
#define EMU68_VTIMER_CONTROL     0x18
#define EMU68_VTIMER_ACK         0x20
#define EMU68_VTIMER_ENABLE      0x01
#define EMU68_VTIMER_PERIODIC    0x02
#define EMU68_VTIMER_VECTOR      29
#define EMU68_IRQ_VTIMER         0

volatile ULONG emu68_vtimer_ticks = 0;
static APTR vtimer_irq_handle;

static inline volatile ULONG *vtimer_reg(ULONG offset)
{
    return (volatile ULONG *)(EMU68_VTIMER_BASE + offset);
}

static void emu68_vtimer_heartbeat(void *unused, void *unused2)
{
    (void)unused;
    (void)unused2;

    *vtimer_reg(EMU68_VTIMER_ACK) = 1;
    emu68_vtimer_ticks++;

    if (SysBase && (IDNESTCOUNT_GET < 0))
        core_Cause(INTB_VERTB, 1L << INTB_VERTB);
}

BOOL Emu68_VTimer_Level5(void)
{
    krnRunIRQHandlers(KernelBase, EMU68_IRQ_VTIMER);
    return TRUE;
}

void Emu68_VTimer_Level5_Direct(void);
asm (
    "   .global Emu68_VTimer_Level5_Direct\n"
    "   .type Emu68_VTimer_Level5_Direct,@function\n"
    "Emu68_VTimer_Level5_Direct:\n"
    "   movem.l %d0/%d1/%a0/%a1/%a5/%a6,%sp@-\n"
    "   jsr     Emu68_VTimer_Level5\n"
    "   tst.w   %d0\n"
    "   beq     0f\n"
    "   jmp     Exec_6_ExitIntr\n"
    "0:\n"
    "   movem.l %sp@+,%d0/%d1/%a0/%a1/%a5/%a6\n"
    "   rte\n"
);

BOOL emu68_vtimer_start(ULONG interval_us)
{
    volatile APTR *vectors = (volatile APTR *)0;

    if (*vtimer_reg(0) != EMU68_VTIMER_ID)
        return FALSE;

    if (!vtimer_irq_handle)
        vtimer_irq_handle =
            KrnAddIRQHandler(EMU68_IRQ_VTIMER, emu68_vtimer_heartbeat,
                             NULL, NULL);
    if (!vtimer_irq_handle)
        return FALSE;

    emu68_vtimer_ticks = 0;
    vectors[EMU68_VTIMER_VECTOR] = Emu68_VTimer_Level5_Direct;
    *vtimer_reg(EMU68_VTIMER_INTERVAL) = interval_us;
    *vtimer_reg(EMU68_VTIMER_CONTROL) =
        EMU68_VTIMER_ENABLE | EMU68_VTIMER_PERIODIC;

    return TRUE;
}
