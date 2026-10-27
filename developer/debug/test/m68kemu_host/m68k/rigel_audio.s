; Copyright (C) 2026, The AROS Development Team. All rights reserved.
;
; m68kemu Rigel milestone 6 audio program.
;
; Plays a 32-byte square wave on audio channel 0 at period 252 (about
; 440 Hz) and volume 64, and counts channel 0 block interrupts through a
; SetIntVector() handler. Silent for the first frames so the host can see
; silence, then plays for the number of frames given as its argument
; (default 250, five seconds of PAL). Returns 0, 6 when no channel 0
; interrupt arrived at all (Rigel ISSUE-0018), or the failing step.
;
;   RunM68K DISPLAY rigel_audio [frames]
;
; Regenerate rigel_audio_hunk.h with "make fixtures".

MEMF_CHIP       equ     $00000002
NT_INTERRUPT    equ     2

_LVOSetIntVector equ    -162
_LVOAddIntServer equ    -168
_LVORemIntServer equ    -174
_LVOAllocMem    equ     -198
_LVOFreeMem     equ     -210

INTB_VERTB      equ     5
INTB_AUD0       equ     7

custom          equ     $dff000
DMACON          equ     $096
INTENA          equ     $09a
INTREQ          equ     $09c
AUD0LC          equ     $0a0
AUD0LEN         equ     $0a4
AUD0PER         equ     $0a6
AUD0VOL         equ     $0a8

SETCLR          equ     $8000
INTF_AUD0       equ     $0080
DMAF_MASTER     equ     $0200
DMAF_AUD0       equ     $0001

SAMPLE_BYTES    equ     32
PERIOD          equ     252             ; 3546895 / (252 * 32) = 439.8 Hz
SILENT_FRAMES   equ     5
DEFAULT_FRAMES  equ     250
SAMPLE_FROM     equ     10              ; block interrupts counted from here
FRAME_TIMEOUT   equ     1000000

        section code,code

start:
        movem.l d2-d7/a2-a6,-(sp)

        move.l  #DEFAULT_FRAMES,d6
        bsr     parse_number
        tst.l   d1
        beq.s   .default
        move.l  d1,d6
.default:
        add.l   #SILENT_FRAMES,d6
        move.l  4.w,a6
        lea     custom,a5

        ; 1. Square wave in Chip RAM: 16 high, 16 low
        moveq   #1,d7
        moveq   #SAMPLE_BYTES,d0
        moveq   #MEMF_CHIP,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,a2
        tst.l   d0
        beq     fail
        move.l  a2,a0
        moveq   #SAMPLE_BYTES/2-1,d0
.high:  move.b  #100,(a0)+
        dbf     d0,.high
        moveq   #SAMPLE_BYTES/2-1,d0
.low:   move.b  #-100,(a0)+
        dbf     d0,.low

        ; 2. Frame counter and channel 0 block handler
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVOAddIntServer(a6)
        moveq   #2,d7
        moveq   #INTB_AUD0,d0
        lea     aud_int,a1
        jsr     _LVOSetIntVector(a6)
        move.l  d0,d5                   ; previous handler
        bne     fail

        ; Silence first
        moveq   #3,d7
        moveq   #SILENT_FRAMES,d2
        bsr     wait_frames
        bne     fail

        move.l  a2,AUD0LC(a5)
        move.w  #SAMPLE_BYTES/2,AUD0LEN(a5)
        move.w  #PERIOD,AUD0PER(a5)
        move.w  #64,AUD0VOL(a5)
        move.w  #SETCLR|INTF_AUD0,INTENA(a5)
        move.w  #SETCLR|DMAF_MASTER|DMAF_AUD0,DMACON(a5)

        ; 3. Play; count block interrupts over 50 frames (one second)
        moveq   #4,d7
        moveq   #SAMPLE_FROM,d2
        bsr     wait_frames
        bne     fail
        move.l  aud_count,d3
        moveq   #SAMPLE_FROM+50,d2
        bsr     wait_frames
        bne     fail
        move.l  aud_count,d4
        sub.l   d3,d4                   ; checked after cleanup

        move.l  d6,d2
        bsr     wait_frames
        bne     fail

        ; Stop the channel before freeing its sample
        move.w  #DMAF_AUD0,DMACON(a5)
        move.w  #INTF_AUD0,INTENA(a5)
        moveq   #INTB_AUD0,d0
        move.l  d5,a1
        jsr     _LVOSetIntVector(a6)
        moveq   #INTB_VERTB,d0
        lea     vb_int,a1
        jsr     _LVORemIntServer(a6)
        move.l  a2,a1
        moveq   #SAMPLE_BYTES,d0
        jsr     _LVOFreeMem(a6)

        ; ~440 block interrupts per second
        moveq   #6,d7
        tst.l   d4
        beq.s   fail
        moveq   #5,d7
        cmp.l   #430,d4
        blt.s   fail
        cmp.l   #450,d4
        bgt.s   fail

        moveq   #0,d7
fail:
        move.l  d7,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

; Wait until vb_count >= D2; D0 = 0 (Z set), or 1 when frames stop coming.
; Preserves D1 and D3 (MOVEM leaves the condition codes alone).
wait_frames:
        movem.l d1/d3,-(sp)
        move.l  vb_count,d3
.frame:
        cmp.l   d2,d3
        bhs.s   .ok
        move.l  #FRAME_TIMEOUT,d1
.wait:
        cmp.l   vb_count,d3
        bne.s   .tick
        subq.l  #1,d1
        bne.s   .wait
        moveq   #1,d0
        movem.l (sp)+,d1/d3
        rts
.tick:
        move.l  vb_count,d3
        bra.s   .frame
.ok:
        moveq   #0,d0
        movem.l (sp)+,d1/d3
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

vb_server:
        addq.l  #1,(a1)
        moveq   #0,d0
        rts

; Channel 0 handler: A0 = custom, acknowledges its own INTREQ bit
aud_handler:
        move.w  #INTF_AUD0,INTREQ(a0)
        addq.l  #1,aud_count
        rts

        section state,data_f

vb_int:         dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,vb_count,vb_server
aud_int:        dc.l    0,0
                dc.b    NT_INTERRUPT,0
                dc.l    0,0,aud_handler

vb_count:       dc.l    0
aud_count:      dc.l    0
