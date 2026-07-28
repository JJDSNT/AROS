/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Initial linear-buffer resource support for the AROS VC4 port.
*/

#include <proto/exec.h>
#include <proto/vc4.h>

#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/u_math.h"

#include "vc4_bufmgr.h"
#include "vc4_resource.h"
#include "vc4_tiling.h"

#include "vc4_drm_compat.h"
#include "vc4_resource_aros.h"

static bool vc4_resource_layout(const struct pipe_resource *tmpl,
    uint32_t *stride_out, uint32_t *size_out, uint8_t *tiling_out,
    uint32_t *cpp_out)
{
    size_t stride;
    size_t size;
    uint32_t cpp;
    uint32_t width;
    uint32_t height;
    uint8_t tiling = VC4_TILING_FORMAT_LINEAR;

    if (!tmpl || !tmpl->width0)
        return false;
    if (tmpl->target != PIPE_BUFFER && tmpl->target != PIPE_TEXTURE_2D)
        return false;
    if (tmpl->last_level >= VC4_MAX_MIP_LEVELS || tmpl->depth0 > 1 ||
        tmpl->array_size > 1 || tmpl->nr_samples > 1 ||
        tmpl->nr_storage_samples > 1 ||
        (tmpl->flags & (PIPE_RESOURCE_FLAG_MAP_PERSISTENT |
            PIPE_RESOURCE_FLAG_MAP_COHERENT)))
        return false;

    if (tmpl->target == PIPE_BUFFER)
    {
        if (tmpl->height0 > 1 || tmpl->last_level != 0)
            return false;
        stride = tmpl->width0;
        size = tmpl->width0;
        cpp = 1;
        goto valid;
    }

    if (tmpl->bind & ~PIPE_BIND_LINEAR)
        return false;
    cpp = util_format_get_blocksize(tmpl->format);
    if (!tmpl->height0 || !cpp)
        return false;

    if (tmpl->bind & PIPE_BIND_LINEAR)
    {
        stride = util_format_get_stride(tmpl->format, tmpl->width0);
        size = util_format_get_2d_size(tmpl->format, stride, tmpl->height0);
    }
    else
    {
        uint32_t utile_width;
        uint32_t utile_height;

        /* Compressed/block formats remain linear in this first cut. */
        if (util_format_get_blockwidth(tmpl->format) != 1 ||
            util_format_get_blockheight(tmpl->format) != 1 ||
            (cpp != 1 && cpp != 2 && cpp != 4 && cpp != 8))
            return false;

        utile_width = vc4_utile_width(cpp);
        utile_height = vc4_utile_height(cpp);
        if (vc4_size_is_lt(tmpl->width0, tmpl->height0, cpp))
        {
            tiling = VC4_TILING_FORMAT_LT;
            width = (tmpl->width0 + utile_width - 1) & ~(utile_width - 1);
            height = (tmpl->height0 + utile_height - 1) &
                ~(utile_height - 1);
        }
        else
        {
            tiling = VC4_TILING_FORMAT_T;
            width = (tmpl->width0 + 8 * utile_width - 1) &
                ~(8 * utile_width - 1);
            height = (tmpl->height0 + 8 * utile_height - 1) &
                ~(8 * utile_height - 1);
        }

        stride = (size_t)width * cpp;
        size = stride * height;
    }

    if (!stride || stride > UINT32_MAX || !size || size > UINT32_MAX)
        return false;

valid:
    if (stride_out)
        *stride_out = (uint32_t)stride;
    if (size_out)
        *size_out = (uint32_t)size;
    if (tiling_out)
        *tiling_out = tiling;
    if (cpp_out)
        *cpp_out = cpp;
    return true;
}

static bool
vc4_resource_template_supported(const struct pipe_resource *tmpl)
{
    return vc4_resource_layout(tmpl, NULL, NULL, NULL, NULL);
}

static bool
vc4_can_create_resource_aros(struct pipe_screen *pscreen,
    const struct pipe_resource *tmpl)
{
    (void)pscreen;
    return vc4_resource_template_supported(tmpl);
}

