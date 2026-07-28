/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Transfer-only Gallium context for the AROS VC4 bring-up.
*/

#include <proto/exec.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/u_math.h"

#include "vc4_resource.h"

#include "vc4_context_aros.h"
#include "vc4_resource_aros.h"

struct vc4_context_aros
{
    struct pipe_context base;
};

struct vc4_transfer_aros
{
    struct pipe_transfer base;
    void *staging;
};

static void vc4_context_destroy_aros(struct pipe_context *pctx)
{
    FreeVec(pctx);
}

static bool
vc4_transfer_box_valid(const struct pipe_resource *prsc, unsigned int level,
    const struct pipe_box *box)
{
    uint32_t x;
    uint32_t width;
    uint32_t y;
    uint32_t height;

    if (!prsc || level > prsc->last_level || !box)
        return false;
    if (box->x < 0 || box->y < 0 || box->z != 0 ||
        box->width < 0 || box->height < 0 || box->depth != 1)
        return false;

    x = (uint32_t)box->x;
    width = (uint32_t)box->width;
    y = (uint32_t)box->y;
    height = (uint32_t)box->height;

    if (prsc->target == PIPE_BUFFER)
        return y == 0 && height == 1 && x <= prsc->width0 &&
            width <= prsc->width0 - x;
    if (prsc->target != PIPE_TEXTURE_2D)
        return false;

    return x <= u_minify(prsc->width0, level) &&
        width <= u_minify(prsc->width0, level) - x &&
        y <= u_minify(prsc->height0, level) &&
        height <= u_minify(prsc->height0, level) - y &&
        x % util_format_get_blockwidth(prsc->format) == 0 &&
        y % util_format_get_blockheight(prsc->format) == 0;
}

static void *
vc4_transfer_map_aros(struct pipe_context *pctx, struct pipe_resource *prsc,
    unsigned int level, unsigned int usage, const struct pipe_box *box,
    struct pipe_transfer **out_transfer)
{
    struct vc4_transfer_aros *transfer;
    struct vc4_resource *rsc;
    struct vc4_resource_slice *slice;
    uint32_t offset;
    uint32_t length;
    uint32_t row_bytes;
    uint32_t rows;
    void *map;

    (void)pctx;
    if (!out_transfer ||
        !(usage & (PIPE_TRANSFER_READ | PIPE_TRANSFER_WRITE)) ||
        (usage & PIPE_TRANSFER_FLUSH_EXPLICIT) ||
        !vc4_transfer_box_valid(prsc, level, box))
        return NULL;

    *out_transfer = NULL;
    transfer = AllocVec(sizeof(*transfer), MEMF_PUBLIC | MEMF_CLEAR);
    if (!transfer)
        return NULL;

    rsc = vc4_resource(prsc);
    slice = &rsc->slices[level];
    if (prsc->target == PIPE_BUFFER)
    {
        offset = (uint32_t)box->x;
        length = (uint32_t)box->width;
        row_bytes = length;
        rows = 1;
    }
    else
    {
        uint32_t block_width = util_format_get_blockwidth(prsc->format);
        uint32_t block_height = util_format_get_blockheight(prsc->format);
        uint32_t block_size = util_format_get_blocksize(prsc->format);

        row_bytes = util_format_get_nblocksx(prsc->format,
            (uint32_t)box->width) * block_size;
        rows = util_format_get_nblocksy(prsc->format,
            (uint32_t)box->height);
        offset = slice->offset + ((uint32_t)box->y / block_height) *
            slice->stride +
            ((uint32_t)box->x / block_width) * block_size;
        length = rows ? (rows - 1) * slice->stride + row_bytes : 0;
    }

    pipe_resource_reference(&transfer->base.resource, prsc);
    transfer->base.level = level;
    transfer->base.usage = usage;
    transfer->base.box = *box;
    transfer->base.stride = prsc->target == PIPE_BUFFER ?
        row_bytes : slice->stride;
    transfer->base.layer_stride = length;

