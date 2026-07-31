/*
 * Locating Emu68's interrupt shadow - PROOF OF CONCEPT.
 *
 * Everything in this file is a deliberate, isolated hack. It is the only
 * place in the port that knows anything about Emu68's compiled internals,
 * and it exists because there is no supported way to do this. If you are
 * looking for somewhere to do better, this is the file to replace; nothing
 * outside it depends on how the address is obtained.
 *
 *
 * WHY THIS IS NEEDED
 *
 * On a standalone (non-PiStorm) Emu68, a real physical IRQ reaches the
 * firmware but stops there. Emu68's core-0 IRQ fast path raises the m68k
 * level-6 line only when its internal INTENA shadow has both INTEN and
 * EXTER set:
 *
 *     ldrh w0, [x1]           ; w0 = INT_shadow.INTENA
 *     and  w0, w0, #0x6000
 *     cmp  w0, #0x6, lsl #12
 *     mov  w0, #1
 *     strb w0, [x1, #4]       ; INT_shadow.ARMPending = 1  (unconditional)
 *     b.ne skip               ; gate: no INTEN|EXTER -> no level 6
 *     mov  w0, #6
 *     strb w0, [x1, #200]     ; ctx->INTF.ARM = 6
 *
 * The code that would normally arm that shadow is the emulation of the
 * Amiga INTENA register, and it is compiled out unless Emu68 is built as a
 * PiStorm variant. So the guest has to arm it itself. It can: INT_shadow is
 * an ordinary global in Emu68's image, and the guest sees physical RAM 1:1.
 *
 * What it cannot do is know *where*, because that is a link-time address
 * that moves with every Emu68 build.
 *
 *
 * HOW THIS FINDS IT
 *
 * The address is not guessed - it is read out of the instruction stream
 * that uses it. The fast path loads INT_shadow with an adrp/add pair, so
 * scanning for the three instructions above and decoding the two before
 * them yields exactly the address Emu68 itself uses.
 *
 *     adrp x1, <page>         ; d00015e1
 *     add  x1, x1, #<lo12>    ; 9102c021
 *     ldrh w0, [x1]           ; <- anchor
 *
 * adrp is PC-relative, and the guest-physical view differs from Emu68's
 * virtual one by a page-aligned constant, so the arithmetic works in
 * physical space without knowing Emu68's virtual base at all.
 *
 * The pattern occurs twice - the IRQ handler and the FIQ handler - and both
 * reference the same shadow. Two independent sites agreeing is the only
 * check made here; if they disagree, this gives up rather than hand out an
 * address that would be written into a running firmware image.
 *
 *
 * HOW IT BREAKS
 *
 * If upstream Emu68 rewrites that instruction sequence, the scan finds
 * nothing and returns 0. The caller then runs without interrupts. That is a
 * loud, diagnosable failure rather than a corrupted firmware, which is the
 * one property worth preserving here.
 */

#include "platform.h"
#include "fdt.h"

#include <aros/macros.h>
#include <exec/types.h>

/*
 * AArch64 instruction words as they sit in memory. A64 instruction fetch is
 * always little-endian regardless of the data endianness the core runs in,
 * so a big-endian m68k 32-bit read returns them byte-reversed.
 */
#define INSN_LDRH_W0_X1  0x79400020UL   /* ldrh w0, [x1]         */
#define INSN_AND_W0_6000 0x12130400UL   /* and  w0, w0, #0x6000  */
#define INSN_CMP_W0_6000 0x7140181fUL   /* cmp  w0, #0x6, lsl #12 */

/* Emu68's image is a few MiB; this covers it with room to spare. */
#define EMU68_SCAN_LENGTH 0x00800000UL

static ULONG read_insn(ULONG address)
{
    return AROS_LE2LONG(*(volatile ULONG *)address);
}

/* adrp Xd, <page>: imm21 = immhi(23:5) : immlo(30:29), scaled by 4 KiB and
 * added to the PC's page. */
static ULONG decode_adrp(ULONG pc, ULONG insn)
{
    ULONG immhi = (insn >> 5) & 0x7ffffUL;
    ULONG immlo = (insn >> 29) & 0x3UL;
    LONG  imm   = (LONG)((immhi << 2) | immlo);

    if (imm & 0x100000L)        /* sign-extend 21 bits */
        imm -= 0x200000L;

    return (pc & ~0xfffUL) + ((ULONG)imm << 12);
}

/* add Xd, Xn, #imm12: imm12 = bits 21:10. The shift bit is not decoded -
 * the sequence this looks for never uses it. */
static ULONG decode_add_imm12(ULONG insn)
{
    return (insn >> 10) & 0xfffUL;
}

/*
 * Emu68 relocates itself to just past the top of the RAM it hands the
 * guest, so the guest's own memory range gives us where to start looking.
 */
static ULONG emu68_image_base(void)
{
    of_node_t *root = dt_find_node("/");
    of_node_t *memory = dt_find_node("/memory");
    of_property_t *reg;
    ULONG address_cells, size_cells;

    if (!root || !memory)
        return 0;

    reg = dt_find_property(memory, "reg");
    if (!reg)
        return 0;

    address_cells = dt_prop_u32_default(root, "#address-cells", 1);
    size_cells = dt_prop_u32_default(root, "#size-cells", 1);

    if (address_cells == 0 || size_cells == 0)
        return 0;

    /* Low cell of each field: the guest's window is 32-bit either way. */
    return dt_prop_u32(reg, address_cells - 1)
         + dt_prop_u32(reg, address_cells + size_cells - 1);
}

/*
 * Returns the guest-physical address of Emu68's INT_shadow, or 0 if it
 * could not be identified. Layout from Emu68's src/aarch64/vectors.c:
 *
 *     +0  uint16_t INTENA
 *     +2  uint16_t INTREQ
 *     +4  uint8_t  ARMPending
 *
 * No byte swapping applies to these: Emu68 is built elf64-bigaarch64, so it
 * shares the guest's byte order. (The BCM peripherals do not - see the
 * drivers under bcm283x/.)
 */
ULONG emu68_find_int_shadow(void)
{
    ULONG base = emu68_image_base();
    ULONG address, limit, found = 0;

    if (base == 0)
        return 0;

    limit = base + EMU68_SCAN_LENGTH;

    for (address = base + 8; address < limit; address += 4)
    {
        ULONG candidate;

        if (read_insn(address) != INSN_LDRH_W0_X1)
            continue;
        if (read_insn(address + 4) != INSN_AND_W0_6000)
            continue;
        if (read_insn(address + 8) != INSN_CMP_W0_6000)
            continue;

        candidate = decode_adrp(address - 8, read_insn(address - 8))
                  + decode_add_imm12(read_insn(address - 4));

        if (found == 0)
            found = candidate;
        else if (found != candidate)
            return 0;   /* the two sites disagree - do not guess */
    }

    return found;
}
