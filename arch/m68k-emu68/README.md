# AROS/Emu68 m68k

This target produces an ELF32, big-endian m68k image for the native Emu68
initramfs loader. Emu68 owns the Raspberry Pi bare-metal environment; AROS
starts only after Emu68 has initialized the machine and loaded the ELF.

Configure and build with:

```sh
../AROS/configure --target=emu68-m68k
make AROS-emu68-m68k
```

The output is:

```text
bin/emu68-m68k/AROS/aros-emu68-m68k.elf
```

Select it in the Raspberry Pi `config.txt` alongside `Emu68.img`:

```text
initramfs aros-emu68-m68k.elf
```

The bootstrap translates Emu68's register ABI into `Emu68BootContext`, validates
the flattened device tree, records its first 32-bit memory range and preserves
the framebuffer and `/chosen/bootargs` information. The ELF carries relocatable
core, OOP/HIDD, timer and graphics residents. It initializes a TLSF memory
header from the FDT RAM range, creates `SysBase`, runs the SINGLETASK and
COLDSTART resident levels, enables multitasking and enters the Exec scheduler.

## m68k Exec ABI

The classic m68k Exec ABI documents `Permit()`, `ObtainSemaphore()`,
`ReleaseSemaphore()` and `ObtainSemaphoreShared()` as preserving
`D0-D1/A0-A1`. Their C implementations use the normal AROS register-clobber
convention, so an m68k bootstrap must install preserving vector wrappers after
`krnPrepareExecBase()` has completed.

Historically this adaptation lived in the Amiga board bootstrap even though it
contains no Amiga chipset knowledge. The wrappers and their installer now live
in `arch/m68k-all/exec`; the Emu68 bootstrap explicitly installs them after
creating `SysBase`. The helper is inert for other m68k targets until their own
bootstrap calls it. This provides a path for Atari, Macintosh or other m68k
targets to adopt the same ABI fix independently, without changing their
hardware boundary or boot sequence.

The target also links `libamiga.a`. Despite its historical name, the code used
here is the compiler `alib` compatibility layer, providing ABI-level helpers
such as `StrDup()` and `GetDataStreamFromFormat()`. Linking it does not import
CIA, Paula, Gayle, custom-chip or other Amiga hardware dependencies.

## Emu68 framebuffer HIDD

`emu68gfx.hidd` is the AROS-side adapter for the linear framebuffer handed to
the m68k ELF by Emu68. Emu68 remains responsible for initializing the physical
display and passes the address, pitch, width and height in the entry ABI. The
driver advertises one fixed RGB565 little-endian mode matching that contract.

Displayable AROS bitmaps remain ordinary managed chunky bitmaps. The driver
declares `aHidd_Gfx_FrameBufferType = vHidd_FrameBuffer_Direct` and hands
`graphics.library` a `CLID_Hidd_ChunkyBM` bitmap bound to the real framebuffer
memory; the generic `Display`/`BitMap` classes then handle `Show()` and
`UpdateRect()` themselves, re-linking a screen's classic `BitMap` to that
framebuffer bitmap whenever it becomes visible. This keeps Raspberry Pi
mailbox, VideoCore and other bare-metal details outside AROS while preserving
the normal graphics HIDD and `graphics.library` boundary - and it avoids
reimplementing bookkeeping the generic code already does correctly.

### Bring-up bugs

Three bugs blocked the boot screen before it rendered correctly:

- **Swapped colors.** The pixel format is declared `RGB16_LE`, but m68k is
  big-endian while the framebuffer is not. Without
  `aHidd_PixFmt_SwapPixelBytes`, `graphics.library`'s color-to-pixel
  conversion skips the byte swap it applies for every other big-endian
  target's little-endian formats, so every color came out with its bytes
  swapped (a solid pink/garbled screen).

