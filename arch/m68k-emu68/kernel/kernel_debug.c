/*
 * Low-level debug output for native Emu68.
 */

#include <aros/kernel.h>
#include <aros/symbolsets.h>

#include <kernel_base.h>
#include <kernel_debug.h>

extern int emu68_console_putc(int chr);
extern void emu68_console_puts(const char *text);

int krnPutC(int chr, struct KernelBase *KernelBase)
{
    (void)KernelBase;
    return emu68_console_putc(chr);
}

/*
 * Bracket the generic m68k kernel initialization hooks.  These messages are
 * also useful after bring-up: they identify the exact point at which the
 * target-specific kernel services become usable, without relying on Emu68's
 * debugger.
 */
static int emu68_kernel_init_begin(struct KernelBase *KernelBase)
{
    (void)KernelBase;
    emu68_console_puts("[AROS/Emu68] kernel init hooks begin\n");
    return TRUE;
}

static int emu68_kernel_init_end(struct KernelBase *KernelBase)
{
    (void)KernelBase;
    emu68_console_puts("[AROS/Emu68] kernel init hooks complete\n");
    return TRUE;
}

ADD2INITLIB(emu68_kernel_init_begin, 127)
ADD2INITLIB(emu68_kernel_init_end, -127)
