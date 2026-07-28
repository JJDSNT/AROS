/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Probe-only Gallium screen for the AROS VC4 port.
*/

#include <proto/exec.h>
#include <proto/vc4.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"

#include "vc4_screen.h"
#include "vc4_context_aros.h"
#include "vc4_resource_aros.h"
#include "vc4_screen_aros.h"

static void vc4_screen_destroy_aros(struct pipe_screen *pscreen)
{
    FreeVec(vc4_screen(pscreen));
}

static const char *vc4_screen_get_name_aros(struct pipe_screen *pscreen)
{
    struct vc4_screen *screen = vc4_screen(pscreen);

    if (screen->v3d_ver == 21)
        return "VC4 V3D 2.1 (AROS probe)";
    if (screen->v3d_ver == 26)
        return "VC4 V3D 2.6 (AROS probe)";
    return "VC4 V3D (AROS probe)";
}

static const char *vc4_screen_get_vendor_aros(struct pipe_screen *pscreen)
{
    (void)pscreen;
    return "Broadcom";
}

static int
vc4_screen_get_param_aros(struct pipe_screen *pscreen, enum pipe_cap param)
{
    (void)pscreen;

    switch (param)
    {
        case PIPE_CAP_VENDOR_ID:
            return 0x14e4;
        case PIPE_CAP_DEVICE_ID:
            return 0;
        case PIPE_CAP_UMA:
            return 1;
        case PIPE_CAP_ACCELERATED:
        case PIPE_CAP_VIDEO_MEMORY:
        default:
            return 0;
    }
}

static float
vc4_screen_get_paramf_aros(struct pipe_screen *pscreen, enum pipe_capf param)
{
    (void)pscreen;
    (void)param;
    return 0.0f;
}

static int
vc4_screen_get_shader_param_aros(struct pipe_screen *pscreen,
    enum pipe_shader_type shader, enum pipe_shader_cap param)
{
    (void)pscreen;
    (void)shader;
    (void)param;
    return 0;
}

static struct pipe_context *
vc4_screen_context_create_aros(struct pipe_screen *pscreen, void *priv,
    unsigned int flags)
{
    return vc4_context_create_aros(pscreen, priv, flags);
}

static bool
vc4_screen_is_format_supported_aros(struct pipe_screen *pscreen,
    enum pipe_format format, enum pipe_texture_target target,
    unsigned int sample_count, unsigned int storage_sample_count,
    unsigned int bindings)
{
    (void)pscreen;
    if (sample_count > 1 || storage_sample_count > 1)
        return false;
    if (target == PIPE_BUFFER)
        return true;
    if (target != PIPE_TEXTURE_2D || (bindings & ~PIPE_BIND_LINEAR) ||
        util_format_get_blocksize(format) == 0)
        return false;
    if (!(bindings & PIPE_BIND_LINEAR) &&
        (util_format_get_blockwidth(format) != 1 ||
         util_format_get_blockheight(format) != 1))
        return false;
    return true;
}

struct pipe_screen *vc4_screen_create_aros(void)
{
    struct vc4_screen *screen;
    uint64_t ident0;
    uint64_t ident1;
    uint32_t major;
    uint32_t minor;

    if (VC4GetParam(VC4_PARAM_V3D_IDENT0, &ident0) != 0 ||
        VC4GetParam(VC4_PARAM_V3D_IDENT1, &ident1) != 0)
        return NULL;

    major = (uint32_t)((ident0 >> 24) & 0xff);
    minor = (uint32_t)(ident1 & 0xf);
    if ((major != 2) || (minor != 1 && minor != 6))
        return NULL;

    screen = AllocVec(sizeof(*screen), MEMF_PUBLIC | MEMF_CLEAR);
    if (!screen)
        return NULL;

    screen->v3d_ver = major * 10 + minor;
    screen->base.destroy = vc4_screen_destroy_aros;
    screen->base.get_name = vc4_screen_get_name_aros;
    screen->base.get_vendor = vc4_screen_get_vendor_aros;
    screen->base.get_device_vendor = vc4_screen_get_vendor_aros;
    screen->base.get_param = vc4_screen_get_param_aros;
    screen->base.get_paramf = vc4_screen_get_paramf_aros;
    screen->base.get_shader_param = vc4_screen_get_shader_param_aros;
    screen->base.context_create = vc4_screen_context_create_aros;
    screen->base.is_format_supported = vc4_screen_is_format_supported_aros;
    vc4_resource_screen_init_aros(&screen->base);

    return &screen->base;
}
