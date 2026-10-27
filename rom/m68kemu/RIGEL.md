# Optional Rigel integration

## Scope

Rigel owns classic Amiga chipset behavior. Moira remains the CPU core, and
`m68kemu.library` owns the address map, native AROS LVO thunks, Hunk loading,
contained memory, and the policy connecting CPU and chipset. The integration
does not add a Kickstart environment, machine scheduler, or frontend.

The legacy `RunHunk()` and `RunFile()` entry points do not initialize Rigel and
retain their previous custom-register fallback. Rigel is selected explicitly
through `RunHunkEx()` or `RunFileEx()` with `M68KEMU_RUN_RIGEL`.

## Build

Initialize the pinned dependencies:

```sh
git submodule update --init rom/m68kemu/Moira rom/m68kemu/Rigel
```

Configure a fresh AROS build tree with the optional integration enabled:

```sh
/path/to/AROS/configure --target=linux-x86_64 --enable-m68kemu-rigel
make kernel-m68kemu-library
```

`kernel-m68kemu-library` depends on `linklibs-rigel` only when configured
with the option. A GNU toolchain built without GCC's `libatomic` fails to
link the C++ runtime with `cannot find -latomic`; a full build provides it,
and for a single-target build `make linklibs-atomic-helpers` supplies AROS's
minimal replacement.

The AROS build invokes Rigel's CMake build as a static library with its tests,
harness, hosted environment hooks, hosted allocation, hosted RTC, and SIMD
disabled. `m68kemu_rigel.c` supplies AROS allocation callbacks.

## Distribution blocker

Rigel currently has no repository `LICENSE` file or other explicit license
grant (Rigel `ISSUE-0013`). This does not block local technical validation, but it does block safe
redistribution of Rigel with AROS. The Rigel copyright holder must choose and
publish a license before this integration can be distributed.

## Implemented bridge

- **Lifecycle:** a Rigel context is created on explicit opt-in and destroyed
  before containment memory.
- **Memory:** Rigel DMA callbacks read and write the lowest 512 KiB of the same
  big-endian containment block used by Moira.
- **MMIO:** byte and word accesses to the classic I/O space
  (`$A00000-$DFFFFF`: CIAs and custom chips) are forwarded through
  `rigel_mmio_read()` and `rigel_mmio_write()`; Rigel decodes them, and
  unmapped addresses read 0. The Fast heap skips that range.
- **Timing:** after each Moira instruction, its elapsed CPU-cycle delta is passed
  to `rigel_step_cpu_cycles()`. The default Rigel ratio is two CPU cycles per
  CCK and retains fractional cycles.
- **Interrupt level:** Rigel's IPL is applied with `Moira::setIPL()` before the
  next instruction.
- **Chip RAM allocation:** enabling Rigel splits the fresh containment heap
  at 512 KiB into a Chip free list (`0x10400`-`0x7ffff`) and a Fast free list
  (512 KiB up to the stack). Emulated `AllocMem()`, `AllocVec()` and pools
  honor `MEMF_CHIP`, `MEMF_FAST` and `MEMF_CLEAR`; `MEMF_CHIP` never falls
  back to Fast. `TypeOfMem()` and `AvailMem()` report the split.
- **Hunk memory attributes:** `RunFileEx()` honors `HUNKF_CHIP`, `HUNKF_FAST`
  and the extended attribute longword in the Hunk header, and clears every
  hunk. `RunHunkEx()` receives an already loaded segment list whose
  attributes `LoadSeg()` has consumed, so its hunks are placed in Fast.
- **Interrupts:** in Rigel mode the program starts in supervisor mode with the
  interrupt mask at 0 and the `INTENA` master bit set, like an ordinary Amiga
  program. Unset level 1-6 autovectors point to contained 68k dispatchers
  (`m68kemu_interrupts.c`) that run Exec server chains and `SetIntVector()`
  handlers with Exec's register conventions, acknowledge chain interrupts,
  and drop pending bits nobody handles. Programs may still install their own
  handler by writing the autovector.
- **Exec interrupt API:** `AddIntServer()` (ordered by `ln_Pri`, enables the
  `INTENA` bit), `RemIntServer()` (disables it with the last server),
  `SetIntVector()`, and in Rigel mode `Disable()`/`Enable()` nesting on the
  contained `INTENA` master bit instead of the host.
