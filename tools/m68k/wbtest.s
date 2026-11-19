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

| wbtest: a program started from its icon. Without a CLI it waits for its
| WBStartup message on pr_MsgPort, writes "wbtest: <NumArgs> <arg0 name>"
| to RAM:wbtest.txt, replies the message and returns 0. A command line
| start has no message to wait for and returns 20.

        .set    OpenLibrary, -552
        .set    CloseLibrary, -414
        .set    FindTask, -294
        .set    WaitPort, -384
        .set    GetMsg, -372
        .set    ReplyMsg, -378
        .set    Open, -30
        .set    Close, -36
        .set    VFWritef, -348
        .set    MODE_NEWFILE, 1006
        .set    pr_MsgPort, 92
        .set    pr_CLI, 172
        .set    sm_NumArgs, 28
        .set    sm_ArgList, 36
        .set    wa_Name, 4
        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A5 = dos.library, A4 = the Process, A3 = the WBStartup message,
| D3 = the file handle.
_start:
        movem.l %d2-%d4/%a2-%a6,-(%sp)
        suba.l  %a5,%a5
        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a5
        tst.l   %d0
        beq     failed

        suba.l  %a1,%a1
        jsr     FindTask(%a6)
        movea.l %d0,%a4
        tst.l   pr_CLI(%a4)
        bne     failed

        movea.l pr_MsgPort(%a4),%a0
        jsr     WaitPort(%a6)
        movea.l pr_MsgPort(%a4),%a0
        jsr     GetMsg(%a6)
        movea.l %d0,%a3
        tst.l   %d0
        beq     failed

        movea.l %a5,%a6
        lea     filename(%pc),%a1
        move.l  #MODE_NEWFILE,%d2
        jsr     Open(%a6)
        move.l  %d0,%d3
        beq     reply

        | The argument array VFWritef reads: NumArgs, then the first name.
        movea.l sm_ArgList(%a3),%a0
        move.l  wa_Name(%a0),-(%sp)
        move.l  sm_NumArgs(%a3),-(%sp)
        move.l  %d3,%d1
        lea     format(%pc),%a0
        move.l  %a0,%d2
        move.l  %sp,%d4
        jsr     VFWritef(%a6)
        addq.l  #8,%sp
        move.l  %d3,%d1
        jsr     Close(%a6)

reply:
        movea.l 4.w,%a6
        movea.l %a3,%a1
        jsr     ReplyMsg(%a6)
        moveq   #0,%d0
        bra.s   done
failed:
        moveq   #RETURN_FAIL,%d0
done:
        move.l  %d0,%d4
        movea.l 4.w,%a6
        movea.l %a5,%a1
        cmpa.w  #0,%a5
        beq.s   out
        jsr     CloseLibrary(%a6)
out:
        move.l  %d4,%d0
        movem.l (%sp)+,%d2-%d4/%a2-%a6
        rts

dosname:  .asciz  "dos.library"
filename: .asciz  "RAM:wbtest.txt"
format:   .asciz  "wbtest: %N %S\n"
        .even