- **Nothing ever updated past the first screen.** An earlier version of this
  driver implemented its own `Show()`/`UpdateRect()` instead of declaring
  `aHidd_Gfx_FrameBufferType`. Without that attribute, `graphics.library`
  never re-links a screen's classic `BitMap` to the real framebuffer object
  when a new screen is shown, so drawing landed on an offscreen buffer that
  was never displayed (solid gray, nothing updating - only the very first
  screen's initial paint ever reached VRAM).

- **Silent hang creating a second screen's bitmap.** `HIDD_Display_CreateObject`
  has a generic shortcut: a displayable bitmap with no class of its own
  inherits the class of the existing framebuffer bitmap. Since this driver
  has no hardware cursor, that framebuffer bitmap is wrapped by
  `graphics.library` in a `CursorFB` proxy object. Inheriting *that* class and
  instantiating it via a plain `OOP_NewObject()` - instead of through its own
  `create_cursorfb()` constructor - produced a `CursorFB` instance whose
  "real bitmap" pointer was never set. Any attribute `Get()` on it (starting
  with `Width`) then hung forwarding to that null reference. The fix is to
  always name the bitmap class explicitly (`CLID_Hidd_ChunkyBM`) in
  `CreateObject()` rather than relying on the inherit-from-framebuffer
  shortcut - the same pattern `vc4gfx` (Raspberry Pi's native framebuffer
  HIDD) already uses.

Emu68 exposes a unified memory domain rather than separate Amiga chip and fast
RAM. The system memory header consequently satisfies both `MEMF_CHIP` and
`MEMF_FAST`. This is needed by generic graphics compatibility paths such as
`AllocSpriteDataA()`, which still request `MEMF_CHIP`; it does not imply the
presence of an Amiga chipset. AArch64 native targets solve the same legacy
requirement by removing `MEMF_CHIP` in their platform allocator.

## Real platform timer and interrupt controller (`platform/`)

This port drives **physical peripherals through a virtual interrupt-delivery
bridge**. The registers are real BCM283x silicon, discovered from the real
board FDT and programmed directly over MMIO; there is no synthetic timer
device on either side. Interrupt *delivery*, however, is not native: the ARM
exception still belongs to Emu68, which translates any physical IRQ into the
m68k EXTER channel. Both halves are described below.

Emu68 hands AROS the real Raspberry Pi FDT (patched only so that `/soc`'s `ranges` point
at the guest-accessible virtual window Emu68 already mapped for its own
peripheral access, in `src/raspi/start_rpi64.c:map_peripheral_ranges()` -
this remap already carries `MMU_ALLOW_EL0`, so the guest gets the same real
register access the host has, no Emu68 changes required); `arch/m68k-emu68/
platform/fdt.c` walks that FDT post-heap, and `platform.c` matches
`compatible` strings under `/soc` against a small static driver table -
`platform/bcm283x/system_timer.c` (`compatible = "brcm,bcm2835-system-timer"`)
and `platform/bcm283x/interrupt_controller.c`
(`compatible = "brcm,bcm2836-armctrl-ic"`) - translating each match's `reg`
through `/soc`'s `ranges` to get a real, guest-writable MMIO address. This is
the same shape `arch/aarch64-native/kernel/platform_bcm2708.c` uses to drive
the identical silicon on bare ARM hardware. Two things differ structurally,
and neither is cosmetic.

### Byte order

BCM283x registers are little-endian. On `aarch64-native` the CPU is too, so a
plain dereference works. Here the guest is **big-endian m68k** and Emu68 maps
the peripheral block straight through without swapping, so every 32-bit MMIO
access has to convert explicitly (`AROS_LE2LONG`/`AROS_LONG2LE`).

This is worth stating plainly because the failure mode is deceptive: reads
*and* writes are reversed, so a write followed by a read-back agrees with
itself. A driver that programs `SYSTIMER_C3` and reads the same value back can
still be handing the hardware a completely different number - which is exactly
what happened here, and why the compare never matched.

### Interrupt delivery

> **This path does not work on a standalone Emu68 build.** The protocol
> below is real, but the code implementing it is compiled out unless Emu68
> is built as a PiStorm variant. See "Interrupt delivery is PiStorm-only"
> under Current status. The description is kept because it is what the port
> targets and what `platform.c` implements.

Dispatch arrives over the m68k level-6 autovector - Emu68's "EXTER" channel
for a real physical IRQ - rather than an ARM64 vector table entry, so the
right driver has to be found and armed at runtime instead of being link-time
fixed. That much is a straightforward remap.

The part that is not: Emu68's core-0 IRQ fast path
(`src/aarch64/vectors.c`, `curr_el_spx_irq`) drives that channel off an
INTENA/INTREQ shadow it maintains for the guest, and the guest has to speak
that protocol:

- The fast path always records its internal `ARMPending` flag, but only
  raises the m68k level-6 line when the shadow has **both** `INTEN` and
  `EXTER` set. The channel must be armed once at startup, or a physical IRQ
  arrives and is silently dropped.
- It clears `ARMPending` (and drops level 6) only on a guest write to
  `INTREQ` with the SET/CLR bit clear and `EXTER` set. Every level-6 entry
  must acknowledge **the bridge** in addition to the peripheral that fired -
  the same thing `arch/m68k-amiga/kernel/amiga_irq.c`'s `PAULA_IRQ_ACK` does
  after running a server chain.

So this is not "the same path as `aarch64-native` entered through a different
vector". There is an extra layer with its own state and its own acknowledge
contract. `platform.c` implements both halves.

This is also the contract Emu68's own guest-side drivers rely on:
`gic400.library` (Pi 4 GIC-400 support, used by `genet.device`) installs
itself with `AddIntServer(INTB_EXTER, ...)` and never touches the custom-chip
registers at all - it depends on the OS owning level 6 and doing the
acknowledge.

`platform.c` installs one shared level-6 trampoline that hands off to
whichever interrupt controller driver was discovered; that driver decodes
`ARMIRQ_PEND`/`GPUIRQ_PEND0`/`GPUIRQ_PEND1` (same registers, same offsets as
`hardware/bcm2708.h`) to find which real source fired and calls
`krnRunIRQHandlers()`. `arch/m68k-emu68/kernel/kernel_arch.h` wires
`ictl_enable_irq()`/`ictl_disable_irq()` to that same driver, so
`KrnAddIRQHandler()` unmasks real hardware exactly as it does on
`aarch64-native`/`arm-native`.

The system timer driver acknowledges its IRQ and translates it into Exec's
standard `INTB_VERTB` heartbeat. The generic `timer.device` consumes that
heartbeat, so it contains no Emu68, Raspberry Pi, CIA, Paula, or custom-chip
knowledge. Its period is derived from `SysBase->VBlankFrequency`.

The bootstrap probe validates both synchronous `TR_GETSYSTIME` and an
asynchronous 40 ms `TR_ADDREQUEST`. During the latter, the probe task blocks,
the scheduler returns to the bootstrap task, and the platform timer wakes the
probe through the normal Exec device path. A second `TR_GETSYSTIME` also checks
that at least the requested 40 ms elapsed on the device clock.

The scheduler/timer stress probe then:

- queues simultaneous 40 ms and 80 ms requests and verifies their completion
  order;
- cancels a pending five-second request with `AbortIO()` and checks for
  `IOERR_ABORTED`;
- runs two CPU-bound tasks at the same priority while the probe task repeatedly
  blocks;
- submits 120 consecutive one-second requests and requires both worker tasks to
  make progress during every wait.

This two-minute soak exercises asynchronous interrupt completion, ready/wait
task lists, timer request queues, preemption, and equal-priority round-robin.

## QEMU validation

Emu68 itself remains the bare-metal owner. QEMU emulates the Raspberry Pi that
runs Emu68; it does not load the m68k ELF directly.

Build the firmware from an unmodified upstream checkout - this port targets
stock Emu68, so validating against a patched one proves nothing:

```sh
git clone https://github.com/michalsc/Emu68.git
cd Emu68 && git submodule update --init --recursive
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.cmake \
      -DTARGET=raspi64
cmake --build build -j$(nproc)
gunzip -c build/Emu68.img.gz > build/Emu68.raw.img
```

The build also downloads Raspberry Pi DTBs into `build/firmware/`, so
`bcm2710-rpi-3-b.dtb` comes from the same tree. (The toolchain file pins
GCC 14; with GCC 13 installed, copy it and adjust the two compiler lines.)

Then run:

```sh
qemu-system-aarch64 \
  -M raspi3b \
  -kernel /path/to/Emu68.raw.img \
  -dtb /path/to/bcm2710-rpi-3-b.dtb \
  -initrd bin/emu68-m68k/AROS/aros-emu68-m68k.elf \
  -serial stdio -display none -no-reboot \
  -monitor unix:/tmp/emu68-monitor.sock,server,nowait
```

Connect to the monitor with:

```sh
nc -U /tmp/emu68-monitor.sock
```

Read memory as **bytes** (`xp /4bx`), not words. The word view renders the
byte order in a way that is easy to misread on a little-endian peripheral,
which is exactly how the byte-order bug above stayed hidden for a while.

Useful probes:

```text
xp /4bx 0x400        bootstrap stage marker (ASCII, e.g. "E005")
xp /4bx 0x3F003000   SYSTIMER CS    (compare-match status)
xp /4bx 0x3F003004   SYSTIMER CLO   (free-running microsecond counter)
xp /4bx 0x3F003018   SYSTIMER C3    (compare target)
xp /4bx 0x3F00B210   GPUIRQ_ENBL0   (bit 3 == System Timer channel 3)
xp /4bx 0x3F00B204   GPUIRQ_PEND0
```

Note these are **host physical** addresses. The guest sees the same registers
through Emu68's peripheral window at `0xf2000000` (so `systimer_base` reads
`0xf2003000` inside AROS). Symbols from the m68k ELF cannot be poked this way
- the kernel is relocated at load, so `nm` addresses are link-time only.

