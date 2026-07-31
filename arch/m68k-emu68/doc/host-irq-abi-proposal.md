# A host interrupt ABI for Emu68

*A proposal for discussion, from the AROS/m68k-emu68 port.*

Line references are against Emu68 commit `9b4379a5c5`.

## Who is asking, and why

We are porting AROS (m68k) to run on Emu68 on a Raspberry Pi 3, with no
PiStorm and no Amiga hardware anywhere in the picture. The guiding idea is
"the Pi is an Amiga without the chipsets": an m68k CPU with real RAM, and
then Pi peripherals reached directly rather than Paula/Agnus/Denise.

That makes us, as far as we know, the second real consumer of Emu68 after
PiStorm — and the first one for which there is no Amiga chipset behind any
of it. Everything below comes out of actually getting a timer interrupt
delivered on a stock, unpatched Emu68 build. It works today, but only
through a hack we are not proud of and are not asking anyone to bless.

The request is narrow: **we would like a supported way for a guest to
receive host interrupts.** This document describes what we found, what we
think is practical, and what we think would be ideal, with costs attached
to each so the trade-offs are visible.

We are offering to implement whatever is agreed. Nothing here is a request
for someone else to do work.

## What we had to do, and why it is a hack

On a stock build, the core-0 IRQ fast path
(`src/aarch64/vectors.c:156-166`, and the FIQ twin at `:181-191`) reads:

```
adrp x1, INT_shadow
add  x1, x1, :lo12:INT_shadow
ldrh w0, [x1, #INTENA]
and  w0, w0, #0x6000        ; INTEN | EXTER
cmp  w0, #0x6000
mov  w0, #1
strb w0, [x1, #ARMPending]  ; unconditional
b.ne 1f                     ; no INTEN|EXTER -> no level 6
mov  x1, CTX
mov  w0, #6
strb w0, [x1, #INTF.ARM]
```

This block is **not** inside `#ifdef PISTORM_ANY_MODEL` — it compiles into
every build. But the only code that ever writes `INT_shadow.INTENA` is the
emulation of Amiga register `0xdff09a`, which **is** inside the ifdef
(`vectors.c:314-784`). So on a stock build the gate is present, permanently
closed, and unreachable by any legitimate means.

We confirmed the IRQ genuinely arrives by observing `ARMPending == 1` in a
running image — the store above happens before the branch, so it lands
regardless of the gate. The interrupt reaches Emu68 and is then dropped.

Our current workaround scans Emu68's own instruction stream for the three
instructions above, decodes the preceding `adrp`/`add` pair to recover
`&INT_shadow`, and has the guest write `0x6000` into `INTENA` itself. It
finds the address at two independent sites (IRQ and FIQ handlers) and
refuses to proceed if they disagree. It works — we get periodic level-6
interrupts from the BCM283x System Timer, and acknowledge them via
`MOVEC` to `JITCTRL2` bit 29.

It is also obviously terrible: a guest writing into firmware globals at an
address recovered by pattern-matching compiled code. It is isolated to one
file, documented as a proof of concept, and it is the reason we are writing
this. We would like to delete it.

## Findings

### 1. The delivery gate is a Paula register

Covered above. A guest with no Paula has no legitimate way to open it.

### 2. Level 6 is hardwired in two independent places

The fast path stores to `INTF.ARM`, and `ExecutionLoop.c:343` turns that
into `level = 6`. Separately, every m68k SR write emits a comparison
against `5 << SRB_IPL` to decide whether to unmask IRQs on the ARM side.
That comparison appears at six sites:

| File | Line | Emitter |
|---|---|---|
| `src/M68k_LINE0.c` | 877 | `EMIT_ORI_TO_SR` |
| `src/M68k_LINE0.c` | 1321 | `EMIT_ANDI_TO_SR` |
| `src/M68k_LINE0.c` | 1691 | `EMIT_EORI_TO_SR` |
| `src/M68k_LINE4.c` | 1174 | `EMIT_MOVEtoSR` |
| `src/M68k_LINE4.c` | 1602 | `EMIT_STOP` |
| `src/M68k_LINE4.c` | 1742 | `EMIT_RTE` |

Level 6 is EXTER. It is a reasonable default for a chipsetless guest too,
but it is currently a constant rather than a choice, and this table is why
making it a choice is not free.

### 3. Only autovectors exist

`ExecutionLoop.c:411`: `vector = 0x60 + (level << 2)`. There is no path by
which a device supplies a vector number.

### 4. Acknowledge is a side effect of a JIT control register

`M68k_LINE4.c:2092`, `JITCTRL2` bit 29, emits `strb WZR, INTF.ARM`. It
works, and we use it. It reads as a debugging affordance that became load
bearing rather than as an interrupt-controller interface.

### 5. Arbitration is Amiga-shaped

`ExecutionLoop.c:325-380`: `ARM_err` → 7, `ARM` → 6, `PPC` → 2, and `IPL`
from Paula via PiStorm. Sensible for the machine it was written for.

