#ifndef VC4GALLIUM_INTERN_H
#define VC4GALLIUM_INTERN_H

/*
    Copyright 2026, The AROS Development Team. All rights reserved.
*/

#include <exec/libraries.h>
#include <oop/oop.h>

#include LC_LIBDEFS_FILE

#define CLID_Hidd_Gallium_VC4 "hidd.gallium.vc4"

struct HiddGalliumVC4Data
{
    APTR pipe_screen;
};

struct vc4gallium_staticdata
{
    OOP_Class *galliumclass;
    OOP_AttrBase hiddGalliumAB;
    APTR vc4Base;
};

LIBBASETYPE
{
    struct Library LibNode;
    struct vc4gallium_staticdata sd;
};

#define METHOD(base, id, name) \
    base ## __ ## id ## __ ## name \
        (OOP_Class *cl, OOP_Object *o, struct p ## id ## _ ## name *msg)

#define BASE(lib) ((LIBBASETYPEPTR)(lib))
#define SD(cl) (&BASE(cl->UserData)->sd)

#endif
