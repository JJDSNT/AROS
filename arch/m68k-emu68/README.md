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

> **The `INTENA`/`INTREQ` half of this is PiStorm-only.** Level-6 delivery
> itself works on any build, but the custom-chip registers used to arm and
> acknowledge it are emulated only on PiStorm variants, so `platform.c`'s
> current implementation is inert here. See "What is actually missing" under
> Current status for the state of play and the direction being pursued.

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

Drop `-display none` to get the framebuffer console in a window; the serial
log keeps coming out on the terminal either way. Close with `Ctrl-A` `X`.

Connect to the monitor with:

```sh
nc -U /tmp/emu68-monitor.sock
```

### Kernel arguments (`-append`)

QEMU writes `-append` into the DTB as `/chosen/bootargs`, `boot.c` passes it
through as `KRN_CmdLine`, and `PrepareExecBase` (`rom/exec/prepareexecbase.c`)
parses `sysdebug=` out of it into `SysBase->ex_DebugFlags`. So AROS's runtime
debug flags work here with no rebuild:

```sh
qemu-system-aarch64 -M raspi3b \
  -kernel /path/to/Emu68.raw.img \
  -dtb /path/to/bcm2710-rpi-3-b.dtb \
  -initrd bin/emu68-m68k/AROS/aros-emu68-m68k.elf \
  -append "sysdebug=InitCode" \
  -serial stdio -no-reboot
```

`InitCode` is the most useful one during bring-up. It turns on two separate
printouts:

- `rom/kernel/prepareexecbase.c` dumps the whole resident list once it is
  built, as `addr: pri flags version name`. The flags column is the init
  class - `01` `RTF_COLDSTART`, `02` `RTF_SINGLETASK`, `04` `RTF_AFTERDOS`,
  `80` `RTF_AUTOINIT` (hence the `81` on most modules).
- `rom/exec/initcode.c` narrates each pass: `enter InitCode(0x01, 0)`, one
  `calling InitResident (pri flags "name")` per module, then `leave`.

```text
Resident modules (addr: pri flags version name):
+ 34605b20:  127 02   4 "kernel.resource"
+ 3460f53c:  120 01  51 "exec.library"
+ 3460fefc:   50 81  41 "timer.device"
+ 34629cda:    9 81  45 "emu68gfx.hidd"
+ 3468044e:  -50 01  41 "dosboot.resource"
+ 3467dcf4: -120 00  50 "dos.library"
+ 34692160: -121 04  41 "DOSBoot cleanup"
```

Flags are comma-separated (`sysdebug=InitCode,InitResident`); the full list
of names is `ExecFlagNames` in `rom/exec/exec_flags.c`. Note that the
`Resident modules` dump comes from the kernel and the `InitCode:` lines come
from exec, so seeing one without the other means the flag was applied later
than the printout you are missing.

To pinpoint a module that hangs during init rather than just knowing which
ones ran, put `#define DEBUG 1` above `#include <aros/debug.h>` in that
module's source and rebuild - its `D(bug(...))` calls reach the same serial
channel. That is how `dosboot.resource` was confirmed to be sitting in its
retry loop rather than stuck.

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

Interrupt delivery: **working, as a proof of concept** - it runs against
stock firmware with the address discovered at runtime. The mechanism and its
one deliberate hack are below.

#### What was missing

Emu68's core-0 IRQ fast path (`vectors.c:148`-`171`) does two things on every
physical IRQ: it sets its internal `ARMPending` flag **unconditionally**, and
it sets `INTF.ARM = 6` - which is what makes the execution loop raise m68k
level 6 - **only** when `(INT_shadow.INTENA & 0x6000) == 0x6000`, i.e. when
the shadow has both `INTEN` and `EXTER`.

Reading Emu68's own `INT_shadow` out of physical memory while the timer is
running shows exactly that state:

```text
0x34c620b0:  00 00   00 00   01
             INTENA  INTREQ  ARMPending
```