static struct pipe_resource *
vc4_resource_create_aros(struct pipe_screen *pscreen,
    const struct pipe_resource *tmpl)
{
    struct vc4_resource *rsc;
    struct pipe_resource level_tmpl;
    uint32_t pot_width;
    uint32_t pot_height;
    uint64_t total_size = 0;
    uint32_t page_shift;
    int level;

    if (!vc4_resource_template_supported(tmpl))
        return NULL;

    rsc = AllocVec(sizeof(*rsc), MEMF_PUBLIC | MEMF_CLEAR);
    if (!rsc)
        return NULL;

    rsc->base = *tmpl;
    rsc->base.screen = pscreen;
    rsc->base.next = NULL;
    pipe_reference_init(&rsc->base.reference, 1);

    level_tmpl = *tmpl;
    level_tmpl.last_level = 0;
    pot_width = util_next_power_of_two(tmpl->width0);
    pot_height = util_next_power_of_two(tmpl->height0);

    for (level = tmpl->last_level; level >= 0; level--)
    {
        uint32_t stride;
        uint32_t size;
        uint32_t cpp;
        uint8_t tiling;

        level_tmpl.width0 = level == 0 ? tmpl->width0 :
            u_minify(pot_width, level);
        level_tmpl.height0 = tmpl->target == PIPE_BUFFER ? 1 :
            (level == 0 ? tmpl->height0 : u_minify(pot_height, level));
        if (!vc4_resource_layout(&level_tmpl, &stride, &size, &tiling, &cpp) ||
            total_size + size > UINT32_MAX)
        {
            FreeVec(rsc);
            return NULL;
        }

        rsc->slices[level].offset = (uint32_t)total_size;
        rsc->slices[level].stride = stride;
        rsc->slices[level].size = size;
        rsc->slices[level].tiling = tiling;
        rsc->cpp = cpp;
        total_size += size;
    }

    page_shift = (uint32_t)((((uint64_t)rsc->slices[0].offset + 4095U) &
        ~(uint64_t)4095U) - rsc->slices[0].offset);
    if (total_size + page_shift > UINT32_MAX)
    {
        FreeVec(rsc);
        return NULL;
    }
    if (page_shift)
    {
        for (level = 0; level <= tmpl->last_level; level++)
            rsc->slices[level].offset += page_shift;
        total_size += page_shift;
    }

    rsc->bo = vc4_bo_alloc(vc4_screen(pscreen), (uint32_t)total_size,
        "resource");
    if (!rsc->bo)
    {
        FreeVec(rsc);
        return NULL;
    }

    rsc->cube_map_stride = (uint32_t)total_size;
    rsc->tiled = rsc->slices[0].tiling != VC4_TILING_FORMAT_LINEAR;

    return &rsc->base;
}

static void
vc4_resource_destroy_aros(struct pipe_screen *pscreen,
    struct pipe_resource *prsc)
{
    struct vc4_resource *rsc;

    (void)pscreen;
    if (!prsc)
        return;

    rsc = vc4_resource(prsc);
    /*
     * The probe backend has no jobs and never shares a BO, so the resource
     * owns the only reference.  Avoid Mesa's shared-handle hash path until
     * that infrastructure is linked.
     */
    vc4_bo_last_unreference(rsc->bo);
    rsc->bo = NULL;
    FreeVec(rsc);
}

void *vc4_resource_map_aros(struct pipe_resource *prsc, uint32_t offset,
    uint32_t length, bool read)
{
    struct vc4_resource *rsc;
    struct vc4_drm_sync_bo sync;
    uint8_t *map;

    if (!prsc || offset > vc4_resource(prsc)->cube_map_stride ||
        length > vc4_resource(prsc)->cube_map_stride - offset)
        return NULL;

    rsc = vc4_resource(prsc);
    if (read && length)
    {
        sync.handle = rsc->bo->handle;
        sync.offset = offset;
        sync.length = length;
        sync.direction = VC4_SYNC_GPU_TO_CPU;
        if (vc4_drm_ioctl(VC4_DRM_SYNC_BO, &sync) != 0)
            return NULL;
    }

    map = vc4_bo_map(rsc->bo);
    return map ? map + offset : NULL;
}

bool vc4_resource_unmap_aros(struct pipe_resource *prsc, uint32_t offset,
    uint32_t length, bool written)
{
    struct vc4_resource *rsc;
    struct vc4_drm_sync_bo sync;

    if (!prsc || offset > vc4_resource(prsc)->cube_map_stride ||
        length > vc4_resource(prsc)->cube_map_stride - offset)
        return false;
    if (!written || !length)
        return true;

    rsc = vc4_resource(prsc);
    sync.handle = rsc->bo->handle;
    sync.offset = offset;
    sync.length = length;
    sync.direction = VC4_SYNC_CPU_TO_GPU;
    return vc4_drm_ioctl(VC4_DRM_SYNC_BO, &sync) == 0;
}

void vc4_resource_screen_init_aros(struct pipe_screen *pscreen)
{
    pscreen->can_create_resource = vc4_can_create_resource_aros;
    pscreen->resource_create = vc4_resource_create_aros;
    pscreen->resource_destroy = vc4_resource_destroy_aros;
}
