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

These are independent of any ABI decision — one is a live bug, the other
latent. We are happy to send them as separate, small PRs regardless of what
happens to the rest of this document.

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

### The core-0 branch of `IRQHandler()` is unreachable

`IRQHandler()` has a `cpu_id == 0` branch (`vectors.c:2462-2471`) that
acknowledges the GIC and calls `M68kReportInterrupt(1)` with no `INTENA`
gate. It cannot execute.

`IRQHandler` is called from exactly one site, `IRQonOtherCores`
(`vectors.c:275`), which is reached only from the `curr_el_spx_irq` and
`curr_el_spx_fiq` fast paths after `tst x0, #3` / `b.ne` — cores 1 to 3.
Core 0 is handled inline and `eret`s without ever branching there. The SP0
vectors go to `SYSHandler`, not `IRQHandler`.

Minor, but worth recording: it is the one piece of code that reads like an
ungated delivery path, and it would become a real inconsistency the moment
anything routes a core-0 interrupt through it, since it would bypass the
gate the fast path enforces two instructions from the same decision.

## The design question that determines everything else: who decodes?

Before any register layout, one decision:

**(a)** Emu68 becomes an interrupt controller — it routes host IRQs to
slots and the guest registers "host IRQ N → slot M".

**(b)** Emu68 reports only that *a* host interrupt occurred. The guest owns
the host interrupt controller and decodes the source itself.

> **Resolved: (b).** Upstream's answer:
>
> > CPU (emu68) reports interrupt by entering interrupt servicing routine
> > (auto level vector 6) and it is up to OS to talk with the interrupt
> > controller to find out one source of interrupt and call appropriate
> > handler to serve it
>
> The rest of the document assumes this. It settles two things: the guest
> decodes the source, and the delivery contract is **autovector level 6**,
> fixed. Nothing below asks Emu68 to learn anything about Pi peripherals,
> and the configurable-level idea is dropped (see below).
>
> It leaves the actual blocker open — see "What this does not yet answer".

**We would argue for (b), and fairly strongly.** The AROS AArch64 port
already contains an ARMCTRL/GIC driver that reads the pending banks and
dispatches; our port reuses that code essentially unchanged. If Emu68 takes
on routing, it duplicates a controller the guest already implements, gains
a configuration table, and acquires an opinion about hardware that differs
across Pi models — for a benefit the guest does not need.

It also matches the stated philosophy better. Emu68 supplies the CPU and an
interrupt line; peripherals belong to the guest.

Option (b) makes the ABI much smaller. With the level now settled at 6, a
guest needs exactly two things it does not have today:

1. **a mask it owns** (today: none — hence writing into `INT_shadow`);
2. **a pending flag that round-trips** (today: broken, see above).

## What this does not yet answer

The upstream answer describes the contract, and we agree with it. Our
finding is that **on a stock, non-PiStorm build the contract is not
currently met** — and specifically not the first clause.

"CPU reports interrupt by entering interrupt servicing routine" is exactly
what does not happen. The interrupt arrives, Emu68 takes the exception, and
the level-6 autovector is never entered, because the fast path gates it on
`INT_shadow.INTENA & 0x6000`, and all five writers of that field are
compiled out unless `PISTORM_ANY_MODEL` is defined. We verified the
interrupt truly
arrives by observing `ARMPending == 1`, stored unconditionally two
instructions before the gate.

So the open question is narrow, and it is the whole reason for this
document:

> On a stock Emu68, how should a guest with no Paula arm level-6 delivery
> and acknowledge it afterwards?

Everything in "What we think is practical" is one answer to that question.
We are not attached to it. Any mechanism that lets a chipsetless guest open
the gate and complete the cycle would let us delete our instruction
scanner, which is the only outcome we actually need.

## The `gic400.library` reference, and what it shows

