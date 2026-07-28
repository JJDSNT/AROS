/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <proto/exec.h>
#include <proto/vc4.h>

#include "vc4_drm_compat.h"
#include "vc4gallium_intern.h"

_Static_assert(sizeof(struct vc4_drm_submit_rcl_surface) ==
    sizeof(struct VC4SubmitRCLSurface), "VC4 submit surface ABI mismatch");
_Static_assert(sizeof(struct vc4_drm_submit_cl) == sizeof(struct VC4SubmitCL),
    "VC4 submit ABI mismatch");

static int vc4_drm_create_bo(struct vc4_drm_create_bo *create)
{
    if (!create || create->flags != 0)
        return -1;

    return VC4CreateBO(create->size, 4096, 0, &create->handle);
}

static int
vc4_drm_create_shader_bo(struct vc4_drm_create_shader_bo *create)
{
    void *map;
    uint32_t bus_address;
    uint32_t allocated_size;
    int error;

    if (!create || !create->data || !create->size || create->flags != 0)
        return -1;

    error = VC4CreateBO(create->size, 4096, VC4_BOF_NOINIT,
        &create->handle);
    if (error)
        return error;

    error = VC4MapBO(create->handle, &map, &bus_address, &allocated_size);
    if (error)
        goto fail;

    CopyMem((const void *)(IPTR)create->data, map, create->size);
    error = VC4SyncBO(create->handle, 0, create->size,
        VC4_SYNC_CPU_TO_GPU);
    if (error)
        goto fail;

    return 0;

fail:
    VC4FreeBO(create->handle);
    create->handle = 0;
    return -1;
}

int vc4_drm_ioctl(enum vc4_drm_request request, void *arg)
{
    switch (request)
    {
        case VC4_DRM_GET_PARAM:
        {
            struct vc4_drm_get_param *get = arg;

            if (!get)
                return -1;
            return VC4GetParam(get->param, &get->value);
        }

        case VC4_DRM_CREATE_BO:
            return vc4_drm_create_bo(arg);

        case VC4_DRM_CREATE_SHADER_BO:
            return vc4_drm_create_shader_bo(arg);

        case VC4_DRM_MMAP_BO:
        {
            struct vc4_drm_mmap_bo *map_arg = arg;
            void *map;
            uint32_t bus_address;
            uint32_t size;

            if (!map_arg || map_arg->flags != 0 ||
                VC4MapBO(map_arg->handle, &map, &bus_address, &size) != 0)
                return -1;

            /*
             * AROS has one address space. The Mesa-side AROS path consumes
             * this value as a pointer and must not pass it to mmap().
             */
            map_arg->offset = (uint64_t)(IPTR)map;
            return 0;
        }

        case VC4_DRM_GEM_CLOSE:
        {
            struct vc4_drm_gem_close *close_arg = arg;

            if (!close_arg)
                return -1;
            return VC4FreeBO(close_arg->handle);
        }

        case VC4_DRM_SYNC_BO:
        {
            struct vc4_drm_sync_bo *sync = arg;

            if (!sync)
                return -1;
            return VC4SyncBO(sync->handle, sync->offset, sync->length,
                sync->direction);
        }

        case VC4_DRM_SUBMIT_CL:
        {
            struct vc4_drm_submit_cl *submit = arg;
            struct VC4SubmitCL native_submit;
            int error;

            if (!submit)
                return VC4_SUBMIT_ERR_INVALID;

            CopyMem(submit, &native_submit, sizeof(native_submit));
            error = VC4ValidateSubmitCL(&native_submit);
            if (error)
                return error;

            /*
             * Validation is intentionally separated from execution.  No V3D
             * registers are touched until command decoding, relocation,
             * timeout and reset exist in vc4.resource.
             */
            return VC4_SUBMIT_ERR_NOT_IMPLEMENTED;
        }

        default:
            return -1;
    }
}
