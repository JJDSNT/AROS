/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Validation boundary for future VC4 command-list submission.
*/

#include <aros/libcall.h>
#include <proto/exec.h>
#include <proto/vc4.h>

#include "vc4_private.h"

#define VC4_SUBMIT_MAX_STREAM_SIZE (16U * 1024U * 1024U)
#define VC4_SUBMIT_MAX_TOTAL_SIZE  (32U * 1024U * 1024U)
#define VC4_SUBMIT_MAX_BO_HANDLES  65536U
#define VC4_SUBMIT_MAX_SHADER_STATES 65536U
#define VC4_SUBMIT_VALID_FLAGS     0x0fU

enum VC4BinPacket
{
    VC4_PACKET_HALT = 0,
    VC4_PACKET_NOP = 1,
    VC4_PACKET_FLUSH = 4,
    VC4_PACKET_FLUSH_ALL = 5,
    VC4_PACKET_START_TILE_BINNING = 6,
    VC4_PACKET_INCREMENT_SEMAPHORE = 7,
    VC4_PACKET_GL_INDEXED_PRIMITIVE = 32,
    VC4_PACKET_GL_ARRAY_PRIMITIVE = 33,
    VC4_PACKET_PRIMITIVE_LIST_FORMAT = 56,
    VC4_PACKET_GL_SHADER_STATE = 64,
    VC4_PACKET_CONFIGURATION_BITS = 96,
    VC4_PACKET_FLAT_SHADE_FLAGS = 97,
    VC4_PACKET_POINT_SIZE = 98,
    VC4_PACKET_LINE_WIDTH = 99,
    VC4_PACKET_RHT_X_BOUNDARY = 100,
    VC4_PACKET_DEPTH_OFFSET = 101,
    VC4_PACKET_CLIP_WINDOW = 102,
    VC4_PACKET_VIEWPORT_OFFSET = 103,
    VC4_PACKET_CLIPPER_XY_SCALING = 105,
    VC4_PACKET_CLIPPER_Z_SCALING = 106,
    VC4_PACKET_TILE_BINNING_MODE_CONFIG = 112,
    VC4_PACKET_GEM_HANDLES = 254
};

struct VC4SubmitShaderState
{
    uint8_t pointer_bits;
    uint32_t max_index;
};

_Static_assert(sizeof(struct VC4SubmitRCLSurface) == 12,
    "VC4 submit surface ABI mismatch");
_Static_assert(sizeof(struct VC4SubmitCL) == 176,
    "VC4 submit ABI mismatch");

static uint32_t vc4_read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

static uint32_t vc4_bin_packet_size(uint8_t opcode)
{
    switch (opcode)
    {
        case VC4_PACKET_HALT:
        case VC4_PACKET_NOP:
        case VC4_PACKET_FLUSH:
        case VC4_PACKET_FLUSH_ALL:
        case VC4_PACKET_START_TILE_BINNING:
        case VC4_PACKET_INCREMENT_SEMAPHORE:
            return 1;
        case VC4_PACKET_PRIMITIVE_LIST_FORMAT:
            return 2;
        case VC4_PACKET_RHT_X_BOUNDARY:
            return 3;
        case VC4_PACKET_CONFIGURATION_BITS:
            return 4;
        case VC4_PACKET_GL_SHADER_STATE:
        case VC4_PACKET_FLAT_SHADE_FLAGS:
        case VC4_PACKET_POINT_SIZE:
        case VC4_PACKET_LINE_WIDTH:
        case VC4_PACKET_DEPTH_OFFSET:
        case VC4_PACKET_VIEWPORT_OFFSET:
            return 5;
        case VC4_PACKET_CLIP_WINDOW:
        case VC4_PACKET_CLIPPER_XY_SCALING:
        case VC4_PACKET_CLIPPER_Z_SCALING:
        case VC4_PACKET_GEM_HANDLES:
            return 9;
        case VC4_PACKET_GL_ARRAY_PRIMITIVE:
            return 10;
        case VC4_PACKET_GL_INDEXED_PRIMITIVE:
            return 14;
        case VC4_PACKET_TILE_BINNING_MODE_CONFIG:
            return 16;
        default:
            return 0;
    }
}

/*
 * Decode the untrusted BCL without following any address embedded in it.
 * The command stream is always little-endian, independently of the CPU
 * byte order used by a possible future AROS target.
 */
