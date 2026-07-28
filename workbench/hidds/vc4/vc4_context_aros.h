/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef VC4_CONTEXT_AROS_H
#define VC4_CONTEXT_AROS_H

struct pipe_context;
struct pipe_screen;

struct pipe_context *vc4_context_create_aros(struct pipe_screen *pscreen,
    void *priv, unsigned int flags);

#endif