Use `screendump /tmp/aros.ppm` in the monitor to capture the framebuffer
console.

### Current status

Validated against a clean upstream Emu68 (`michalsc/Emu68`, no local
patches), built for `raspi64`.

Working:

- FDT discovery and `/soc` address translation - both nodes resolve to the
  real guest-accessible window.
- System Timer programming. With the byte-order fix, `CLO` reads as a sane
  microsecond counter (advancing ~2.7 ms between adjacent traces instead of
  jumping by billions), `C3` is programmed to `CLO + interval`, and the
  compare **matches**.
- Interrupt controller. `GPUIRQ_ENBL0` bit 3 is unmasked and, once the
  compare latches, `GPUIRQ_PEND0` bit 3 reads pending.

Not working - **blocked, cause identified**:

The interrupt is never delivered to AROS. Everything upstream of Emu68's
bridge works; the bridge itself does not exist in this build.

```text
[exter] INTENAR     0x00000000   Emu68's shadow: never written
[exter] INTENA rdbk 0x0000e000   0xdff09a reads back what we wrote
[intc]  PEND0 (raw) 0x08000000   byte-reversed 0x08 -> IRQ 3 is pending
[exter] INTREQR     0x00000000   Emu68 never set ARMPending
```

#### Interrupt delivery is PiStorm-only

