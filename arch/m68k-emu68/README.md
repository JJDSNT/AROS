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

There is no synthetic timer device on either side of this port. Emu68 hands
AROS the real Raspberry Pi FDT (patched only so that `/soc`'s `ranges` point
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
the identical silicon on bare ARM hardware; the only structural difference is
that dispatch arrives over the m68k level-6 autovector (Emu68's fixed
"EXTER" channel, which every real physical IRQ not claimed by one of Emu68's
own virtual devices already uses unconditionally) instead of an ARM64 vector
table entry, so the right driver has to be found and armed at runtime rather
than being link-time fixed.

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
the scheduler returns to the bootstrap task, and the virtual timer wakes the
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
runs Emu68; it does not load the m68k ELF directly. With an Emu68 raw image and
Raspberry Pi 3 DTB available, run:

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

The command below reads the four-byte bootstrap marker kept immediately above
the m68k vector table:

```text
xp /4bx 0x400
```

Before the virtual timer was removed, this exact QEMU setup validated the
full soak to `45 30 31 38` (`E018`): Exec initialized, scheduled a user task,
received timer interrupts (then via Emu68's synthetic device, backed by the
ARM CPU's own architectural timer and delivered through the ARM-local
`BCM2836_TIMER_INT_CTRL0`/CNT-IRQ path), advanced `timer.device`, blocked on
`TR_ADDREQUEST`, woke the task again, and held up for two minutes of
simultaneous requests, `AbortIO()`, and equal-priority round-robin. That
confirms this QEMU configuration is not, in general, too slow or unreliable
for interrupt-driven timing.

**Current status with the real platform timer**: FDT discovery, `/soc`
address translation and IRQ registration all run and log correctly (the
`brcm,bcm2835-system-timer`/`brcm,bcm2836-armctrl-ic` nodes resolve to the
same real, guest-accessible addresses Emu68's own host-side code uses).
But under `qemu-system-aarch64 -M raspi3b`, the System Timer's compare-match
status bit never sets, even though `SYSTIMER_CLO` visibly free-runs past the
programmed `SYSTIMER_C3` target. Given the architectural-timer/CNT-IRQ path
above is proven reliable on this exact QEMU machine, this looks like a gap
specific to QEMU's emulation of the legacy BCM2835 system timer's
compare-match/IRQ-generation logic (a peripheral path modern Linux mostly
doesn't exercise anymore, unlike the ARM generic timer) rather than a bug in
this driver or in Emu68's FDT/MMIO exposure - but it has not been confirmed
against real Raspberry Pi 3 hardware yet, which is required before treating
that as settled. Use `screendump /tmp/aros.ppm` in the QEMU monitor to
capture the framebuffer console.

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
