# m68kemu: the chipset backend and the high-level emulation

Hi terminills — thanks for the m68kemu series ([#1855](https://github.com/aros-development-team/AROS/pull/1855)–[#1859](https://github.com/aros-development-team/AROS/pull/1859)). I've been working on another part of m68kemu: an optional chipset backend that uses [Rigel](https://github.com/JJDSNT/rigel), an OCS chipset library, for programs that use the custom chips. It's on JJDSNT/AROS `feature/m68kemu-rigel` and is off by default (`--enable-m68kemu-rigel`, `RunHunkEx`/`RunFileEx` with `M68KEMU_RUN_RIGEL`). So far it covers Chip/Fast memory, Exec interrupt servers and handlers, the Copper, a display window, CIAs with keyboard and mouse, and Paula through AHI.

Our two directions don't meet for programs that only use the OS, or for programs that take over the machine. They do meet for system-friendly programs that use the chipset:

- through the OS: HAM/EHB/dual-playfield screens, `UCopList`, sprites, `audio.device`, `ciab.resource`;
- alongside it: `OwnBlitter` plus direct Blitter writes into their own screen, `audio.device` channels driven through the `AUDx` registers, `JOY1DAT`.

The same program reaches the same hardware both ways, and I'd like to agree on a design before either of us builds more in that area. I see two candidates:

- **A. One owner per run.** When a program starts, its display, Paula, CIAs, interrupts and input go either to your native path or to the chipset, never both. In chipset mode, m68kemu thunks implement the relevant calls directly on the chipset. It's small and self-contained. The cost is that system UI on a chipset screen (windows, gadgets, menus on a HAM screen) has to be reimplemented in m68kemu, or isn't available.
- **B. The chipset as the shared truth.** Native Intuition and graphics stay in charge of deciding what to draw, but a chipset screen is a planar bitmap in the program's Chip RAM, shown by a display driver whose hardware is the emulated chipset. In the same way, the CIA resources run on the emulated CIAs. Sound needs little: Paula's output already goes to the host's AHI, where it mixes with your native `audio.device`. Only a program that starts a sound through `audio.device` and then pokes the same channel's registers would need `audio.device` on the emulated Paula. The AmigaOS arbitration APIs (`OwnBlitter`, `MrgCop`, `GetSprite`, audio allocation keys, `AddICRVector`) then let the OS and the program share the hardware, as on a real Amiga. This would reuse the amiga-m68k drivers (mainly `amigavideo.hidd`, plus `cia` and `potgo`), rehosted on the emulated chipset. It also builds on the generic `planarbm` class and `DDRV_ExternalPlanar`. Your `OpenScreen` would need no special path: a screen opened on that monitor would simply be a chipset screen. It's much more work: the drivers cast pointers to 32-bit DMA addresses, there's no public call to remove a display, and access to the chipset from several tasks needs serialising.

They aren't exclusive. A could come first, with B as the route to full coexistence.

Either way, a few points in the series would help:

1. **Allocate anything a DMA engine reads with `MEMF_CHIP`.** That covers the screen planes `OpenScreen` allocates (currently `M68KEmu_HeapAlloc(ctx, size, 0)`), sprite data, Copper lists and audio buffers. It changes nothing without the chipset.
2. **One interrupt model with two sources.** Your VERTB stub (`0x140`, vector `$6C`, host timer) overlaps my dispatcher stubs (`0x110–0x217`, full server chains plus `SetIntVector` handlers). I'd suggest one dispatcher, fed by your host-timer VBlank when there's no chipset and by the chipset when there is. I'm happy to do that on top of your series.
3. **CIA decode order:** with the chipset, it gets `$A00000–$DFFFFF` first, and your stand-in handles everything else.
4. **CPU model per run mode:** LC040 for FPU trapping, 68000 for A500 chipset programs.

There's also a constraint that applies to both designs. `Wait()`/`WaitPort()` block natively, so the chipset stops in time while the program waits, which means Copper effects and sound freeze. That needs solving on my side regardless of which design we pick.

The details and evidence are in [`RIGEL.md`](RIGEL.md#coexistence-with-the-high-level-emulation) ("Coexistence with the high-level emulation"), next to this file. Which direction makes more sense to you?