Writing `0xE000` to `0xdff09a` and reading it back proves that address is
ordinary RAM here - Emu68 never faults on the access. But the memory map is
not the blocker, and changing it would not help. The chain, verified against
upstream `michalsc/Emu68`:

| Fact | Location |
|---|---|
| All `INTENA`/`INTREQ`/`INT_shadow` handling sits inside `#ifdef PISTORM_ANY_MODEL` | `vectors.c:314`-`723` |
| A standalone build is `VARIANT=none`, so that macro is undefined | `CMakeLists.txt:245` |
| The non-PiStorm `SYSWriteValToAddr` special-cases only `0xdeadbeef`; everything else writes through to a linear alias | `vectors.c:726` |
| Core 0 is served entirely by the assembly IRQ fast path | `vectors.c:148`-`171` |
| That fast path raises level 6 only when `(INT_shadow.INTENA & 0x6000) == 0x6000` | same |
| `IRQHandler` (C), whose `cpu_id == 0` branch would set `INTF.ARM` *without* that gate, is reached only from `IRQonOtherCores` - cores 1-3 | `vectors.c:170`, `271` |

`INT_shadow` is a zero-initialised global that nothing writes on a
standalone build, so the gate never opens: **a physical IRQ cannot raise m68k
level 6 on stock non-PiStorm Emu68.** This is not misconfiguration - Emu68
assumes a real Amiga supplies `INTENA`/`INTREQ` across the PiStorm bus.

