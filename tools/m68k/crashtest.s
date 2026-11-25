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

| Leaves a task behind that a crash has damaged: prints a line and waits for
| a signal that never comes, while a task it starts writes over the links of
| its Task, which exec keeps on its list of waiting tasks, and ends.
|
| With an argument starting with t it instead asks timer.device for a reply
| in eight seconds and waits for a signal that never comes, so the request
| is still pending when something ends it. A public port it adds lies
| between its own port and the request, so that ending it frees the two
| apart and the request starts a free chunk of its own.

        .set    AllocMem, -198
        .set    AddTask, -282
        .set    FindTask, -294
        .set    Wait, -318
        .set    CloseLibrary, -414
        .set    OpenLibrary, -552
        .set    Write, -48
        .set    Output, -60
        .set    OpenDevice, -444
        .set    SendIO, -462
        .set    CreateIORequest, -654
        .set    CreateMsgPort, -666
        .set    AddPort, -354

        .set    mp_Flags, 14
        .set    mp_MsgList, 20
        .set    MP_SIZE, 34
        .set    PA_IGNORE, 2

        .set    io_Command, 28
        .set    tv_secs, 32
        .set    tv_micro, 36
        .set    TR_SIZE, 40
        .set    TR_ADDREQUEST, 9
        .set    UNIT_MICROHZ, 0

        .set    LN_TYPE, 8
        .set    LN_NAME, 10
        .set    TC_SPLOWER, 58
        .set    TC_SPUPPER, 62
        .set    TC_SPREG, 54
        .set    NT_TASK, 1
        .set    TASK_BYTES, 92
        .set    STACK_BYTES, 1024
        .set    MEMF_PUBLIC_CLEAR, 0x10001

        .set    RETURN_FAIL, 20

        .text
        .globl  _start

_start:
        movem.l %d2-%d4/%a2-%a3/%a5-%a6,-(%sp)
        move.b  (%a0),%d4
        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        tst.l   %d0
        beq.s   fail
        movea.l %d0,%a5
        cmpi.b  #'t',%d4
        beq.s   timer
        movea.l %a5,%a6
        jsr     Output(%a6)
        move.l  %d0,%d1
        lea     message(%pc),%a0
        move.l  %a0,%d2
        moveq   #message_end-message,%d3
        jsr     Write(%a6)
        movea.l 4.w,%a6
        suba.l  %a1,%a1
        jsr     FindTask(%a6)
        lea     parent(%pc),%a0
        move.l  %d0,(%a0)
        move.l  #TASK_BYTES+STACK_BYTES,%d0
        move.l  #MEMF_PUBLIC_CLEAR,%d1
        jsr     AllocMem(%a6)
        tst.l   %d0
        beq.s   fail
        movea.l %d0,%a2
        move.b  #NT_TASK,LN_TYPE(%a2)
        lea     childname(%pc),%a0
        move.l  %a0,LN_NAME(%a2)
        lea     TASK_BYTES(%a2),%a0
        move.l  %a0,TC_SPLOWER(%a2)
        lea     STACK_BYTES(%a0),%a0
        move.l  %a0,TC_SPUPPER(%a2)
        move.l  %a0,TC_SPREG(%a2)
        movea.l %a2,%a1
        lea     child(%pc),%a2
        suba.l  %a3,%a3
        jsr     AddTask(%a6)
        moveq   #0,%d0
        bset    #30,%d0
        jsr     Wait(%a6)
fail:   moveq   #RETURN_FAIL,%d0
        movem.l (%sp)+,%d2-%d4/%a2-%a3/%a5-%a6
        rts

| The pending timer request.
timer:  movea.l %a5,%a6
        jsr     Output(%a6)
        move.l  %d0,%d1
        lea     timing(%pc),%a0
        move.l  %a0,%d2
        moveq   #timing_end-timing,%d3
        jsr     Write(%a6)
        movea.l 4.w,%a6
        jsr     CreateMsgPort(%a6)
        tst.l   %d0
        beq.s   fail
        move.l  %d0,%d4
        moveq   #MP_SIZE,%d0
        move.l  #MEMF_PUBLIC_CLEAR,%d1
        jsr     AllocMem(%a6)
        tst.l   %d0
        beq.s   fail
        movea.l %d0,%a1
        lea     portname(%pc),%a0
        move.l  %a0,LN_NAME(%a1)
        move.b  #PA_IGNORE,mp_Flags(%a1)
        lea     mp_MsgList+4(%a1),%a0
        move.l  %a0,mp_MsgList(%a1)
        lea     mp_MsgList(%a1),%a0
        move.l  %a0,mp_MsgList+8(%a1)
        jsr     AddPort(%a6)
        movea.l %d4,%a0
        moveq   #TR_SIZE,%d0
        jsr     CreateIORequest(%a6)
        tst.l   %d0
        beq.s   fail
        movea.l %d0,%a2
        lea     timername(%pc),%a0
        moveq   #UNIT_MICROHZ,%d0
        movea.l %a2,%a1
        moveq   #0,%d1
        jsr     OpenDevice(%a6)
        tst.l   %d0
        bne.s   fail
        move.w  #TR_ADDREQUEST,io_Command(%a2)
        move.l  #8,tv_secs(%a2)
        clr.l   tv_micro(%a2)
        movea.l %a2,%a1
        jsr     SendIO(%a6)
never:  moveq   #0,%d0
        bset    #30,%d0
        jsr     Wait(%a6)
        bra.s   never

| The started task: the parent's node now leads nowhere.
child:  movea.l parent(%pc),%a0
        move.l  #0xFFFFFFF1,(%a0)
        move.l  #3,4(%a0)
        rts

parent:         .long   0
dosname:        .asciz  "dos.library"
childname:      .asciz  "crashtest child"
timername:      .asciz  "timer.device"
portname:       .asciz  "crashtest.port"
message:        .ascii  "crashtest: waiting\n"
message_end:
timing:         .ascii  "crashtest: timing\n"
timing_end:
        .even
