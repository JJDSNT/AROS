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
    uint32_t record_offset;
    uint32_t uniform_offset[3];
};

struct VC4QPUValidation
{
    uint32_t uniform_data_bytes;
    uint32_t texture_count;
};

struct VC4BinInfo
{
    uint32_t config_offset;
    uint8_t tiles_x;
    uint8_t tiles_y;
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

static uint64_t vc4_read_le64(const uint8_t *bytes)
{
    return (uint64_t)vc4_read_le32(bytes) |
        ((uint64_t)vc4_read_le32(bytes + 4) << 32);
}

static void vc4_write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void vc4_write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

/*
 * First conservative QPU pass.  This mirrors the upstream termination,
 * signal, branch-boundary and immediately dangerous write checks.  It is not
 * yet sufficient to authorize execution: TMU and uniform data-flow analysis
 * is deliberately left for the complete validator.
 */
static BOOL vc4_submit_qpu_shader_valid(const struct VC4BO *bo,
    struct VC4QPUValidation *result)
{
    const uint8_t *code;
    uint32_t instructions;
    uint32_t ip;
    uint32_t end_ip = UINT32_MAX;
    uint32_t uniforms = 0;
    int32_t last_thread_switch = -3;
    BOOL threaded = FALSE;
    BOOL upper_registers = FALSE;
    BOOL saw_branch = FALSE;
    BOOL saw_tmu = FALSE;
    uint8_t tmu_write_count[2] = { 0, 0 };

    if (!result || !bo || !(bo->bo_Flags & VC4_BOF_SHADER) ||
        !bo->bo_CPUAddress || bo->bo_LogicalSize < 3U * sizeof(uint64_t) ||
        (bo->bo_LogicalSize & (sizeof(uint64_t) - 1U)) != 0)
        return FALSE;

    result->uniform_data_bytes = 0;
    result->texture_count = 0;
    code = bo->bo_CPUAddress;
    instructions = bo->bo_LogicalSize / sizeof(uint64_t);

    for (ip = 0; ip < instructions; ip++)
    {
        uint64_t instruction = vc4_read_le64(code + ip * 8U);
        uint32_t signal = (uint32_t)(instruction >> 60);
        uint32_t waddr_add = (uint32_t)((instruction >> 38) & 0x3fU);
        uint32_t waddr_mul = (uint32_t)((instruction >> 32) & 0x3fU);
        uint32_t raddr_a = (uint32_t)((instruction >> 18) & 0x3fU);
        uint32_t raddr_b = (uint32_t)((instruction >> 12) & 0x3fU);

        if (signal == 0 || signal == 7 || signal == 9 || signal == 12)
            return FALSE;

        if (signal == 15)
        {
            int32_t branch_offset = (int32_t)(uint32_t)instruction;
            int64_t target;

            if ((instruction & ((uint64_t)1 << 50)) != 0 ||
                (instruction & ((uint64_t)1 << 51)) == 0 ||
                (branch_offset & 7) != 0 ||
                waddr_add != 39 || waddr_mul != 39 ||
                ip + 4U >= instructions ||
                (int32_t)ip < last_thread_switch + 3)
                return FALSE;
            if (saw_tmu || tmu_write_count[0] || tmu_write_count[1])
                return FALSE;
            saw_branch = TRUE;
            target = (int64_t)ip + 4 + branch_offset / 8;
            if (target < 0 || target >= instructions)
                return FALSE;
        }
        else
        {
            BOOL add_writes_tmu = waddr_add >= 56 && waddr_add <= 63;
            BOOL mul_writes_tmu = waddr_mul >= 56 && waddr_mul <= 63;

            if (waddr_add == 36 || waddr_add == 38 ||
                waddr_add == 47 || waddr_add == 50 || waddr_add == 51 ||
                waddr_mul == 36 || waddr_mul == 38 ||
                waddr_mul == 47 || waddr_mul == 50 || waddr_mul == 51)
                return FALSE;

            if (add_writes_tmu && mul_writes_tmu)
                return FALSE;
            if (add_writes_tmu || mul_writes_tmu)
            {
                uint32_t waddr = add_writes_tmu ? waddr_add : waddr_mul;
                uint32_t tmu = waddr >= 60 ? 1U : 0U;
                BOOL submit_sample = waddr == 56 || waddr == 60;

                /*
                 * Direct TMU submission needs the upstream MIN/MAX clamp
                 * proof.  Reject it until that data-flow pass is ported.
                 */
                if (saw_branch ||
                    (submit_sample && tmu_write_count[tmu] == 0) ||
                    tmu_write_count[tmu] >= 4 ||
                    raddr_a == 32 || (signal != 13 && raddr_b == 32))
                    return FALSE;
                saw_tmu = TRUE;
                tmu_write_count[tmu]++;
                if (uniforms > UINT32_MAX - sizeof(uint32_t))
                    return FALSE;
                uniforms += sizeof(uint32_t);
                if (submit_sample)
                {
                    if (result->texture_count == UINT32_MAX)
                        return FALSE;
                    result->texture_count++;
                    tmu_write_count[tmu] = 0;
                }
            }

            if ((waddr_add >= 16 && waddr_add < 32) ||
                (waddr_mul >= 16 && waddr_mul < 32))
                upper_registers = TRUE;
            if (signal != 14 &&
                ((raddr_a >= 16 && raddr_a < 32) ||
                 (signal != 13 && raddr_b >= 16 && raddr_b < 32)))
                upper_registers = TRUE;

            if (signal == 2 || signal == 6)
            {
                if ((int32_t)ip < last_thread_switch + 3 ||
                    tmu_write_count[0] || tmu_write_count[1])
                    return FALSE;
                threaded = TRUE;
                last_thread_switch = (int32_t)ip;
            }

            /*
             * LOAD_IMM and BRANCH reuse these bit positions for other
             * fields.  SMALL_IMM replaces only raddr_b; raddr_a remains a
             * real read port.
             */
            if (signal != 14 &&
                (raddr_a == 32 || (signal != 13 && raddr_b == 32)))
            {
                if (uniforms > UINT32_MAX - sizeof(uint32_t))
                    return FALSE;
                uniforms += sizeof(uint32_t);
            }
        }

        if (signal == 3)
        {
            if (ip + 2U >= instructions ||
                tmu_write_count[0] || tmu_write_count[1])
                return FALSE;
            end_ip = ip + 2U;
        }
        if (ip == end_ip)
        {
            if (threaded && upper_registers)
                return FALSE;
            result->uniform_data_bytes = uniforms;
            return TRUE;
        }
    }

