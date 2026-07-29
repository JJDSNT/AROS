/*
 * Native Emu68 bootstrap.
 *
 * Emu68 has already initialized the Raspberry Pi and its m68k execution
 * environment before entering this code. This layer translates Emu68's
 * register ABI into persistent state which the AROS kernel can consume.
 */

#include "boot.h"

#include <aros/kernel.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

#include "kernel_base.h"
#include "kernel_romtags.h"

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

extern struct TagItem *BootMsg;
extern char __aros_resident_start[];
extern char __aros_resident_end[];
extern void Exec_Supervisor_Trap(void);
extern void emu68_enter_user(void (*entry)(void), void *stack)
    __attribute__((noreturn));
extern BOOL emu68_vtimer_start(ULONG base, ULONG irq_level,
                               ULONG interval_us);
extern void m68k_ExecInstallPreserveAll(struct ExecBase *SysBase);

static struct TagItem emu68_boot_tags[9];

void emu68_set_stage(uint32_t stage)
{
    struct Emu68BootContext *ctx = &emu68_boot_context;

    ctx->stage = stage;

    /*
     * The first page is kept out of the allocator. Leave a big-endian marker
     * immediately above the 68k vector table so a bare-metal monitor can
     * diagnose boot progress before a console is available.
     */
    *(volatile uint32_t *)0x400 = stage;
}

static void coldstart_user(void)
{
    struct Emu68BootContext *ctx = &emu68_boot_context;
    ULONG timer_interval_us;

    emu68_set_stage(EMU68_STAGE_COLDSTART);
    emu68_console_puts("[AROS/Emu68] InitCode COLDSTART in user mode\n");
    InitCode(RTF_COLDSTART, 0);
    ctx->flags |= EMU68_BOOT_COLDSTART_READY;

    if (FindName(&SysBase->DeviceList, "timer.device"))
    {
        ctx->flags |= EMU68_BOOT_TIMER_DEVICE;
        emu68_console_puts("[AROS/Emu68] timer.device initialized\n");
    }
    else
        emu68_console_puts("[AROS/Emu68] timer.device unavailable\n");

    if (FindName(&SysBase->LibList, "utility.library") &&
        FindName(&SysBase->LibList, "oop.library") &&
        FindName(&SysBase->LibList, "hiddclass.hidd"))
        emu68_console_puts(
            "[AROS/Emu68] utility/oop/HIDD residents initialized\n");
    else
        emu68_console_puts(
            "[AROS/Emu68] foundational resident initialization failed\n");

    if (FindName(&SysBase->ResourceList, "bootloader.resource") &&
        FindName(&SysBase->LibList, "gfx.hidd"))
        emu68_console_puts(
            "[AROS/Emu68] bootloader/gfx HIDD residents initialized\n");
    else
        emu68_console_puts(
            "[AROS/Emu68] pre-graphics resident initialization failed\n");

    if (FindName(&SysBase->LibList, "graphics.library"))
        emu68_console_puts(
            "[AROS/Emu68] graphics.library initialized without display driver\n");
    else
        emu68_console_puts(
            "[AROS/Emu68] graphics.library initialization failed\n");

    if (FindName(&SysBase->LibList, "layers.library"))
        emu68_console_puts("[AROS/Emu68] layers.library initialized\n");
    else
        emu68_console_puts("[AROS/Emu68] layers.library initialization failed\n");

    if (FindName(&SysBase->LibList, "keymap.library"))
        emu68_console_puts("[AROS/Emu68] keymap.library initialized\n");
    else
        emu68_console_puts("[AROS/Emu68] keymap.library initialization failed\n");

    if (FindName(&SysBase->DeviceList, "input.device"))
        emu68_console_puts("[AROS/Emu68] input.device initialized\n");
    else
        emu68_console_puts("[AROS/Emu68] input.device initialization failed\n");

    emu68_set_stage(EMU68_STAGE_MULTITASKING);
    emu68_console_puts("[AROS/Emu68] Exec multitasking enabled\n");

    timer_interval_us = SysBase->VBlankFrequency
        ? 1000000UL / SysBase->VBlankFrequency
        : 20000UL;
    if ((ctx->flags & EMU68_BOOT_TIMER_VALID) &&
        timer_interval_us &&
        emu68_vtimer_start(ctx->timer_base, ctx->timer_irq,
                           timer_interval_us))
        emu68_console_puts("[AROS/Emu68] virtual timer enabled\n");
    else
        emu68_console_puts("[AROS/Emu68] virtual timer not found\n");

    if (!emu68_scheduler_selftest_start())
        emu68_console_puts("[AROS/Emu68] failed to start scheduler selftest\n");

    ctx->flags |= EMU68_BOOT_SCHEDULER_ENTER;
    emu68_set_stage(EMU68_STAGE_SCHEDULER);
    Reschedule();
    emu68_set_stage(EMU68_STAGE_SCHED_RETURN);
    emu68_console_puts("[AROS/Emu68] scheduler returned to bootstrap\n");

    for (;;)
        ;
}

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