## Two pre-existing defects

These are independent of any ABI decision, and we mention them because we
believe they are real bugs worth fixing on their own. We are happy to send
them as separate, small PRs regardless of what happens to the rest of this
document.

### `JITCTRL2` bit 29 does not read back for fast-path interrupts

The assembly fast path stores `INTF.ARM = 6` (`mov w0, #6`), while
`M68kReportInterrupt()` (`ExecutionLoop.c:553`) stores `1`. The `JITCTRL2`
read path (`M68k_LINE4.c:2366`) does `bfi(reg, tmp, 29, 1)` — a 1-bit field
insert, so it propagates only bit 0 of the byte. `6 & 1 == 0`.

Delivery still works, because `ExecutionLoop.c:341` tests the byte for
truthiness. But a guest that asks "is a host interrupt pending?" via
`MOVEC` gets `0` for every interrupt delivered through the fast path — that
is, for essentially all of them. Storing `1` instead of `6` in the two
assembly handlers would fix it; the value is never used as a level.

### The two delivery paths have different gating policies

`curr_el_spx_irq` (an IRQ taken while executing JIT-translated code) honours
the `INTENA` gate. `IRQHandler()`'s core-0 branch (`vectors.c:2462-2471`,
reached from the SP0 vectors when the IRQ lands while Emu68 is in its own C
code) calls `M68kReportInterrupt(1)` with **no gate at all**.

The same physical interrupt line therefore has two different masking
semantics depending on where the CPU happened to be. Under PiStorm the
window is small and the guest usually has `INTENA` open anyway, so we would
not expect anyone to have noticed. For a guest whose mask is genuinely
closed during early boot, it is a real hazard.

## The design question that determines everything else: who decodes?

Before any register layout, one decision:

**(a)** Emu68 becomes an interrupt controller — it routes host IRQs to
slots and the guest registers "host IRQ N → slot M".

**(b)** Emu68 reports only that *a* host interrupt occurred. The guest owns
the host interrupt controller and decodes the source itself.

**We would argue for (b), and fairly strongly.** The AROS AArch64 port
already contains an ARMCTRL/GIC driver that reads the pending banks and
dispatches; our port reuses that code essentially unchanged. If Emu68 takes
on routing, it duplicates a controller the guest already implements, gains
a configuration table, and acquires an opinion about hardware that differs
across Pi models — for a benefit the guest does not need.

It also matches the stated philosophy better. Emu68 supplies the CPU and an
interrupt line; peripherals belong to the guest.

Option (b) makes the ABI much smaller. A guest needs exactly three things it
does not have today:

1. **a mask it owns** (today: none — hence writing into `INT_shadow`);
2. **a pending flag that round-trips** (today: broken, see above);
3. **a level** (today: constant 6).

## What we think is practical

One new `MOVEC` control register.

`MOVEC` is the right namespace: it is where Emu68 already puts everything
non-Amiga (`JITCTRL` `0x1e0`, `DBGCTRL`/`DBGADDRLO`/`DBGADDRHI` at
`0x0ed`-`0x0ef`), it is privileged by the m68k architecture definition, it
is per-CPU, it collides with no memory map, and it behaves identically in
PiStorm and stock builds.

### `HOSTIRQ` — control register `0x1e1`

```
 bit  0     PEND     read: host IRQ pending
                     write 1: acknowledge (clears PEND and INTF.HOST)
 bit  1     ENA      guest's master enable for host interrupt delivery
 bits 4-6   LEVEL    m68k IPL to raise
 bit  31    PRESENT  reads 1 on any Emu68 implementing this ABI
```

`PRESENT` matters. Today an unknown control register falls through to
`EMIT_Exception(VECTOR_ILLEGAL_INSTRUCTION)` (`M68k_LINE4.c:2395`), so
feature detection means installing an illegal-instruction handler around a
probe. That works, but a presence bit is cheaper and clearer.

**For v1 we would suggest `LEVEL` be read-only, reading 6.** The field is
reserved in the layout and readable, but making it writable means the six
SR-write sites in the table above must compare against a runtime value
instead of a constant — emitted code on the hot path of every SR write. We
do not think that cost is worth paying to get a level other than 6, and we
would rather not have it be the reason the proposal stalls.

### Firmware side

The fast path becomes, in both the IRQ and FIQ handlers:

```
ldr  w0, [x1, #HOSTIRQ]      ; x1 = CTX
tbz  w0, #ENA, 1f
orr  w0, w0, #PEND
str  w0, [x1, #HOSTIRQ]
mov  w2, #6
strb w2, [x1, #INTF.HOST]
1: eret
```

and arbitration gains one line in `ExecutionLoop.c`:

```c
if (ctx->INTF.HOST > level)
    level = ctx->INTF.HOST;
```

### The one constraint we would flag as load bearing

**The pending indication must live inside `INT64`.** The JIT's inner-loop
check is `ldr64 INT64; cbz` (`M68k_Translator.c:598`), emitted for every
translated inner loop regardless of build type. Anything outside those
eight bytes costs a second load in the hottest loop in the system.

