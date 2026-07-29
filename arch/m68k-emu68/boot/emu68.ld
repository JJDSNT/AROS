/*
 * Emu68 relocates ET_REL images at runtime. Its loader places non-writable
 * allocated sections first, followed by writable allocated sections, then
 * applies the supported m68k RELA relocations.
 */
SECTIONS
{
    .text : ALIGN(4)
    {
        KEEP(*(.text.boot))
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4)
    {
        *(.data .data.*)
    }

    .bss : ALIGN(4)
    {
        *(.bss .bss.* COMMON)
    }

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
    }
}