That was the state before arming: `ARMPending = 1` proves the whole hardware
path works - the compare matched, the interrupt controller routed to core 0,
ARM IRQs are unmasked, and the fast path ran. It skipped the `INTF.ARM` store
only because `INTENA` was zero.

So the single missing step is arming that shadow. Nothing else is broken.

#### Why `0xdff09a` is the wrong way to arm it

The custom-chip register alias only exists on PiStorm builds:

| Fact | Location |
|---|---|
| All `INTENA`/`INTREQ` MMIO emulation sits inside `#ifdef PISTORM_ANY_MODEL` | `vectors.c:314`-`723` |
| A standalone build is `VARIANT=none`, so that macro is undefined | `CMakeLists.txt:245` |
| The non-PiStorm `SYSWriteValToAddr` special-cases only `0xdeadbeef`; everything else writes through to a linear alias | `vectors.c:726` |

Hence the trace above: writing `0xE000` to `0xdff09a` and reading `0xE000`
back means the write landed in ordinary RAM. `platform.c`'s current
arm/acknowledge pair is therefore inert and has never been exercised - it
implements the PiStorm protocol on a build that does not have it.

#### Working approach - proven end to end

`INT_shadow` is an ordinary Emu68 global, and the guest sees physical RAM
1:1 - the same flat map that made `0xdff000` plain RAM works in our favour
here. Arming it directly delivers interrupts on stock firmware:

| Step | Mechanism |
|---|---|
| Arm | write `0x6000` into `INT_shadow.INTENA` |
| Deliver | fast path sets `INTF.ARM = 6` -> loop raises level 6 -> vector at `VBR + 0x78` |
| Acknowledge | `MOVEC` to `JITCTRL2` with `JC2F_INT_FROM_ARM` (bit 29, `M68k.h:207`) - clears `INTF.ARM`, and is *not* PiStorm-gated |

Measured under QEMU against an unpatched upstream Emu68:

```text
[exter] shadow        0x00006000    arming accepted
[exter] LEVEL6 entry  0x00000001    m68k took level 6
[systimer] IRQ tick   0x00000001    AROS handler ran
[exter] LEVEL6 entry  0x00000002
[systimer] IRQ tick   0x00000002
[exter] LEVEL6 entry  0x00000003
[systimer] IRQ tick   0x00000003
```

Ticks 2 and 3 are what prove the acknowledge: without clearing `INTF.ARM`
the level-6 line stays asserted and the run either storms or stops after
one entry.

Two things this settles that earlier revisions of this document got wrong:

- **No byte swap on Emu68's own structures.** Emu68 is built
  `elf64-bigaarch64` - the ARM runs big-endian, sharing the guest's byte
  order. Swapping is needed only for the genuinely little-endian BCM
  peripherals. Writing a byte-swapped `0x0060` into the shadow leaves the
  gate closed, which is a silent failure.
- **The `SPSR` IRQ masking in the fast path is not a blocker.** Repeated
  delivery works in practice; do not design around it.

Still true and load-bearing: `INTF.ARM` is **not** cleared when the exception
is taken (`ExecutionLoop.c:387`-`436` pushes the frame and loads PC without
touching `INTF`), so it is level-sensitive and the acknowledge is mandatory
on every entry.

#### Locating the shadow (`emu68_bridge.c`) - proof of concept

The address is `Emu68 relocation base + link offset of INT_shadow`. The base
is derivable - Emu68 moves itself to just past the top of guest RAM, which
the guest reads from the FDT. The offset is not: it is a link-time address
that moves with every Emu68 build.

Rather than guess it, `platform/emu68_bridge.c` reads it out of the
instruction stream that uses it. The fast path loads the shadow with an
`adrp`/`add` pair, so scanning for the three instructions after it and
decoding the two before gives exactly the address Emu68 itself uses:

```text
adrp x1, <page>         d00015e1
add  x1, x1, #<lo12>    9102c021
ldrh w0, [x1]           79400020   <- anchor
and  w0, w0, #0x6000    12130400
cmp  w0, #0x6, lsl #12  7140181f
```

