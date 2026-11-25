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

        .set    AllocMem, -198
        .set    AddTask, -282
        .set    FindTask, -294
        .set    Wait, -318
        .set    CloseLibrary, -414
        .set    OpenLibrary, -552
        .set    Write, -48
        .set    Output, -60

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
        movem.l %d2-%d3/%a2-%a3/%a5-%a6,-(%sp)
        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        tst.l   %d0
        beq.s   fail
        movea.l %d0,%a5
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
        movem.l (%sp)+,%d2-%d3/%a2-%a3/%a5-%a6
        rts

| The started task: the parent's node now leads nowhere.
child:  movea.l parent(%pc),%a0
        move.l  #0xFFFFFFF1,(%a0)
        move.l  #3,4(%a0)
        rts

parent:         .long   0
dosname:        .asciz  "dos.library"
childname:      .asciz  "crashtest child"
message:        .ascii  "crashtest: waiting\n"
message_end:
        .even
