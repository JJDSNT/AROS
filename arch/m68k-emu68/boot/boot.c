/*
 * Native Emu68 bootstrap.
 *
 * Emu68 has already initialized the Raspberry Pi and its m68k execution
 * environment before entering this code. This layer translates Emu68's
 * register ABI into persistent state which the AROS kernel can consume.
 */

#include "boot.h"

#define FDT_MAGIC       0xd00dfeedUL
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

struct FdtHeader
{
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct Emu68BootContext emu68_boot_context;

static uint32_t align4(uint32_t value)
{
    return (value + 3) & ~3UL;
}

static int bounded_string_equal(const char *value, uint32_t value_size,
                                const char *expected)
{
    uint32_t i = 0;

    while (expected[i] != '\0')
    {
        if (i >= value_size || value[i] != expected[i])
            return 0;
        i++;
    }

    return i < value_size && value[i] == '\0';
}

static int node_is_memory(const char *name, const uint8_t *limit)
{
    static const char prefix[] = "memory";
    uint32_t i;

    for (i = 0; i < sizeof(prefix) - 1; i++)
    {
        if ((const uint8_t *)&name[i] >= limit || name[i] != prefix[i])
            return 0;
    }

    return (const uint8_t *)&name[i] < limit &&
           (name[i] == '\0' || name[i] == '@');
}

static const char *fdt_string(const uint8_t *strings, uint32_t strings_size,
                              uint32_t offset)
{
    uint32_t i;

    if (offset >= strings_size)
        return 0;

    for (i = offset; i < strings_size; i++)
    {
        if (strings[i] == '\0')
            return (const char *)&strings[offset];
    }

    return 0;
}

static uint32_t cells_to_u32(const uint32_t *cells, uint32_t count)
{
    if (count == 0)
        return 0;

    /*
     * m68k addresses are 32-bit. For multi-cell FDT values, accept only
     * ranges whose high cells are zero and return the least significant one.
     */
    while (count > 1)
    {
        if (*cells++ != 0)
            return 0;
        count--;
    }

    return *cells;
}

static void parse_fdt(struct Emu68BootContext *ctx)
{
    const struct FdtHeader *header = ctx->fdt;
    const uint8_t *base = ctx->fdt;
    const uint8_t *structure;
    const uint8_t *structure_end;
    const uint8_t *strings;
    uint32_t address_cells = 1;
    uint32_t size_cells = 1;
    uint32_t depth = 0;
    int in_memory = 0;
    int in_chosen = 0;

    if (!header || header->magic != FDT_MAGIC ||
        header->totalsize < sizeof(*header))
        return;

    if (header->off_dt_struct > header->totalsize ||
        header->size_dt_struct > header->totalsize - header->off_dt_struct ||
        header->off_dt_strings > header->totalsize ||
        header->size_dt_strings > header->totalsize - header->off_dt_strings)
        return;

    ctx->fdt_size = header->totalsize;
    ctx->flags |= EMU68_BOOT_FDT_VALID;

    structure = base + header->off_dt_struct;
    structure_end = structure + header->size_dt_struct;
    strings = base + header->off_dt_strings;

    while (structure + sizeof(uint32_t) <= structure_end)
    {
        uint32_t token = *(const uint32_t *)structure;
        structure += sizeof(uint32_t);

        if (token == FDT_BEGIN_NODE)
        {
            const char *name = (const char *)structure;
            const uint8_t *cursor = structure;

            while (cursor < structure_end && *cursor != '\0')
                cursor++;
            if (cursor == structure_end)
                return;

            depth++;
            in_memory = depth == 2 && node_is_memory(name, structure_end);
            in_chosen = depth == 2 &&
                        bounded_string_equal(name,
                                             (uint32_t)(cursor - structure + 1),
                                             "chosen");
            structure += align4((uint32_t)(cursor - structure + 1));
        }
        else if (token == FDT_END_NODE)
        {
            if (depth == 0)
                return;
            if (depth == 2)
            {
                in_memory = 0;
                in_chosen = 0;
            }
            depth--;
        }
        else if (token == FDT_PROP)
        {
            uint32_t length;
            uint32_t name_offset;
            const char *name;
            const uint8_t *value;

            if (structure + 2 * sizeof(uint32_t) > structure_end)
                return;

            length = *(const uint32_t *)structure;
            name_offset = *(const uint32_t *)(structure + sizeof(uint32_t));
            structure += 2 * sizeof(uint32_t);
            if (length > (uint32_t)(structure_end - structure))
                return;

            value = structure;
            name = fdt_string(strings, header->size_dt_strings, name_offset);
            if (!name)
                return;

            if (depth == 1 && length == sizeof(uint32_t))
            {
                if (bounded_string_equal(name, 15, "#address-cells"))
                    address_cells = *(const uint32_t *)value;
                else if (bounded_string_equal(name, 12, "#size-cells"))
                    size_cells = *(const uint32_t *)value;
            }
            else if (in_memory &&
                     bounded_string_equal(name, 4, "reg") &&
                     length >= (address_cells + size_cells) * sizeof(uint32_t))
            {
                const uint32_t *cells = (const uint32_t *)value;
                uint32_t memory_base = cells_to_u32(cells, address_cells);
                uint32_t memory_size =
                    cells_to_u32(cells + address_cells, size_cells);

                if (memory_size != 0)
                {
                    ctx->memory_base = memory_base;
                    ctx->memory_size = memory_size;
                    ctx->flags |= EMU68_BOOT_MEMORY_VALID;
                }
            }
            else if (in_chosen &&
                     bounded_string_equal(name, 9, "bootargs") &&
                     length != 0)
            {
                ctx->bootargs = (const char *)value;
                ctx->bootargs_size = length;
                ctx->flags |= EMU68_BOOT_BOOTARGS_VALID;
            }

            structure += align4(length);
        }
        else if (token == FDT_NOP)
        {
            continue;
        }
        else if (token == FDT_END)
        {
            return;
        }
        else
        {
            return;
        }
    }
}

void emu68_bootstrap(const void *fdt, void *framebuffer, uint32_t pitch,
                     uint32_t width, uint32_t height)
{
    emu68_boot_context.magic = EMU68_BOOT_MAGIC;
    emu68_boot_context.abi_version = EMU68_BOOT_ABI;
    emu68_boot_context.flags = 0;
    emu68_boot_context.fdt = fdt;
    emu68_boot_context.fdt_size = 0;
    emu68_boot_context.framebuffer = framebuffer;
    emu68_boot_context.framebuffer_pitch = pitch;
    emu68_boot_context.framebuffer_width = width;
    emu68_boot_context.framebuffer_height = height;
    emu68_boot_context.memory_base = 0;
    emu68_boot_context.memory_size = 0;
    emu68_boot_context.bootargs = 0;
    emu68_boot_context.bootargs_size = 0;

    if (framebuffer && pitch && width && height)
        emu68_boot_context.flags |= EMU68_BOOT_FRAMEBUFFER;

    parse_fdt(&emu68_boot_context);

    for (;;)
        __asm__ volatile ("stop #0x2700");
}
