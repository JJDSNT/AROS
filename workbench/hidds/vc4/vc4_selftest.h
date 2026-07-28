/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef VC4_SELFTEST_H
#define VC4_SELFTEST_H

#include <stdbool.h>

struct pipe_screen;

bool vc4_selftest_run(struct pipe_screen *pscreen);

#endif
