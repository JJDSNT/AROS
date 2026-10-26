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
- **Compatibility:** Hunk loading, containment, fake library bases, and native
  LVO dispatch use the existing paths unchanged.

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

## Remaining work

- **Chip RAM allocation:** `AllocMem()` and Hunk memory attributes still ignore
  `MEMF_CHIP`; callers must currently use addresses in the 512 KiB Chip RAM
  window deliberately. Split Chip/Fast allocation is required for general
  applications.
- **Copper:** Rigel implements Copper execution and DMA, but the AROS bridge has
  no focused Copper-list integration test yet.
- **Blitter and DMA:** the one-word Blitter proof works. CPU bus stalls and
  `BLTPRI` arbitration are not yet applied to Moira, and other DMA engines need
  bridge-level tests.
- **Interrupts:** IPL propagation is present. A contained 68k autovector test
  and policy for native Exec interrupt servers are still missing.
- **Video:** Rigel exposes completed frames and scanlines; no AROS graphics
  presentation backend consumes them yet.
- **Audio:** Rigel exposes mixed Paula samples; no AHI/native audio backend is
  connected yet.
- **Input, CIA, RTC, disk, and serial:** Rigel has APIs for these domains, but
  the initial bridge intentionally forwards only custom-register space.
