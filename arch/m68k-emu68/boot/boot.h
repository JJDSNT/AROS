#ifndef AROS_M68K_EMU68_BOOT_H
#define AROS_M68K_EMU68_BOOT_H

#include <stdint.h>

#define EMU68_BOOT_MAGIC   0x45363842UL /* "E68B" */
#define EMU68_BOOT_ABI     5

#define EMU68_BOOT_FDT_VALID       (1UL << 0)
#define EMU68_BOOT_MEMORY_VALID    (1UL << 1)
#define EMU68_BOOT_BOOTARGS_VALID  (1UL << 2)
#define EMU68_BOOT_FRAMEBUFFER     (1UL << 3)
#define EMU68_BOOT_EXEC_READY      (1UL << 4)
#define EMU68_BOOT_KERNEL_READY    (1UL << 5)
#define EMU68_BOOT_COLDSTART_READY (1UL << 6)
#define EMU68_BOOT_SCHEDULER_ENTER (1UL << 7)
#define EMU68_BOOT_TASK_RUNNING    (1UL << 8)

#define EMU68_STAGE_ENTRY          0x45303031UL /* "E001" */
#define EMU68_STAGE_EXEC_READY     0x45303032UL /* "E002" */
#define EMU68_STAGE_SINGLETASK     0x45303033UL /* "E003" */
#define EMU68_STAGE_KERNEL_READY   0x45303034UL /* "E004" */
#define EMU68_STAGE_COLDSTART      0x45303035UL /* "E005" */
#define EMU68_STAGE_MULTITASKING   0x45303036UL /* "E006" */
#define EMU68_STAGE_SCHEDULER      0x45303037UL /* "E007" */
#define EMU68_STAGE_TASK_RUNNING   0x45303038UL /* "E008" */
#define EMU68_STAGE_SCHED_RETURN   0x45303039UL /* "E009" */

struct Emu68BootContext
{
    uint32_t magic;
    uint32_t abi_version;
    uint32_t flags;

    const void *fdt;
    uint32_t fdt_size;

    void *framebuffer;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;

    uint32_t memory_base;
    uint32_t memory_size;

    const char *bootargs;
    uint32_t bootargs_size;

    void *exec_base;
    uint32_t stage;
};

extern struct Emu68BootContext emu68_boot_context;

void emu68_bootstrap(const void *fdt, void *framebuffer, uint32_t pitch,
                     uint32_t width, uint32_t height)
    __attribute__((noreturn));

void emu68_console_init(void *framebuffer, uint32_t pitch,
                        uint32_t width, uint32_t height);
int emu68_console_putc(int chr);
void emu68_console_puts(const char *text);

#endif
