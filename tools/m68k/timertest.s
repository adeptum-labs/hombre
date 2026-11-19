| The Hombre Operating System.
| Copyright © 2026 Adam Waldenberg and Adeptum AB.
|
| SPDX-License-Identifier: GPL-3.0-or-later
|
| This program is free software: you can redistribute it and/or modify
| it under the terms of the GNU General Public License as published by
| the Free Software Foundation, either version 3 of the License, or
| (at your option) any later version.
|
| This program is distributed in the hope that it will be useful,
| but WITHOUT ANY WARRANTY; without even the implied warranty of
| MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
| GNU General Public License for more details.
|
| You should have received a copy of the GNU General Public License
| along with this program.  If not, see <https://www.gnu.org/licenses/>.
|
| Website: https://www.adeptum.se
| Contact: info@adeptum.se

| Exercises timer.device (OpenDevice, GetSysTime, SubTime, DoIO, SendIO,
| CheckIO, AbortIO, WaitIO, CloseDevice) and dos.library Delay.

        .set    CloseLibrary, -414
        .set    OpenDevice, -444
        .set    CloseDevice, -450
        .set    DoIO, -456
        .set    SendIO, -462
        .set    CheckIO, -468
        .set    WaitIO, -474
        .set    AbortIO, -480
        .set    OpenLibrary, -552
        .set    CreateIORequest, -654
        .set    DeleteIORequest, -660
        .set    CreateMsgPort, -666
        .set    DeleteMsgPort, -672

        .set    Delay, -198
        .set    PutStr, -948
        .set    VPrintf, -954

        .set    SubTime, -48
        .set    GetSysTime, -66

        .set    io_Device, 20
        .set    io_Command, 28
        .set    io_Error, 31
        .set    tv_secs, 32
        .set    tv_micro, 36
        .set    TR_SIZE, 40

        .set    UNIT_MICROHZ, 0
        .set    TR_ADDREQUEST, 9
        .set    IOERR_ABORTED, -2
        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A5 = dos.library, A4 = TimerBase, A3 = timerequest, D7 = MsgPort,
| D6 = return code.
_start:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        moveq   #RETURN_FAIL,%d6
        suba.l  %a5,%a5
        suba.l  %a4,%a4
        suba.l  %a3,%a3
        moveq   #0,%d7

        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a5
        tst.l   %d0
        beq     done

        jsr     CreateMsgPort(%a6)
        move.l  %d0,%d7
        beq     close_dos

        movea.l %d7,%a0
        moveq   #TR_SIZE,%d0
        jsr     CreateIORequest(%a6)
        movea.l %d0,%a3
        tst.l   %d0
        beq     del_port

        lea     timername(%pc),%a0
        moveq   #UNIT_MICROHZ,%d0
        movea.l %a3,%a1
        moveq   #0,%d1
        jsr     OpenDevice(%a6)
        tst.l   %d0
        bne     del_req
        movea.l io_Device(%a3),%a4

        | Part 1: 1 s DoIO request measured with GetSysTime.
        movea.l %a4,%a6
        lea     t0,%a0
        jsr     GetSysTime(%a6)

        move.w  #TR_ADDREQUEST,io_Command(%a3)
        move.l  #1,tv_secs(%a3)
        clr.l   tv_micro(%a3)
        movea.l 4.w,%a6
        movea.l %a3,%a1
        jsr     DoIO(%a6)
        tst.l   %d0
        bne     close_dev

        movea.l %a4,%a6
        lea     t1,%a0
        jsr     GetSysTime(%a6)

        lea     t1,%a0
        lea     t0,%a1
        bsr     diff_ms
        move.l  %d0,%d4

        move.l  %d4,-(%sp)
        lea     fmt_slept(%pc),%a0
        move.l  %a0,%d1
        move.l  %sp,%d2
        movea.l %a5,%a6
        jsr     VPrintf(%a6)
        addq.l  #4,%sp

        cmpi.l  #900,%d4
        blt.s   timer_bad
        cmpi.l  #1500,%d4
        bgt.s   timer_bad
        lea     msg_timer_ok(%pc),%a0
        bsr     put_str
        bra.s   part_abort
