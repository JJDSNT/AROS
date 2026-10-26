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
grant. This does not block local technical validation, but it does block safe
redistribution of Rigel with AROS. The Rigel copyright holder must choose and
publish a license before this integration can be distributed.

## Implemented bridge

- **Lifecycle:** a Rigel context is created on explicit opt-in and destroyed
  before containment memory.
- **Memory:** Rigel DMA callbacks read and write the lowest 512 KiB of the same
  big-endian containment block used by Moira.
- **MMIO:** byte and word accesses in custom space are forwarded through
  `rigel_mmio_read()` and `rigel_mmio_write()`.
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

## Remaining work

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
- **Video:** Rigel exposes completed frames and scanlines; no AROS graphics
  presentation backend consumes them yet.
- **Audio:** Rigel exposes mixed Paula samples; no AHI/native audio backend is
  connected yet.
- **Input, CIA, RTC, disk, and serial:** Rigel has APIs for these domains, but
  the bridge forwards only custom-register space.
- **Rigel headers:** the public headers have no `extern "C"` guards, so C++
  callers wrap the include themselves.
- **Rigel tracing:** the Copper emits a structured `copper_write` trace event
  for every colour-register write; with stdio logging and no callback it
  floods stderr.
