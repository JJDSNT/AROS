/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#define DEBUG 0

#include <aros/debug.h>
#include <aros/libcall.h>
#include <aros/macros.h>
#include <proto/exec.h>
#include <proto/mbox.h>
#include <proto/vc4.h>

#include <hardware/videocore.h>

#include "vc4_private.h"

#undef MBoxBase
#define MBoxBase VC4Base->vc4_MBoxBase

static BOOL vc4_property_call(struct VC4Base *VC4Base, ULONG *msg)
{
    return MBoxCall((APTR)VCMB_BASE, VCMB_PROPCHAN, msg) == msg &&
        (AROS_LE2LONG(msg[1]) & VCTAG_RESP) != 0 &&
        (AROS_LE2LONG(msg[4]) & VCTAG_RESP) != 0;
}

static uint32_t vc4_firmware_alloc(struct VC4Base *VC4Base,
    uint32_t size, uint32_t alignment, uint32_t flags)
{
    ULONG raw[9 + 4];
    ULONG *msg = (ULONG *)(((IPTR)raw + 15) & ~(IPTR)15);

    msg[0] = AROS_LONG2LE(9 * sizeof(*msg));
    msg[1] = AROS_LONG2LE(VCTAG_REQ);
    msg[2] = AROS_LONG2LE(VCTAG_ALLOCMEM);
    msg[3] = AROS_LONG2LE(3 * sizeof(ULONG));
    msg[4] = AROS_LONG2LE(3 * sizeof(ULONG));
    msg[5] = AROS_LONG2LE(size);
    msg[6] = AROS_LONG2LE(alignment);
    msg[7] = AROS_LONG2LE(flags);
    msg[8] = 0;

    if (!vc4_property_call(VC4Base, msg))
        return 0;

    return AROS_LE2LONG(msg[5]);
}

static BOOL vc4_firmware_word(struct VC4Base *VC4Base,
    uint32_t tag, uint32_t input, uint32_t *output)
{
    ULONG raw[7 + 4];
    ULONG *msg = (ULONG *)(((IPTR)raw + 15) & ~(IPTR)15);

    msg[0] = AROS_LONG2LE(7 * sizeof(*msg));
    msg[1] = AROS_LONG2LE(VCTAG_REQ);
    msg[2] = AROS_LONG2LE(tag);
    msg[3] = AROS_LONG2LE(sizeof(ULONG));
    msg[4] = AROS_LONG2LE(sizeof(ULONG));
    msg[5] = AROS_LONG2LE(input);
    msg[6] = 0;

    if (!output || !vc4_property_call(VC4Base, msg))
        return FALSE;

    *output = AROS_LE2LONG(msg[5]);
    return TRUE;
}

struct VC4BO *vc4_find_bo(struct VC4Base *VC4Base, uint32_t handle)
{
    struct VC4BO *bo;

    for (bo = (struct VC4BO *)VC4Base->vc4_BOs.lh_Head;
         bo->bo_Node.ln_Succ;
         bo = (struct VC4BO *)bo->bo_Node.ln_Succ)
    {
        if (bo->bo_Handle == handle)
            return bo;
    }

    return NULL;
}

AROS_LH4(int, VC4CreateBO,
    AROS_LHA(unsigned int, size, D0),
    AROS_LHA(unsigned int, alignment, D1),
    AROS_LHA(unsigned int, flags, D2),
    AROS_LHA(unsigned int *, handle, A0),
    struct VC4Base *, VC4Base, 2, Vc4)
{
    AROS_LIBFUNC_INIT

    struct VC4BO *bo;
    uint32_t firmware_handle;
    uint32_t bus_address;

    if (!handle || size == 0 ||
        (flags & ~(VC4_BOF_NOINIT | VC4_BOF_SHADER)) != 0)
        return -1;

    if (alignment == 0)
        alignment = 4096;
    if ((alignment & (alignment - 1)) != 0)
        return -1;

    if (size > UINT32_MAX - 4095)
        return -1;
    size = (size + 4095) & ~4095U;

    bo = AllocMem(sizeof(*bo), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bo)
        return -1;

    ObtainSemaphore(&VC4Base->vc4_Lock);

    firmware_handle = vc4_firmware_alloc(VC4Base, size, alignment,
        VCMEM_DIRECT |
        ((flags & VC4_BOF_NOINIT) ? VCMEM_NOINIT : VCMEM_ZERO));
    if (!firmware_handle)
        goto fail;

    if (!vc4_firmware_word(VC4Base, VCTAG_LOCKMEM, firmware_handle,
        &bus_address) || (bus_address & 0xc0000000U) == 0)
    {
        uint32_t ignored;
        vc4_firmware_word(VC4Base, VCTAG_FREEMEM, firmware_handle,
            &ignored);
        goto fail;
    }

