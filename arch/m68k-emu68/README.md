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

The initial implementation is a loader/ABI bring-up image. It records the
device-tree and framebuffer arguments supplied by Emu68 and then idles. Exec
and the rest of AROS will be connected incrementally.
