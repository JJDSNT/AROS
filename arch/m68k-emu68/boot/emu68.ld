/*
 * Emu68 relocates ET_REL images at runtime. Its loader places non-writable
 * allocated sections first, followed by writable allocated sections, then
 * applies the supported m68k RELA relocations.
 */
SECTIONS
{
    .text : ALIGN(4)
    {
        __aros_resident_start = .;
        KEEP(*(.text.boot))

        /*
         * Port-local ROMTags go here, ahead of every module's.
         *
         * krnRomTagScanner() sweeps this whole range linearly, and on each
         * ROMTag it finds it jumps straight to that tag's rt_EndSkip. A
         * module's rt_EndSkip is a symbol in the module itself, and because
         * this script gathers all .text before all .rodata, some of those
         * spans reach far past the module's own code -- dosboot's covers
         * roughly 70KB, swallowing everything the linker happened to place
         * in between. A ROMTag landing inside such a span is never seen.
         * Being first is the only placement that cannot be skipped.
         */
        KEEP(*(.aros.romtag))

        *(.text .text.*)
        *(.rodata .rodata.*)
        __aros_resident_end = .;
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