Consequently the level-6 arm/acknowledge code in `platform.c` is correct in
form but inert, and has never been exercised. It also explains, in
retrospect, why this port originally carried an Emu68-side virtual timer
device: on standalone Emu68 that was the only way to get an interrupt into
the guest at all.

Who decides the guest memory map, for the record: Emu68 does. It maps the
FDT `/memory` blocks 1:1 up to `0xf2000000` (`start.c:1362`) and punches
exactly one deliberate hole, for the `0xdeadbeef` debug channel
(`start.c:1373`). AROS receives the resulting RAM range via FDT and has no
say in it.

#### Options

| Option | Assessment |
|---|---|
| Upstream Emu68 patch - seed `INT_shadow.INTENA` at init on non-PiStorm builds, or let core 0 reach `M68kReportInterrupt(1)` | Small, fixes a genuine gap (no standalone guest can receive a physical IRQ today), upstreamable. Moves "stock Emu68" to a future release rather than abandoning it |
| Revive the Emu68-side virtual device | Known to work, but reintroduces a firmware fork |
| Poll instead of interrupt | No preemption; degrades the scheduler |
| Build a PiStorm variant | Expects real Amiga hardware on the bus; not viable on a plain Pi |

Real Raspberry Pi 3 hardware validation remains outstanding, but is not
expected to differ: the blocker is a build-time conditional, not timing.

### Superseded diagnosis

An earlier revision of this document attributed the dead timer to a gap in
QEMU's emulation of the legacy BCM2835 system timer, on the grounds that the
compare-match status bit never set while `CLO` visibly ran past the target.
That was wrong, and is recorded here so the reasoning is not repeated: the
driver was writing byte-reversed values, so the target the hardware actually
held was ~512 seconds in the future while `CLO` was still in the hundreds of
milliseconds. QEMU emulates this peripheral correctly. The same bug would
have failed identically on real hardware.

## Debug console (0xdeadbeef)

Emu68 leaves the guest physical address `0xdeadbeef` deliberately unmapped
(`src/aarch64/start.c`). A one-byte guest write there faults into Emu68 itself,
which forwards the byte to its own host-side `kprintf()`
(`src/aarch64/vectors.c`), reaching the real UART that QEMU exposes through
`-serial`. This gives `bug()`/`kprintf()` real scrollback text output,
independent of framebuffer state, instead of the single-value bootstrap marker
below.

`krnPutC()` (`arch/m68k-emu68/kernel/kernel_debug.c`) and the framebuffer
console's `emu68_console_putc()` (`arch/m68k-emu68/boot/console.c`) both write
through this channel now, so existing boot-progress messages show up on serial
as-is.

TODO: now that this channel exists, revisit whether the `emu68_set_stage()`
single-marker mechanism below is still needed, or whether boot-stage tracking
should move entirely to text messages over this channel.
