; Copyright (C) 2026, The AROS Development Team. All rights reserved.
;
; m68kemu Rigel milestone 2 acceptance program.
;
; Allocates Chip RAM through exec, takes a Blitter-done interrupt through a
; contained level 3 autovector handler, and calls native exec services after
; the interrupt. Returns 0 in D0 on success, otherwise the failing step.
;
; Regenerate rigel_chip_irq_hunk.h with "make fixtures".

MEMF_CHIP       equ     $00000002
MEMF_CLEAR      equ     $00010000
CHIP_END        equ     $00080000       ; Rigel Chip RAM window: 512 KiB
BUFSIZE         equ     64

_LVOAllocMem    equ     -198
_LVOFreeMem     equ     -210
_LVOAvailMem    equ     -216
_LVOTypeOfMem   equ     -534

custom          equ     $dff000
INTREQR         equ     $01e
BLTCON0         equ     $040
BLTCON1         equ     $042
BLTAFWM         equ     $044
BLTALWM         equ     $046
BLTAPT          equ     $050
BLTDPT          equ     $054
BLTSIZE         equ     $058
BLTAMOD         equ     $064
BLTDMOD         equ     $066
DMACON          equ     $096
INTENA          equ     $09a
INTREQ          equ     $09c

SETCLR          equ     $8000
INTF_INTEN      equ     $4000
INTF_BLIT       equ     $0040
INTF_LEVEL3     equ     $0070           ; COPER | VERTB | BLIT
DMAF_MASTER     equ     $0200
DMAF_BLITTER    equ     $0040

VEC_LEVEL3      equ     $6c

        section code,code

start:
        movem.l d2-d7/a2-a6,-(sp)
        move.l  4.w,a6
        lea     custom,a5

        ; Hunk memory attributes: bss_c in Chip, data_f in Fast
        moveq   #1,d7
        lea     chipbuf,a0
        cmpa.l  #CHIP_END,a0
        bhs     fail
        moveq   #2,d7
        lea     blit_irqs,a0
        cmpa.l  #CHIP_END,a0
        blo     fail

        ; 1. Allocate source and destination with MEMF_CHIP
        moveq   #3,d7
        moveq   #BUFSIZE,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,a2
        tst.l   d0
        beq     fail
        moveq   #BUFSIZE,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,a3
        tst.l   d0
        beq     fail

        ; 2. Both buffers lie entirely inside the Chip RAM window
        moveq   #4,d7
        lea     BUFSIZE(a2),a0
        cmpa.l  #CHIP_END,a0
        bhi     fail
        lea     BUFSIZE(a3),a0
        cmpa.l  #CHIP_END,a0
        bhi     fail
        moveq   #5,d7
        move.l  a2,a1
        jsr     _LVOTypeOfMem(a6)
        and.l   #MEMF_CHIP,d0
        beq     fail

        ; 3. Install the contained level 3 autovector handler
        move.l  VEC_LEVEL3.w,old_vec
        move.l  #irq_level3,VEC_LEVEL3.w

        move.w  #$cafe,(a2)

        ; 4. Enable the BLIT interrupt and copy one word A -> D
        move.w  #SETCLR|INTF_INTEN|INTF_BLIT,INTENA(a5)
        move.w  #SETCLR|DMAF_MASTER|DMAF_BLITTER,DMACON(a5)
        move.w  #$09f0,BLTCON0(a5)
        move.w  #0,BLTCON1(a5)
        move.w  #$ffff,BLTAFWM(a5)
        move.w  #$ffff,BLTALWM(a5)
        move.w  #0,BLTAMOD(a5)
        move.w  #0,BLTDMOD(a5)
        move.l  a2,BLTAPT(a5)
        move.l  a3,BLTDPT(a5)
        move.w  #(1<<6)|1,BLTSIZE(a5)

        ; 5. Wait (bounded) for the handler to count the interrupt
        moveq   #6,d7
        move.l  #100000,d1
.wait:
        tst.l   blit_irqs
        bne.s   .seen
        subq.l  #1,d1
        bne.s   .wait
        bra     fail
.seen:
        move.w  #1000,d1                ; let chipset time pass
.settle:
        dbf     d1,.settle

        ; 6. DMA result and exactly one interrupt, acknowledged
        moveq   #7,d7
        cmp.w   #$cafe,(a3)
        bne     fail
        moveq   #8,d7
        cmp.l   #1,blit_irqs
        bne     fail
        moveq   #9,d7
        tst.l   other_irqs
        bne     fail
        moveq   #10,d7
        move.w  INTREQR(a5),d0
        and.w   #INTF_BLIT,d0
        bne     fail

        move.w  #INTF_BLIT,INTENA(a5)
        move.l  old_vec,VEC_LEVEL3.w

        ; 7. Native exec services after the interrupt
        moveq   #11,d7
        move.l  a2,a1
        moveq   #BUFSIZE,d0
        jsr     _LVOFreeMem(a6)
        move.l  a3,a1
        moveq   #BUFSIZE,d0
        jsr     _LVOFreeMem(a6)
        move.l  #MEMF_CHIP,d1
        jsr     _LVOAvailMem(a6)
        tst.l   d0
        beq     fail

        ; 8. Deterministic result
        moveq   #0,d7
fail:
        move.l  d7,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

irq_level3:
        move.l  d0,-(sp)
        move.w  custom+INTREQR,d0
        and.w   #INTF_BLIT,d0
        beq.s   .other
        move.w  #INTF_BLIT,custom+INTREQ
        addq.l  #1,blit_irqs
        bra.s   .done
.other:
        move.w  #INTF_LEVEL3&~INTF_BLIT,custom+INTREQ
        addq.l  #1,other_irqs
.done:
        move.l  (sp)+,d0
        rte

        section state,data_f

blit_irqs:      dc.l    0
other_irqs:     dc.l    0
old_vec:        dc.l    0

        section chipbuf,bss_c

chipbuf:        ds.b    BUFSIZE
