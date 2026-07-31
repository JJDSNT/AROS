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

/*
 * Bring-up tracing.
 *
 * Writes a byte at a time to Emu68's 0xdeadbeef host debug channel, the
 * same one arch/m68k-emu68/boot/console.c uses. Kept self-contained here so
 * the platform layer can trace during early boot without depending on
 * anything from boot/ having been linked or initialised yet.
 *
 * Left enabled while interrupt delivery is still unresolved -- see the
 * "Interrupt delivery" section of this directory's README. Set to 0 to
 * silence it; the call sites stay in place.
 */
#define PLATFORM_TRACE_BRINGUP 1

#if PLATFORM_TRACE_BRINGUP

static inline void platform_trace(const char *text)
{
    while (*text)
        *(volatile UBYTE *)0xdeadbeef = (UBYTE)*text++;
}

static inline void platform_trace_hex(ULONG value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    platform_trace("0x");
    for (shift = 28; shift >= 0; shift -= 4)
        *(volatile UBYTE *)0xdeadbeef = (UBYTE)digits[(value >> shift) & 0xf];
}

static inline void platform_trace_val(const char *label, ULONG value)
{
    platform_trace(label);
    platform_trace_hex(value);
    platform_trace("\n");
}

#else

static inline void platform_trace(const char *text) { (void)text; }
static inline void platform_trace_hex(ULONG value) { (void)value; }
static inline void platform_trace_val(const char *label, ULONG value)
{
    (void)label;
    (void)value;
}

#endif /* PLATFORM_TRACE_BRINGUP */

/* Guest-physical address of Emu68's INT_shadow, or 0 if it could not be
 * identified. See emu68_bridge.c -- that file is the only thing in this port
 * that knows about Emu68's internals, and is a proof of concept. */
ULONG emu68_find_int_shadow(void);

/* Discover the real platform timer and interrupt controller under /soc in
 * `fdt`, wire the level-6 autovector, and start the timer ticking at
 * `interval_us`. Returns FALSE if either device is missing/unrecognised. */
BOOL platform_timer_start(const void *fdt, ULONG interval_us);

#endif /* PLATFORM_H */
