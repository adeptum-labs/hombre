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

| Exercises input.device: adds an input handler at priority 60 that turns
| X into Y and takes the presses of Q out, writes a Z with IND_WRITEEVENT,
| reports PeekQualifier, reads a line from its console through the handler,
| and takes the handler out again. With the argument "wait" it keeps the
| handler and waits until it is ended; with "spin" its handler never
| returns, and it waits the same way.

        .set    CloseLibrary, -414
        .set    OpenDevice, -444
        .set    CloseDevice, -450
        .set    DoIO, -456
        .set    OpenLibrary, -552
        .set    CreateIORequest, -654
        .set    DeleteIORequest, -660
        .set    CreateMsgPort, -666
        .set    DeleteMsgPort, -672

        .set    Read, -42
        .set    Input, -54
        .set    Delay, -198
        .set    PutStr, -948
        .set    VPrintf, -954

        .set    PeekQualifier, -42

        .set    LN_PRI, 9
        .set    LN_NAME, 10
        .set    IS_DATA, 14
        .set    IS_CODE, 18
        .set    IS_SIZE, 22

        .set    io_Device, 20
        .set    io_Command, 28
        .set    io_Error, 31
        .set    io_Length, 36
        .set    io_Data, 40
        .set    IOSTD_SIZE, 48

        .set    ie_NextEvent, 0
        .set    ie_Class, 4
        .set    ie_SubClass, 5
        .set    ie_Code, 6
        .set    ie_Qualifier, 8
        .set    ie_SIZEOF, 22

        .set    IND_ADDHANDLER, 9
        .set    IND_REMHANDLER, 10
        .set    IND_WRITEEVENT, 11
        .set    IECLASS_RAWKEY, 1
        .set    KEY_Q, 0x10
        .set    KEY_Y, 0x15
        .set    KEY_Z, 0x31
        .set    KEY_X, 0x32
        .set    KEY_UP, 0x80
        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A5 = dos.library, A3 = IOStdReq, A2 = arguments, D7 = MsgPort,
| D6 = return code.
_start:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l %a0,%a2
        moveq   #RETURN_FAIL,%d6
        suba.l  %a5,%a5
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
        moveq   #IOSTD_SIZE,%d0
        jsr     CreateIORequest(%a6)
        movea.l %d0,%a3
        tst.l   %d0
        beq     del_port

        lea     inputname(%pc),%a0
        moveq   #0,%d0
        movea.l %a3,%a1
        moveq   #0,%d1
        jsr     OpenDevice(%a6)
        tst.l   %d0
        bne     del_req

        lea     interrupt,%a0
        move.b  #60,LN_PRI(%a0)
        lea     handlername(%pc),%a1
        move.l  %a1,LN_NAME(%a0)
        clr.l   IS_DATA(%a0)
        lea     handler(%pc),%a1
        cmpi.b  #'s',(%a2)
        bne.s   1f
        lea     spin(%pc),%a1
1:      move.l  %a1,IS_CODE(%a0)
        move.w  #IND_ADDHANDLER,io_Command(%a3)
        move.l  %a0,io_Data(%a3)
        movea.l %a3,%a1
        jsr     DoIO(%a6)
        tst.l   %d0
        bne     close_dev

        cmpi.b  #'w',(%a2)
        beq     wait_forever
        cmpi.b  #'s',(%a2)
        beq     wait_forever

        | A Z typed by the program, its press and its release.
        moveq   #KEY_Z,%d0
        bsr     write_key
        bne     remove
        move.w  #KEY_Z+KEY_UP,%d0
        bsr     write_key
        bne     remove

        movea.l io_Device(%a3),%a6
        jsr     PeekQualifier(%a6)
        andi.l  #0xFFFF,%d0
        move.l  %d0,-(%sp)
        lea     fmt_qualifier(%pc),%a0
        bsr     printf
        addq.l  #4,%sp

        lea     msg_ready(%pc),%a0
        bsr     put_str

        | One line from the console, whose keys pass the handler. The
        | argument line, empty, comes first.
        moveq   #1,%d4
