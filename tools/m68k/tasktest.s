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

| Exercises Exec task creation, Signal/Wait, blocked Wait on a lower-priority
| child, default finalPC exit, timer preemption between equal-priority tasks,
| RemTask, and early child fault recovery.

        .set    AllocMem, -198
        .set    FreeMem, -210
        .set    AddTask, -282
        .set    RemTask, -288
        .set    FindTask, -294
        .set    Wait, -318
        .set    Signal, -324
        .set    AllocSignal, -330
        .set    FreeSignal, -336
        .set    CloseLibrary, -414
        .set    OpenLibrary, -552

        .set    Write, -48
        .set    Output, -60

        .set    LN_TYPE, 8
        .set    LN_PRI, 9
        .set    LN_NAME, 10
        .set    TC_SPREG, 54
        .set    TC_SPLOWER, 58
        .set    TC_SPUPPER, 62
        .set    TC_SIZE, 92

        .set    NT_TASK, 1
        .set    MEMF_PUBLIC_CLEAR, 0x10001
        .set    STACK_BYTES, 4096
        .set    SIGBREAKF_CTRL_E, 0x4000
        .set    SIGBREAKF_CTRL_F, 0x8000
        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A5 = dos.library, A4 = child Task, A3 = child stack, D7 = allocated signal,
| D6 = mode (0 = normal, 1 = fault, 2 = second run), D2 = return code.
_start:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        moveq   #0,%d6
        cmpi.b  #'f',(%a0)
        bne.s   1f
        moveq   #1,%d6
        bra.s   2f
1:      cmpi.b  #'2',(%a0)
        bne.s   2f
        moveq   #2,%d6
2:      moveq   #RETURN_FAIL,%d2
        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a5
        tst.l   %d0
        beq     done

        suba.l  %a1,%a1
        jsr     FindTask(%a6)
        move.l  %d0,parent_task

        moveq   #-1,%d0
        jsr     AllocSignal(%a6)
        move.l  %d0,%d7
        bmi     close_dos

        cmpi.l  #1,%d6
        beq     part_fault

        | Part 1: child waits for CTRL_F, signals parent CTRL_E, returns.
        lea     name_child1,%a0
        moveq   #1,%d0
        bsr     alloc_child
        beq     free_sig

        move.l  %a3,-(%sp)
        movea.l %a4,%a1
        lea     child1_entry(%pc),%a2
        suba.l  %a3,%a3
        jsr     AddTask(%a6)
        movea.l (%sp)+,%a3
        tst.l   %d0
        beq     free_child

        movea.l %a4,%a1
        move.l  #SIGBREAKF_CTRL_F,%d0
        jsr     Signal(%a6)

        move.l  #SIGBREAKF_CTRL_E,%d0
        jsr     Wait(%a6)

        lea     msg_signalled(%pc),%a0
        bsr     print
        bsr     release_child

        | Part 2: priority-0 spinner child vs priority-0 parent loop.
        clr.l   spin_counter
        lea     name_spinner,%a0
        moveq   #0,%d0
        bsr     alloc_child
        beq     free_sig

        move.l  %a3,-(%sp)
        movea.l %a4,%a1
        lea     spinner_entry(%pc),%a2
        suba.l  %a3,%a3
        jsr     AddTask(%a6)
        movea.l (%sp)+,%a3
        tst.l   %d0
        beq     free_child

spin_wait:
        tst.l   spin_counter
        beq.s   spin_wait

        lea     msg_preempted(%pc),%a0
        bsr     print

        movea.l %a4,%a1
        jsr     RemTask(%a6)
        bsr     release_child

        | Part 3: priority -1 child so parent really blocks in Wait before
        | the child can signal CTRL_E.
        lea     name_low,%a0
        moveq   #-1,%d0
        bsr     alloc_child
        beq     free_sig

        move.l  %a3,-(%sp)
        movea.l %a4,%a1
        lea     low_entry(%pc),%a2
        suba.l  %a3,%a3
        jsr     AddTask(%a6)
        movea.l (%sp)+,%a3
        tst.l   %d0
        beq     free_child

        move.l  #SIGBREAKF_CTRL_E,%d0
        jsr     Wait(%a6)

        lea     msg_blocked(%pc),%a0
        bsr     print
        bsr     release_child

        cmpi.l  #2,%d6
        bne.s   3f
        lea     msg_run2(%pc),%a0
        bsr     print
3:      lea     msg_done(%pc),%a0
        bsr     print
        moveq   #0,%d2
        bra     free_sig

        | Part 4: child ends early on an unknown opcode while parent waits on
        | a timeout-free signal the child never sends.