Upstream pointed at [`rondoval/emu68-gic400-library`](https://github.com/rondoval/emu68-gic400-library)
as the reference for how this should behave. We read it, and we agree with
it completely — it is exactly the shape we want, and it confirms (b) in
practice rather than in principle.

What it does: discovers the GIC-400 through the device tree, programs the
distributor and CPU interface itself, keeps a handler table across the SPI
range, and acknowledges at the GIC with `GICC_EOIR`. Emu68 is told nothing
about any of it. Our `bcm283x/interrupt_controller.c` is the same design
against the older ARMCTRL controller, which is what a Pi 3 has.

The part worth drawing out is how it receives control. It contains **no
Emu68-specific interrupt code at all** — no `MOVEC`, no firmware structure,
nothing. The entire hook is one line (`src/gic400_api.c:178`):

```c
AddIntServer(INTB_EXTER, &gicBase->dispatcher_interrupt);
```

That is plain AmigaOS exec. It works because Emu68 impersonates Paula
end to end, in five steps:

1. AmigaOS writes `INTENA` (`0xdff09a`) with `INTEN|EXTER` during normal
   startup; Emu68 traps it and updates `INT_shadow.INTENA` — the gate opens
   as a side effect of the guest OS doing ordinary Amiga things.
2. A host IRQ arrives; the fast path sees the gate open and sets
   `ARMPending = 1` and `INTF.ARM`, raising level 6.
3. AmigaOS's level-6 handler reads `INTREQR` (`0xdff01e`); Emu68 traps the
   read and ORs in `0x2000` (`INTF_EXTER`) when `ARMPending` is set
   (`vectors.c:671-687`), so the OS concludes Paula raised EXTER and runs
   the EXTER server chain — which is where the library's dispatcher sits.
4. The dispatcher reads the GIC, calls the device handler, writes `EOIR`.
5. AmigaOS writes `INTREQ` to clear EXTER; Emu68 traps it and clears
   `ARMPending` and `INTF.ARM`.

Steps 1, 3 and 5 are Paula MMIO emulation, and all three live inside
`#ifdef PISTORM_ANY_MODEL` (`vectors.c:314-784`). `VARIANT=none` — the
default, and the build we run — has none of them, and on that build the
`0xdff09a` alias is ordinary RAM.

So the reference confirms the model and simultaneously shows why we cannot
follow it: **the guest side is portable, but it rides on a Paula that only
exists in PiStorm builds.** A guest with no Paula has no step 1, no step 3
and no step 5.

This is not an inference about how the library is deployed. It follows from
the fact that on `VARIANT=none` **`INT_shadow.INTENA` has no writer at
all**. Every assignment to it in the tree is inside the PiStorm block:

```
src/aarch64/vectors.c:377   INT_shadow.INTENA |= value & 0x7fff;
src/aarch64/vectors.c:380   INT_shadow.INTENA &= ~(value & 0x7fff);
src/aarch64/vectors.c:660   INT_shadow.INTENA = *value;
src/aarch64/vectors.c:663   INT_shadow.INTENA = (INT_shadow.INTENA & 0xff00) | ...
src/aarch64/vectors.c:666   INT_shadow.INTENA = (INT_shadow.INTENA & 0x00ff) | ...
```

against `#ifdef PISTORM_ANY_MODEL` at `:314` and its `#else` at `:724`. On a
stock build the field is zero from reset and stays zero, the `and`/`cmp`
against `0x6000` can never match, and `INTF.ARM` is never set from the fast
path. The gate is not merely closed by default — there is no code path in
the firmware that can open it.

### Which makes the ask smaller than it first looked

Read that way, we are not proposing a new mechanism. We are asking for the
three Paula-impersonating steps to have a Paula-free spelling. `HOSTIRQ` is
exactly that, one bit per step:

| PiStorm path (impersonating Paula) | Paula-free equivalent |
|---|---|
| write `INTENA` `0xdff09a` with `INTEN\|EXTER` | `HOSTIRQ.ENA = 1` |
| read `INTREQR` `0xdff01e`, EXTER ORed in when `ARMPending` | `HOSTIRQ.PEND` reads 1 |
| write `INTREQ` `0xdff09c` clearing EXTER | write 1 to `HOSTIRQ.PEND` |

Same state machine, same `INT_shadow` semantics, same level-6 autovector
contract, same guest-side dispatch shape as `gic400.library`. The only
thing removed is the requirement that the guest own a Paula in order to
say those three things.

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
 bit  31    PRESENT  reads 1 on any Emu68 implementing this ABI
```

`PRESENT` matters. Today an unknown control register falls through to
`EMIT_Exception(VECTOR_ILLEGAL_INSTRUCTION)` (`M68k_LINE4.c:2395`), so
feature detection means installing an illegal-instruction handler around a
probe. That works, but a presence bit is cheaper and clearer.

There is deliberately no level field. Upstream has stated the contract as
autovector level 6, so the level is not a guest-visible parameter, and the
six SR-write sites in the table above keep comparing against a constant.
That removes the most expensive part of our original sketch.

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

Hence the split: **pending goes in a free `INTF` byte**, while **mask and
configuration live in the control register**, which is only read on the
cold path. The hot path keeps touching `INT64` and nothing else.

The union at `include/M68k.h:164-173` is what makes this free:

```c
union {
    struct {
        uint8_t ARM;        /* +0 */
        uint8_t ARM_err;    /* +1 */
        uint8_t IPL;        /* +2 */
        uint8_t RESET;      /* +3 */
        uint8_t PPC;        /* +4 */
    } INTF;
    uint64_t INT64;
} __attribute__((aligned(8)));
```

Five of eight bytes are declared, so offsets +5, +6 and +7 are available,
and `M68kReportInterrupt()` already carries the comment *"we have 8 slots in
total"* pointing at exactly them. A new `INTF.HOST` at +5 costs nothing
measurable: the same `ldr64` and the same `cbz` already read it, because
`INT64` aliases the whole thing and it is `aligned(8)` so the load stays
single-access. `ExecutionLoop.c:325` (`if (unlikely(ctx->INT64 != 0))`)
picks it up with no change at all.

Two incidental notes on that structure:

- `INTF.RESET` is declared but referenced nowhere in the tree. We assume it
  is reserved rather than dead, and have not proposed using it.
- Emu68 is built `elf64-bigaarch64`, so `INTF.ARM` is the *most* significant
  byte of `INT64` and the free slots are the least significant. Irrelevant
  to a non-zero test, but it does mean `INT64` is not a portable way to
  express priority, and we have not tried to use it as one.

### Why one mask bit is enough

`Disable()`/`Enable()` in AROS exec is already `SR.IPL` manipulation, and
Emu68 already honours it (`level > IPL_mask`, `ExecutionLoop.c:387`). Fine
grained masking would duplicate that. `ENA` exists for the one case `SR`
cannot express: "the guest has not installed a handler yet." That is the
state we need during early boot, and it is the state a stock guest has no
way to leave today.

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

### ~~Writable `LEVEL`~~ — dropped

Withdrawn. Upstream has stated the contract as autovector level 6, and we
have no case that needs otherwise. Noted only because it was in the first
version of this document.

### Vectored delivery

The 68k supports non-autovectored interrupts, where the device supplies the
vector during the acknowledge cycle. `vector = 0x60 + (level << 2)` forecloses
this today.

**With (b) settled, we withdraw this.** One handler per device requires the
raising side to know which device raised the line — which is precisely what
(b) says Emu68 does not do. A guest-supplied vector number would be
mechanically easy (the guest programs a vector into `HOSTIRQ`, Emu68 uses it
instead of the autovector), but against a single aggregate line it buys
nothing: the guest still reads the pending banks to find the source, and
then dispatches internally. Autovector 6 is the right answer here.

Recorded so the reasoning is not rediscovered later, not as a request.

### Documented delivery semantics

Edge versus level; what happens if a second host IRQ arrives before
acknowledge; exactly when `DAIF` is restored relative to the guest lowering
`SR.IPL`. We reverse-engineered our answers to these by reading emitted
code, and we could easily have got them wrong in a way that only shows up
under load. Writing them down would be valuable independently of any new
register.

## Open questions

1. Is there already a plan for the free `INTF` slots? The *"we have 8 slots
   in total"* comment in `M68kReportInterrupt()` reads like they were always
   intended as an extension point, and `INTF.RESET` is declared but unused,
   which suggests reservations we cannot see from outside. If `+5` is spoken
   for, any of the free offsets works equally well for us.

2. Is `MOVEC` the namespace you want extended, or would you prefer this in
   an MMIO window?

3. Would you rather the two defects above be fixed as part of this, or
   separately and first? We lean towards separately and first — they stand
   on their own and are much easier to review in isolation.

4. ~~Is there a non-PiStorm consumer whose expectations we would break?~~
   **Answered, in the code: no, there cannot be.** Since `INT_shadow.INTENA`
   has no writer outside `PISTORM_ANY_MODEL`, no guest on a stock build is
   receiving host interrupts today — there is no mechanism by which it
   could be. Adding one therefore has no existing behaviour to preserve.

## Summary

Settled with upstream: the guest owns the host interrupt controller and
decodes the source, and delivery is autovector level 6. We agree, and both
decisions shrink the proposal — no routing in Emu68, no level field, no
vectored delivery.

Still open, and the only thing we actually need: on a stock build there is
no supported way for a chipsetless guest to arm level-6 delivery, because
the gate is Paula's `INTENA` and its only writer is compiled out.
`gic400.library`, offered as the reference for correct behaviour, is a good
reference for the guest side and we match it — but it reaches level 6 only
because Emu68 impersonates Paula for it, which `VARIANT=none` does not do.

This also means nothing can regress: with no writer for `INT_shadow.INTENA`
outside `PISTORM_ANY_MODEL`, no stock-build guest receives host interrupts
today, so there is no existing behaviour for a new mechanism to break.

Practical answer: one control register with two meaningful bits, pending
folded into a free `INTF` byte so the JIT's inner-loop poll is unchanged.
Roughly 40 lines, no behavioural change for PiStorm, and it lets us delete
an instruction scanner from our port.

Ideal: the above plus a firmware descriptor block, so that the next guest in
our position does not have to invent the same hack we did.