`adrp` is PC-relative and the guest-physical view differs from Emu68's
virtual one by a page-aligned constant, so the arithmetic works in physical
space without needing Emu68's virtual base. The pattern occurs twice - the
IRQ and FIQ handlers - and both must decode to the same address or the scan
gives up.

Confirmed at runtime: discovery returns `0x34c620b0`, the same address that
was hardcoded while the mechanism was being proven.

**This is a proof of concept and is deliberately the only thing in the port
that knows about Emu68's internals.** It is one file, and nothing outside it
depends on how the address is obtained - replacing it with something better
touches nothing else. If upstream rewrites that instruction sequence the
scan finds nothing and returns 0, and the port runs without interrupts:
a diagnosable failure rather than a write into a running firmware image.

Real Raspberry Pi 3 hardware validation remains outstanding.

### Superseded diagnosis

An earlier revision of this document attributed the dead timer to a gap in
QEMU's emulation of the legacy BCM2835 system timer, on the grounds that the
compare-match status bit never set while `CLO` visibly ran past the target.
That was wrong, and is recorded here so the reasoning is not repeated: the
driver was writing byte-reversed values, so the target the hardware actually
held was ~512 seconds in the future while `CLO` was still in the hundreds of
milliseconds. QEMU emulates this peripheral correctly. The same bug would
have failed identically on real hardware.

## Adding a ROMTag: `.aros.romtag` in `boot/emu68.ld`

A port-local module can join the normal AROS startup by dropping a
`struct Resident` into the image - `krnRomTagScanner()` sweeps
`__aros_resident_start` .. `__aros_resident_end` looking for
`RTC_MATCHWORD`, so no `.conf` file or genmodule wrapper is needed. But on
this target the tag **must** go in the `.aros.romtag` section:

```c
static const struct Resident my_romtag
    __attribute__((section(".aros.romtag"), used)) =
{
    RTC_MATCHWORD, (struct Resident *)&my_romtag, (APTR)(&my_romtag + 1),
    RTF_COLDSTART, 41, NT_UNKNOWN, -49,
    "myname", "my id string\r\n", (APTR)my_init
};
```

The reason is a trap. The scanner is a linear sweep, and every time it finds
a ROMTag it jumps straight to that tag's `rt_EndSkip` - which is a symbol
belonging to the module, intended to skip past that module's own body. But
`boot/emu68.ld` gathers **all** `.text` from every object before **all**
`.rodata`, so a module whose `rt_EndSkip` resolves into the `.rodata` block
ends up claiming everything the linker happened to place in between.
`dosboot.resource` is the worst case: its `rt_EndSkip` points at
`db_Cleanup` (the `addromtag db_Cleanup` in `rom/dosboot/dosboot.conf`),
roughly 70 KB further on. A ROMTag landing inside that span is skipped in
complete silence - no warning, no error, the module simply never
initializes.

`.aros.romtag` is placed immediately after `.text.boot` and ahead of every
module's `.text`, which is the one position that cannot be inside another
tag's skip range. Verify placement with:

```sh
m68k-aros-nm -n bin/emu68-m68k/AROS/aros-emu68-m68k.elf | grep -i romtag
```

Your tag should appear near address 0, before `Timer_ROMTag`.

`rt_Pri` chooses when it runs within its init class - the list is sorted
descending. `timer.device` is 50 and `dosboot.resource` is -50, so `-49` is
the last slot before dosboot, with the entire system already up. That is
where `boot/selftest.c` registers itself (`EMU68_SELFTEST`, off by default).

That -50 boundary matters more than it looks: `InitCode(RTF_COLDSTART, 0)`
never returns. `dosboot_Init()` either hands over to dos.library or loops
forever retrying for boot media, which is why `arch/aarch64-native` treats a
return from it as fatal (`krnPanic("System Boot Failed!")`). Anything that
needs to run at COLDSTART has to be a resident with `rt_Pri > -50`; code
placed after the `InitCode()` call in `coldstart_user()` is unreachable.

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
