/*
    AROS build wrapper for Mesa VC4 tiling.

    vc4_tiling.c does not use vc4_screen.h or vc4_context.h directly.  Skip
    those Linux/DRM-heavy headers and provide the pipe definitions it needs.
*/

#include <assert.h>
#include <strings.h>

#include "pipe/p_state.h"
#include "util/u_math.h"

#include "kernel/vc4_packet.h"

#define VC4_SCREEN_H
#define VC4_CONTEXT_H
#include "vc4_tiling.c"
