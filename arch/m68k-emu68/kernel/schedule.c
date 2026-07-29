/*
 * Observable scheduler entry for the native Emu68 target.
 */

#include <aros/kernel.h>

#include <kernel_base.h>
#include <kernel_syscall.h>

#include <proto/kernel.h>

extern void emu68_console_puts(const char *text);

AROS_LH0(void, KrnSchedule,
    struct KernelBase *, KernelBase, 6, Kernel)
{
    AROS_LIBFUNC_INIT

    emu68_console_puts("[AROS/Emu68] entering Exec scheduler\n");
    Supervisor(__AROS_GETVECADDR(SysBase, 7));

    AROS_LIBFUNC_EXIT
}