part_fault:
        lea     name_fault,%a0
        moveq   #-1,%d0
        bsr     alloc_child
        beq     free_sig

        move.l  %a3,-(%sp)
        movea.l %a4,%a1
        lea     fault_entry(%pc),%a2
        suba.l  %a3,%a3
        jsr     AddTask(%a6)
        movea.l (%sp)+,%a3
        tst.l   %d0
        beq     free_child

        move.l  #SIGBREAKF_CTRL_E,%d0
        jsr     Wait(%a6)

        moveq   #0,%d2
free_child:
        bsr     release_child
free_sig:
        move.l  %d7,%d0
        jsr     FreeSignal(%a6)
close_dos:
        movea.l %a5,%a1
        jsr     CloseLibrary(%a6)
done:
        move.l  %d2,%d0
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| Allocates Task in A4 and stack in A3 with LN_NAME = A0, LN_PRI = D0.B.
| Sets Z on failure (freeing Task if stack allocation failed).
alloc_child:
        movem.l %d2/%a2,-(%sp)
        move.l  %d0,%d2
        movea.l %a0,%a2
        move.l  #TC_SIZE,%d0
        move.l  #MEMF_PUBLIC_CLEAR,%d1
        jsr     AllocMem(%a6)
        movea.l %d0,%a4
        tst.l   %d0
        beq.s   1f

        move.l  #STACK_BYTES,%d0
        move.l  #MEMF_PUBLIC_CLEAR,%d1
        jsr     AllocMem(%a6)
        movea.l %d0,%a3
        tst.l   %d0
        bne.s   2f

        movea.l %a4,%a1
        move.l  #TC_SIZE,%d0
        jsr     FreeMem(%a6)
        suba.l  %a4,%a4
        moveq   #0,%d0
        bra.s   1f

2:      move.b  #NT_TASK,LN_TYPE(%a4)
        move.b  %d2,LN_PRI(%a4)
        move.l  %a2,LN_NAME(%a4)
        move.l  %a3,TC_SPLOWER(%a4)
        lea     STACK_BYTES(%a3),%a0
        move.l  %a0,TC_SPUPPER(%a4)
        move.l  %a0,TC_SPREG(%a4)
        moveq   #1,%d0
1:      movem.l (%sp)+,%d2/%a2
        rts

release_child:
        movea.l %a3,%a1
        move.l  #STACK_BYTES,%d0
        jsr     FreeMem(%a6)
        movea.l %a4,%a1
        move.l  #TC_SIZE,%d0
        jsr     FreeMem(%a6)
        rts

child1_entry:
        movea.l 4.w,%a6
        move.l  #SIGBREAKF_CTRL_F,%d0
        jsr     Wait(%a6)
        movea.l parent_task,%a1
        move.l  #SIGBREAKF_CTRL_E,%d0
        jsr     Signal(%a6)
        rts

spinner_entry:
        lea     spin_counter,%a0
1:      addq.l  #1,(%a0)
        bra.s   1b

low_entry:
        movea.l 4.w,%a6
        movea.l parent_task,%a1
        move.l  #SIGBREAKF_CTRL_E,%d0
        jsr     Signal(%a6)
        rts

fault_entry:
        .word   0xF000

| Prints NUL-terminated string at A0 through dos Write(Output()).
print:
        movem.l %d2-%d3/%a2/%a6,-(%sp)
        movea.l %a0,%a2
        moveq   #0,%d3
1:      tst.b   (%a0)+
        beq.s   2f
        addq.l  #1,%d3
        bra.s   1b
2:      movea.l %a5,%a6
        jsr     Output(%a6)
        move.l  %d0,%d1
        move.l  %a2,%d2
        jsr     Write(%a6)
        movem.l (%sp)+,%d2-%d3/%a2/%a6
        rts

dosname:        .asciz  "dos.library"
msg_signalled:  .asciz  "tasktest: signalled\n"
msg_preempted:  .asciz  "tasktest: preempted\n"
msg_blocked:    .asciz  "tasktest: blocked wait ok\n"
msg_run2:       .asciz  "tasktest: run2\n"
msg_done:       .asciz  "tasktest: done\n"
        .even

        .data
        .even
name_child1:    .asciz  "tasktest.child1"
name_spinner:   .asciz  "tasktest.spinner"
name_low:       .asciz  "tasktest.low"
name_fault:     .asciz  "tasktest.fault"
        .even
parent_task:    .long   0
spin_counter:   .long   0
