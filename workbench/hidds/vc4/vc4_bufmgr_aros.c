/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Minimal AROS buffer manager for Mesa's Gallium VC4 driver.
*/

#include <proto/exec.h>

#include "vc4_bufmgr.h"
#include "vc4_screen.h"

#include "vc4_drm_compat.h"

static uint32_t vc4_align_page(uint32_t size)
{
    if (!size || size > UINT32_MAX - 4095)
        return 0;

    return (size + 4095) & ~4095U;
}

static struct vc4_bo *
vc4_bo_create(struct vc4_screen *screen, uint32_t size, const char *name,
    enum vc4_drm_request request, const void *data)
{
    struct vc4_bo *bo;
    uint32_t allocation_size = vc4_align_page(size);
    uint32_t handle;
    int error;

    if (!screen || !allocation_size)
        return NULL;

    if (request == VC4_DRM_CREATE_SHADER_BO)
    {
        struct vc4_drm_create_shader_bo create = {
            .size = size,
            .data = (uint64_t)(IPTR)data
        };

        if (!data)
            return NULL;
        error = vc4_drm_ioctl(request, &create);
        handle = create.handle;
    }
    else
    {
        struct vc4_drm_create_bo create = {
            .size = allocation_size
        };

        error = vc4_drm_ioctl(request, &create);
        handle = create.handle;
    }

    if (error)
        return NULL;

    bo = AllocVec(sizeof(*bo), MEMF_PUBLIC | MEMF_CLEAR);
    if (!bo)
    {
        struct vc4_drm_gem_close close = { .handle = handle };

        vc4_drm_ioctl(VC4_DRM_GEM_CLOSE, &close);
        return NULL;
    }

    pipe_reference_init(&bo->reference, 1);
    bo->screen = screen;
    bo->name = name;
    bo->handle = handle;
    bo->size = allocation_size;

    /*
     * AROS does not support external GEM handles yet.  Treat every BO as
     * private, including shader BOs, and free it instead of caching it.
     */
    bo->private = true;
    screen->bo_count++;
    screen->bo_size += allocation_size;

    return bo;
}

struct vc4_bo *
vc4_bo_alloc(struct vc4_screen *screen, uint32_t size, const char *name)
{
    return vc4_bo_create(screen, size, name, VC4_DRM_CREATE_BO, NULL);
}

struct vc4_bo *
vc4_bo_alloc_shader(struct vc4_screen *screen, const void *data, uint32_t size)
{
    return vc4_bo_create(screen, size, "code",
        VC4_DRM_CREATE_SHADER_BO, data);
}

void vc4_bo_last_unreference(struct vc4_bo *bo)
{
    struct vc4_drm_gem_close close;

    if (!bo)
        return;

    close.handle = bo->handle;
    close.pad = 0;
    vc4_drm_ioctl(VC4_DRM_GEM_CLOSE, &close);

    bo->screen->bo_count--;
    bo->screen->bo_size -= bo->size;
    FreeVec(bo);
}

void
vc4_bo_last_unreference_locked_timed(struct vc4_bo *bo, time_t time)
{
    (void)time;
    vc4_bo_last_unreference(bo);
}

void *vc4_bo_map_unsynchronized(struct vc4_bo *bo)
{
    struct vc4_drm_mmap_bo map;

    if (!bo)
        return NULL;
    if (bo->map)
        return bo->map;

    map.handle = bo->handle;
    map.flags = 0;
    map.offset = 0;
    if (vc4_drm_ioctl(VC4_DRM_MMAP_BO, &map) != 0)
        return NULL;

    bo->map = (void *)(IPTR)map.offset;
    return bo->map;
}

void *vc4_bo_map(struct vc4_bo *bo)
{
    /*
     * Submission/wait is not implemented yet, so there can be no in-flight
     * GPU writer at this stage.
     */
    return vc4_bo_map_unsynchronized(bo);
}

bool
vc4_bo_wait(struct vc4_bo *bo, uint64_t timeout_ns, const char *reason)
{
    (void)bo;
    (void)timeout_ns;
    (void)reason;
    return true;
}

bool
vc4_wait_seqno(struct vc4_screen *screen, uint64_t seqno, uint64_t timeout_ns,
    const char *reason)
{
    (void)screen;
    (void)seqno;
    (void)timeout_ns;
    (void)reason;
    return true;
}

struct vc4_bo *
vc4_bo_open_name(struct vc4_screen *screen, uint32_t name)
{
    (void)screen;
    (void)name;
    return NULL;
}

struct vc4_bo *
vc4_bo_open_dmabuf(struct vc4_screen *screen, int fd)
{
    (void)screen;
    (void)fd;
    return NULL;
}

bool vc4_bo_flink(struct vc4_bo *bo, uint32_t *name)
{
    (void)bo;
    (void)name;
    return false;
}

int vc4_bo_get_dmabuf(struct vc4_bo *bo)
{
    (void)bo;
    return -1;
}

void vc4_bo_label(struct vc4_screen *screen, struct vc4_bo *bo,
    const char *fmt, ...)
{
    (void)screen;
    (void)bo;
    (void)fmt;
}

void vc4_bo_debug_describe(char *buf, const struct vc4_bo *bo)
{
    static const char description[] = "vc4_bo";
    unsigned int i;

    (void)bo;
    for (i = 0; i < sizeof(description); i++)
        buf[i] = description[i];
}

void vc4_bufmgr_destroy(struct pipe_screen *pscreen)
{
    (void)pscreen;
}
