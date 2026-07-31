/*
 * Real Raspberry Pi (BCM283x) System Timer driver.
 *
 * Matched via FDT compatible="brcm,bcm2835-system-timer" under /soc; the
 * register layout is the real, documented BCM283x peripheral -- the same
 * physical device arch/aarch64-native/kernel/platform_bcm2708.c programs
 * on bare hardware. Compare channel 3 is used, mirroring that driver's own
 * VBLANK_TIMER choice: channels 0 and 2 are reserved for VideoCore
 * firmware use on real Raspberry Pi boards.
 */
#include "../platform.h"

#include <aros/kernel.h>
#include <exec/types.h>
#include <hardware/intbits.h>

#include <kernel_base.h>
#include <kernel_intr.h>
#include <kernel_interrupts.h>
#include <proto/kernel.h>

#include "exec_platform.h"

#define SYSTIMER_CS   0x00
#define SYSTIMER_CLO  0x04
#define SYSTIMER_CHI  0x08
#define SYSTIMER_C0   0x0c

#define SYSTIMER_CHANNEL 3
#define SYSTIMER_IRQ      3    /* logical IRQ: bank 0 (GPU), bit 3 */

static ULONG systimer_base;
static ULONG systimer_interval_us;

/* Kept for arch/m68k-emu68/boot/selftest.c, which polls this to confirm
 * the platform timer is actually ticking before trusting timer.device. */
volatile ULONG emu68_vtimer_ticks = 0;

static inline volatile ULONG *systimer_reg(ULONG offset)
{
    return (volatile ULONG *)(systimer_base + offset);
}

static void systimer_heartbeat(void *unused, void *unused2)
{
    ULONG now;

    (void)unused;
    (void)unused2;

    *systimer_reg(SYSTIMER_CS) = 1UL << SYSTIMER_CHANNEL;
    emu68_vtimer_ticks++;

    now = *systimer_reg(SYSTIMER_CLO);
    *systimer_reg(SYSTIMER_C0 + SYSTIMER_CHANNEL * 4) = now + systimer_interval_us;

    if (SysBase && (IDNESTCOUNT_GET < 0))
        core_Cause(INTB_VERTB, 1L << INTB_VERTB);
}

static BOOL systimer_init(const struct PlatformNode *node)
{
    systimer_base = node->base;

    return KrnAddIRQHandler(SYSTIMER_IRQ, systimer_heartbeat, NULL, NULL) != NULL;
}

static void systimer_set_period(ULONG interval_us)
{
    systimer_interval_us = interval_us;
}

static void systimer_enable(void)
{
    ULONG now = *systimer_reg(SYSTIMER_CLO);

    emu68_vtimer_ticks = 0;
    *systimer_reg(SYSTIMER_C0 + SYSTIMER_CHANNEL * 4) = now + systimer_interval_us;
}

static void systimer_disable(void)
{
    /* Masking the IRQ at the controller (ictl_disable_irq) is enough to
     * stop delivery; the compare register can be left alone. */
}

const struct PlatformTimerOps bcm283x_system_timer_ops = {
    systimer_init,
    systimer_set_period,
    systimer_enable,
    systimer_disable,
};

const struct PlatformDriver bcm283x_system_timer_driver = {
    "brcm,bcm2835-system-timer",
    &bcm283x_system_timer_ops,
    NULL,
};