    return FALSE;
}

static uint32_t vc4_round_up_u32(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static BOOL vc4_submit_texture_bounds_valid(const struct VC4BO *bo,
    uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3)
{
    uint32_t offset = p0 & 0xfffff000U;
    uint32_t width = (p1 >> 8) & 0x7ffU;
    uint32_t height = (p1 >> 20) & 0x7ffU;
    uint32_t type = ((p0 >> 4) & 0xfU) | ((p1 >> 27) & 0x10U);
    uint32_t cpp;
    uint32_t utile_width;
    uint32_t utile_height;
    uint32_t aligned_width;
    uint32_t aligned_height;
    uint64_t level_size;
    uint32_t mip_levels = p0 & 0xfU;
    uint32_t level;
    uint32_t cube_stride = 0;
    uint64_t base_offset;
    BOOL linear;
    BOOL lt;

    if ((p0 & (1U << 9)) != 0)
    {
        if ((p2 >> 30) == 1)
            cube_stride = p2 & 0x3ffff000U;
        if ((p3 >> 30) == 1)
        {
            if (cube_stride)
                return FALSE;
            cube_stride = p3 & 0x3ffff000U;
        }
        if (!cube_stride)
            return FALSE;
    }
    if (!width)
        width = 2048;
    if (!height)
        height = 2048;

    switch (type)
    {
        case 0:
        case 1:
        case 16:
            cpp = 4;
            break;
        case 2:
        case 3:
        case 4:
        case 7:
        case 9:
        case 11:
            cpp = 2;
            break;
        case 5:
        case 6:
        case 10:
            cpp = 1;
            break;
        case 8:
            cpp = 8;
            width = (width + 3U) >> 2;
            height = (height + 3U) >> 2;
            break;
        default:
            return FALSE;
    }

    utile_width = cpp <= 2 ? 8U : (cpp == 4 ? 4U : 2U);
    utile_height = cpp == 1 ? 8U : 4U;
    linear = type == 16;
    lt = !linear &&
        (width <= 4U * utile_width || height <= 4U * utile_height);

    if (linear)
    {
        aligned_width = vc4_round_up_u32(width, utile_width);
        aligned_height = height;
    }
    else if (lt)
    {
        aligned_width = vc4_round_up_u32(width, utile_width);
        aligned_height = vc4_round_up_u32(height, utile_height);
    }
    else
    {
        aligned_width = vc4_round_up_u32(width, utile_width * 8U);
        aligned_height = vc4_round_up_u32(height, utile_height * 8U);
    }

    level_size = (uint64_t)aligned_width * aligned_height * cpp;
    base_offset = (uint64_t)offset + (uint64_t)cube_stride * 5U;
    if (base_offset >= bo->bo_Size ||
        level_size > bo->bo_Size - base_offset ||
        (uint64_t)bo->bo_BusAddress + p0 > UINT32_MAX)
        return FALSE;

    /*
     * Mip levels precede the base image.  The tiling decision is repeated at
     * each level and may transition from T to LT, but never back to T.
     */
    for (level = 1; level <= mip_levels; level++)
    {
        uint32_t level_width = width >> level;
        uint32_t level_height = height >> level;

        if (!level_width)
            level_width = 1;
        if (!level_height)
            level_height = 1;
        if (!linear && !lt &&
            (level_width <= 4U * utile_width ||
             level_height <= 4U * utile_height))
            lt = TRUE;

        if (linear)
        {
            aligned_width = vc4_round_up_u32(level_width, utile_width);
            aligned_height = level_height;
        }
        else if (lt)
        {
            aligned_width = vc4_round_up_u32(level_width, utile_width);
            aligned_height = vc4_round_up_u32(level_height, utile_height);
        }
        else
        {
            aligned_width = vc4_round_up_u32(level_width,
                utile_width * 8U);
            aligned_height = vc4_round_up_u32(level_height,
                utile_height * 8U);
        }

        level_size = (uint64_t)aligned_width * aligned_height * cpp;
        if (level_size > offset)
            return FALSE;
        offset -= (uint32_t)level_size;
    }

    return TRUE;
}

static BOOL vc4_submit_qpu_textures_valid(struct VC4Base *VC4Base,
    const struct VC4BO *shader, const uint8_t *texture_handles,
    uint32_t texture_count, const uint8_t *uniform_data,
    uint32_t uniform_data_size, uint8_t *validated_uniform_data,
    const uint32_t *handles, uint32_t handle_count)
{
    const uint8_t *code = shader->bo_CPUAddress;
    uint32_t instructions = shader->bo_LogicalSize / sizeof(uint64_t);
    uint32_t offsets[2][4];
    uint8_t counts[2] = { 0, 0 };
    uint32_t uniform_offset = 0;
    uint32_t sample = 0;
    uint32_t end_ip = UINT32_MAX;
    uint32_t ip;

    CopyMem(uniform_data, validated_uniform_data, uniform_data_size);

    for (ip = 0; ip < instructions; ip++)
    {
        uint64_t instruction = vc4_read_le64(code + ip * 8U);
        uint32_t signal = (uint32_t)(instruction >> 60);
        uint32_t waddr_add = (uint32_t)((instruction >> 38) & 0x3fU);
        uint32_t waddr_mul = (uint32_t)((instruction >> 32) & 0x3fU);
        uint32_t raddr_a = (uint32_t)((instruction >> 18) & 0x3fU);
        uint32_t raddr_b = (uint32_t)((instruction >> 12) & 0x3fU);
        uint32_t waddr = (waddr_add >= 56 && waddr_add <= 63) ?
            waddr_add : waddr_mul;

        if (waddr >= 56 && waddr <= 63)
        {
            uint32_t tmu = waddr >= 60 ? 1U : 0U;
            BOOL submit_sample = waddr == 56 || waddr == 60;

            if (counts[tmu] >= 4 ||
                uniform_data_size - uniform_offset < sizeof(uint32_t))
                return FALSE;
            offsets[tmu][counts[tmu]++] = uniform_offset;
            uniform_offset += sizeof(uint32_t);

            if (submit_sample)
            {
                uint32_t hindex;
                uint32_t p0;
                uint32_t p1;
                uint32_t p2;
                uint32_t p3;
                struct VC4BO *texture_bo;

                if (counts[tmu] < 2 || sample >= texture_count)
                    return FALSE;
                hindex = vc4_read_le32(texture_handles +
                    sample * sizeof(uint32_t));
                if (hindex >= handle_count)
                    return FALSE;
                texture_bo = vc4_find_bo(VC4Base, handles[hindex]);
                p0 = vc4_read_le32(uniform_data + offsets[tmu][0]);
                p1 = vc4_read_le32(uniform_data + offsets[tmu][1]);
                p2 = counts[tmu] > 2 ?
                    vc4_read_le32(uniform_data + offsets[tmu][2]) : 0;
                p3 = counts[tmu] > 3 ?
                    vc4_read_le32(uniform_data + offsets[tmu][3]) : 0;
                if (!texture_bo ||
                    !vc4_submit_texture_bounds_valid(texture_bo,
                        p0, p1, p2, p3))
                    return FALSE;
                vc4_write_le32(validated_uniform_data + offsets[tmu][0],
                    texture_bo->bo_BusAddress + p0);
                counts[tmu] = 0;
                sample++;
            }
        }

        if (signal != 14 &&
            (raddr_a == 32 || (signal != 13 && raddr_b == 32)))
        {
            if (uniform_data_size - uniform_offset < sizeof(uint32_t))
                return FALSE;
            uniform_offset += sizeof(uint32_t);
        }
        if (signal == 3)
            end_ip = ip + 2U;
        if (ip == end_ip)
            break;
    }

    return sample == texture_count && uniform_offset == uniform_data_size;
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
static BOOL vc4_submit_bin_cl_valid(struct VC4Base *VC4Base,
    const uint8_t *cl, uint32_t size, const uint32_t *handles,
    uint32_t bo_count, uint32_t shader_rec_count,
    struct VC4SubmitShaderState *states, uint32_t *state_count,
    uint8_t *validated_cl, uint32_t *validated_size,
    struct VC4BinInfo *bin_info)
{
    uint32_t offset = 0;
    uint32_t destination_offset = 0;
    uint32_t packet_size;
    uint32_t shader_states = 0;
    uint32_t i;
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
        if (opcode != VC4_PACKET_GEM_HANDLES)
        {
            CopyMem(cl + offset, validated_cl + destination_offset,
                packet_size);
        }

        switch (opcode)
        {
            case VC4_PACKET_TILE_BINNING_MODE_CONFIG:
                if (found_config || cl[offset + 13] == 0 ||
                    cl[offset + 14] == 0 ||
                    (cl[offset + 15] & ((1U << 7) | (1U << 1))) != 0)
                    return FALSE;
                found_config = TRUE;
                bin_info->config_offset = destination_offset;
                bin_info->tiles_x = cl[offset + 13];
                bin_info->tiles_y = cl[offset + 14];
                /*
                 * Tile allocation/state addresses are resource-owned and
                 * will be filled only after their BO exists.
                 */
                for (i = 1; i <= 12; i++)
                    validated_cl[destination_offset + i] = 0;
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
                vc4_write_le32(validated_cl + destination_offset + 1,
                    states[shader_states].pointer_bits);
                shader_states++;
                break;

            case VC4_PACKET_GL_INDEXED_PRIMITIVE:
            {
                struct VC4BO *index_bo;
                uint32_t length;
                uint32_t index_offset;
                uint32_t index_size;
                uint64_t end;

                if (!shader_states || bo_index[0] >= bo_count)
                    return FALSE;
                index_bo = vc4_find_bo(VC4Base, handles[bo_index[0]]);
                if (!index_bo || (index_bo->bo_Flags & VC4_BOF_SHADER))
                    return FALSE;
                length = vc4_read_le32(cl + offset + 2);
                index_offset = vc4_read_le32(cl + offset + 6);
                index_size = (cl[offset + 1] >> 4) ? 2U : 1U;
                end = (uint64_t)index_offset +
                    (uint64_t)length * index_size;
                if (end > index_bo->bo_Size ||
                    (uint64_t)index_bo->bo_BusAddress + index_offset >
                        UINT32_MAX)
                    return FALSE;
                vc4_write_le32(validated_cl + destination_offset + 6,
                    index_bo->bo_BusAddress + index_offset);
                if (vc4_read_le32(cl + offset + 10) >
                    states[shader_states - 1].max_index)
                    states[shader_states - 1].max_index =
                        vc4_read_le32(cl + offset + 10);
                break;
            }

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
        if (opcode != VC4_PACKET_GEM_HANDLES)
            destination_offset += packet_size;
    }

    *state_count = shader_states;
    *validated_size = destination_offset;
    return found_config && found_start && found_increment && found_flush;
}

static BOOL vc4_submit_shader_recs_valid(struct VC4Base *VC4Base,
    const uint8_t *records, uint32_t records_size, const uint32_t *handles,
    uint32_t handle_count, struct VC4SubmitShaderState *states,
    uint32_t state_count, const uint8_t *uniforms, uint32_t uniforms_size,
    uint8_t *validated_records, uint32_t *validated_records_size,
    uint8_t *validated_uniforms, uint32_t *validated_uniforms_size)
{
    static const uint32_t shader_offsets[3] = { 4, 16, 28 };
    uint32_t offset = 0;
    uint32_t state_index;
    uint32_t uniform_offset = 0;
    uint32_t destination_uniform_offset = 0;
    uint32_t destination_record_offset = 0;

    for (state_index = 0; state_index < state_count; state_index++)
    {
        struct VC4SubmitShaderState *state = &states[state_index];
        uint32_t attributes = state->pointer_bits & 7U;
        uint32_t record_size;
        uint32_t relocations;
        uint32_t relocation_size;
        const uint8_t *relocation_data;
        const uint8_t *record;
        uint8_t *validated_record;
        uint32_t aligned_record_size;
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
        aligned_record_size = (record_size + 15U) & ~15U;
        if (aligned_record_size > records_size - destination_record_offset)
            return FALSE;
        validated_record = validated_records + destination_record_offset;
        state->record_offset = destination_record_offset;
        CopyMem(record, validated_record, record_size);
        destination_record_offset += aligned_record_size;

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
                struct VC4QPUValidation qpu = { 0 };
                uint64_t shader_uniform_bytes;
                uint32_t texture;

                if (!(bo->bo_Flags & VC4_BOF_SHADER) ||
                    vc4_read_le32(record + shader_offsets[i]) != 0 ||
                    !vc4_submit_qpu_shader_valid(bo, &qpu))
                    return FALSE;
                state->uniform_offset[i] = destination_uniform_offset;
                vc4_write_le32(validated_record + shader_offsets[i],
                    bo->bo_BusAddress);
                /*
                 * Filled after validated uniforms reside in a GPU-visible
                 * internal BO.
                 */
                vc4_write_le32(validated_record + shader_offsets[i] + 4U,
                    0);
                shader_uniform_bytes = (uint64_t)qpu.texture_count *
                    sizeof(uint32_t) + qpu.uniform_data_bytes;
                if (shader_uniform_bytes > uniforms_size - uniform_offset)
                    return FALSE;

                for (texture = 0; texture < qpu.texture_count; texture++)
                {
                    uint32_t hindex = vc4_read_le32(uniforms +
                        uniform_offset + texture * sizeof(uint32_t));
                    struct VC4BO *texture_bo;

                    if (hindex >= handle_count)
                        return FALSE;
                    texture_bo = vc4_find_bo(VC4Base, handles[hindex]);
                    if (!texture_bo ||
                        (texture_bo->bo_Flags & VC4_BOF_SHADER))
                        return FALSE;
                }
                if (!vc4_submit_qpu_textures_valid(VC4Base, bo,
                    uniforms + uniform_offset, qpu.texture_count,
                    uniforms + uniform_offset +
                        qpu.texture_count * sizeof(uint32_t),
                    qpu.uniform_data_bytes,
                    validated_uniforms + destination_uniform_offset,
                    handles, handle_count))
                    return FALSE;
                destination_uniform_offset += qpu.uniform_data_bytes;
                uniform_offset += (uint32_t)shader_uniform_bytes;
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
                if (last_byte > bo->bo_Size ||
                    (uint64_t)bo->bo_BusAddress + bo_offset > UINT32_MAX)
                    return FALSE;
                vc4_write_le32(validated_record + attribute_offset,
                    bo->bo_BusAddress + bo_offset);
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
    if (uniforms_size - uniform_offset > 15)
        return FALSE;
    while (uniform_offset < uniforms_size)
    {
        if (uniforms[uniform_offset++] != 0)
            return FALSE;
    }
    *validated_records_size = destination_record_offset;
    *validated_uniforms_size = destination_uniform_offset;
    return destination_record_offset <= records_size &&
        destination_uniform_offset <= uniforms_size;
}

static BOOL vc4_submit_patch_staging(uint8_t *bin_cl, uint32_t bin_cl_size,
    uint8_t *shader_records, uint32_t shader_bus, uint32_t uniforms_bus,
    const struct VC4SubmitShaderState *states, uint32_t state_count)
{
    static const uint32_t shader_offsets[3] = { 4, 16, 28 };
    uint32_t offset = 0;
    uint32_t state_index = 0;
    uint32_t i;

    while (offset < bin_cl_size)
    {
        uint32_t packet_size = vc4_bin_packet_size(bin_cl[offset]);

        if (!packet_size || packet_size > bin_cl_size - offset)
            return FALSE;
        if (bin_cl[offset] == VC4_PACKET_GL_SHADER_STATE)
        {
            uint64_t address;

            if (state_index >= state_count)
                return FALSE;
            address = (uint64_t)shader_bus +
                states[state_index].record_offset;
            if (address > UINT32_MAX)
                return FALSE;
            vc4_write_le32(bin_cl + offset + 1,
                (uint32_t)address | states[state_index].pointer_bits);
            state_index++;
        }
        offset += packet_size;
    }

    if (state_index != state_count)
        return FALSE;
    for (state_index = 0; state_index < state_count; state_index++)
    {
        uint8_t *record = shader_records +
            states[state_index].record_offset;

        for (i = 0; i < 3; i++)
        {
            uint64_t address = (uint64_t)uniforms_bus +
                states[state_index].uniform_offset[i];

            if (address > UINT32_MAX)
                return FALSE;
            vc4_write_le32(record + shader_offsets[i] + 4U,
                (uint32_t)address);
        }
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

static BOOL vc4_submit_rcl_plan_valid(const struct VC4SubmitCL *submit,
    const struct VC4BinInfo *bin_info, uint32_t *rcl_size)
{
    const struct VC4SubmitRCLSurface *surfaces[6] = {
        &submit->color_read, &submit->color_write,
        &submit->zs_read, &submit->zs_write,
        &submit->msaa_color_write, &submit->msaa_zs_write
    };
    uint32_t xtiles = (uint32_t)submit->max_x_tile -
        submit->min_x_tile + 1U;
    uint32_t ytiles = (uint32_t)submit->max_y_tile -
        submit->min_y_tile + 1U;
    uint32_t loop_size = 3U;
    uint32_t stores = 0;
    uint64_t size = 11U + 1U;
    uint32_t i;

    if (submit->max_x_tile >= bin_info->tiles_x ||
        submit->max_y_tile >= bin_info->tiles_y)
        return FALSE;

    for (i = 0; i < 6; i++)
    {
        if (surfaces[i]->hindex == UINT32_MAX)
        {
            if (surfaces[i]->offset || surfaces[i]->bits ||
                surfaces[i]->flags)
                return FALSE;
        }
        else if (surfaces[i]->flags & ~1U)
            return FALSE;
    }

    if (submit->msaa_color_write.flags ||
        submit->msaa_color_write.bits ||
        submit->msaa_zs_write.flags ||
        submit->msaa_zs_write.bits)
        return FALSE;
    if (submit->msaa_color_write.hindex != UINT32_MAX)
    {
        if (submit->msaa_color_write.offset & 0xfU)
            return FALSE;
        loop_size += 5U;
        stores++;
    }
    if (submit->msaa_zs_write.hindex != UINT32_MAX)
    {
        if (submit->msaa_zs_write.offset & 0xfU)
            return FALSE;
        loop_size += 5U;
        stores++;
    }
    if (submit->zs_write.hindex != UINT32_MAX)
    {
        loop_size += 7U;
        stores++;
    }
    if (submit->color_write.hindex != UINT32_MAX)
    {
        loop_size += 1U;
        stores++;
    }
    if (!stores)
        return FALSE;
    loop_size += 3U * (stores - 1U);

    if (submit->color_read.hindex != UINT32_MAX)
        loop_size += (submit->color_read.flags & 1U) ? 5U : 7U;
    if (submit->zs_read.hindex != UINT32_MAX)
    {
        if (submit->color_read.hindex != UINT32_MAX)
            loop_size += 10U;
        loop_size += (submit->zs_read.flags & 1U) ? 5U : 7U;
    }
    loop_size += 5U; /* branch to the tile's bin sub-list */

    if (submit->flags & VC4_SUBMIT_USE_CLEAR_COLOR)
        size += 14U + 3U + 7U;
    size += (uint64_t)xtiles * ytiles * loop_size;
    if (size > UINT32_MAX)
        return FALSE;
    *rcl_size = (uint32_t)size;
    return TRUE;
}

static BOOL vc4_submit_emit_simple_rcl(const struct VC4SubmitCL *submit,
    const struct VC4BinInfo *bin_info, uint32_t color_bus,
    uint32_t tile_bus, uint32_t tile_alloc_offset, uint8_t *rcl,
    uint32_t rcl_size)
{
    uint32_t offset = 0;
    uint32_t xi;
    uint32_t yi;
    uint32_t xtiles = (uint32_t)submit->max_x_tile -
        submit->min_x_tile + 1U;
    uint32_t ytiles = (uint32_t)submit->max_y_tile -
        submit->min_y_tile + 1U;
    BOOL positive_x = !(submit->flags & VC4_SUBMIT_FIXED_RCL_ORDER) ||
        (submit->flags & VC4_SUBMIT_RCL_ORDER_INCREASING_X);
    BOOL positive_y = !(submit->flags & VC4_SUBMIT_FIXED_RCL_ORDER) ||
        (submit->flags & VC4_SUBMIT_RCL_ORDER_INCREASING_Y);

    if (submit->color_write.hindex == UINT32_MAX ||
        submit->color_write.flags ||
        (submit->color_write.bits & ~0x00ddU) ||
        submit->color_read.hindex != UINT32_MAX ||
        submit->zs_read.hindex != UINT32_MAX ||
        submit->zs_write.hindex != UINT32_MAX ||
        submit->msaa_color_write.hindex != UINT32_MAX ||
        submit->msaa_zs_write.hindex != UINT32_MAX ||
        (submit->flags & VC4_SUBMIT_USE_CLEAR_COLOR))
        return FALSE;

    rcl[offset++] = 113;
    vc4_write_le32(rcl + offset, color_bus + submit->color_write.offset);
    offset += 4;
    vc4_write_le16(rcl + offset, submit->width);
    offset += 2;
    vc4_write_le16(rcl + offset, submit->height);
    offset += 2;
    vc4_write_le16(rcl + offset, submit->color_write.bits);
    offset += 2;

    for (yi = 0; yi < ytiles; yi++)
    {
        uint32_t y = positive_y ? submit->min_y_tile + yi :
            submit->max_y_tile - yi;

        for (xi = 0; xi < xtiles; xi++)
        {
            uint32_t x = positive_x ? submit->min_x_tile + xi :
                submit->max_x_tile - xi;
            BOOL first = xi == 0 && yi == 0;
            BOOL last = xi == xtiles - 1U && yi == ytiles - 1U;
            uint64_t sublist = (uint64_t)tile_bus + tile_alloc_offset +
                ((uint64_t)y * bin_info->tiles_x + x) * 32U;

            if (sublist > UINT32_MAX || offset > rcl_size - (first ? 10U : 9U))
                return FALSE;
            rcl[offset++] = 115;
            rcl[offset++] = (uint8_t)x;
            rcl[offset++] = (uint8_t)y;
            if (first)
                rcl[offset++] = 8;
            rcl[offset++] = 17;
            vc4_write_le32(rcl + offset, (uint32_t)sublist);
            offset += 4;
            rcl[offset++] = last ? 25 : 24;
        }
    }

    return offset == rcl_size;
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
    APTR validated_bin_cl = NULL;
    APTR validated_uniforms = NULL;
    APTR validated_shader_recs = NULL;
    struct VC4SubmitShaderState *shader_states = NULL;
    uint64_t total_size;
    uint32_t handles_size;
    uint32_t shader_states_size;
    uint32_t shader_state_count = 0;
    uint32_t validated_bin_cl_size = 0;
    uint32_t validated_uniforms_size = 0;
    uint32_t validated_shader_recs_size = 0;
    uint32_t staging_handle = 0;
    uint32_t staging_bus_address;
    uint32_t staging_allocated_size;
    uint32_t staging_shader_offset;
    uint32_t staging_uniform_offset;
    uint32_t staging_used_size;
    uint32_t tile_handle = 0;
    uint32_t tile_bus_address;
    uint32_t tile_allocated_size;
    uint32_t tile_alloc_offset;
    uint32_t tile_alloc_size;
    uint32_t tile_used_size;
    uint32_t planned_rcl_size = 0;
    uint32_t rcl_handle = 0;
    uint32_t rcl_bus_address;
    uint32_t rcl_allocated_size;
    uint32_t color_bus_address;
    uint32_t color_allocated_size;
    APTR staging_map;
    APTR tile_map;
    APTR rcl_map;
    APTR color_map;
    struct VC4BinInfo bin_info = { 0 };
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
        (submit->uniforms_size & (sizeof(uint32_t) - 1U)) != 0 ||
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
    validated_bin_cl = AllocMem(submit->bin_cl_size,
        MEMF_PUBLIC | MEMF_CLEAR);
    validated_uniforms = AllocMem(submit->uniforms_size,
        MEMF_PUBLIC | MEMF_CLEAR);
    validated_shader_recs = AllocMem(submit->shader_rec_size,
        MEMF_PUBLIC | MEMF_CLEAR);
    if (shader_states_size)
        shader_states = AllocMem(shader_states_size,
            MEMF_PUBLIC | MEMF_CLEAR);
    if (!bin_cl_copy || !shader_rec_copy || !uniforms_copy || !handles_copy ||
        !validated_bin_cl ||
        !validated_uniforms ||
        !validated_shader_recs ||
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
        valid = vc4_submit_bin_cl_valid(VC4Base, bin_cl_copy,
            submit->bin_cl_size, handles, submit->bo_handle_count,
            submit->shader_rec_count, shader_states, &shader_state_count,
            validated_bin_cl, &validated_bin_cl_size, &bin_info);
        if (valid && (!validated_bin_cl_size ||
            validated_bin_cl_size > submit->bin_cl_size))
            valid = FALSE;
    }

    if (valid)
    {
        valid = vc4_submit_shader_recs_valid(VC4Base, shader_rec_copy,
            submit->shader_rec_size, handles, submit->bo_handle_count,
            shader_states, shader_state_count, uniforms_copy,
            submit->uniforms_size, validated_shader_recs,
            &validated_shader_recs_size, validated_uniforms,
            &validated_uniforms_size);
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
        if (valid)
            valid = vc4_submit_rcl_plan_valid(submit, &bin_info,
                &planned_rcl_size);
    }

    ReleaseSemaphore(&VC4Base->vc4_Lock);

    if (valid)
    {
        uint64_t used_size;
        uint32_t tile_count = (uint32_t)bin_info.tiles_x *
            bin_info.tiles_y;
        uint64_t tile_size;

        staging_shader_offset = (validated_bin_cl_size + 15U) & ~15U;
        staging_uniform_offset =
            (staging_shader_offset + validated_shader_recs_size + 15U) &
            ~15U;
        used_size = (uint64_t)staging_uniform_offset +
            validated_uniforms_size;
        if (!used_size || used_size > UINT32_MAX)
            valid = FALSE;
        else
            staging_used_size = (uint32_t)used_size;

        tile_alloc_offset =
            (48U * tile_count + 4095U) & ~4095U;
        tile_alloc_size =
            (32U * tile_count + 255U) & ~255U;
        tile_alloc_size += 1024U * 1024U;
        tile_size = (uint64_t)tile_alloc_offset + tile_alloc_size;
        if (!tile_count || tile_size > UINT32_MAX)
            valid = FALSE;
        else
            tile_used_size = (uint32_t)tile_size;
    }

    if (valid)
    {
        /*
         * Exercise the final GPU-visible layout without retaining or
         * executing it. Cross-stream pointers stay zero until all their
         * offsets are patched in a later validation stage.
         */
        if (VC4CreateBO(staging_used_size, 4096, VC4_BOF_NOINIT,
            &staging_handle) != 0 ||
            VC4MapBO(staging_handle, &staging_map, &staging_bus_address,
                &staging_allocated_size) != 0 ||
            staging_used_size > staging_allocated_size)
        {
            valid = FALSE;
        }
        else
        {
            uint64_t staging_end = (uint64_t)staging_bus_address +
                staging_used_size;
            uint64_t shader_bus64 = (uint64_t)staging_bus_address +
                staging_shader_offset;
            uint64_t uniforms_bus64 = (uint64_t)staging_bus_address +
                staging_uniform_offset;
            uint32_t shader_bus;
            uint32_t uniforms_bus;

            if (staging_end > (uint64_t)UINT32_MAX + 1U ||
                (shader_state_count &&
                 (shader_bus64 > UINT32_MAX ||
                  uniforms_bus64 > UINT32_MAX)))
            {
                valid = FALSE;
                goto staging_done;
            }
            if (VC4CreateBO(tile_used_size, 4096, 0, &tile_handle) != 0 ||
                VC4MapBO(tile_handle, &tile_map, &tile_bus_address,
                    &tile_allocated_size) != 0 ||
                tile_used_size > tile_allocated_size ||
                (uint64_t)tile_bus_address + tile_used_size >
                    (uint64_t)UINT32_MAX + 1U)
            {
                valid = FALSE;
                goto staging_done;
            }
            shader_bus = (uint32_t)shader_bus64;
            uniforms_bus = (uint32_t)uniforms_bus64;
            CopyMem(validated_bin_cl, staging_map,
                validated_bin_cl_size);
            CopyMem(validated_shader_recs,
                (uint8_t *)staging_map + staging_shader_offset,
                validated_shader_recs_size);
            CopyMem(validated_uniforms,
                (uint8_t *)staging_map + staging_uniform_offset,
                validated_uniforms_size);
            if (!vc4_submit_patch_staging(staging_map,
                validated_bin_cl_size,
                (uint8_t *)staging_map + staging_shader_offset,
                shader_bus, uniforms_bus, shader_states,
                shader_state_count))
                valid = FALSE;
            if (valid)
            {
                uint8_t *config = (uint8_t *)staging_map +
                    bin_info.config_offset;

                vc4_write_le32(config + 1,
                    tile_bus_address + tile_alloc_offset);
                vc4_write_le32(config + 5, tile_alloc_size);
                vc4_write_le32(config + 9, tile_bus_address);
                config[15] = (config[15] & ~0x78U) |
                    (2U << 5) | (1U << 2);
            }
            if (valid &&
                submit->color_write.hindex != UINT32_MAX &&
                submit->color_read.hindex == UINT32_MAX &&
                submit->zs_read.hindex == UINT32_MAX &&
                submit->zs_write.hindex == UINT32_MAX &&
                submit->msaa_color_write.hindex == UINT32_MAX &&
                submit->msaa_zs_write.hindex == UINT32_MAX &&
                !(submit->flags & VC4_SUBMIT_USE_CLEAR_COLOR))
            {
                if (VC4MapBO(handles[submit->color_write.hindex],
                    &color_map, &color_bus_address,
                    &color_allocated_size) != 0 ||
                    (uint64_t)color_bus_address +
                        submit->color_write.offset > UINT32_MAX ||
                    VC4CreateBO(planned_rcl_size, 4096, VC4_BOF_NOINIT,
                        &rcl_handle) != 0 ||
                    VC4MapBO(rcl_handle, &rcl_map, &rcl_bus_address,
                        &rcl_allocated_size) != 0 ||
                    planned_rcl_size > rcl_allocated_size ||
                    !vc4_submit_emit_simple_rcl(submit, &bin_info,
                        color_bus_address, tile_bus_address,
                        tile_alloc_offset, rcl_map, planned_rcl_size) ||
                    VC4SyncBO(rcl_handle, 0, planned_rcl_size,
                        VC4_SYNC_CPU_TO_GPU) != 0)
                    valid = FALSE;
            }
            if (!valid ||
                VC4SyncBO(staging_handle, 0, staging_used_size,
                VC4_SYNC_CPU_TO_GPU) != 0)
                valid = FALSE;
        }
    }

staging_done:
    if (rcl_handle && VC4FreeBO(rcl_handle) != 0)
        valid = FALSE;
    if (tile_handle && VC4FreeBO(tile_handle) != 0)
        valid = FALSE;
    if (staging_handle && VC4FreeBO(staging_handle) != 0)
        valid = FALSE;

cleanup:
    if (validated_shader_recs)
        FreeMem(validated_shader_recs, submit->shader_rec_size);
    if (validated_uniforms)
        FreeMem(validated_uniforms, submit->uniforms_size);
    if (validated_bin_cl)
        FreeMem(validated_bin_cl, submit->bin_cl_size);
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
