; Copyright (C) 2026, The AROS Development Team. All rights reserved.
;
; m68kemu Rigel milestone 3 acceptance program.
;
; Runs a Copper list from Chip RAM, counts VERTB and COPER interrupts
; through Exec servers, checks Disable()/Enable(), and runs a Blitter
; handler installed with SetIntVector(). Returns 0 in D0 on success,
; otherwise the failing step.
;
; Regenerate rigel_exec_irq_hunk.h with "make fixtures".

MEMF_CHIP       equ     $00000002
MEMF_CLEAR      equ     $00010000
NT_INTERRUPT    equ     2

_LVODisable     equ     -120
_LVOEnable      equ     -126
_LVOSetIntVector equ    -162
_LVOAddIntServer equ    -168
_LVORemIntServer equ    -174
_LVOAllocMem    equ     -198

custom          equ     $dff000
INTENAR         equ     $01c
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
COP1LC          equ     $080
COPJMP1         equ     $088
DMACON          equ     $096
INTENA          equ     $09a
INTREQ          equ     $09c
COLOR00         equ     $180

SETCLR          equ     $8000
INTF_INTEN      equ     $4000
INTF_BLIT       equ     $0040
INTF_VERTB      equ     $0020
INTF_COPER      equ     $0010
INTB_COPER      equ     4
INTB_VERTB      equ     5
INTB_BLIT       equ     6
DMAF_MASTER     equ     $0200
DMAF_COPPER     equ     $0080
DMAF_BLITTER    equ     $0040

WAIT_LINE       equ     $80
FRAMES          equ     5
COPSIZE         equ     20
TIMEOUT         equ     2000000
SPIN            equ     40000           ; several frames of CPU time

        section code,code

start:
        movem.l d2-d7/a2-a6,-(sp)
        move.l  4.w,a6
        lea     custom,a5

        ; 1. Copper list in Chip RAM
        moveq   #1,d7
        moveq   #COPSIZE,d0
        move.l  #MEMF_CHIP|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,a2
        tst.l   d0
        beq     fail
        lea     coplist,a0
        move.l  a2,a1
        moveq   #COPSIZE/4-1,d0
.copy:
        move.l  (a0)+,(a1)+
        dbf     d0,.copy

        ; 2. Exec servers, then start the Copper
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVOAddIntServer(a6)
        moveq   #INTB_COPER,d0
        lea     cop_int,a1
        jsr     _LVOAddIntServer(a6)
        moveq   #2,d7
        move.w  INTENAR(a5),d0
        and.w   #INTF_INTEN|INTF_VERTB|INTF_COPER,d0
        cmp.w   #INTF_INTEN|INTF_VERTB|INTF_COPER,d0
        bne     fail

        move.l  a2,COP1LC(a5)
        move.w  d0,COPJMP1(a5)
        move.w  #SETCLR|DMAF_MASTER|DMAF_COPPER,DMACON(a5)

        ; 3. VERTB and COPER counts move together
        moveq   #3,d7
        moveq   #FRAMES,d2
        bsr     wait_vb
        bne     fail
        moveq   #4,d7
        move.l  cop_count,d0
        sub.l   vb_count,d0
        addq.l  #1,d0                   ; -1..1 -> 0..2
        cmp.l   #2,d0
        bhi     fail
        tst.l   cop_count
        beq     fail
        moveq   #5,d7
        tst.l   bad_args
        bne     fail

        ; 4. No server runs under Disable(); they resume after Enable()
        moveq   #6,d7
        jsr     _LVODisable(a6)
        move.l  vb_count,d3
        move.l  #SPIN,d1
.spin1:
        subq.l  #1,d1
        bne.s   .spin1
        cmp.l   vb_count,d3
        bne     fail
        jsr     _LVOEnable(a6)
        moveq   #7,d7
        move.l  d3,d2
        addq.l  #2,d2
        bsr     wait_vb
        bne     fail

        ; 5. Blitter handler through SetIntVector()
        moveq   #8,d7
        moveq   #INTB_BLIT,d0
        lea     blit_int,a1
        jsr     _LVOSetIntVector(a6)
        move.l  d0,d4                   ; previous handler
        bne     fail
        move.w  #SETCLR|INTF_BLIT,INTENA(a5)
        move.w  #SETCLR|DMAF_MASTER|DMAF_BLITTER,DMACON(a5)
        lea     blitbuf,a0
        move.w  #$1234,(a0)
        clr.w   2(a0)
        move.w  #$09f0,BLTCON0(a5)
        move.w  #0,BLTCON1(a5)
        move.w  #$ffff,BLTAFWM(a5)
        move.w  #$ffff,BLTALWM(a5)
        move.w  #0,BLTAMOD(a5)
        move.w  #0,BLTDMOD(a5)
        move.l  a0,BLTAPT(a5)
        pea     2(a0)
        move.l  (sp)+,BLTDPT(a5)
        move.w  #(1<<6)|1,BLTSIZE(a5)

        moveq   #9,d7
        move.l  #TIMEOUT,d1
