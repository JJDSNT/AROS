; Copyright (C) 2026, The AROS Development Team. All rights reserved.
;
; m68kemu Rigel milestone 5 acceptance program.
;
; A PORTS server reads CIA-A: keyboard bytes from SDR (with the SPMODE
; handshake) and timer A interrupts. Timer A runs at about 100 Hz. After
; FRAMES frames the program checks the keys, mouse counters and buttons
; the host injected (see test_rigel_input.cpp) and returns 0, or the
; failing step.
;
; Regenerate rigel_input_hunk.h with "make fixtures".

NT_INTERRUPT    equ     2
_LVOAddIntServer equ    -168
_LVORemIntServer equ    -174

INTB_PORTS      equ     3
INTB_VERTB      equ     5

custom          equ     $dff000
JOY0DAT         equ     $00a
POTGOR          equ     $016

ciaa_pra        equ     $bfe001
ciaa_talo       equ     $bfe401
ciaa_tahi       equ     $bfe501
ciaa_sdr        equ     $bfec01
ciaa_icr        equ     $bfed01
ciaa_cra        equ     $bfee01

ICRF_SETCLR     equ     $80
ICRF_SP         equ     $08
ICRF_TA         equ     $01
CRAF_SPMODE     equ     $40
CRAF_LOAD       equ     $10
CRAF_START      equ     $01

TIMER_LATCH     equ     7094            ; E clock 709379 Hz / 100
FRAMES          equ     30
SAMPLE_FROM     equ     5               ; timer measured over frames 5..25
SAMPLE_TO       equ     25
FRAME_TIMEOUT   equ     1000000
MAX_KEYS        equ     8

        section code,code

start:
        movem.l d2-d7/a2-a6,-(sp)
        move.l  4.w,a6
        lea     custom,a5

        ; 1. Servers first, then CIA-A: timer A continuous, TA and SP on
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVOAddIntServer(a6)
        moveq   #INTB_PORTS,d0
        lea     ports_int,a1
        jsr     _LVOAddIntServer(a6)

        move.b  #$7f,ciaa_icr           ; mask everything
        move.b  #0,ciaa_cra             ; stopped, continuous, SP input
        move.b  #TIMER_LATCH&$ff,ciaa_talo
        move.b  #TIMER_LATCH>>8,ciaa_tahi
        move.b  #ICRF_SETCLR|ICRF_SP|ICRF_TA,ciaa_icr
        move.b  #CRAF_LOAD|CRAF_START,ciaa_cra

        ; 2. Run FRAMES frames, sampling the timer count on the way
        moveq   #2,d7
        moveq   #0,d3                   ; frames seen
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
        cmp.l   #SAMPLE_FROM,d3
        bne.s   .not_from
        move.l  timer_count,d4
.not_from:
        cmp.l   #SAMPLE_TO,d3
        bne.s   .not_to
        move.l  timer_count,d5
.not_to:
        cmp.l   #FRAMES,d3
        blo.s   .frame

        move.b  #0,ciaa_cra             ; stop timer A
        move.b  #ICRF_TA|ICRF_SP,ciaa_icr
        moveq   #INTB_PORTS,d0
        lea     ports_int,a1
        jsr     _LVORemIntServer(a6)
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVORemIntServer(a6)

        ; 3. Keys in order: A down, A up, Space down, Space up
        moveq   #3,d7
        cmp.l   #4,key_count
        bne     fail
        moveq   #4,d7
        lea     keys,a0
        lea     expected_keys,a1
        moveq   #4-1,d0
.key:
        cmpm.b  (a0)+,(a1)+
        bne     fail
        dbf     d0,.key

        ; Mouse counters: X +5 +2, Y -3 +1
        moveq   #5,d7
        cmp.w   #$fe07,JOY0DAT(a5)
        bne     fail

        ; Left button on CIA-A PRA bit 6, right on POTGOR bit 10 (low)
        moveq   #6,d7
        btst    #6,ciaa_pra
        bne     fail
        moveq   #7,d7
        move.w  POTGOR(a5),d0
        btst    #10,d0
        bne     fail

        ; 4. Timer A at 100 Hz: two per 20 ms frame over 20 frames
        moveq   #8,d7
        sub.l   d4,d5
        cmp.l   #38,d5
        blt     fail
        cmp.l   #42,d5
        bgt     fail

        moveq   #0,d7
fail:
        move.l  d7,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

; VERTB server: A1 = &vb_count
vb_server:
        addq.l  #1,(a1)
        moveq   #0,d0
        rts

; PORTS server: reading ICR acknowledges CIA-A
ports_server:
        move.l  d2,-(sp)
        move.b  ciaa_icr,d2
        btst    #0,d2
        beq.s   .no_timer
        addq.l  #1,timer_count
.no_timer:
        btst    #3,d2
        beq.s   .done
        move.b  ciaa_sdr,d0
        not.b   d0
        ror.b   #1,d0                   ; keycode | up << 7
        move.l  key_count,d1
        cmp.l   #MAX_KEYS,d1
        bhs.s   .handshake
        lea     keys,a0
        move.b  d0,(a0,d1.l)
        addq.l  #1,key_count
.handshake:
        or.b    #CRAF_SPMODE,ciaa_cra   ; acknowledge: pulse KDAT low
        moveq   #20,d1
.hold:
        dbf     d1,.hold
        and.b   #~CRAF_SPMODE&$ff,ciaa_cra
.done:
        move.l  (sp)+,d2
        moveq   #0,d0                   ; Z: let other PORTS servers run
        rts

expected_keys:
        dc.b    $20,$a0,$40,$c0

        section state,data_f

vb_int:         dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,vb_count,vb_server
ports_int:      dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,0,ports_server

vb_count:       dc.l    0
timer_count:    dc.l    0
key_count:      dc.l    0
keys:           ds.b    MAX_KEYS
