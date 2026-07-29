#define __OOP_NOATTRBASES__

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <hidd/gfx.h>

#include "emu68gfx_intern.h"

#include LC_LIBDEFS_FILE

OOP_Object *Emu68BM__Root__New(OOP_Class *cl, OOP_Object *o,
                               struct pRoot_New *msg)
{
    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (o)
    {
        struct Emu68BitMapData *data = OOP_INST_DATA(cl, o);

        OOP_GetAttr(o, aHidd_BitMap_Display, (IPTR *)&data->display);
        OOP_GetAttr(o, aHidd_BitMap_PixFmt, (IPTR *)&data->pixfmt);
        OOP_GetAttr(o, aHidd_ChunkyBM_Buffer, (IPTR *)&data->buffer);
        data->bytes_per_row = OOP_GET(o, aHidd_BitMap_BytesPerRow);
        data->width = OOP_GET(o, aHidd_BitMap_Width);
        data->height = OOP_GET(o, aHidd_BitMap_Height);
    }
    return o;
}

VOID Emu68BM__Root__Get(OOP_Class *cl, OOP_Object *o,
                        struct pRoot_Get *msg)
{
    struct Emu68BitMapData *data = OOP_INST_DATA(cl, o);
    ULONG idx;

    if (IS_BM_ATTR(msg->attrID, idx) && idx == aoHidd_BitMap_Visible)
    {
        *msg->storage = data->visible;
        return;
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

VOID Emu68BM__Root__Set(OOP_Class *cl, OOP_Object *o,
                        struct pRoot_Set *msg)
{
    struct Emu68BitMapData *data = OOP_INST_DATA(cl, o);
    struct TagItem *state = msg->attrList;
    struct TagItem *tag;
    ULONG idx;

    while ((tag = NextTagItem(&state)))
        if (IS_BM_ATTR(tag->ti_Tag, idx) &&
            idx == aoHidd_BitMap_Visible)
            data->visible = tag->ti_Data;

    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

void emu68gfx_refresh(OOP_Class *cl, OOP_Object *bitmap,
                      ULONG x, ULONG y, ULONG width, ULONG height)
{
    OOP_Class *bmclass = OOP_OCLASS(bitmap);
    struct Emu68BitMapData *data = OOP_INST_DATA(bmclass, bitmap);
    struct Emu68GfxStaticData *xsd = XSD(bmclass);
    ULONG row;

    (void)cl;
    if (!data->visible || !data->buffer ||
        x >= xsd->width || y >= xsd->height ||
        x >= data->width || y >= data->height)
        return;
    if (width > xsd->width - x)
        width = xsd->width - x;
    if (width > data->width - x)
        width = data->width - x;
    if (height > xsd->height - y)
        height = xsd->height - y;
    if (height > data->height - y)
        height = data->height - y;

    LOCK_FB(xsd);
    for (row = 0; row < height; row++)
    {
        UBYTE *src = data->buffer + (y + row) * data->bytes_per_row + x * 2;
        UBYTE *dst = xsd->framebuffer + (y + row) * xsd->pitch + x * 2;
        CopyMem(src, dst, width * 2);
    }
    UNLOCK_FB(xsd);
}

VOID Emu68BM__Hidd_BitMap__UpdateRect(
    OOP_Class *cl, OOP_Object *o, struct pHidd_BitMap_UpdateRect *msg)
{
    emu68gfx_refresh(cl, o, msg->x, msg->y, msg->width, msg->height);
}