static BOOL vc4_submit_bin_cl_valid(const uint8_t *cl, uint32_t size,
    uint32_t bo_count, uint32_t shader_rec_count,
    struct VC4SubmitShaderState *states, uint32_t *state_count)
{
    uint32_t offset = 0;
    uint32_t packet_size;
    uint32_t shader_states = 0;
    uint32_t bo_index[2] = { UINT32_MAX, UINT32_MAX };
    BOOL found_config = FALSE;
    BOOL found_start = FALSE;
    BOOL found_increment = FALSE;
    BOOL found_flush = FALSE;

    while (offset < size)
    {
        uint8_t opcode = cl[offset];

        packet_size = vc4_bin_packet_size(opcode);
        if (!packet_size || packet_size > size - offset)
            return FALSE;

        switch (opcode)
        {
            case VC4_PACKET_TILE_BINNING_MODE_CONFIG:
                if (found_config || cl[offset + 13] == 0 ||
                    cl[offset + 14] == 0 ||
                    (cl[offset + 15] & ((1U << 7) | (1U << 1))) != 0)
                    return FALSE;
                found_config = TRUE;
                break;

            case VC4_PACKET_START_TILE_BINNING:
                if (found_start || !found_config)
                    return FALSE;
                found_start = TRUE;
                break;

            case VC4_PACKET_INCREMENT_SEMAPHORE:
                if (offset != size - 2)
                    return FALSE;
                found_increment = TRUE;
                break;

            case VC4_PACKET_FLUSH:
                if (offset != size - 1)
                    return FALSE;
                found_flush = TRUE;
                break;

            case VC4_PACKET_GEM_HANDLES:
                bo_index[0] = vc4_read_le32(cl + offset + 1);
                bo_index[1] = vc4_read_le32(cl + offset + 5);
                if (bo_index[0] >= bo_count || bo_index[1] >= bo_count)
                    return FALSE;
                break;

            case VC4_PACKET_GL_SHADER_STATE:
                if ((vc4_read_le32(cl + offset + 1) & ~0x0fU) != 0 ||
                    shader_states >= shader_rec_count)
                    return FALSE;
                states[shader_states].pointer_bits =
                    (uint8_t)vc4_read_le32(cl + offset + 1);
                states[shader_states].max_index = 0;
                shader_states++;
                break;

            case VC4_PACKET_GL_INDEXED_PRIMITIVE:
                if (!shader_states || bo_index[0] >= bo_count)
                    return FALSE;
                if (vc4_read_le32(cl + offset + 10) >
                    states[shader_states - 1].max_index)
                    states[shader_states - 1].max_index =
                        vc4_read_le32(cl + offset + 10);
                break;

            case VC4_PACKET_GL_ARRAY_PRIMITIVE:
            {
                uint32_t length;
                uint32_t base_index;
                uint32_t max_index;

                if (!shader_states)
                    return FALSE;
                length = vc4_read_le32(cl + offset + 2);
                base_index = vc4_read_le32(cl + offset + 6);
                if (!length || base_index > UINT32_MAX - (length - 1))
                    return FALSE;
                max_index = base_index + length - 1;
                if (max_index > states[shader_states - 1].max_index)
                    states[shader_states - 1].max_index = max_index;
                break;
            }

            case VC4_PACKET_HALT:
                return FALSE;
        }

        offset += packet_size;
    }

    *state_count = shader_states;
    return found_config && found_start && found_increment && found_flush;
}

