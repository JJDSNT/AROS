/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifndef VC4_RESOURCE_AROS_H
#define VC4_RESOURCE_AROS_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_resource;
struct pipe_screen;

void vc4_resource_screen_init_aros(struct pipe_screen *pscreen);
void *vc4_resource_map_aros(struct pipe_resource *prsc, uint32_t offset,
    uint32_t length, bool read);
bool vc4_resource_unmap_aros(struct pipe_resource *prsc, uint32_t offset,
    uint32_t length, bool written);

#endif
