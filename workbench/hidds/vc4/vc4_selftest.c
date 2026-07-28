/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Optional BO/transfer smoke test for real Raspberry Pi hardware.
*/

#include <aros/debug.h>
#include <proto/vc4.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "kernel/vc4_packet.h"
#include "vc4_bufmgr.h"
#include "vc4_resource.h"
#include "vc4_tiling.h"

#include "vc4_drm_compat.h"
#include "vc4_selftest.h"

#ifndef VC4GALLIUM_SELFTEST
#define VC4GALLIUM_SELFTEST 0
#endif

#define VC4_SELFTEST_SIZE   (16 * 1024 + 257)
#define VC4_SELFTEST_OFFSET 31
#define VC4_SELFTEST_LENGTH (VC4_SELFTEST_SIZE - 79)

static uint8_t vc4_selftest_pattern(uint32_t index)
{
    uint32_t value = index + 0x6d2b79f5U;

    value ^= value >> 15;
    value *= 0x2c1b3c6dU;
    value ^= value >> 12;
    return (uint8_t)(value ^ (value >> 8) ^ (value >> 16));
}

#if VC4GALLIUM_SELFTEST
static bool
vc4_selftest_tiling(uint32_t width, uint32_t height, uint8_t tiling)
{
    const uint32_t cpp = 4;
    uint32_t utile_width = vc4_utile_width(cpp);
    uint32_t utile_height = vc4_utile_height(cpp);
    uint32_t alignment = tiling == VC4_TILING_FORMAT_T ? 8 : 1;
    uint32_t padded_width = (width + alignment * utile_width - 1) &
        ~(alignment * utile_width - 1);
    uint32_t padded_height = (height + alignment * utile_height - 1) &
        ~(alignment * utile_height - 1);
    uint32_t cpu_stride = width * cpp;
    uint32_t gpu_stride = padded_width * cpp;
    uint32_t cpu_size = cpu_stride * height;
    uint32_t gpu_size = gpu_stride * padded_height;
    struct pipe_box box = {
        .x = 0,
        .y = 0,
        .z = 0,
        .width = width,
        .height = height,
        .depth = 1
    };
    uint8_t *source = NULL;
    uint8_t *tiled = NULL;
    uint8_t *result = NULL;
    uint32_t i;
    bool success = false;

    source = AllocVec(cpu_size, MEMF_PUBLIC);
    tiled = AllocVec(gpu_size, MEMF_PUBLIC | MEMF_CLEAR);
    result = AllocVec(cpu_size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!source || !tiled || !result)
        goto done;

    for (i = 0; i < cpu_size; i++)
        source[i] = vc4_selftest_pattern(i);

    vc4_store_tiled_image(tiled, gpu_stride, source, cpu_stride,
        tiling, cpp, &box);
    vc4_load_tiled_image(result, cpu_stride, tiled, gpu_stride,
        tiling, cpp, &box);

    for (i = 0; i < cpu_size; i++)
    {
        if (result[i] != source[i])
        {
            D(bug("[VC4Gallium] tiling %u mismatch at %lu\n",
                tiling, (unsigned long)i));
            goto done;
        }
    }

    success = true;

done:
    FreeVec(result);
    FreeVec(tiled);
    FreeVec(source);
    return success;
}

static bool
vc4_selftest_tiled_resource(struct pipe_screen *pscreen,
    struct pipe_context *context)
{
    const uint32_t width = 37;
    const uint32_t height = 35;
    struct pipe_resource tmpl = { 0 };
    struct pipe_resource *resource = NULL;
    struct pipe_transfer *transfer = NULL;
    struct pipe_box box = {
        .x = 0,
        .y = 0,
        .z = 0,
        .width = 0,
        .height = 0,
        .depth = 1
    };
    uint8_t *map;
    uint32_t x;
    uint32_t y;
    uint32_t level;
    bool success = false;

    tmpl.target = PIPE_TEXTURE_2D;
    tmpl.format = PIPE_FORMAT_R8G8B8A8_UNORM;
    tmpl.width0 = width;
    tmpl.height0 = height;
    tmpl.depth0 = 1;
    tmpl.array_size = 1;
    tmpl.last_level = 3;
    tmpl.usage = PIPE_USAGE_STAGING;

    resource = pscreen->resource_create(pscreen, &tmpl);
    if (!resource)
        goto done;

    for (level = 0; level <= tmpl.last_level; level++)
    {
        uint32_t row_bytes;

        box.width = width >> level;
        box.height = height >> level;
        if (!box.width)
            box.width = 1;
        if (!box.height)
            box.height = 1;
        row_bytes = (uint32_t)box.width * 4;

        map = context->transfer_map(context, resource, level,
            PIPE_TRANSFER_WRITE, &box, &transfer);
        if (!map || !transfer)
            goto done;

        for (y = 0; y < (uint32_t)box.height; y++)
        {
            for (x = 0; x < row_bytes; x++)
                map[y * transfer->stride + x] =
                    vc4_selftest_pattern((level << 24) +
                        y * row_bytes + x);
        }
        context->transfer_unmap(context, transfer);
        transfer = NULL;

        map = context->transfer_map(context, resource, level,
            PIPE_TRANSFER_READ, &box, &transfer);
        if (!map || !transfer)
            goto done;

        for (y = 0; y < (uint32_t)box.height; y++)
        {
            for (x = 0; x < row_bytes; x++)
            {
                if (map[y * transfer->stride + x] !=
                    vc4_selftest_pattern((level << 24) +
                        y * row_bytes + x))
                {
                    D(bug("[VC4Gallium] mip %lu mismatch at %lu,%lu\n",
                        (unsigned long)level, (unsigned long)x,
                        (unsigned long)y));
                    goto done;
                }
            }
        }
        context->transfer_unmap(context, transfer);
        transfer = NULL;
    }