static BOOL vc4_submit_shader_recs_valid(struct VC4Base *VC4Base,
    const uint8_t *records, uint32_t records_size, const uint32_t *handles,
    uint32_t handle_count, const struct VC4SubmitShaderState *states,
    uint32_t state_count)
{
    static const uint32_t shader_offsets[3] = { 4, 16, 28 };
    uint32_t offset = 0;
    uint32_t state_index;

    for (state_index = 0; state_index < state_count; state_index++)
    {
        const struct VC4SubmitShaderState *state = &states[state_index];
        uint32_t attributes = state->pointer_bits & 7U;
        uint32_t record_size;
        uint32_t relocations;
        uint32_t relocation_size;
        const uint8_t *relocation_data;
        const uint8_t *record;
        uint32_t i;

        if (!attributes)
            attributes = 8;
        record_size = (state->pointer_bits & 8U) ?
            100U + attributes * 4U : 36U + attributes * 8U;
        relocations = 3U + attributes;
        relocation_size = relocations * sizeof(uint32_t);

        if (relocation_size > records_size - offset)
            return FALSE;
        relocation_data = records + offset;
        offset += relocation_size;
        if (record_size > records_size - offset)
            return FALSE;
        record = records + offset;
        offset += record_size;

        for (i = 0; i < relocations; i++)
        {
            uint32_t hindex = vc4_read_le32(relocation_data + i * 4U);
            struct VC4BO *bo;

            if (hindex >= handle_count)
                return FALSE;
            bo = vc4_find_bo(VC4Base, handles[hindex]);
            if (!bo)
                return FALSE;

            if (i < 3)
            {
                if (!(bo->bo_Flags & VC4_BOF_SHADER) ||
                    vc4_read_le32(record + shader_offsets[i]) != 0 ||
                    bo->bo_Size < sizeof(uint64_t))
                    return FALSE;
            }
            else
            {
                uint32_t attribute = i - 3U;
                uint32_t attribute_offset = 36U + attribute * 8U;
                uint32_t bo_offset =
                    vc4_read_le32(record + attribute_offset);
                uint32_t attribute_size =
                    (uint32_t)record[attribute_offset + 4U] + 1U;
                uint32_t stride = record[attribute_offset + 5U];
                uint64_t last_byte;

                if (bo->bo_Flags & VC4_BOF_SHADER)
                    return FALSE;
                if (state->pointer_bits & 8U)
                    stride |= vc4_read_le32(record + 100U +
                        attribute * 4U) & ~0xffU;
                last_byte = (uint64_t)bo_offset + attribute_size;
                if (stride)
                    last_byte += (uint64_t)state->max_index * stride;
                if (last_byte > bo->bo_Size)
                    return FALSE;
            }
        }
    }

    /*
     * Mesa may leave up to one alignment unit after the last record.  Accept
     * only zero-filled padding so it cannot hide another record.
     */
    if (records_size - offset > 15)
        return FALSE;
    while (offset < records_size)
    {
        if (records[offset++] != 0)
            return FALSE;
    }
    return TRUE;
}

static BOOL vc4_submit_range_valid(uint64_t pointer, uint32_t size,
    uint32_t alignment)
{
    uintptr_t address;

    if (!pointer || !size || pointer > UINTPTR_MAX)
        return FALSE;
    address = (uintptr_t)pointer;
    if ((address & (alignment - 1)) != 0 ||
        address > UINTPTR_MAX - (uintptr_t)size)
        return FALSE;
    return TRUE;
}

static BOOL vc4_submit_surface_valid(struct VC4Base *VC4Base,
    const struct VC4SubmitRCLSurface *surface, const uint32_t *handles,
    uint32_t handle_count)
{
    struct VC4BO *bo;

    if (surface->hindex == UINT32_MAX)
        return surface->offset == 0;
    if (surface->hindex >= handle_count)
        return FALSE;

    bo = vc4_find_bo(VC4Base, handles[surface->hindex]);
    return bo && surface->offset < bo->bo_Size;
}

AROS_LH1(int, VC4ValidateSubmitCL,
    AROS_LHA(const struct VC4SubmitCL *, submit, A0),
    struct VC4Base *, VC4Base, 6, Vc4)
{
    AROS_LIBFUNC_INIT

    const uint32_t *handles;
    APTR bin_cl_copy = NULL;
    APTR shader_rec_copy = NULL;
    APTR uniforms_copy = NULL;
    APTR handles_copy = NULL;
    struct VC4SubmitShaderState *shader_states = NULL;
    uint64_t total_size;
    uint32_t handles_size;
    uint32_t shader_states_size;
    uint32_t shader_state_count = 0;
    uint32_t i;
    BOOL valid;

    if (!submit || submit->seqno != 0 || submit->pad2 != 0 ||
        submit->perfmonid != 0 || submit->in_sync != 0 ||
        submit->out_sync != 0 || submit->pad[0] != 0 ||
        submit->pad[1] != 0 || submit->pad[2] != 0 ||
        (submit->flags & ~VC4_SUBMIT_VALID_FLAGS) != 0 ||
        !submit->width || !submit->height ||
        submit->min_x_tile > submit->max_x_tile ||
        submit->min_y_tile > submit->max_y_tile ||
        !submit->bo_handle_count ||
        submit->bo_handle_count > VC4_SUBMIT_MAX_BO_HANDLES ||
        submit->bin_cl_size > VC4_SUBMIT_MAX_STREAM_SIZE ||
        submit->shader_rec_size > VC4_SUBMIT_MAX_STREAM_SIZE ||
        submit->uniforms_size > VC4_SUBMIT_MAX_STREAM_SIZE ||
        submit->shader_rec_count > VC4_SUBMIT_MAX_SHADER_STATES ||
        submit->shader_rec_count > submit->shader_rec_size / sizeof(uint32_t))
        return VC4_SUBMIT_ERR_INVALID;