timer_bad:
        lea     msg_timer_wrong(%pc),%a0
        bsr     put_str
        bra     close_dev

        | Part 2: SendIO 5 s request, CheckIO == 0, AbortIO, WaitIO == -2.
part_abort:
        move.w  #TR_ADDREQUEST,io_Command(%a3)
        move.l  #5,tv_secs(%a3)
        clr.l   tv_micro(%a3)
        movea.l 4.w,%a6
        movea.l %a3,%a1
        jsr     SendIO(%a6)

        movea.l %a3,%a1
        jsr     CheckIO(%a6)
        tst.l   %d0
        bne     close_dev

        movea.l %a3,%a1
        jsr     AbortIO(%a6)

        movea.l %a3,%a1
        jsr     WaitIO(%a6)
        cmpi.l  #IOERR_ABORTED,%d0
        bne     close_dev
        cmpi.b  #IOERR_ABORTED,io_Error(%a3)
        bne     close_dev

        lea     msg_abort_ok(%pc),%a0
        bsr     put_str

        | Part 3: Delay(25) measured with GetSysTime (400..900 ms).
        movea.l %a4,%a6
        lea     t0,%a0
        jsr     GetSysTime(%a6)

        moveq   #25,%d1
        movea.l %a5,%a6
        jsr     Delay(%a6)

        movea.l %a4,%a6
        lea     t1,%a0
        jsr     GetSysTime(%a6)

        lea     t1,%a0
        lea     t0,%a1
        bsr     diff_ms
        move.l  %d0,%d4

        cmpi.l  #400,%d4
        blt     close_dev
        cmpi.l  #900,%d4
        bgt     close_dev

        lea     msg_delay_ok(%pc),%a0
        bsr     put_str
        moveq   #0,%d6

close_dev:
        movea.l 4.w,%a6
        movea.l %a3,%a1
        jsr     CloseDevice(%a6)
del_req:
        movea.l 4.w,%a6
        movea.l %a3,%a0
        jsr     DeleteIORequest(%a6)
del_port:
        movea.l 4.w,%a6
        movea.l %d7,%a0
        jsr     DeleteMsgPort(%a6)
close_dos:
        movea.l 4.w,%a6
        movea.l %a5,%a1
        jsr     CloseLibrary(%a6)
done:
        move.l  %d6,%d0
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| Computes (A0 - A1) in milliseconds into D0 using TimerBase in A4.
diff_ms:
        lea     tdiff,%a2
        move.l  (%a0),(%a2)
        move.l  4(%a0),4(%a2)
        movea.l %a2,%a0
        movea.l %a4,%a6
        jsr     SubTime(%a6)
        move.l  (%a2),%d0
        move.l  %d0,%d1
        lsl.l   #8,%d0
        lsl.l   #2,%d0
        lsl.l   #4,%d1
        sub.l   %d1,%d0
        lsr.l   #1,%d1
        sub.l   %d1,%d0
        move.l  4(%a2),%d1
        divu.w  #1000,%d1
        andi.l  #0xFFFF,%d1
        add.l   %d1,%d0
        rts

put_str:
        move.l  %a0,%d1
        movea.l %a5,%a6
        jsr     PutStr(%a6)
        rts

dosname:        .asciz  "dos.library"
timername:      .asciz  "timer.device"
fmt_slept:      .asciz  "timertest: slept %ld ms\n"
msg_timer_ok:   .asciz  "timertest: timer ok\n"
msg_timer_wrong:.asciz  "timertest: timer wrong\n"
msg_abort_ok:   .asciz  "timertest: abort ok\n"
msg_delay_ok:   .asciz  "timertest: delay ok\n"
        .even
        .data
        .even
t0:             .long   0, 0
t1:             .long   0, 0
tdiff:          .long   0, 0