- **Display:** with `M68KEMU_RUN_DISPLAY`, every `RIGEL_EVENT_FRAME_READY`
  hands the completed frame to the context's presenter
  (`struct M68KEmuPresenter`). The AROS presenter (`m68kemu_display.c`)
  opens a window on the default public screen at the first frame, resizes
  it when the frame size changes (the first frame precedes the program's
  display setup), copies frames with `WritePixelArray()`, paces to the
  frame rate with `timer.device`, and stops the program with `RETURN_WARN`
  on the close gadget or Ctrl-C. Window and timer belong to the
  task that runs the program and are released when it returns.
- **Input:** `M68KEmu_RigelInputKey()`, `M68KEmu_RigelInputMouse()` and
  `M68KEmu_RigelInputButtons()` feed raw keys, port 0 mouse counters, the
  left button (CIA-A `/FIR0`) and the right button (`POTY`). Keys are queued
  and handed to Rigel one per frame. The display window forwards its raw
  keys (without auto-repeat), mouse movement and buttons; Escape is an
  ordinary key.
- **Audio:** with a presenter `audio_rate`, the bridge samples
  `rigel_get_audio_sample()` at that rate of chipset time (CPU cycles
  against `rigel_get_clock_hz()`) and hands each frame's interleaved 16-bit
  stereo batch to `presenter.audio()` before the frame. The AROS presenter
  streams it at 22050 Hz to `ahi.device` unit 0 with four chained
  `CMD_WRITE` requests, and runs silently when no AHI unit opens. The AHI
  declarations come from `workbench/devs/AHI/Include/C`, so building the
  library needs no built AHI.
- **Launching:** `RunM68K [RIGEL] [DISPLAY] <file> [args]` selects the
  chipset and the display; without switches it behaves as before.
- **Logging:** Rigel's text log goes to the AROS debug log through
  `config.log_fn`; its structured trace events are discarded.
- **Compatibility:** without `M68KEMU_RUN_RIGEL` the heap is not split, the
  mask stays at 7, and Hunk loading, containment, fake library bases, and
  native LVO dispatch behave as before.

Timing is instruction-boundary accurate with the current M68040 Moira setup;
it is not synchronized to an individual bus access within an instruction.

## Validation

The host suite requires a C++20 compiler and CMake:

```sh
cd developer/debug/test/m68kemu_host
make test
```

`test_rigel_integration` executes 68k code that calls a native LVO thunk,
programs Rigel's Blitter through custom MMIO, and copies a word between two
addresses in shared Chip RAM while chipset time advances from Moira cycles.

`test_rigel_audio` is the milestone 6 acceptance test. It runs
`m68k/rigel_audio.s` with a presenter that records audio and checks 440-441
samples per frame at 22050 Hz, initial silence, a 440 Hz tone on the left
and silence on the right. The program's block-interrupt count is an
expected failure until Rigel `ISSUE-0018` is fixed; a patched Rigel copy
passes it with 440 interrupts per second. `M68KEMU_WAV=file.wav` writes the
recorded audio.

`test_rigel_input` is the milestone 5 acceptance test. It checks that the
Fast heap never reaches the I/O space, then runs `m68k/rigel_input.s`,
injecting keys, mouse movement and buttons from the per-frame callback;
the program reads them through CIA-A and the custom chips and counts CIA-A
timer A at 100 Hz.

`test_rigel_display` is the milestone 4 acceptance test. It runs
`m68k/rigel_display.s` with a presenter that records frames, checks frame
order and duration, locates the checkerboard and both colours, sees
`COLOR01` change every frame, stops a run early through the presenter, and
checks that the left button turns the background red and Escape ends the
program.
`M68KEMU_DUMP=file.ppm ./test_rigel_display` writes the last frame.

On hosted AROS (linux-x86_64), `RunM68K DISPLAY rigel_display 1000` shows
the animated checkerboard in a window. Measured there: about 5.3 ms of
emulation and 0.3 ms of drawing per frame, the rest spent pacing, so the
program runs at the PAL 50 Hz frame rate.

`test_rigel_exec_irq` is the milestone 3 acceptance test. It checks server
ordering and the non-Rigel API directly, runs `m68k/rigel_exec_irq.s`, and
then inspects Rigel's completed frame: black above the Copper `WAIT` line,
red from line `$80` down.

