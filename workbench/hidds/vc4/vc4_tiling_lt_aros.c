/*
    AROS build wrapper for Mesa's base LT tiling implementation.

    The separate AArch32 NEON source is intentionally not included.  Mesa's
    common utile helper selects its AArch64 implementation when available.
*/

#include "vc4_tiling_lt.c"
