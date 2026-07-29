#define __OOP_NOATTRBASES__

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <hidd/gfx.h>

#include "emu68gfx_intern.h"

#include LC_LIBDEFS_FILE

OOP_Object *Emu68Gfx__Root__New(OOP_Class *cl, OOP_Object *o,
                                struct pRoot_New *msg)
{
    struct TagItem pixel_tags[] =
    {
        { aHidd_PixFmt_RedShift,       16 },
        { aHidd_PixFmt_GreenShift,     21 },
        { aHidd_PixFmt_BlueShift,      27 },
        { aHidd_PixFmt_AlphaShift,      0 },
        { aHidd_PixFmt_RedMask,    0xf800 },
        { aHidd_PixFmt_GreenMask,  0x07e0 },
        { aHidd_PixFmt_BlueMask,   0x001f },
        { aHidd_PixFmt_AlphaMask,       0 },
        { aHidd_PixFmt_ColorModel, vHidd_ColorModel_TrueColor },
        { aHidd_PixFmt_Depth,          16 },
        { aHidd_PixFmt_BytesPerPixel,   2 },
        { aHidd_PixFmt_BitsPerPixel,   16 },
        { aHidd_PixFmt_StdPixFmt, vHidd_StdPixFmt_RGB16_LE },
        { aHidd_PixFmt_BitMapType, vHidd_BitMapType_Chunky },
        { TAG_DONE, 0 }
    };
    struct TagItem sync_tags[] =
    {
        { aHidd_Sync_PixelClock, 0 },
        { aHidd_Sync_HTotal, 0 },
        { aHidd_Sync_HDisp, 0 },
        { aHidd_Sync_VDisp, 0 },
        { aHidd_Sync_HMax, 16384 },
        { aHidd_Sync_VMax, 16384 },
        { aHidd_Sync_Description, (IPTR)"Emu68:%hx%v" },
        { TAG_DONE, 0 }
    };
    struct TagItem mode_tags[] =
    {
        { aHidd_DMEnum_PixFmtTags, (IPTR)pixel_tags },
        { aHidd_DMEnum_SyncTags, (IPTR)sync_tags },
        { TAG_DONE, 0 }
    };
    struct TagItem new_tags[] =
    {
        { aHidd_Name, (IPTR)"emu68gfx.hidd" },
        { aHidd_HardwareName, (IPTR)"Emu68 linear framebuffer" },
        { aHidd_ProducerName, (IPTR)"Emu68" },
        { TAG_MORE, 0 }
    };
    struct pRoot_New new_msg;

    if (XSD(cl)->gfx)
        return NULL;

    sync_tags[0].ti_Data = 60 * XSD(cl)->width * XSD(cl)->height;
    sync_tags[1].ti_Data = XSD(cl)->width;
    sync_tags[2].ti_Data = XSD(cl)->width;
    sync_tags[3].ti_Data = XSD(cl)->height;

    new_tags[3].ti_Data = (IPTR)msg->attrList;
    if (!msg->attrList)
        new_tags[3].ti_Tag = TAG_DONE;
    new_msg.mID = msg->mID;
    new_msg.attrList = new_tags;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&new_msg);
    if (o)
    {
        struct TagItem display_tags[] =
        {
            { aHidd_Display_GfxHidd, (IPTR)o },
            { aHidd_Display_ModeTags, (IPTR)mode_tags },
            { TAG_DONE, 0 }
        };

        XSD(cl)->display =
            OOP_NewObject(XSD(cl)->displayclass, NULL, display_tags);
        if (!XSD(cl)->display)
        {
            OOP_DisposeObject(o);
            return NULL;
        }

        XSD(cl)->gfx = o;
        OOP_GetAttr(XSD(cl)->display, aHidd_Display_DMEnumerator,
                    (IPTR *)&XSD(cl)->dmenum);
    }
    return o;
}

VOID Emu68Gfx__Root__Get(OOP_Class *cl, OOP_Object *o,
                         struct pRoot_Get *msg)
{
    ULONG idx;

    if (IS_GFX_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_Gfx_DriverName:
            *msg->storage = (IPTR)"Emu68";
            return;
        case aoHidd_Gfx_DisplayDefault:
            *msg->storage = (IPTR)XSD(cl)->display;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

OOP_Object *Emu68Display__Hidd_Display__CreateObject(
    OOP_Class *cl, OOP_Object *o, struct pHidd_Display_CreateObject *msg)
{
    if (msg->cl == XSD(cl)->basebm)
    {
        struct TagItem tags[] =
        {
            { TAG_IGNORE, 0 },
            { TAG_MORE, (IPTR)msg->attrList }
        };
        struct pHidd_Display_CreateObject create_msg;
        BOOL displayable =
            GetTagData(aHidd_BitMap_Displayable, FALSE, msg->attrList);

        if (displayable)
        {
            tags[0].ti_Tag = aHidd_BitMap_ClassPtr;
            tags[0].ti_Data = (IPTR)XSD(cl)->bmclass;
        }
        else
        {
            tags[0].ti_Tag = aHidd_BitMap_ClassID;
            tags[0].ti_Data = (IPTR)CLID_Hidd_ChunkyBM;
        }

        create_msg.mID = msg->mID;
        create_msg.cl = msg->cl;
        create_msg.attrList = tags;
        return (OOP_Object *)OOP_DoSuperMethod(
            cl, o, (OOP_Msg)&create_msg);
    }
    return (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

OOP_Object *Emu68Display__Hidd_Display__Show(
    OOP_Class *cl, OOP_Object *o, struct pHidd_Display_Show *msg)
{
    struct TagItem visible[] =
    {
        { aHidd_BitMap_Visible, FALSE },
        { TAG_DONE, 0 }
    };

    LOCK_FB(XSD(cl));
    if (XSD(cl)->visible)
        OOP_SetAttrs(XSD(cl)->visible, visible);

    if (msg->bitMap)
    {
        visible[0].ti_Data = TRUE;
        OOP_SetAttrs(msg->bitMap, visible);
    }
    else
        SetMem(XSD(cl)->framebuffer, 0,
               XSD(cl)->pitch * XSD(cl)->height);
    XSD(cl)->visible = msg->bitMap;
    UNLOCK_FB(XSD(cl));

    if (msg->bitMap)
        emu68gfx_refresh(cl, msg->bitMap, 0, 0,
                         XSD(cl)->width, XSD(cl)->height);
    return msg->bitMap;
}