`test_rigel_chip_irq` is the milestone 2 acceptance test. It checks the
Chip/Fast heap, Hunk attribute placement and the short relocation formats
directly, then runs `m68k/rigel_chip_irq.s`, a real Hunk executable with
`bss_c` and `data_f` sections, which follows the acceptance steps below and
returns the failing step number in `D0`. The program runs twice: assembled
with `-kick1hunks` (`HUNK_RELOC32` only) and with vasm's defaults
(`HUNK_DREL32` and a symbol table). Rigel state is inspected only through
its public API.

The assembled programs are committed as `m68k/rigel_chip_irq*_hunk.h`, so the
suite needs no m68k toolchain. After editing the source, regenerate it with
vasm from the `amigadev/crosstools:m68k-amigaos` Docker image through
Rigel's `scripts/m68k` wrapper:

```sh
make fixtures
```

## Coexistence with the high-level emulation

`m68kemu.library` grows in two directions. The high-level one (upstream pull
requests #1855-#1859) runs system-friendly programs on native AROS: system
calls become native operations, screens become native screens whose planes
are mirrored from CPU writes, devices get native twins, and where programs
touch hardware there are light stand-ins (a CIA with timers derived from the
E clock, a 50 Hz VBlank from the host timer). The Rigel direction runs the
chipset itself. Programs fall into three groups:

1. **Operating system only** (windows, menus, gadgets): high-level
   emulation alone; Rigel is not involved.
2. **System-friendly programs that use the chipset**, through system calls,
   through registers, or both. This is where the two directions meet.
3. **Programs that take the machine over**: Rigel alone. Running WHDLoad
   *slaves* with `m68kemu.library` as the WHDLoad host (the `resload_*` API
   provided natively, Rigel as the chipset) is a possible future route.

### The problem

On an Amiga the operating system is itself a client of the chipset:
graphics and Intuition build Copper lists and draw into bitplanes in Chip
RAM, `audio.device` plays on Paula, the CIA resources and timers run on the
CIAs. A group 2 program reaches that same hardware both through system calls
and through registers. With high-level emulation the calls become native
operations while the registers reach Rigel, so the program ends up with two
copies of its hardware that do not see each other.

Graphics shows it most clearly. A program opens a HAM or dual-playfield
screen, adds a `UCopList`, allocates sprites with `GetSprite`, draws with
`BltBitMap`, and after `OwnBlitter` writes the Blitter registers directly
into its own screen bitmap. A native screen mirrored from CPU writes never
shows what the Blitter or the Copper did, and a HAM screen has no native
equivalent. The same split exists for sound (`audio.device` and `AUDx`),
CIAs and timers (`ciaa`/`ciab.resource` and the CIA registers), input
(`gameport.device` and `JOYxDAT`) and the vertical blank (an `AddIntServer`
VERTB server and the chipset's interrupt). How hard each one is varies:
sound, for instance, already mixes at the host's AHI from both paths.

### Option A: m68kemu implements the chipset-facing calls on the chipset

When a program runs with Rigel, the system calls that touch chipset
hardware are m68kemu thunks working directly on Rigel: `OpenScreen` builds
the screen and its Copper list in Chip RAM, sprites come from the chipset,
`ciab.resource` runs on Rigel's CIAs. The native paths stay as they are for
programs without Rigel.

- **For:** small and self-contained in `m68kemu.library`.
- **Against:** everything the system itself does on that hardware must be
  reimplemented in m68kemu or is unavailable: on a chipset screen, that is
  Intuition's windows, gadgets, menus and text. It duplicates logic AROS
  already has, and the duplication grows with every call covered.

### Option B: the emulated chipset is the hardware under native AROS

Rigel becomes hardware that native AROS drives through its own drivers, as
on a real Amiga: a display driver whose screens are planar bitmaps in the
program's Chip RAM, the CIA and POTGO resources on Rigel's CIAs, and, where
it matters, an `audio.device` unit on Rigel's Paula. Native graphics,
Intuition and the devices keep doing the work; they draw and play on Rigel.
The truth cannot be the native side, because chipset effects (HAM, Copper,
sprites, DMA writes) have no native representation, while everything the
native side does can be done on the chipset.

The system and the program then share the hardware through the arbitration
AmigaOS already has, because a real Amiga's system and programs shared one
chipset: `OwnBlitter`/`WaitBlit`, `MrgCop` merging a `UCopList` into the
system's list, `GetSprite`, `audio.device` allocation keys, `AddICRVector`,
Exec interrupt server chains. Programs that bypass it would break a real
Amiga's system too; they belong to group 3.

- **For:** true coexistence in one run, including system UI on chipset
  screens and programs that mix calls and registers; reuses AROS's drivers
  and the system's arbitration; the high-level calls need little change (a
  screen opened on the chipset monitor is a chipset screen).
- **Against:** much more work, partly outside `m68kemu.library`.

What the AROS code shows today:

| Need | State | Evidence |
| --- | --- | --- |
| Display driver on Rigel | ~7,200 lines; ~115 direct register accesses through `(struct Custom *)0xdff000`; DMA addresses made by casting native pointers to `ULONG` (planes, sprites, Copper lists), which must become Chip RAM offsets on a 64-bit host; one inline 68k assembly helper; Chip RAM from native `AllocMem(MEMF_CHIP)` | `arch/m68k-amiga/hidd/amigavideo` |
| Native drawing into Chip RAM planes | available: the generic planar bitmap class can wrap an existing `BitMap` | `rom/hidds/gfx/gfx_planarbitmapclass.c`, `aoHidd_PlanarBM_BitMap` |
| Screens whose planes the caller owns | available on AmigaVideo since September 2026 | `DDRV_ExternalPlanar`, commit e3ac66f33a |
| A monitor per program | adding: `AddDisplayDriverA()`; removing: no public call, only the boot-display handover path expunges a display | `rom/graphics/adddisplaydrivera.c`, `graphics_driver.c` |
| CIA and POTGO resources | a handful of hardware accesses, C | `arch/m68k-amiga/cia`, `arch/m68k-amiga/potgo` |
| Paula `audio.device`, only for programs that start a sound through it and then change the same channel's registers | ~24 hardware accesses, C | `arch/m68k-amiga/devs/audio/audio_hardware.c` |

Beyond the drivers it needs Chip RAM for native code (the rehosted drivers
allocate from, and translate pointers into, the program's containment Chip
heap), serialised access to a Rigel context from several tasks, native
interrupt servers run by the dispatcher next to 68k ones, and a monitor
lifecycle (one chipset monitor that attaches to the running program, or a
graphics.library call to remove a display). How Intuition moves the pointer
onto a monitor added at run time is unverified.

### Recommendation: B as the target, A only where both agree

Both can exist if the boundary follows the kind of call, not the time:

- **Calls that only control the chipset**, with no system rendering behind
  them: `WaitTOF`, `WaitBlit`, `OwnBlitter`/`DisownBlitter`,
  `LoadView(NULL)`, the vertical blank, CIA timers. Implementing these
  directly on Rigel (A) is what B would do anyway, since native graphics
  would only pass them to the driver. Here A is not a stopgap, it is the
  implementation.
- **Calls where the system draws or keeps display state**: `OpenScreen`,
  windows, gadgets, menus, text, `MrgCop` with the system's list, system
  sprites. These belong to B. A must not grow into them, because that is
  where duplication starts.

The rule that keeps both coherent: **each call has exactly one
implementation per run mode**, and A's part never extends into the system's
own rendering.

### Order of work

1. Chipset-control calls on Rigel. This already serves programs that take
   the display or the Blitter through system calls and then use the
   hardware.
2. Chipset time while the program waits (below), which every group 2
   program needs.
3. The chipset monitor (B), starting with a feasibility study of
   `amigavideo.hidd` rehosted on Rigel.

### Prerequisite: chipset time while the program waits

System-friendly programs spend most of their time in `Wait()` or
`WaitPort()`. Those thunks call the native functions and block, so the CPU
loop stops, and with it Rigel's time: a Copper effect, a playing sample or
a CIA timer freezes until the program wakes. Rigel must advance in real
time while the CPU waits, for example by stepping it to wall-clock time
from the blocking thunks or from a chipset task, delivering interrupts to
the program when it resumes.

### Implementation details to settle with the high-level series

Not design questions, but points where the two code bases touch:

- Memory a DMA engine reads (screen planes, sprite data, Copper lists,
  audio buffers) allocated with `MEMF_CHIP`; `OpenScreen` currently uses
  `M68KEmu_HeapAlloc(ctx, size, 0)`. Without Rigel this changes nothing.
- One interrupt dispatcher fed by either the host-timer VBlank or Rigel; the
  series' VERTB stub (`0x140-0x177`, vector `$6C`) overlaps this branch's
  dispatcher stubs (`0x110-0x217`).
- With Rigel, the I/O space goes to Rigel before the series' CIA stand-in.

## Milestone 2: Chip RAM allocation and interrupts

Status: implemented; `test_rigel_chip_irq` passes with the legacy host tests
unchanged.

This milestone makes DMA and chipset interrupts usable by ordinary
contained 68k applications. It remains below the level of a complete Amiga
machine or native presentation backend.

### Scope

- Split the contained address space into explicit Chip and Fast memory
  allocation regions.
- Make emulated `AllocMem()` and `AllocVec()` honor `MEMF_CHIP` and prevent DMA
  clients from receiving addresses outside Rigel's Chip RAM window.
- Preserve and apply relevant Hunk memory attributes when loading segments.
- Validate interrupt requests from Rigel through `Moira::setIPL()`, 68k
  autovector entry, handler execution, `INTREQ` acknowledgement, and `RTE`.
- Keep Hunk execution, containment, and native AROS LVO calls working before
  and after interrupt delivery.

CPU bus stalls, `BLTPRI` arbitration, native Exec interrupt servers, graphics
presentation, and audio output remain outside this milestone.

### Acceptance test

A focused 68k test program must:

1. Allocate source and destination buffers with `MEMF_CHIP`.
2. Verify that both buffers are inside Rigel's Chip RAM window.
3. Install a contained 68k autovector handler.
4. Enable the relevant `INTENA` bit and start a Blitter copy.
5. Observe the `BLIT` interrupt, acknowledge it through `INTREQ`, update a
   counter, and return with `RTE`.
6. Verify the DMA result and exactly one expected interrupt.
7. Call a native AROS service through an LVO after the interrupt.
8. Exit with a deterministic result suitable for the host test suite.

Completion requires this test to pass with Rigel enabled while all legacy
non-Rigel host tests continue to pass unchanged.

## Milestone 3: Exec interrupt API and Copper

Status: implemented; `test_rigel_exec_irq` passes with the milestone 2 and
legacy host tests, and `kernel-m68kemu-library` builds with and without
`--enable-m68kemu-rigel`.

Milestone 2 delivers interrupts only to programs that write an autovector
directly. Ordinary applications use the Exec API instead, and on a hosted
build `Disable()`/`Enable()` currently reach the native host Exec. This
milestone routes the Exec interrupt API to Rigel for contained programs and
validates Copper DMA through the bridge. It remains headless.

### Scope

- Implement `AddIntServer()`, `RemIntServer()` and `SetIntVector()` for
  contained 68k `struct Interrupt` nodes. Server chains (`PORTS`, `COPER`,
  `VERTB`, `EXTER`) are ordered by `ln_Pri`; adding a server enables its
  `INTENA` bit and removing the last one disables it.
- Replace the default autovector stubs with contained per-level dispatchers
  that follow Exec's calling conventions:
  - servers get `A1 = is_Data`, `A5 = is_Code`, `A0 = custom`,
    `A6 = SysBase`; a server returning with Z clear ends a non-`VERTB` chain;
  - handlers get `D1 = INTENAR & INTREQR`, `A0 = custom`, `A1 = is_Data`,
    `A5 = is_Code`, `A6 = SysBase`, and acknowledge `INTREQ` themselves;
  - chain interrupts are acknowledged by the dispatcher, and pending bits
    without a server or handler are acknowledged and dropped.
- In Rigel mode, `Disable()`/`Enable()` nest on the contained `INTENA`
  master bit instead of the host, and the master bit starts enabled as on a
  running AmigaOS.
- Programs that install an autovector directly keep working.
- Validate a Copper list running from Chip RAM: `WAIT`, `MOVE` to a colour
  register, and a Copper-raised `COPER` interrupt.

Software interrupts (`Cause()`), CIA interrupt sources, native Exec
interrupt servers, and video presentation remain outside this milestone.

### Acceptance test

A focused 68k test program must:

1. Allocate a Copper list with `MEMF_CHIP` that sets `COLOR00` to black,
   waits for a line, sets `COLOR00` to red, and sets `INTREQ` `COPER`.
2. Add `VERTB` and `COPER` servers with `AddIntServer()`, start the list
   through `COP1LC`/`COPJMP1` and `DMACON`.
3. Count a fixed number of `VERTB` interrupts and the same number (within
   one) of `COPER` interrupts.
4. Under `Disable()`, wait longer than a frame and observe no server call;
   after `Enable()`, observe interrupts resume.
5. Install a Blitter handler with `SetIntVector()`, run a blit, and check
   the handler arguments, its own `INTREQ` acknowledgement, and that the
   previous vector is returned and restored.
6. Remove the servers with `RemIntServer()` and observe their `INTENA` bits
   cleared.
7. Exit with a deterministic result suitable for the host test suite.

The host test must then confirm, through Rigel's public API, a completed
frame that is black above the waited line and red below it.

Completion requires this test, the milestone 2 test and all legacy
non-Rigel host tests to pass, and `kernel-m68kemu-library` to build with
and without `--enable-m68kemu-rigel`.

## Milestone 4: video presentation

Status: implemented; `test_rigel_display` passes with the earlier milestone
and legacy host tests, `kernel-m68kemu-library` builds with and without
`--enable-m68kemu-rigel`, and the display program runs at 50 Hz in a window
on hosted AROS.

Milestones 1-3 run chipset programs headless; nothing a program draws is
visible, and `dos.library` still starts Hunk executables without Rigel.
This milestone shows Rigel's frames in a native AROS window and gives users
an explicit way to start a program with the chipset and a display.

### Scope

- Add `M68KEMU_RUN_DISPLAY` for `RunHunkEx()`/`RunFileEx()`. It implies
  `M68KEMU_RUN_RIGEL` and opens no window until the first completed frame.
- Introduce a presenter hook between the bridge and any display: on each
  `RIGEL_EVENT_FRAME_READY` the bridge hands the completed frame to the
  context's presenter, which may ask the program to stop. The hook keeps
  the frame pump testable without a display.
- Implement the AROS presenter: a window on the default public screen sized
  to the frame, `WritePixelArray()` per frame in the pixel layout Rigel
  actually produces, pacing to the video standard's frame rate with
  `timer.device`, and stopping the program on the close gadget, Escape or
  Ctrl-C. A stopped program returns `RETURN_WARN`.
- Extend `RunM68K` with `RIGEL` and `DISPLAY` switches.

Input (keyboard, mouse, joystick), audio, CIA timers, screen modes beyond
the frame Rigel composes, and choosing Rigel automatically in `dos.library`
remain outside this milestone.

### Acceptance test

A focused 68k test program must:

1. Allocate a one-bitplane 320x256 playfield and a Copper list with
   `MEMF_CHIP`, and draw a 16x16 checkerboard.
2. Program the display window, data fetch, `BPLCON0` and colours through the
   Copper, which also reloads the bitplane pointer every frame.
3. Change `COLOR01` from a `VERTB` server every frame.
4. Run a fixed number of frames and exit with a deterministic result.

The host test, with a presenter that records frames instead of drawing them,
must confirm:

- one presented frame per `VERTB`, in increasing frame order;
- the checkerboard geometry and both colours in a presented frame, and
  `COLOR01` changing between frames;
- that a presenter asking to stop ends the program early with `RETURN_WARN`.

On a hosted AROS build, `RunM68K DISPLAY` must show the program in a window
at the PAL frame rate and close cleanly.

Completion requires this test and the earlier milestone and legacy host
tests to pass, and `kernel-m68kemu-library` to build with and without
`--enable-m68kemu-rigel`.

## Milestone 5: CIAs and input

Status: implemented; `test_rigel_input` passes with the earlier host tests,
and on hosted AROS the display program reacts to the mouse button and quits
on Escape.

Milestone 4 shows a program's display but nothing reaches it from the
user, and its CIA accesses land in contained RAM instead of Rigel's CIAs.
This milestone routes the CIA address space to Rigel and feeds the
display window's keyboard and mouse into the chipset, so a program that
reads the hardware directly can be interactive.

### Scope

- In Rigel mode, forward CPU accesses to the classic I/O space
  (`$A00000-$DFFFFF`: CIAs and custom chips) to Rigel's MMIO decoder, and
  keep the containment heap out of that range so no allocation shadows it.
- Add bridge input calls built on Rigel's public input API: keyboard
  events, mouse movement (port 0 quadrature counters), left and right
  mouse buttons. Keyboard events are queued and delivered one per frame,
  because Rigel injects a key straight into CIA-A's serial register
  without the keyboard's handshake (Rigel `ISSUE-0016`).
- Feed the display window's raw keys, mouse movement and buttons into
  those calls. Escape becomes an ordinary key; the close gadget and Ctrl-C
  still stop the program.
- CIA timers and their interrupts reach contained `PORTS`/`EXTER` servers
  through the milestone 3 dispatcher.

`ciaa.resource`/`ciab.resource` emulation, joystick input from the host,
the keyboard's reset sequence, and audio remain outside this milestone.

### Acceptance test

A focused 68k test program must:

1. Install a `PORTS` server that reads CIA-A `ICR`, decodes keyboard bytes
   from `SDR` and performs the `SPMODE` handshake, and counts timer A
   interrupts.
2. Run CIA-A timer A continuously at about 100 Hz with its interrupt
   enabled.
3. Over a fixed number of frames, receive the key events the host injects
   in order, and see the injected mouse movement in `JOY0DAT`, the left
   button on CIA-A `PRA` and the right button on `POTGOR`.
4. Count timer A interrupts within the expected range for the elapsed
   chipset time, and exit with a deterministic result.

The host test injects the events through the bridge calls at chosen frames.
On hosted AROS, the display program must react to the mouse button and quit
on Escape when run with `RunM68K DISPLAY`.

Completion requires this test and all earlier host tests to pass, and
`kernel-m68kemu-library` to build with and without `--enable-m68kemu-rigel`.

## Milestone 6: audio

Status: implemented, with one item waiting on Rigel: `test_rigel_audio`
passes except the channel interrupt count, which Rigel never raises
(`ISSUE-0018`). On hosted AROS the program runs with its window; the test
install had no configured AHI unit, so the presenter ran silently and
audible output there is unverified.

Programs can draw and read input, but Paula's output goes nowhere. This
milestone samples Rigel's mixed audio at a host rate and plays it through
AHI next to the display window.

### Scope

- Sample `rigel_get_audio_sample()` at a host-chosen rate on the chipset's
  time axis: CPU cycles accumulate against `rigel_get_clock_hz()` (the CPU
  rate, matching the cycles the bridge steps with), so samples track
  emulated time exactly, independent of host speed.
- Extend the presenter with an audio rate and an audio callback that
  receives each frame's interleaved 16-bit stereo samples before the frame
  itself.
- In the AROS presenter, stream the samples to `ahi.device` with chained
  `CMD_WRITE` requests at 22050 Hz. Without a usable AHI unit the program
  keeps running silently.

Audio filters (the A500 low-pass and LED filter), per-voice output for a
host mixer (Rigel `ISSUE-0007`), and sample-accurate audio/video sync
beyond per-frame batches remain outside this milestone.

### Acceptance test

A focused 68k test program must:

1. Allocate a 32-byte square-wave sample with `MEMF_CHIP`.
2. Play it on audio channel 0 at period 252 (about 440 Hz) and volume 64
   with audio DMA, with a channel 0 handler installed with
   `SetIntVector()` counting block interrupts.
3. Run a fixed number of frames, see about 440 block interrupts per
   second, stop DMA and exit with a deterministic result.

The host test, with a presenter that records samples, must confirm:

- 441 (+/-1) stereo samples per PAL frame at 22050 Hz;
- a tone of about 440 Hz on the left output, where Paula routes channel 0,
  and silence on the right;
- silence before the program starts audio DMA.

Completion requires this test and all earlier host tests to pass, and
`kernel-m68kemu-library` to build with and without
`--enable-m68kemu-rigel`.

## Remaining work

- **Chipset time while waiting:** `Wait()`/`WaitPort()` block natively, so
  Rigel stops advancing while a program waits; displays, sounds and CIA
  timers freeze until it wakes (see *Coexistence*, prerequisite).
- **Remaining Hunk blocks:** the loader handles `HUNK_RELOC32`,
  `HUNK_RELOC32SHORT`, `HUNK_DREL32` (V37 short form), `HUNK_RELRELOC32`, and
  skips `HUNK_SYMBOL`, `HUNK_DEBUG` and `HUNK_NAME`. `HUNK_ABSRELOC16`,
  `HUNK_RELOC16`/`HUNK_RELOC8` and overlays still stop loading.
- **Segment-list attributes:** `RunHunkEx()` cannot recover `HUNKF_CHIP`
  from a segment list; a caller that needs Chip hunks must use `RunFileEx()`.
- **Blitter and DMA:** CPU bus stalls and `BLTPRI` arbitration are not yet
  applied to Moira, and other DMA engines need bridge-level tests.
- **Interrupts:** `Cause()` software interrupts, CIA interrupt sources
  (`PORTS`/`EXTER` from the CIAs) and native (non-68k) interrupt servers are
  not bridged. Interrupt nodes are tracked by address; a node must not be
  freed while installed.
- **Video:** frames are shown in a window as Rigel composes them. Screen
  modes (hires, interlace, overscan) are untested, the window is not
  scaled, and the frame's dirty-line information is not used. Closing the
  window from a child process started with a non-zero `stackSize` cannot
  see Ctrl-C, which goes to the parent shell.
- **Input:** host joysticks are not read; port 1 and the middle mouse
  button are not fed. Keys reach Rigel one per frame rather than through
  the keyboard handshake (Rigel `ISSUE-0016`), and the keyboard's reset and
  power-up sequences are not sent.
- **CIA resources:** `ciaa.resource`/`ciab.resource` (`AddICRVector()` and
  friends) are not emulated; programs that read the CIAs directly work.
- **Audio:** Paula's audio interrupts are not raised by Rigel
  (`ISSUE-0018`), so interrupt-driven players do not advance. No audio
  filters, no per-voice output, a fixed 22050 Hz rate and AHI unit 0, and
  audio/video sync only per frame.
- **RTC, disk, and serial:** Rigel has APIs for these domains; their
  registers are reachable through the I/O space, but no host side (disk
  images, a serial port) is connected.

## Waiting on Rigel

Findings recorded in Rigel's own backlog (`AI_context/issues/`). Each has a
follow-up here once Rigel resolves it.

| Rigel issue | Current workaround here | Follow-up once fixed in Rigel |
| --- | --- | --- |
| `ISSUE-0010` frames labelled `RGBA8888` are host `0x00RRGGBB` | `m68kemu_display.c` picks `RECTFMT_BGRA32`/`ARGB32` by host endianness; `test_rigel_exec_irq` pins the layout | Choose the `RECTFMT` from the format Rigel reports and update the pinned value |
| `ISSUE-0011` Copper tracing always on, process-wide | `m68kemu_rigel.c` installs a `log_event_fn` that discards events | Keep the callback but drop the comment's workaround rationale, or forward events if they become opt-in |
| `ISSUE-0012` no `extern "C"` in public headers | Host tests wrap `#include <rigel/rigel.h>` in `extern "C"` | Remove the wrappers |
| `ISSUE-0013` no license | Integration cannot be distributed (see *Distribution blocker*) | Record the license here and in `README.md`; lift the blocker |
| `ISSUE-0014` `rigel_get_clock_hz()` is the CPU rate | `present_frame()` converts at `rigel_get_clock_hz() / 2`, per `docs/timing_model.md` | Switch to the colour-clock accessor or context-aware conversion if one is added |
| `ISSUE-0015` plain CMake build unoptimized; `cycle_exact` cost unmeasured | The AROS build passes `Release`; the host test `Makefile` now does too | Decide whether m68kemu enables `cycle_exact` once its cost is known |
| `ISSUE-0016` keyboard injection without queue or handshake | `m68kemu_rigel.c` queues keys and delivers one per frame | Inject keys as they arrive and drop the bridge queue |
| `ISSUE-0017` no audio stream | `sample_audio()` polls `rigel_get_audio_sample()` per step | Read Rigel's stream and drop the accumulator |
| `ISSUE-0018` audio interrupts never raised | `test_rigel_audio` reports the count as XFAIL | Expect the count to pass and remove the XFAIL branch |