    success = true;

done:
    if (transfer)
        context->transfer_unmap(context, transfer);
    if (resource)
        pscreen->resource_destroy(pscreen, resource);
    return success;
}

static bool vc4_selftest_submit_validation(struct pipe_resource *resource)
{
    struct vc4_resource *rsc = vc4_resource(resource);
    union {
        uint32_t words[5];
        uint8_t bytes[20];
    } bin_cl = { .bytes = {
        VC4_PACKET_TILE_BINNING_MODE_CONFIG,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 0,
        VC4_PACKET_START_TILE_BINNING,
        VC4_PACKET_INCREMENT_SEMAPHORE,
        VC4_PACKET_FLUSH
    } };
    uint32_t shader_rec = 0;
    uint32_t uniforms = 0;
    uint32_t handles[1] = { rsc->bo->handle };
    struct vc4_drm_submit_cl submit = { 0 };
    struct vc4_drm_submit_rcl_surface absent = {
        .hindex = UINT32_MAX
    };

    submit.bin_cl = (uint64_t)(IPTR)bin_cl.bytes;
    submit.shader_rec = (uint64_t)(IPTR)&shader_rec;
    submit.uniforms = (uint64_t)(IPTR)&uniforms;
    submit.bo_handles = (uint64_t)(IPTR)handles;
    submit.bin_cl_size = 19;
    submit.shader_rec_size = sizeof(shader_rec);
    submit.shader_rec_count = 1;
    submit.uniforms_size = sizeof(uniforms);
    submit.bo_handle_count = 1;
    submit.width = 1;
    submit.height = 1;
    submit.color_read = absent;
    submit.color_write = absent;
    submit.zs_read = absent;
    submit.zs_write = absent;
    submit.msaa_color_write = absent;
    submit.msaa_zs_write = absent;
    submit.color_write.hindex = 0;

    if (vc4_drm_ioctl(VC4_DRM_SUBMIT_CL, &submit) !=
        VC4_SUBMIT_ERR_NOT_IMPLEMENTED)
        return false;

    handles[0] = UINT32_MAX;
    return vc4_drm_ioctl(VC4_DRM_SUBMIT_CL, &submit) ==
        VC4_SUBMIT_ERR_INVALID;
}
#endif

bool vc4_selftest_run(struct pipe_screen *pscreen)
{
#if VC4GALLIUM_SELFTEST
    struct pipe_resource tmpl = { 0 };
    struct pipe_resource *resource = NULL;
    struct pipe_context *context = NULL;
    struct pipe_transfer *transfer = NULL;
    struct pipe_box box = {
        .x = VC4_SELFTEST_OFFSET,
        .y = 0,
        .z = 0,
        .width = VC4_SELFTEST_LENGTH,
        .height = 1,
        .depth = 1
    };
    uint8_t *map;
    uint32_t i;
    bool success = false;

    if (!pscreen || !pscreen->resource_create || !pscreen->resource_destroy ||
        !pscreen->context_create)
        goto done;

    tmpl.target = PIPE_BUFFER;
    tmpl.format = PIPE_FORMAT_R8_UNORM;
    tmpl.width0 = VC4_SELFTEST_SIZE;
    tmpl.height0 = 1;
    tmpl.depth0 = 1;
    tmpl.array_size = 1;
    tmpl.usage = PIPE_USAGE_STAGING;

    resource = pscreen->resource_create(pscreen, &tmpl);
    if (!resource)
        goto done;

    context = pscreen->context_create(pscreen, NULL, 0);
    if (!context || !context->transfer_map || !context->transfer_unmap)
        goto done;

    map = context->transfer_map(context, resource, 0, PIPE_TRANSFER_WRITE,
        &box, &transfer);
    if (!map || !transfer)
        goto done;

    for (i = 0; i < VC4_SELFTEST_LENGTH; i++)
        map[i] = vc4_selftest_pattern(i);
    context->transfer_unmap(context, transfer);
    transfer = NULL;

    map = context->transfer_map(context, resource, 0, PIPE_TRANSFER_READ,
        &box, &transfer);
    if (!map || !transfer)
        goto done;

    for (i = 0; i < VC4_SELFTEST_LENGTH; i++)
    {
        if (map[i] != vc4_selftest_pattern(i))
        {
            D(bug("[VC4Gallium] selftest mismatch at %lu: %02x != %02x\n",
                (unsigned long)i, map[i], vc4_selftest_pattern(i)));
            goto done;
        }
    }

    if (!vc4_selftest_submit_validation(resource))
        goto done;

    if (!vc4_selftest_tiling(13, 11, VC4_TILING_FORMAT_LT) ||
        !vc4_selftest_tiling(37, 35, VC4_TILING_FORMAT_T) ||
        !vc4_selftest_tiled_resource(pscreen, context))
        goto done;

    success = true;

done:
    if (transfer && context)
        context->transfer_unmap(context, transfer);
    if (context)
        context->destroy(context);
    if (resource)
        pscreen->resource_destroy(pscreen, resource);

    D(bug("[VC4Gallium] BO/transfer selftest %s\n",
        success ? "passed" : "FAILED"));
    return success;
#else
    (void)pscreen;
    return true;
#endif
}
