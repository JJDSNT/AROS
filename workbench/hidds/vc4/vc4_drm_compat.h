/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef VC4_DRM_COMPAT_H
#define VC4_DRM_COMPAT_H

#include <stdint.h>

enum vc4_drm_request
{
    VC4_DRM_GET_PARAM = 1,
    VC4_DRM_CREATE_BO,
    VC4_DRM_CREATE_SHADER_BO,
    VC4_DRM_MMAP_BO,
    VC4_DRM_GEM_CLOSE,
    VC4_DRM_SYNC_BO,
    VC4_DRM_SUBMIT_CL
};

struct vc4_drm_get_param
{
    uint32_t param;
    uint32_t pad;
    uint64_t value;
};

struct vc4_drm_create_bo
{
    uint32_t size;
    uint32_t flags;
    uint32_t handle;
    uint32_t pad;
};

struct vc4_drm_create_shader_bo
{
    uint32_t size;
    uint32_t flags;
    uint64_t data;
    uint32_t handle;
    uint32_t pad;
};

struct vc4_drm_mmap_bo
{
    uint32_t handle;
    uint32_t flags;
    uint64_t offset;
};

struct vc4_drm_gem_close
{
    uint32_t handle;
    uint32_t pad;
};

struct vc4_drm_sync_bo
{
    uint32_t handle;
    uint32_t offset;
    uint32_t length;
    uint32_t direction;
};

struct vc4_drm_submit_rcl_surface
{
    uint32_t hindex;
    uint32_t offset;
    uint16_t bits;
    uint16_t flags;
};

struct vc4_drm_submit_cl
{
    uint64_t bin_cl;
    uint64_t shader_rec;
    uint64_t uniforms;
    uint64_t bo_handles;
    uint32_t bin_cl_size;
    uint32_t shader_rec_size;
    uint32_t shader_rec_count;
    uint32_t uniforms_size;
    uint32_t bo_handle_count;
    uint16_t width;
    uint16_t height;
    uint8_t min_x_tile;
    uint8_t min_y_tile;
    uint8_t max_x_tile;
    uint8_t max_y_tile;
    struct vc4_drm_submit_rcl_surface color_read;
    struct vc4_drm_submit_rcl_surface color_write;
    struct vc4_drm_submit_rcl_surface zs_read;
    struct vc4_drm_submit_rcl_surface zs_write;
    struct vc4_drm_submit_rcl_surface msaa_color_write;
    struct vc4_drm_submit_rcl_surface msaa_zs_write;
    uint32_t clear_color[2];
    uint32_t clear_z;
    uint8_t clear_s;
    uint8_t pad[3];
    uint32_t flags;
    uint64_t seqno;
    uint32_t perfmonid;
    uint32_t in_sync;
    uint32_t out_sync;
    uint32_t pad2;
};

int vc4_drm_ioctl(enum vc4_drm_request request, void *arg);

#endif
