#define __OOP_NOATTRBASES__

#include <aros/symbolsets.h>
#include <graphics/driver.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/oop.h>

#include "emu68gfx_intern.h"

#include LC_LIBDEFS_FILE

static const STRPTR interfaces[ATTRBASES_NUM] =
{
    IID_Hidd_ChunkyBM,
    IID_Hidd_BitMap,
    IID_Hidd_Gfx,
    IID_Hidd_PixFmt,
    IID_Hidd_Sync,
    IID_Hidd,
    IID_Hidd_Display,
    IID_Hidd_DMEnum
};

static void release_attr_bases(struct Emu68GfxStaticData *xsd, ULONG count)
{
    ULONG i;

    for (i = 0; i < count; i++)
        if (xsd->attrBases[i])
            OOP_ReleaseAttrBase(interfaces[i]);
}

static BOOL obtain_attr_bases(struct Emu68GfxStaticData *xsd)
{
    ULONG i;

    for (i = 0; i < ATTRBASES_NUM; i++)
    {
        xsd->attrBases[i] = OOP_ObtainAttrBase(interfaces[i]);
        if (!xsd->attrBases[i])
        {
            release_attr_bases(xsd, i);
            return FALSE;
        }
    }
    return TRUE;
}

static int Emu68Gfx_Init(LIBBASETYPEPTR LIBBASE)
{
    struct Emu68GfxStaticData *xsd = &LIBBASE->vsd;
    struct Emu68BootContext *ctx = &emu68_boot_context;
    struct GfxBase *GfxBase;
    ULONG error;

    if (!(ctx->flags & EMU68_BOOT_FRAMEBUFFER) ||
        !ctx->framebuffer || !ctx->framebuffer_pitch ||
        !ctx->framebuffer_width || !ctx->framebuffer_height ||
        ctx->framebuffer_pitch < ctx->framebuffer_width * 2)
        return FALSE;

    xsd->framebuffer = ctx->framebuffer;
    xsd->pitch = ctx->framebuffer_pitch;
    xsd->width = ctx->framebuffer_width;
    xsd->height = ctx->framebuffer_height;

    GfxBase = (struct GfxBase *)TaggedOpenLibrary(TAGGEDOPEN_GRAPHICS);
    if (!GfxBase)
        return FALSE;

    if (!obtain_attr_bases(xsd))
    {
        CloseLibrary(&GfxBase->LibNode);
        return FALSE;
    }

    xsd->basebm = OOP_FindClass(CLID_Hidd_BitMap);
    error = AddDisplayDriver(xsd->gfxclass, NULL,
                             DDRV_BootMode, TRUE, TAG_DONE);
    CloseLibrary(&GfxBase->LibNode);

    if (error)
    {
        release_attr_bases(xsd, ATTRBASES_NUM);
        return FALSE;
    }

    LIBBASE->library.lib_OpenCnt = 1;
    return TRUE;
}

ADD2INITLIB(Emu68Gfx_Init, 0)
