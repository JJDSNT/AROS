# m68kemu: graphics calls when the chipset is emulated

Hi terminills — thanks for the m68kemu series ([#1855](https://github.com/aros-development-team/AROS/pull/1855)–[#1859](https://github.com/aros-development-team/AROS/pull/1859)). I've been working on an optional chipset backend for m68kemu. It uses [Rigel](https://github.com/JJDSNT/rigel), an OCS chipset library, lives on JJDSNT/AROS `feature/m68kemu-rigel`, and is off by default. It already runs Copper lists, bitplane DMA and the Blitter, and shows the chipset's frames in a window.

Before either of us builds more, I'd like to agree on one thing: how graphics calls are handled for programs that use the chipset while staying system-friendly. Some examples:

- `OpenScreen` with HAM, EHB or dual playfield, `LoadRGB4`, `ScrollVPort`;
- Copper effects through `UCopList`/`MrgCop`;
- hardware sprites through `GetSprite`/`MoveSprite`/`ChangeSprite`;
- `OwnBlitter`/`WaitBlit`, followed by direct Blitter writes into the program's own screen bitmap.

In your series, `OpenScreen` gives the program a native screen, and its planes are mirrored from CPU writes. With the chipset backend, the same program's Copper lists, sprites and Blitter run on the emulated chipset. A Blitter write or a Copper effect never reaches a mirrored native screen, and a HAM screen has no native equivalent. So the question is: who implements these graphics calls when the program's display is the emulated chipset?

## A. m68kemu implements them on the chipset

When a program runs with the chipset, the graphics calls that touch its display (`OpenScreen` and its ViewPort, colours, `MrgCop`, sprites, blitter ownership) are m68kemu thunks working directly on the emulated chipset. They build the screen in the program's Chip RAM and its Copper list. Native screens and mirroring stay as they are for programs without the chipset.

- Small, and self-contained in m68kemu.
- But the system's own rendering on a chipset screen (window borders, gadgets, menus, text drawn by Intuition) has to be reimplemented in m68kemu, or isn't available. That duplicates graphics and Intuition logic AROS already has.

## B. Native graphics renders to a chipset monitor

The emulated chipset becomes an AROS display: a driver whose hardware is the emulated chipset and whose screens are planar bitmaps in the program's Chip RAM. Native graphics and Intuition do all the work, as they would on a real Amiga. A screen opened on that monitor is a chipset screen. `MrgCop` merges the program's `UCopList` into the driver's list, `GetSprite` allocates the chipset's sprites, and `OwnBlitter` arbitrates the chipset's Blitter. The program's direct register writes then land on the same chipset the system draws with.

- Real coexistence: system UI on chipset screens, and programs mixing graphics calls and registers.
- `OpenScreen` in your series needs no special path. Opening the screen on that monitor is the whole difference.
- It reuses the amiga-m68k `amigavideo.hidd` rehosted on the emulated chipset, the generic `planarbm` class, and `DDRV_ExternalPlanar`.
- Much more work. The driver turns native pointers into 32-bit DMA addresses (planes, sprites, Copper lists), there's no public call to remove a display, and the chipset is reached from several tasks.

The two aren't exclusive: A could come first, with B as the route to full coexistence.

## What would help in either case

- Screen planes (and anything else the chipset reads) allocated with `MEMF_CHIP`. `OpenScreen` currently uses `M68KEmu_HeapAlloc(ctx, size, 0)`. Without the chipset this changes nothing.
- A single place where `OpenScreen` decides between a native screen and a chipset screen, so the rest of the graphics calls follow that screen.

The details and the evidence from the code are in [`RIGEL.md`](RIGEL.md#coexistence-with-the-high-level-emulation), next to this file. Which direction makes more sense to you?
