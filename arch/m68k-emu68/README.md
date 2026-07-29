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
`kernel.resource`, `exec.library`, and `timer.device` residents. It initializes
a TLSF memory header from the FDT RAM range, creates `SysBase`, runs the
SINGLETASK and COLDSTART resident levels, enables multitasking and enters the
Exec scheduler.

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
probe through the normal Exec device path.

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

The currently validated final marker is `45 30 31 31` (`E011`), meaning that
Exec initialized, scheduled a user task, received virtual timer interrupts,
advanced `timer.device`, blocked on `TR_ADDREQUEST`, and woke the task again.
Use `screendump /tmp/aros.ppm` in the QEMU monitor to capture the framebuffer
console.