    handles_size = submit->bo_handle_count * sizeof(uint32_t);
    shader_states_size = submit->shader_rec_count *
        sizeof(*shader_states);
    total_size = (uint64_t)submit->bin_cl_size +
        submit->shader_rec_size + submit->uniforms_size + handles_size;
    if (total_size > VC4_SUBMIT_MAX_TOTAL_SIZE)
        return VC4_SUBMIT_ERR_INVALID;

    if (!vc4_submit_range_valid(submit->bin_cl, submit->bin_cl_size, 4) ||
        !vc4_submit_range_valid(submit->shader_rec,
            submit->shader_rec_size, 4) ||
        !vc4_submit_range_valid(submit->uniforms,
            submit->uniforms_size, 4) ||
        !vc4_submit_range_valid(submit->bo_handles, handles_size, 4))
        return VC4_SUBMIT_ERR_INVALID;

    bin_cl_copy = AllocMem(submit->bin_cl_size, MEMF_PUBLIC);
    shader_rec_copy = AllocMem(submit->shader_rec_size, MEMF_PUBLIC);
    uniforms_copy = AllocMem(submit->uniforms_size, MEMF_PUBLIC);
    handles_copy = AllocMem(handles_size, MEMF_PUBLIC);
    if (shader_states_size)
        shader_states = AllocMem(shader_states_size,
            MEMF_PUBLIC | MEMF_CLEAR);
    if (!bin_cl_copy || !shader_rec_copy || !uniforms_copy || !handles_copy ||
        (shader_states_size && !shader_states))
    {
        valid = FALSE;
        goto cleanup;
    }

    /*
     * From this point onward validation uses resource-owned snapshots.  AROS
     * has one address space, so a completely invalid source pointer may still
     * fault during CopyMem; no pointer is retained after this call.
     */
    CopyMem((const void *)(uintptr_t)submit->bin_cl, bin_cl_copy,
        submit->bin_cl_size);
    CopyMem((const void *)(uintptr_t)submit->shader_rec, shader_rec_copy,
        submit->shader_rec_size);
    CopyMem((const void *)(uintptr_t)submit->uniforms, uniforms_copy,
        submit->uniforms_size);
    CopyMem((const void *)(uintptr_t)submit->bo_handles, handles_copy,
        handles_size);

    handles = handles_copy;
    ObtainSemaphoreShared(&VC4Base->vc4_Lock);

    valid = TRUE;
    for (i = 0; i < submit->bo_handle_count; i++)
    {
        if (!vc4_find_bo(VC4Base, handles[i]))
        {
            valid = FALSE;
            break;
        }
    }

    if (valid)
    {
        valid = vc4_submit_bin_cl_valid(bin_cl_copy,
            submit->bin_cl_size, submit->bo_handle_count,
            submit->shader_rec_count, shader_states, &shader_state_count);
    }

    if (valid)
    {
        valid = vc4_submit_shader_recs_valid(VC4Base, shader_rec_copy,
            submit->shader_rec_size, handles, submit->bo_handle_count,
            shader_states, shader_state_count);
    }

    if (valid)
    {
        valid =
            vc4_submit_surface_valid(VC4Base, &submit->color_read,
                handles, submit->bo_handle_count) &&
            vc4_submit_surface_valid(VC4Base, &submit->color_write,
                handles, submit->bo_handle_count) &&
            vc4_submit_surface_valid(VC4Base, &submit->zs_read,
                handles, submit->bo_handle_count) &&
            vc4_submit_surface_valid(VC4Base, &submit->zs_write,
                handles, submit->bo_handle_count) &&
            vc4_submit_surface_valid(VC4Base, &submit->msaa_color_write,
                handles, submit->bo_handle_count) &&
            vc4_submit_surface_valid(VC4Base, &submit->msaa_zs_write,
                handles, submit->bo_handle_count);
    }

    ReleaseSemaphore(&VC4Base->vc4_Lock);

cleanup:
    if (shader_states)
        FreeMem(shader_states, shader_states_size);
    if (handles_copy)
        FreeMem(handles_copy, handles_size);
    if (uniforms_copy)
        FreeMem(uniforms_copy, submit->uniforms_size);
    if (shader_rec_copy)
        FreeMem(shader_rec_copy, submit->shader_rec_size);
    if (bin_cl_copy)
        FreeMem(bin_cl_copy, submit->bin_cl_size);
    return valid ? 0 : VC4_SUBMIT_ERR_INVALID;

    AROS_LIBFUNC_EXIT
}
