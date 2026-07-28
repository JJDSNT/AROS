/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#define DEBUG 0

#include <aros/debug.h>
#include <aros/libcall.h>
#include <aros/macros.h>
#include <aros/symbolsets.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <proto/mbox.h>
#include <proto/vc4.h>

#include <hardware/videocore.h>

#include "vc4_private.h"

APTR KernelBase __attribute__((used)) = NULL;
APTR MBoxBase __attribute__((used)) = NULL;

static int vc4_init(struct VC4Base *VC4Base)
{
    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
        return FALSE;

    VC4Base->vc4_periiobase =
        (uintptr_t)KrnGetSystemAttr(KATTR_PeripheralBase);
    if (!VC4Base->vc4_periiobase)
        return FALSE;

    MBoxBase = VC4Base->vc4_MBoxBase = OpenResource("mbox.resource");
    if (!MBoxBase)
        return FALSE;

    InitSemaphore(&VC4Base->vc4_Lock);
    VC4Base->vc4_QPUState = 0;
    NewList(&VC4Base->vc4_BOs);
    VC4Base->vc4_NextHandle = 1;

    D(bug("[VC4] V3D MMIO base @ %p\n", (APTR)(uintptr_t)V3D_BASE));
    return TRUE;
}

ADD2INITLIB(vc4_init, 0)

static int vc4_enable_qpu(struct VC4Base *VC4Base)
{
    ULONG *raw;
    ULONG *msg;
    int enabled = FALSE;

    ObtainSemaphore(&VC4Base->vc4_Lock);

    if (VC4Base->vc4_QPUState != 0)
    {
        enabled = VC4Base->vc4_QPUState > 0;
        ReleaseSemaphore(&VC4Base->vc4_Lock);
        return enabled;
    }

    raw = AllocMem(7 * sizeof(*msg) + 15, MEMF_PUBLIC | MEMF_CLEAR);
    if (raw)
    {
        msg = (ULONG *)(((IPTR)raw + 15) & ~(IPTR)15);
        msg[0] = AROS_LONG2LE(7 * sizeof(*msg));
        msg[1] = AROS_LONG2LE(VCTAG_REQ);
        msg[2] = AROS_LONG2LE(VCTAG_ENABLEQPU);
        msg[3] = AROS_LONG2LE(sizeof(ULONG));
        msg[4] = AROS_LONG2LE(sizeof(ULONG));
        msg[5] = AROS_LONG2LE(1);
        msg[6] = 0;

        MBoxBase = VC4Base->vc4_MBoxBase;
        if (MBoxCall((APTR)VCMB_BASE, VCMB_PROPCHAN, msg) == msg &&
            (AROS_LE2LONG(msg[1]) & VCTAG_RESP) != 0 &&
            (AROS_LE2LONG(msg[4]) & VCTAG_RESP) != 0 &&
            AROS_LE2LONG(msg[5]) != 0)
            enabled = TRUE;

        FreeMem(raw, 7 * sizeof(*msg) + 15);
    }

    VC4Base->vc4_QPUState = enabled ? 1 : -1;
    ReleaseSemaphore(&VC4Base->vc4_Lock);

    D(bug("[VC4] firmware QPU enable %s\n",
        enabled ? "succeeded" : "failed"));
    return enabled;
}

static inline uint32_t vc4_read_reg(uintptr_t reg)
{
    /*
     * V3D MMIO registers are little-endian. AROS_LE2LONG is intentionally
     * kept even when it is a no-op on raspi-aarch64: this source directory
     * is shared with ARM targets, including possible big-endian builds.
     * The value returned to callers is always in native CPU byte order.
     */
    __asm__ __volatile__("dmb sy" ::: "memory");
    return AROS_LE2LONG(*(volatile uint32_t *)reg);
}

AROS_LH2(int, VC4GetParam,
    AROS_LHA(unsigned int, param, D0),
    AROS_LHA(uint64_t *, value, A0),
    struct VC4Base *, VC4Base, 1, Vc4)
{
    AROS_LIBFUNC_INIT

    if (!value)
        return -1;

    if (!vc4_enable_qpu(VC4Base))
        return -1;

    switch (param)
    {
        case VC4_PARAM_V3D_IDENT0:
            *value = vc4_read_reg(V3D_IDENT0);
            break;
        case VC4_PARAM_V3D_IDENT1:
            *value = vc4_read_reg(V3D_IDENT1);
            break;
        case VC4_PARAM_V3D_IDENT2:
            *value = vc4_read_reg(V3D_IDENT2);
            break;

        /*
         * Optional capabilities stay disabled until their complete resource
         * paths exist. A successful false result matches DRM GET_PARAM.
         */
        case VC4_PARAM_SUPPORTS_BRANCHES:
        case VC4_PARAM_SUPPORTS_ETC1:
        case VC4_PARAM_SUPPORTS_THREADED_FS:
        case VC4_PARAM_SUPPORTS_FIXED_RCL_ORDER:
        case VC4_PARAM_SUPPORTS_MADVISE:
        case VC4_PARAM_SUPPORTS_PERFMON:
            *value = 0;
            break;
        default:
            return -1;
    }

    return 0;

    AROS_LIBFUNC_EXIT
}