read_line:
        movea.l %a5,%a6
        jsr     Input(%a6)
        move.l  %d0,%d1
        move.l  #line,%d2
        moveq   #79,%d3
        jsr     Read(%a6)
        tst.l   %d0
        bmi     remove
        lea     line,%a0
        clr.b   (%a0,%d0.l)
        subq.l  #1,%d0
        bmi.s   1f
        cmpi.b  #10,(%a0,%d0.l)
        bne.s   1f
        clr.b   (%a0,%d0.l)
1:      tst.b   (%a0)
        dbne    %d4,read_line
        pea     line
        lea     fmt_read(%pc),%a0
        bsr     printf
        addq.l  #4,%sp
        moveq   #0,%d6

remove:
        movea.l 4.w,%a6
        move.w  #IND_REMHANDLER,io_Command(%a3)
        move.l  #interrupt,io_Data(%a3)
        movea.l %a3,%a1
        jsr     DoIO(%a6)
        lea     msg_done(%pc),%a0
        bsr     put_str

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

wait_forever:
        lea     msg_waiting(%pc),%a0
        bsr     put_str
1:      moveq   #50,%d1
        movea.l %a5,%a6
        jsr     Delay(%a6)
        bra.s   1b

| Writes a RAWKEY event with the code in D0; Z set when io_Error is 0.
write_key:
        lea     event,%a0
        clr.l   ie_NextEvent(%a0)
        move.b  #IECLASS_RAWKEY,ie_Class(%a0)
        clr.b   ie_SubClass(%a0)
        move.w  %d0,ie_Code(%a0)
        clr.w   ie_Qualifier(%a0)
        move.w  #IND_WRITEEVENT,io_Command(%a3)
        move.l  %a0,io_Data(%a3)
        moveq   #ie_SIZEOF,%d0
        move.l  %d0,io_Length(%a3)
        movea.l 4.w,%a6
        movea.l %a3,%a1
        jsr     DoIO(%a6)
        tst.b   io_Error(%a3)
        rts

| The handler: called with the events in A0, it returns them in D0, the
| presses of Q taken out and every X a Y.
handler:
        move.l  %a0,-(%sp)
        movea.l %sp,%a1
1:      movea.l (%a1),%a0
        move.l  %a0,%d0
        beq.s   4f
        cmpi.b  #IECLASS_RAWKEY,ie_Class(%a0)
        bne.s   3f
        cmpi.w  #KEY_Q,ie_Code(%a0)
        bne.s   2f
        move.l  (%a0),(%a1)
        bra.s   1b
2:      cmpi.w  #KEY_X,ie_Code(%a0)
        bne.s   3f
        move.w  #KEY_Y,ie_Code(%a0)
3:      movea.l %a0,%a1
        bra.s   1b
4:      move.l  (%sp)+,%d0
        rts

| A handler that never returns.
spin:
        bra.s   spin

| VPrintf of the format at A0 with the arguments above the return address.
printf:
        move.l  %a0,%d1
        lea     4(%sp),%a0
        move.l  %a0,%d2
        movea.l %a5,%a6
        jsr     VPrintf(%a6)
        rts

put_str:
        move.l  %a0,%d1
        movea.l %a5,%a6
        jsr     PutStr(%a6)
        rts

dosname:        .asciz  "dos.library"
inputname:      .asciz  "input.device"
handlername:    .asciz  "inputtest"
fmt_qualifier:  .asciz  "inputtest: qualifier %ld\n"
fmt_read:       .asciz  "inputtest: read %s\n"
msg_ready:      .asciz  "inputtest: ready\n"
msg_waiting:    .asciz  "inputtest: waiting\n"
msg_done:       .asciz  "inputtest: done\n"
        .even
        .bss
        .even
interrupt:      .skip   IS_SIZE
        .even
event:          .skip   ie_SIZEOF
        .even
line:           .skip   80
