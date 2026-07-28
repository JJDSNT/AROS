/*
    Copyright 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>
#include <gallium/gallium.h>
#include <hidd/gallium.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <proto/vc4.h>

#include "pipe/p_screen.h"

#include "vc4_screen_aros.h"
#include "vc4_selftest.h"
#include "vc4gallium_intern.h"

#undef HiddGalliumAttrBase
#define HiddGalliumAttrBase (SD(cl)->hiddGalliumAB)

OOP_Object *METHOD(HiddVC4Gallium, Root, New)
{
    IPTR interface_version;

    interface_version = GetTagData(aHidd_Gallium_InterfaceVersion, (IPTR)-1,
        msg->attrList);
    if (interface_version != GALLIUM_INTERFACE_VERSION)
        return NULL;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (o)
    {
        struct HiddGalliumVC4Data *data = OOP_INST_DATA(cl, o);
        data->pipe_screen = NULL;
    }

    return o;
}

VOID METHOD(HiddVC4Gallium, Root, Dispose)
{
    struct HiddGalliumVC4Data *data = OOP_INST_DATA(cl, o);

    if (data->pipe_screen)
    {
        struct pipe_screen *screen = data->pipe_screen;

        data->pipe_screen = NULL;
        screen->destroy(screen);
    }

    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

VOID METHOD(HiddVC4Gallium, Root, Get)
{
    ULONG idx;

    if (IS_GALLIUM_ATTR(msg->attrID, idx) &&
        idx == aoHidd_Gallium_InterfaceVersion)
    {
        *msg->storage = GALLIUM_INTERFACE_VERSION;
        return;
    }

    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

APTR METHOD(HiddVC4Gallium, Hidd_Gallium, CreatePipeScreen)
{
    struct HiddGalliumVC4Data *data = OOP_INST_DATA(cl, o);
    struct pipe_screen *screen;
    uint64_t ident0;
    uint64_t ident1;
    uint64_t ident2;

    if (data->pipe_screen)
        return data->pipe_screen;

    if (VC4GetParam(VC4_PARAM_V3D_IDENT0, &ident0) == 0 &&
        VC4GetParam(VC4_PARAM_V3D_IDENT1, &ident1) == 0 &&
        VC4GetParam(VC4_PARAM_V3D_IDENT2, &ident2) == 0)
    {
        D(bug("[VC4Gallium] V3D IDENT0=%08lx IDENT1=%08lx IDENT2=%08lx\n",
            (unsigned long)ident0, (unsigned long)ident1,
            (unsigned long)ident2));
    }
    else
    {
        D(bug("[VC4Gallium] V3D probe failed\n"));
        return NULL;
    }

    screen = vc4_screen_create_aros();
    if (!screen)
    {
        D(bug("[VC4Gallium] unsupported V3D revision\n"));
        return NULL;
    }

    if (!vc4_selftest_run(screen))
    {
        screen->destroy(screen);
        D(bug("[VC4Gallium] refusing screen after selftest failure\n"));
        return NULL;
    }

    data->pipe_screen = screen;
    D(bug("[VC4Gallium] probe pipe_screen @ %p (rendering disabled)\n",
        screen));
    return screen;
}

VOID METHOD(HiddVC4Gallium, Hidd_Gallium, DestroyPipeScreen)
{
    struct HiddGalliumVC4Data *data = OOP_INST_DATA(cl, o);
    struct pipe_screen *screen = msg->screen;

    if (!screen)
        return;

    if (data->pipe_screen == screen)
        data->pipe_screen = NULL;
    screen->destroy(screen);
}

VOID METHOD(HiddVC4Gallium, Hidd_Gallium, DisplayResource)
{
    /*
     * Presentation will initially map a VC4 resource and copy it to the
     * vc4gfx bitmap. It must not be reached until CreatePipeScreen succeeds.
     */
    D(bug("[VC4Gallium] DisplayResource called without a pipe_screen\n"));
}