(`EMIT_STOP` also waits on `INT64` — but only under `PISTORM_ANY_MODEL`
(`M68k_LINE4.c:1619`). On a stock build, `STOP` masks `DAIF` and issues a
bare `wfi()` (`M68k_LINE4.c:1611`), which wakes on a masked IRQ and so
happens to work, but never consults `INT64` and cannot distinguish sources.
If host interrupts become a supported guest feature, this asymmetry is
probably worth revisiting — a stock-build guest calling `STOP` is a normal
idle path, not an edge case.)

Hence the split: **pending goes in a free `INTF` byte** — the union uses
five of eight (`ARM`, `ARM_err`, `IPL`, `RESET`, `PPC`), and
`M68kReportInterrupt()` already carries the comment *"we have 8 slots in
total"* — while **mask and configuration live in the control register**,
which is only read on the cold path. Hot path touches `INT64` only.

### Why one mask bit is enough

`Disable()`/`Enable()` in AROS exec is already `SR.IPL` manipulation, and
Emu68 already honours it (`level > IPL_mask`, `ExecutionLoop.c:387`). Fine
grained masking would duplicate that. `ENA` exists for the case `SR` cannot
express: "the guest has not installed a handler yet." `LEVEL == 0` as
"never raise" gives a clean third state for early boot.

### Cost

We estimate roughly 40 lines: one field in `struct M68KState`, one read case
and one write case in `M68k_LINE4.c`, the two assembly fast paths, one line
in arbitration.

### What does not change for PiStorm

Nothing. Under `#ifdef PISTORM_ANY_MODEL`, the `0xdff09a` write handler
additionally maintains `ENA` alongside `INT_shadow.INTENA`
(`ENA = (INTENA & 0x6000) == 0x6000`), and `INTF.ARM` continues to exist and
behave exactly as it does now. No PiStorm path is made to depend on the new
register. If desired, the whole thing can sit behind `#ifndef
PISTORM_ANY_MODEL` — though we would gently argue against that, since a
uniform interface is easier to document and test than a conditional one.

## What we think would be ideal

Everything below costs materially more than the section above. We are not
asking for any of it, and we would be glad to have only the practical
version. We describe it because the practical version should not
accidentally foreclose these, and one of them addresses a problem that will
otherwise recur.

### A capability/descriptor register

The single most useful thing, and the one that generalises past our own
problem.

A control register returning the guest-physical address of a versioned,
read-only descriptor block published by the firmware: version, build
identity, feature bits, and the addresses of whatever structures are
intended to be guest-visible.

The reason we care is direct: our current hack exists **only** because
there is no way to ask Emu68 where anything is. Faced with the same wall,
the next non-PiStorm guest will write another instruction scanner. A
descriptor block ends that category of hack permanently, and it makes
feature negotiation explicit rather than inferred from probing behaviour.

`HOSTIRQ.PRESENT` is a one-bit stand-in for this. If a descriptor block
ever exists, the presence bit becomes redundant, which is fine.

### Writable `LEVEL`

Costs the six SR-write sites. Worth it only if guests genuinely want host
interrupts at a level other than 6 — plausible for a guest that wants to
mirror the Amiga's level structure for its own reasons, but speculative.

### Vectored delivery

The 68k supports non-autovectored interrupts, where the device supplies the
vector during the acknowledge cycle. `vector = 0x60 + (level << 2)` forecloses
this today.

For a chipsetless guest this is the real prize: one handler per device,
with no polling decode at all. It is also the largest change, it interacts
with option (a) above — someone has to know which source maps to which
vector — and it is not needed to make our port work. We list it as the
direction we would grow toward, not as a request.

### Documented delivery semantics

Edge versus level; what happens if a second host IRQ arrives before
acknowledge; exactly when `DAIF` is restored relative to the guest lowering
`SR.IPL`. We reverse-engineered our answers to these by reading emitted
code, and we could easily have got them wrong in a way that only shows up
under load. Writing them down would be valuable independently of any new
register.

## Open questions

1. Is there already a plan here? The *"we have 8 slots in total"* comment in
   `M68kReportInterrupt()` reads like the free `INTF` bytes were always
   intended as an extension point — but that is our reading of a comment,
   and we would rather ask than assume.

2. Is `MOVEC` the namespace you want extended, or would you prefer this in
   an MMIO window?

3. Would you rather the two defects above be fixed as part of this, or
   separately and first? We lean towards separately and first — they stand
   on their own and are much easier to review in isolation.

4. Is there a non-PiStorm consumer we do not know about whose expectations
   we would break?

## Summary

Practical: one control register, pending folded into a free `INTF` byte,
`LEVEL` read-only at 6, the guest keeps owning the host interrupt
controller. About 40 lines, no behavioural change for PiStorm, and it lets
us delete an instruction scanner from our port.

Ideal: the above plus a firmware descriptor block, so that the next guest in
our position does not have to invent the same hack we did.
