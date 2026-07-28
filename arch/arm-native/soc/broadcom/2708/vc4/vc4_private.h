/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef VC4_PRIVATE_H
#define VC4_PRIVATE_H

#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/semaphores.h>
#include <inttypes.h>

struct VC4BO
{
    struct Node bo_Node;
    uint32_t bo_Handle;
    uint32_t bo_FirmwareHandle;
    uint32_t bo_BusAddress;
    uint32_t bo_Size;
    uint32_t bo_LogicalSize;
    uint32_t bo_Flags;
    APTR bo_CPUAddress;
};

struct VC4Base
{
    struct Node vc4_Node;
    uintptr_t vc4_periiobase;
    APTR vc4_MBoxBase;
    struct SignalSemaphore vc4_Lock;
    int vc4_QPUState;
    struct List vc4_BOs;
    uint32_t vc4_NextHandle;
};

struct VC4BO *vc4_find_bo(struct VC4Base *VC4Base, uint32_t handle);

#define ARM_PERIIOBASE VC4Base->vc4_periiobase
#include <hardware/bcm2708.h>

#define V3D_IDENT0 (V3D_BASE + 0x000)
#define V3D_IDENT1 (V3D_BASE + 0x004)
#define V3D_IDENT2 (V3D_BASE + 0x008)

#endif
