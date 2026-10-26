; Copyright (C) 2026, The AROS Development Team. All rights reserved.
;
; m68kemu Rigel milestone 4 display program.
;
; Shows a one-bitplane 320x256 PAL playfield with a 16x16 checkerboard.
; The Copper sets up the display and reloads the bitplane pointer every
; frame; a VERTB server cycles COLOR01. Runs for the number of frames given
; as its argument (default 250, five seconds of PAL), then returns 0, or
; the failing step.
;
;   RunM68K DISPLAY rigel_display [frames]
;
; Regenerate rigel_display_hunk.h with "make fixtures".

MEMF_CHIP       equ     $00000002
MEMF_CLEAR      equ     $00010000
NT_INTERRUPT    equ     2

_LVOAddIntServer equ    -168
_LVORemIntServer equ    -174
_LVOAllocMem    equ     -198
_LVOFreeMem     equ     -210

custom          equ     $dff000
COP1LC          equ     $080
COPJMP1         equ     $088
DIWSTRT         equ     $08e
DIWSTOP         equ     $090
DDFSTRT         equ     $092
DDFSTOP         equ     $094
DMACON          equ     $096
BPL1PTH         equ     $0e0
BPL1PTL         equ     $0e2
BPLCON0         equ     $100
BPLCON1         equ     $102
BPLCON2         equ     $104
BPL1MOD         equ     $108
COLOR00         equ     $180
COLOR01         equ     $182

SETCLR          equ     $8000
INTB_VERTB      equ     5
DMAF_MASTER     equ     $0200
DMAF_RASTER     equ     $0100
DMAF_COPPER     equ     $0080

WIDTH           equ     320
HEIGHT          equ     256
ROW_BYTES       equ     WIDTH/8
PLANE_SIZE      equ     ROW_BYTES*HEIGHT
CELL            equ     16
DEFAULT_FRAMES  equ     250
FRAME_TIMEOUT   equ     1000000         ; loop iterations without a VERTB

        section code,code

start:
        movem.l d2-d7/a2-a6,-(sp)

        ; Frame count from the argument line (A0/D0 at entry)
        move.l  #DEFAULT_FRAMES,d6
        bsr     parse_number
        tst.l   d1
        beq.s   .default
        move.l  d1,d6
.default:
        move.l  4.w,a6
        lea     custom,a5

        ; 1. Playfield and Copper list in Chip RAM
        moveq   #1,d7
        move.l  #PLANE_SIZE,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,a2
        tst.l   d0
        beq     fail
        moveq   #COPSIZE,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,a3
        tst.l   d0
        beq     fail

        ; Checkerboard: word i of row y is set when (y/16 + i) is odd
        move.l  a2,a0
        moveq   #0,d2                   ; y
.row:
        moveq   #0,d3                   ; word index
.col:
        move.l  d2,d4
        lsr.l   #4,d4
        add.l   d3,d4
        btst    #0,d4
        beq.s   .clear
        move.w  #$ffff,(a0)+
        bra.s   .next
.clear:
        clr.w   (a0)+
.next:
        addq.l  #1,d3
        cmp.l   #ROW_BYTES/2,d3
        blo.s   .col
        addq.l  #1,d2
        cmp.l   #HEIGHT,d2
        blo.s   .row

        ; 2. Copper list: display setup plus the bitplane pointer
        lea     copper,a0
        move.l  a3,a1
        moveq   #COPSIZE/4-1,d0
.copy:
        move.l  (a0)+,(a1)+
        dbf     d0,.copy
        move.l  a2,d0
        move.w  d0,cop_bplptl+2-copper(a3)
        swap    d0
        move.w  d0,cop_bplpth+2-copper(a3)

        ; 3. VERTB server cycles COLOR01
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVOAddIntServer(a6)

        move.l  a3,COP1LC(a5)
        move.w  d0,COPJMP1(a5)
        move.w  #SETCLR|DMAF_MASTER|DMAF_COPPER|DMAF_RASTER,DMACON(a5)

        ; 4. Run the requested number of frames
        moveq   #2,d7
        move.l  vb_count,d3
.frame:
        move.l  #FRAME_TIMEOUT,d1
.wait:
        cmp.l   vb_count,d3
        bne.s   .tick
        subq.l  #1,d1
        bne.s   .wait
        bra     fail
.tick:
        move.l  vb_count,d3
        cmp.l   d6,d3
        blo.s   .frame

        ; 5. Stop the display before freeing what it reads
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVORemIntServer(a6)
        move.w  #DMAF_COPPER|DMAF_RASTER,DMACON(a5)
        move.l  a2,a1
        move.l  #PLANE_SIZE,d0
        jsr     _LVOFreeMem(a6)
        move.l  a3,a1
        moveq   #COPSIZE,d0
        jsr     _LVOFreeMem(a6)

        moveq   #0,d7
fail:
        move.l  d7,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

; D1 = decimal number at the start of the argument (A0, length D0), or 0
parse_number:
        moveq   #0,d1
        move.l  a0,d2
        beq.s   .done
.skip:
        tst.l   d0
        beq.s   .done
        cmp.b   #' ',(a0)
        bne.s   .digit
        addq.l  #1,a0
        subq.l  #1,d0
        bra.s   .skip
.digit:
        tst.l   d0
        beq.s   .done
        moveq   #0,d2
        move.b  (a0)+,d2
        sub.b   #'0',d2
        cmp.b   #9,d2
        bhi.s   .done
        mulu.w  #10,d1
        add.l   d2,d1
        subq.l  #1,d0
        bra.s   .digit
.done:
        rts

; VERTB server: A1 = is_Data (the frame counter), A0 = custom
vb_server:
        addq.l  #1,(a1)
        move.l  (a1),d0
        and.w   #15,d0
        add.w   d0,d0
        lea     colors,a5
        move.w  (a5,d0.w),COLOR01(a0)
        moveq   #0,d0
        rts

copper:
        dc.w    DIWSTRT,$2c81
        dc.w    DIWSTOP,$2cc1
        dc.w    DDFSTRT,$0038
        dc.w    DDFSTOP,$00d0
        dc.w    BPLCON0,$1200           ; one bitplane, colour
        dc.w    BPLCON1,$0000
        dc.w    BPLCON2,$0000
        dc.w    BPL1MOD,$0000
        dc.w    COLOR00,$0026
cop_bplpth:
        dc.w    BPL1PTH,$0000
cop_bplptl:
        dc.w    BPL1PTL,$0000
        dc.w    $ffff,$fffe
copper_end:
COPSIZE equ     copper_end-copper

colors:
        dc.w    $0f00,$0f40,$0f80,$0fc0,$0ff0,$0af0,$04f0,$00f4
        dc.w    $00fa,$00ff,$00af,$004f,$040f,$0a0f,$0f0f,$0f08

        section state,data_f

vb_int:         dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,vb_count,vb_server
vb_count:       dc.l    0