static int node_is_virtual_timer(const char *name, const uint8_t *limit)
{
    static const char prefix[] = "virtual-timer@";
    uint32_t i;

    for (i = 0; i < sizeof(prefix) - 1; i++)
    {
        if ((const uint8_t *)&name[i] >= limit || name[i] != prefix[i])
            return 0;
    }

    return 1;
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
    int in_virtual_timer = 0;
    int virtual_timer_compatible = 0;

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
            in_virtual_timer =
                depth == 3 && node_is_virtual_timer(name, structure_end);
            if (in_virtual_timer)
            {
                virtual_timer_compatible = 0;
                ctx->timer_base = 0;
                ctx->timer_size = 0;
                ctx->timer_irq = 0;
                ctx->timer_frequency = 0;
            }
            structure += align4((uint32_t)(cursor - structure + 1));
        }
        else if (token == FDT_END_NODE)
        {
            if (depth == 0)
                return;
            if (depth == 3 && in_virtual_timer)
            {
                if (virtual_timer_compatible && ctx->timer_base &&
                    ctx->timer_size && ctx->timer_irq > 0 &&
                    ctx->timer_irq < 8)
                    ctx->flags |= EMU68_BOOT_TIMER_VALID;
                in_virtual_timer = 0;
            }
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
            else if (in_virtual_timer)
            {
                if (bounded_string_equal(name, 11, "compatible") &&
                    length == 23 &&
                    bounded_string_equal((const char *)value, length,
                                         "emu68,virtual-timer-v1"))
                    virtual_timer_compatible = 1;
                else if (bounded_string_equal(name, 4, "reg") &&
                         length >= 2 * sizeof(uint32_t))
                {
                    ctx->timer_base = ((const uint32_t *)value)[0];
                    ctx->timer_size = ((const uint32_t *)value)[1];
                }
                else if (bounded_string_equal(name, 11, "interrupts") &&
                         length >= sizeof(uint32_t))
                    ctx->timer_irq = *(const uint32_t *)value;
                else if (bounded_string_equal(name, 16, "clock-frequency") &&
                         length >= sizeof(uint32_t))
                    ctx->timer_frequency = *(const uint32_t *)value;
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

static void add_boot_tag(uint32_t *index, uint32_t tag, uint32_t data)
{
    emu68_boot_tags[*index].ti_Tag = tag;
    emu68_boot_tags[*index].ti_Data = data;
    (*index)++;
}

static void start_aros(struct Emu68BootContext *ctx)
{
    UWORD *ranges[3];
    struct MemHeader *memory;
    struct ExecBase *sys_base;
    void *user_stack;
    uint32_t lower;
    uint32_t upper;
    uint32_t tag_index = 0;

    if (!(ctx->flags & EMU68_BOOT_MEMORY_VALID))
        return;

    upper = ctx->memory_base + ctx->memory_size;
    if (upper < ctx->memory_base)
        return;

    /*
     * Keep the vector table and the absolute SysBase slot at address 4 out of
     * the allocator. Emu68 has already removed its FDT and the loaded ELF from
     * the top of the advertised memory range.
     */
    lower = ctx->memory_base;
    if (lower < 0x1000)
        lower = 0x1000;
    lower = (lower + 15) & ~15UL;

    if (upper <= lower || upper - lower < 0x10000)
        return;

    add_boot_tag(&tag_index, KRN_KernelBase,
                 (uint32_t)__aros_resident_start);
    add_boot_tag(&tag_index, KRN_KernelLowest,
                 (uint32_t)__aros_resident_start);
    add_boot_tag(&tag_index, KRN_KernelHighest,
                 (uint32_t)__aros_resident_end);
    add_boot_tag(&tag_index, KRN_MEMLower, lower);
    add_boot_tag(&tag_index, KRN_MEMUpper, upper);
    add_boot_tag(&tag_index, KRN_OpenFirmwareTree, (uint32_t)ctx->fdt);
    if (ctx->flags & EMU68_BOOT_BOOTARGS_VALID)
        add_boot_tag(&tag_index, KRN_CmdLine, (uint32_t)ctx->bootargs);
    add_boot_tag(&tag_index, TAG_DONE, 0);

    BootMsg = emu68_boot_tags;
    memory = (struct MemHeader *)lower;
    krnCreateTLSFMemHeader("System Memory", 0, memory, upper - lower,
                           MEMF_FAST | MEMF_PUBLIC | MEMF_KICK | MEMF_LOCAL);

    ranges[0] = (UWORD *)__aros_resident_start;
    ranges[1] = (UWORD *)__aros_resident_end;
    ranges[2] = (UWORD *)~0UL;

    sys_base = krnPrepareExecBase(ranges, memory, BootMsg);
    if (sys_base)
    {
        m68k_ExecInstallPreserveAll(sys_base);
        ctx->exec_base = sys_base;
        ctx->flags |= EMU68_BOOT_EXEC_READY;
        emu68_set_stage(EMU68_STAGE_EXEC_READY);
        emu68_console_puts("[AROS/Emu68] ExecBase ready\n");

        emu68_set_stage(EMU68_STAGE_SINGLETASK);
        emu68_console_puts("[AROS/Emu68] InitCode SINGLETASK\n");
        InitCode(RTF_SINGLETASK, 0);
        ctx->flags |= EMU68_BOOT_KERNEL_READY;
        emu68_set_stage(EMU68_STAGE_KERNEL_READY);
        emu68_console_puts("[AROS/Emu68] kernel.resource ready\n");

        ((volatile void **)0)[8] = Exec_Supervisor_Trap;
        user_stack = AllocMem(64 * 1024, MEMF_PUBLIC | MEMF_CLEAR);
        if (user_stack)
            emu68_enter_user(coldstart_user, user_stack + 64 * 1024);

        emu68_console_puts("[AROS/Emu68] failed to allocate user stack\n");
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
    emu68_boot_context.exec_base = 0;
    emu68_set_stage(EMU68_STAGE_ENTRY);

    if (framebuffer && pitch && width && height)
        emu68_boot_context.flags |= EMU68_BOOT_FRAMEBUFFER;

    emu68_console_init(framebuffer, pitch, width, height);
    emu68_console_puts("[AROS/Emu68] native m68k bootstrap\n");

    parse_fdt(&emu68_boot_context);
    start_aros(&emu68_boot_context);

    for (;;)
        __asm__ volatile ("stop #0x2700");
}
