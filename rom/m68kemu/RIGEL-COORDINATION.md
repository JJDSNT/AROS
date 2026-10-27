# m68kemu: system calls when the chipset is emulated

Hi terminills — thanks for the m68kemu series ([#1855](https://github.com/aros-development-team/AROS/pull/1855)–[#1859](https://github.com/aros-development-team/AROS/pull/1859)). I've been working on an optional chipset backend for m68kemu. It uses [Rigel](https://github.com/JJDSNT/rigel), an OCS chipset library, lives on JJDSNT/AROS `feature/m68kemu-rigel`, and is off by default. It already runs the Copper, bitplane DMA, the Blitter, Paula and the CIAs, with the chipset's frames shown in a window and its sound played through AHI.

Your series and this backend don't meet for programs that only use the OS, or for programs that take over the machine. They do meet for system-friendly programs that use the chipset. Before either of us builds more there, I'd like to agree on the design.

## The problem

On an Amiga the operating system is itself a client of the chipset: graphics and Intuition build Copper lists and draw into bitplanes in Chip RAM, `audio.device` plays on Paula, and the CIA resources and timers run on the CIAs. A system-friendly program reaches the same hardware in two ways: through system calls, and through the registers.

With high-level emulation, system calls become native AROS operations. With the chipset backend, register accesses reach the emulated chipset. A program that does both ends up with two copies of its hardware that don't see each other.

Graphics shows it most clearly:

- **Through the OS:** a program opens a HAM or dual-playfield screen, adds a `UCopList`, allocates sprites with `GetSprite`, and draws with `BltBitMap`.
- **Beside it:** after `OwnBlitter`, the same program writes the Blitter registers directly into its own screen bitmap.

Today `OpenScreen` gives it a native screen whose planes are mirrored from CPU writes. That screen never shows what the Blitter or the Copper did on the chipset, and a HAM screen has no native equivalent.

The same split exists for sound (`audio.device` versus `AUDx` registers), CIAs and timers (`ciaa`/`ciab.resource` versus CIA registers), input (`gameport.device` versus `JOYxDAT`) and the vertical blank (an `AddIntServer` VERTB server versus the chipset's interrupt). How hard each one is varies: sound, for instance, already mixes at the host's AHI from both paths. But the design question is the same for all of them.

## A. m68kemu implements the chipset-facing calls on the chipset

When a program runs with the chipset, the system calls that touch chipset hardware are m68kemu thunks working directly on the emulated chipset. For example, `OpenScreen` builds the screen in Chip RAM with its Copper list, sprites come from the chipset, and `ciab.resource` runs on the emulated CIAs. Your native paths stay as they are for programs without the chipset.

- Small, and self-contained in m68kemu.
- But everything the system itself does on that hardware has to be reimplemented in m68kemu, or isn't available. On a chipset screen that means Intuition's windows, gadgets, menus and text. It duplicates logic AROS already has, and the duplication grows with every call covered.

## B. The emulated chipset is the hardware under native AROS

The emulated chipset becomes hardware that native AROS drives through its own drivers, as on a real Amiga. Concretely:

- a display driver whose screens are planar bitmaps in the program's Chip RAM;
- the CIA and POTGO resources running on the emulated CIAs;
- where it matters, an `audio.device` unit on the emulated Paula.

Native graphics, Intuition and the devices do the work and draw or play on the emulated chipset. AmigaOS's own arbitration then lets the system and the program share it: `OwnBlitter`, `MrgCop` merging a `UCopList`, `GetSprite`, audio allocation keys, `AddICRVector`, interrupt server chains.

- Real coexistence in one run: system UI on chipset screens, and programs mixing system calls and registers.
- Your high-level calls need little change. For example, a screen opened on the chipset's monitor is a chipset screen with no special path in `OpenScreen`.
- It reuses the amiga-m68k drivers (`amigavideo.hidd`, `cia`, `potgo`) rehosted on the emulated chipset, and builds on the generic `planarbm` class and `DDRV_ExternalPlanar`.
- Much more work, partly outside m68kemu. The drivers turn native pointers into 32-bit DMA addresses, there's no public call to remove a display, and the chipset is reached from several tasks.

The two aren't exclusive: A could come first, with B as the route to full coexistence.

## What I'd like to agree on

Which direction makes more sense to you, and how the high-level calls should decide, per program, whether a piece of hardware is native or the emulated chipset. The details and the evidence from the code are in [`RIGEL.md`](RIGEL.md#coexistence-with-the-high-level-emulation), next to this file.
