/*
 * Low-level debug output for native Emu68.
 */

#include <aros/kernel.h>

#include <kernel_base.h>
#include <kernel_debug.h>

extern int emu68_console_putc(int chr);

int krnPutC(int chr, struct KernelBase *KernelBase)
{
    (void)KernelBase;
    return emu68_console_putc(chr);
}