    bo->bo_Handle = VC4Base->vc4_NextHandle++;
    if (VC4Base->vc4_NextHandle == 0)
        VC4Base->vc4_NextHandle = 1;
    bo->bo_FirmwareHandle = firmware_handle;
    bo->bo_BusAddress = bus_address;
    bo->bo_Size = size;
    bo->bo_Flags = flags;
    bo->bo_CPUAddress = (APTR)(IPTR)(bus_address & 0x3fffffffU);
    AddTail(&VC4Base->vc4_BOs, &bo->bo_Node);
    *handle = bo->bo_Handle;

    ReleaseSemaphore(&VC4Base->vc4_Lock);
    return 0;

fail:
    ReleaseSemaphore(&VC4Base->vc4_Lock);
    FreeMem(bo, sizeof(*bo));
    return -1;

    AROS_LIBFUNC_EXIT
}

AROS_LH4(int, VC4MapBO,
    AROS_LHA(unsigned int, handle, D0),
    AROS_LHA(void **, cpu_address, A0),
    AROS_LHA(unsigned int *, bus_address, A1),
    AROS_LHA(unsigned int *, size, A2),
    struct VC4Base *, VC4Base, 3, Vc4)
{
    AROS_LIBFUNC_INIT

    struct VC4BO *bo;

    if (!cpu_address || !bus_address || !size)
        return -1;

    ObtainSemaphoreShared(&VC4Base->vc4_Lock);
    bo = vc4_find_bo(VC4Base, handle);
    if (!bo)
    {
        ReleaseSemaphore(&VC4Base->vc4_Lock);
        return -1;
    }

    *cpu_address = bo->bo_CPUAddress;
    *bus_address = bo->bo_BusAddress;
    *size = bo->bo_Size;
    ReleaseSemaphore(&VC4Base->vc4_Lock);
    return 0;

    AROS_LIBFUNC_EXIT
}

AROS_LH1(int, VC4FreeBO,
    AROS_LHA(unsigned int, handle, D0),
    struct VC4Base *, VC4Base, 4, Vc4)
{
    AROS_LIBFUNC_INIT

    struct VC4BO *bo;
    uint32_t status;

    ObtainSemaphore(&VC4Base->vc4_Lock);
    bo = vc4_find_bo(VC4Base, handle);
    if (!bo)
    {
        ReleaseSemaphore(&VC4Base->vc4_Lock);
        return -1;
    }

    if (!vc4_firmware_word(VC4Base, VCTAG_UNLOCKMEM,
        bo->bo_FirmwareHandle, &status) || status != 0)
    {
        ReleaseSemaphore(&VC4Base->vc4_Lock);
        return -1;
    }

    if (!vc4_firmware_word(VC4Base, VCTAG_FREEMEM,
        bo->bo_FirmwareHandle, &status) || status != 0)
    {
        ReleaseSemaphore(&VC4Base->vc4_Lock);
        return -1;
    }

    Remove(&bo->bo_Node);
    ReleaseSemaphore(&VC4Base->vc4_Lock);
    FreeMem(bo, sizeof(*bo));
    return 0;

    AROS_LIBFUNC_EXIT
}

AROS_LH4(int, VC4SyncBO,
    AROS_LHA(unsigned int, handle, D0),
    AROS_LHA(unsigned int, offset, D1),
    AROS_LHA(unsigned int, length, D2),
    AROS_LHA(unsigned int, direction, D3),
    struct VC4Base *, VC4Base, 5, Vc4)
{
    AROS_LIBFUNC_INIT

    struct VC4BO *bo;
    APTR address;
    ULONG cache_flags;

    if (direction == VC4_SYNC_CPU_TO_GPU)
        cache_flags = CACRF_ClearD;
    else if (direction == VC4_SYNC_GPU_TO_CPU)
        cache_flags = CACRF_InvalidateD;
    else
        return -1;

    ObtainSemaphoreShared(&VC4Base->vc4_Lock);
    bo = vc4_find_bo(VC4Base, handle);
    if (!bo || offset > bo->bo_Size || length > bo->bo_Size - offset)
    {
        ReleaseSemaphore(&VC4Base->vc4_Lock);
        return -1;
    }

    address = (APTR)((UBYTE *)bo->bo_CPUAddress + offset);
    CacheClearE(address, length, cache_flags);
    ReleaseSemaphore(&VC4Base->vc4_Lock);
    return 0;

    AROS_LIBFUNC_EXIT
}