    if (rsc->tiled)
    {
        uint32_t staging_size = row_bytes * (uint32_t)box->height;
        void *gpu_map;

        transfer->staging = AllocVec(staging_size, MEMF_PUBLIC | MEMF_CLEAR);
        if (!transfer->staging)
            map = NULL;
        else
        {
            gpu_map = vc4_resource_map_aros(prsc, slice->offset,
                slice->size, (usage & PIPE_TRANSFER_READ) != 0);
            if (!gpu_map)
                map = NULL;
            else
            {
                if (usage & PIPE_TRANSFER_READ)
                    vc4_load_tiled_image(transfer->staging, row_bytes,
                        gpu_map, slice->stride,
                        slice->tiling, rsc->cpp, box);
                map = transfer->staging;
                transfer->base.stride = row_bytes;
                transfer->base.layer_stride = staging_size;
            }
        }
    }
    else
    {
        map = vc4_resource_map_aros(prsc, offset, length,
            (usage & PIPE_TRANSFER_READ) != 0);
    }
    if (!map)
    {
        FreeVec(transfer->staging);
        pipe_resource_reference(&transfer->base.resource, NULL);
        FreeVec(transfer);
        return NULL;
    }

    *out_transfer = &transfer->base;
    return map;
}

static void
vc4_transfer_unmap_aros(struct pipe_context *pctx,
    struct pipe_transfer *ptransfer)
{
    bool written;
    struct vc4_resource *rsc;
    struct vc4_resource_slice *slice;
    uint32_t offset;
    uint32_t length;

    (void)pctx;
    if (!ptransfer)
        return;

    rsc = vc4_resource(ptransfer->resource);
    slice = &rsc->slices[ptransfer->level];
    if (ptransfer->resource->target == PIPE_BUFFER)
    {
        offset = (uint32_t)ptransfer->box.x;
        length = (uint32_t)ptransfer->box.width;
    }
    else
    {
        uint32_t block_width =
            util_format_get_blockwidth(ptransfer->resource->format);
        uint32_t block_height =
            util_format_get_blockheight(ptransfer->resource->format);
        uint32_t block_size =
            util_format_get_blocksize(ptransfer->resource->format);
        uint32_t rows = util_format_get_nblocksy(ptransfer->resource->format,
            (uint32_t)ptransfer->box.height);
        uint32_t row_bytes =
            util_format_get_nblocksx(ptransfer->resource->format,
                (uint32_t)ptransfer->box.width) * block_size;

        offset = slice->offset +
            ((uint32_t)ptransfer->box.y / block_height) * slice->stride +
            ((uint32_t)ptransfer->box.x / block_width) * block_size;
        length = rows ? (rows - 1) * slice->stride + row_bytes : 0;
    }

    written = (ptransfer->usage & PIPE_TRANSFER_WRITE) != 0;
    if (rsc->tiled)
    {
        struct vc4_transfer_aros *transfer =
            (struct vc4_transfer_aros *)ptransfer;

        if (written)
        {
            void *gpu_map = vc4_resource_map_aros(ptransfer->resource,
                slice->offset, slice->size, false);

            if (gpu_map)
                vc4_store_tiled_image(gpu_map, slice->stride,
                    transfer->staging, ptransfer->stride,
                    slice->tiling, rsc->cpp, &ptransfer->box);
            else
                written = false;
        }
        vc4_resource_unmap_aros(ptransfer->resource, slice->offset,
            slice->size, written);
        FreeVec(transfer->staging);
    }
    else
        vc4_resource_unmap_aros(ptransfer->resource, offset, length, written);
    pipe_resource_reference(&ptransfer->resource, NULL);
    FreeVec(ptransfer);
}

struct pipe_context *vc4_context_create_aros(struct pipe_screen *pscreen,
    void *priv, unsigned int flags)
{
    struct vc4_context_aros *context;

    (void)flags;
    if (!pscreen)
        return NULL;

    context = AllocVec(sizeof(*context), MEMF_PUBLIC | MEMF_CLEAR);
    if (!context)
        return NULL;

    context->base.screen = pscreen;
    context->base.priv = priv;
    context->base.destroy = vc4_context_destroy_aros;
    context->base.transfer_map = vc4_transfer_map_aros;
    context->base.transfer_unmap = vc4_transfer_unmap_aros;

    return &context->base;
}
