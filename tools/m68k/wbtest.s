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
| to RAM:wbtest.txt, replies the message under Forbid and returns 0. A
| command line start has no message to wait for and returns 20.
|
| Before it replies it does what the Shell icon's System/CLI does: it makes
| itself a CLI with AllocDosObject, names its directory SYS: with
| SetCurrentDirName and opens a shell with System("NewShell"). The reply
| reads AbsExecBase again, which must still be there.

        .set    OpenLibrary, -552
        .set    CloseLibrary, -414
        .set    FindTask, -294
        .set    WaitPort, -384
        .set    GetMsg, -372
        .set    ReplyMsg, -378
        .set    Forbid, -132
        .set    Open, -30
        .set    Close, -36
        .set    VFWritef, -348
        .set    AllocDosObject, -228
        .set    FreeDosObject, -234
        .set    SetCurrentDirName, -558
        .set    SystemTagList, -606
        .set    DOS_CLI, 4
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
| D4 = the file handle.
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

        | pr_MsgPort is the port itself, inside the Process.
        lea     pr_MsgPort(%a4),%a0
        jsr     WaitPort(%a6)
        lea     pr_MsgPort(%a4),%a0
        jsr     GetMsg(%a6)
        movea.l %d0,%a3
        tst.l   %d0
        beq     failed

        movea.l %a5,%a6
        | A2 = the CLI this process makes for itself.
        moveq   #DOS_CLI,%d1
        moveq   #0,%d2
        jsr     AllocDosObject(%a6)
        movea.l %d0,%a2
        tst.l   %d0
        beq.s   opened
        lsr.l   #2,%d0
        move.l  %d0,pr_CLI(%a4)
        lea     sysname(%pc),%a0
        move.l  %a0,%d1
        jsr     SetCurrentDirName(%a6)
        lea     newshell(%pc),%a0
        move.l  %a0,%d1
        moveq   #0,%d2
        jsr     SystemTagList(%a6)
        clr.l   pr_CLI(%a4)
        moveq   #DOS_CLI,%d1
        move.l  %a2,%d2
        jsr     FreeDosObject(%a6)
opened:
        lea     filename(%pc),%a0
        move.l  %a0,%d1
        move.l  #MODE_NEWFILE,%d2
        jsr     Open(%a6)
        move.l  %d0,%d4
        beq     reply

        | The argument array VFWritef reads: NumArgs, then the first name.
        movea.l sm_ArgList(%a3),%a0
        move.l  wa_Name(%a0),-(%sp)
        move.l  sm_NumArgs(%a3),-(%sp)
        move.l  %d4,%d1
        lea     format(%pc),%a0
        move.l  %a0,%d2
        move.l  %sp,%d3
        jsr     VFWritef(%a6)
        addq.l  #8,%sp
        move.l  %d4,%d1
        jsr     Close(%a6)

        | The reply lets the Workbench unload this code, so task switches
        | stay off from here until the task has ended.
reply:
        movea.l 4.w,%a6
        jsr     Forbid(%a6)
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
sysname:  .asciz  "SYS:"
newshell: .asciz  "NewShell \"CON:0/50//130/AmigaShell/CLOSE\""
        .even