.bwait:
        tst.l   blit_count
        bne.s   .bdone
        subq.l  #1,d1
        bne.s   .bwait
        bra     fail
.bdone:
        moveq   #10,d7
        cmp.l   #1,blit_count
        bne     fail
        cmp.w   #$1234,blitbuf+2
        bne     fail
        tst.l   bad_args
        bne     fail
        move.w  INTREQR(a5),d0
        and.w   #INTF_BLIT,d0
        bne     fail

        moveq   #11,d7
        move.w  #INTF_BLIT,INTENA(a5)
        moveq   #INTB_BLIT,d0
        move.l  d4,a1
        jsr     _LVOSetIntVector(a6)
        cmp.l   #blit_int,d0
        bne     fail

        ; 6. Removing the servers clears their INTENA bits
        moveq   #12,d7
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVORemIntServer(a6)
        moveq   #INTB_COPER,d0
        lea     cop_int,a1
        jsr     _LVORemIntServer(a6)
        move.w  INTENAR(a5),d0
        and.w   #INTF_VERTB|INTF_COPER,d0
        bne     fail
        moveq   #13,d7
        move.l  vb_count,d3
        move.l  #SPIN,d1
.spin2:
        subq.l  #1,d1
        bne.s   .spin2
        cmp.l   vb_count,d3
        bne     fail

        ; 7. Deterministic result; the Copper keeps drawing for the host
        moveq   #0,d7
fail:
        move.l  d7,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

; Wait until vb_count >= D2. Returns D0 = 0 (Z set) or 1 on timeout.
wait_vb:
        move.l  #TIMEOUT,d1
.loop:
        cmp.l   vb_count,d2
        bls.s   .ok
        subq.l  #1,d1
        bne.s   .loop
        moveq   #1,d0
        rts
.ok:
        moveq   #0,d0
        rts

; Servers: A0 = custom, A1 = is_Data. Return Z set to continue the chain.
vb_server:
        cmp.l   #vb_data,a1
        bne.s   bad_server
        cmp.l   #custom,a0
        bne.s   bad_server
        addq.l  #1,vb_count
        moveq   #0,d0
        rts

cop_server:
        cmp.l   #cop_data,a1
        bne.s   bad_server
        cmp.l   #custom,a0
        bne.s   bad_server
        addq.l  #1,cop_count
        moveq   #0,d0
        rts

bad_server:
        addq.l  #1,bad_args
        moveq   #0,d0
        rts

; Handler: D1 = INTENAR & INTREQR, A0 = custom, A1 = is_Data.
blit_handler:
        move.w  #INTF_BLIT,INTREQ(a0)
        cmp.l   #blit_data,a1
        bne.s   .bad
        btst    #INTB_BLIT,d1
        beq.s   .bad
        addq.l  #1,blit_count
        rts
.bad:
        addq.l  #1,bad_args
        rts

; COLOR00 black, wait for WAIT_LINE, COLOR00 red, raise COPER, end
coplist:
        dc.w    COLOR00,$0000
        dc.w    (WAIT_LINE<<8)|$01,$fffe
        dc.w    COLOR00,$0f00
        dc.w    INTREQ,SETCLR|INTF_COPER
        dc.w    $ffff,$fffe

        section state,data_f

; struct Interrupt: ln_Succ, ln_Pred, ln_Type, ln_Pri, ln_Name,
;                   is_Data, is_Code
vb_int:         dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,vb_data,vb_server
cop_int:        dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,cop_data,cop_server
blit_int:       dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,blit_data,blit_handler

vb_data:        dc.l    0
cop_data:       dc.l    0
blit_data:      dc.l    0
vb_count:       dc.l    0
cop_count:      dc.l    0
blit_count:     dc.l    0
bad_args:       dc.l    0

        section chipbuf,bss_c

blitbuf:        ds.w    2
