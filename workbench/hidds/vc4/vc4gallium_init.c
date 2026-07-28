/*
    Copyright 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/symbolsets.h>
#include <hidd/gallium.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/vc4.h>

#include "vc4gallium_intern.h"

APTR VC4Base __attribute__((used)) = NULL;

static int HiddVC4Gallium_ExpungeLib(LIBBASETYPEPTR LIBBASE)
{
    if (LIBBASE->sd.hiddGalliumAB)
        OOP_ReleaseAttrBase((STRPTR)IID_Hidd_Gallium);

    return TRUE;
}

static int HiddVC4Gallium_InitLib(LIBBASETYPEPTR LIBBASE)
{
    LIBBASE->sd.hiddGalliumAB =
        OOP_ObtainAttrBase((STRPTR)IID_Hidd_Gallium);
    if (!LIBBASE->sd.hiddGalliumAB)
        return FALSE;

    VC4Base = LIBBASE->sd.vc4Base = OpenResource("vc4.resource");
    if (!VC4Base)
    {
        OOP_ReleaseAttrBase((STRPTR)IID_Hidd_Gallium);
        LIBBASE->sd.hiddGalliumAB = 0;
        return FALSE;
    }

    return TRUE;
}

ADD2INITLIB(HiddVC4Gallium_InitLib, 0)
ADD2EXPUNGELIB(HiddVC4Gallium_ExpungeLib, 0)

ADD2LIBS((STRPTR)"gallium.hidd", 7,
    static struct Library *, GalliumHiddBase);
