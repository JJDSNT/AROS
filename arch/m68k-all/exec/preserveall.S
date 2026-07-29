/*
 * m68k Exec ABI wrappers for calls documented to preserve scratch registers.
 *
 * The C implementations use the normal AROS register-clobber convention.
 * Classic m68k callers, however, rely on these Exec vectors preserving
 * D0-D1/A0-A1.
 */

#include "aros/m68k/asm.h"

#undef ObtainSemaphore
#undef ReleaseSemaphore

        .text
        .balign 4

#define PRESERVE_ALL_WRAPPER(wrapper, function, vector) \
        .globl wrapper;                                 \
wrapper:                                                \
        movem.l %d0-%d1/%a0-%a1,%sp@-;                 \
        jsr AROS_SLIB_ENTRY(function,Exec,vector);       \
        movem.l %sp@+,%d0-%d1/%a0-%a1;                 \
        rts

PRESERVE_ALL_WRAPPER(m68k_Exec_Permit, Permit, 23)
PRESERVE_ALL_WRAPPER(m68k_Exec_ObtainSemaphore, ObtainSemaphore, 94)
PRESERVE_ALL_WRAPPER(m68k_Exec_ReleaseSemaphore, ReleaseSemaphore, 95)
PRESERVE_ALL_WRAPPER(m68k_Exec_ObtainSemaphoreShared, ObtainSemaphoreShared, 113)
