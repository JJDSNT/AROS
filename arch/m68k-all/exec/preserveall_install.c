/*
 * Install the register-preserving Exec vector wrappers required by the
 * classic m68k library ABI. Bootstraps call this after ExecBase is complete.
 */

#include <exec/execbase.h>
#include <defines/exec_LVO.h>

extern void m68k_Exec_Permit(void);
extern void m68k_Exec_ObtainSemaphore(void);
extern void m68k_Exec_ReleaseSemaphore(void);
extern void m68k_Exec_ObtainSemaphoreShared(void);

void m68k_ExecInstallPreserveAll(struct ExecBase *SysBase)
{
    __AROS_SETVECADDR(SysBase, LVOPermit, m68k_Exec_Permit);
    __AROS_SETVECADDR(SysBase, LVOObtainSemaphore,
                     m68k_Exec_ObtainSemaphore);
    __AROS_SETVECADDR(SysBase, LVOReleaseSemaphore,
                     m68k_Exec_ReleaseSemaphore);
    __AROS_SETVECADDR(SysBase, LVOObtainSemaphoreShared,
                     m68k_Exec_ObtainSemaphoreShared);
}
