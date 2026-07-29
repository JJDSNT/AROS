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

## Virtual platform timer

Emu68 owns the physical ARM timer and publishes an `emu68,virtual-timer-v1`
device through the guest FDT. AROS discovers its MMIO range and m68k autovector
level from that FDT instead of relying on fixed addresses.

The m68k kernel acknowledges this virtual IRQ and translates it into Exec's
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
  -serial none -display none -no-reboot \
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

The currently validated final marker is `45 30 31 38` (`E018`), meaning that
Exec initialized, scheduled a user task, received virtual timer interrupts,
advanced `timer.device`, blocked on `TR_ADDREQUEST`, and woke the task again.
It additionally confirms coherent elapsed time, simultaneous requests,
`AbortIO()`, two-minute stability, and continued progress of two competing
worker tasks. Use `screendump /tmp/aros.ppm` in the QEMU monitor to capture the
framebuffer console.
