# m68kemu: system calls when the chipset is emulated

Hi terminills — thanks for the m68kemu series ([#1855](https://github.com/aros-development-team/AROS/pull/1855)–[#1859](https://github.com/aros-development-team/AROS/pull/1859)). I've been working on an optional chipset backend for m68kemu. It uses [Rigel](https://github.com/JJDSNT/rigel), an OCS chipset library, lives on JJDSNT/AROS `feature/m68kemu-rigel`, and is off by default. It already runs the Copper, bitplane DMA, the Blitter, Paula and the CIAs, with the chipset's frames shown in a window and its sound played through AHI.

Your series and this backend don't meet for programs that only use the OS, or for programs that take over the machine. They do meet for system-friendly programs that use the chipset, and I'd like us to agree on a design for those before either of us builds more there.

## The problem

On an Amiga the operating system is itself a client of the chipset:

- graphics and Intuition build Copper lists and draw into bitplanes in Chip RAM;
- `audio.device` plays on Paula;
- the CIA resources and timers run on the CIAs.

A system-friendly program reaches the same hardware in two ways: through system calls, and through the registers. With high-level emulation, the calls become native operations. With the chipset backend, the registers reach the emulated chipset. A program that does both ends up with two copies of its hardware that don't see each other.

Graphics shows it most clearly. Say a program:

- opens a HAM or dual-playfield screen;
- adds a `UCopList`;
- allocates sprites with `GetSprite`;
- draws with `BltBitMap`;
- after `OwnBlitter`, writes the Blitter registers directly into its own screen bitmap.

A native screen mirrored from CPU writes never shows what the Blitter or the Copper did, and a HAM screen has no native equivalent. The same split exists elsewhere, though how hard each case is varies (sound, for instance, already mixes at the host's AHI from both paths):

- sound: `audio.device` versus the `AUDx` registers;
- CIAs and timers: `ciaa`/`ciab.resource` versus the CIA registers;
- input: `gameport.device` versus `JOYxDAT`;
- vertical blank: an `AddIntServer` VERTB server versus the chipset's interrupt.

## Two designs

**A. m68kemu implements the chipset-facing calls on the emulated chipset.** When a program runs with the chipset, `OpenScreen` builds the screen and its Copper list in Chip RAM, sprites come from the chipset, `ciab.resource` runs on the emulated CIAs, and so on. It's small and self-contained. The cost is that everything the system itself does on that hardware has to be reimplemented in m68kemu. On a chipset screen, that means Intuition's windows, gadgets, menus and text. The duplication grows with every call covered.

**B. The emulated chipset is the hardware under native AROS.** Native AROS drives the chipset through its own drivers, as on a real Amiga:

- a display driver whose screens are planar bitmaps in the program's Chip RAM;
- the CIA and POTGO resources on the emulated CIAs;
- where it matters, an `audio.device` unit on the emulated Paula.

Native graphics, Intuition and the devices keep doing the work, and draw and play on the emulated chipset. The system and the program then share it through AmigaOS's own arbitration, as they did on real hardware: `OwnBlitter`, `MrgCop` merging a `UCopList`, `GetSprite`, audio allocation keys, `AddICRVector`, interrupt server chains.

Your high-level calls need little change: a screen opened on the chipset's monitor is simply a chipset screen. B reuses the amiga-m68k drivers (`amigavideo.hidd`, `cia`, `potgo`) rehosted on the emulated chipset, plus the generic `planarbm` class and `DDRV_ExternalPlanar`. It's much more work, though, mostly in the display driver, and some of it falls outside m68kemu.

## What I'd suggest

B as the target, with A used only where the two give the same result. The boundary follows the kind of call:

- **Calls that only control the chipset**, with no system rendering behind them: `WaitTOF`, `WaitBlit`, `OwnBlitter`/`DisownBlitter`, `LoadView(NULL)`, the vertical blank, CIA timers. Implementing these directly on the emulated chipset is what B would do anyway.
- **Calls where the system draws or keeps display state**: `OpenScreen`, windows, gadgets, menus, text, `MrgCop` with the system's list, system sprites. These belong to B, and A shouldn't grow into them.

The rule that keeps the two coherent is that each call has exactly one implementation per run mode.

For your series, this would mean the high-level calls keep their native paths when there's no chipset, and the display state they own goes through the chipset's monitor when there is one. Does that work for you? Is there anything in your plans for screens, devices or resources that would make it hard? The details and the evidence from the code are in [`RIGEL.md`](RIGEL.md#coexistence-with-the-high-level-emulation), next to this file.
