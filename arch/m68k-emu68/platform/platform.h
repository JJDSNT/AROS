/*
 * Port-common platform abstraction for arch/m68k-emu68.
 *
 * Emu68 hands AROS the real board FDT (System Timer, legacy BCM interrupt
 * controller, etc. all present with their real "compatible" strings and
 * MMIO already mapped and guest-accessible -- see the boot documentation
 * in boot.c). This layer discovers real hardware drivers by matching
 * "compatible" under /soc, the same way arch/aarch64-native's
 * platform_bcm2708.c does on bare ARM -- the only difference here is that
 * dispatch arrives over the m68k level-6 autovector (Emu68's fixed
 * "EXTER" channel for any real physical IRQ) instead of an ARM64 vector
 * table entry, so the driver underneath has to be found and armed at
 * runtime rather than being link-time fixed.
 *
 * Nothing outside this directory needs to know which SoC is actually
 * present.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <exec/types.h>
#include <inttypes.h>

#include "fdt.h"

struct KernelBase;

/* A discovered device's MMIO window, already translated into a real,
 * guest-accessible address (see platform.c:soc_translate()). */
struct PlatformNode
{
    ULONG base;
    ULONG size;
};

/*
 * Field names deliberately avoid Enable/Disable: exec.library defines those
 * as zero-argument inline macros (Enable()/Disable(), supervisor interrupt
 * control), and the preprocessor mangles any struct member access spelled
 * that way before the compiler ever sees it.
 */
struct PlatformTimerOps
{
    BOOL (*Init)(const struct PlatformNode *node);
    void (*SetPeriod)(ULONG interval_us);
    void (*Start)(void);
    void (*Stop)(void);
};

struct PlatformIntcOps
{
    BOOL (*Init)(const struct PlatformNode *node);

    /* Unmask/mask one of *this* controller's own logical IRQ numbers
     * (see bcm283x/interrupt_controller.h) -- called from ictl_enable_irq()
     * / ictl_disable_irq(), same as arm-native/aarch64-native. */
    void (*EnableIRQ)(ULONG irq);
    void (*DisableIRQ)(ULONG irq);

    /* Called from the level-6 autovector trampoline: decode which real
     * source(s) are pending and run their handlers via
     * krnRunIRQHandlers(). */
    void (*Dispatch)(struct KernelBase *KernelBase);
};

struct PlatformDriver
{
    const char *compatible;
    const struct PlatformTimerOps *timer_ops; /* NULL for an intc driver */
    const struct PlatformIntcOps  *intc_ops;  /* NULL for a timer driver */
};

/* Discover the real platform timer and interrupt controller under /soc in
 * `fdt`, wire the level-6 autovector, and start the timer ticking at
 * `interval_us`. Returns FALSE if either device is missing/unrecognised. */
BOOL platform_timer_start(const void *fdt, ULONG interval_us);

#endif /* PLATFORM_H */
