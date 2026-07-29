#ifndef AROS_M68K_EMU68_BOOT_H
#define AROS_M68K_EMU68_BOOT_H

#include <stdint.h>

#define EMU68_BOOT_MAGIC   0x45363842UL /* "E68B" */
#define EMU68_BOOT_ABI     2

#define EMU68_BOOT_FDT_VALID       (1UL << 0)
#define EMU68_BOOT_MEMORY_VALID    (1UL << 1)
#define EMU68_BOOT_BOOTARGS_VALID  (1UL << 2)
#define EMU68_BOOT_FRAMEBUFFER     (1UL << 3)
#define EMU68_BOOT_EXEC_READY      (1UL << 4)

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
};

extern struct Emu68BootContext emu68_boot_context;

void emu68_bootstrap(const void *fdt, void *framebuffer, uint32_t pitch,
                     uint32_t width, uint32_t height)
    __attribute__((noreturn));

#endif
