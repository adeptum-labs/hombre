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

| Opens an Intuition window on the front screen and prints each IDCMP
| activation, mouse move, mouse button and close event to Output() until
| the window is closed or Ctrl+C is pressed.
|
| Returns 0 on close, or 20 when a library or the window cannot be opened.

        .set    GetMsg, -372
        .set    ReplyMsg, -378
        .set    Wait, -318
        .set    CloseLibrary, -414
        .set    RawDoFmt, -522
        .set    OpenLibrary, -552

        .set    Write, -48
        .set    Output, -60

        .set    CloseWindow, -72
        .set    OpenWindow, -204

        .set    wd_UserPort, 86

        .set    im_Class, 20
        .set    im_Code, 24
        .set    im_MouseX, 32
        .set    im_MouseY, 34

        .set    IDCMP_MOUSEBUTTONS, 0x8
        .set    IDCMP_MOUSEMOVE, 0x10
        .set    IDCMP_CLOSEWINDOW, 0x200
        .set    IDCMP_ACTIVEWINDOW, 0x40000

        .set    WFLG_DRAGBAR, 0x2
        .set    WFLG_DEPTHGADGET, 0x4
        .set    WFLG_CLOSEGADGET, 0x8
        .set    WFLG_REPORTMOUSE, 0x200
        .set    WFLG_ACTIVATE, 0x1000

        .set    WBENCHSCREEN, 1

        .set    SELECTDOWN, 0x68
        .set    SELECTUP, 0xE8
        .set    MENUDOWN, 0x69
        .set    MENUUP, 0xE9

        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A5 = dos.library, A4 = intuition.library, A3 = Window, D2 = return code,
| D7 = the signals Wait is given: the window port and Ctrl+C.
_start:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a5
        lea     intname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a4
        move.l  %a5,%d0
        beq     no_dos
        move.l  %a4,%d0
        beq     no_intuition

        lea     newwindow,%a0
        movea.l %a4,%a6
        jsr     OpenWindow(%a6)
        movea.l %d0,%a3
        tst.l   %d0
        beq     no_window

        lea     msg_ready(%pc),%a0
        bsr     print

        movea.l wd_UserPort(%a3),%a0
        moveq   #0,%d0
        move.b  15(%a0),%d0
        moveq   #1,%d7
        lsl.l   %d0,%d7
        ori.l   #0x1000,%d7

loop:
        movea.l wd_UserPort(%a3),%a0
        movea.l 4.w,%a6
        jsr     GetMsg(%a6)
        tst.l   %d0
        bne.s   have_msg
        move.l  %d7,%d0
        jsr     Wait(%a6)
        btst    #12,%d0
        bne     got_break
        bra     loop

have_msg:
        movea.l %d0,%a1
        move.l  im_Class(%a1),%d3
        moveq   #0,%d4
        move.w  im_Code(%a1),%d4
        move.w  im_MouseX(%a1),%d5
        ext.l   %d5
        move.w  im_MouseY(%a1),%d6
        ext.l   %d6
        jsr     ReplyMsg(%a6)

        cmpi.l  #IDCMP_ACTIVEWINDOW,%d3
        bne.s   check_move
        lea     msg_active(%pc),%a0
        bsr     print
        bra     loop

check_move:
        cmpi.l  #IDCMP_MOUSEMOVE,%d3
        bne.s   check_buttons
        lea     fmt_move(%pc),%a0
        bsr     print_xy
        bra     loop

check_buttons:
        cmpi.l  #IDCMP_MOUSEBUTTONS,%d3
        bne.s   check_close
        cmpi.w  #SELECTDOWN,%d4
        beq.s   sel_down
        cmpi.w  #SELECTUP,%d4
        beq.s   sel_up
        cmpi.w  #MENUDOWN,%d4
        beq.s   menu_down
        cmpi.w  #MENUUP,%d4
        beq.s   menu_up
        bra     loop

sel_down:
        lea     fmt_sel_down(%pc),%a0
        bra.s   emit_xy
sel_up:
        lea     fmt_sel_up(%pc),%a0
        bra.s   emit_xy
menu_down:
        lea     fmt_menu_down(%pc),%a0
        bra.s   emit_xy
menu_up:
        lea     fmt_menu_up(%pc),%a0
emit_xy:
        bsr     print_xy
        bra     loop

got_break:
        lea     msg_break(%pc),%a0
        bsr     print
        movea.l %a3,%a0
        movea.l %a4,%a6
        jsr     CloseWindow(%a6)
        moveq   #0,%d2
        bra     close_int

check_close:
        cmpi.l  #IDCMP_CLOSEWINDOW,%d3
        bne     loop
        lea     msg_close(%pc),%a0
        bsr     print
        movea.l %a3,%a0
        movea.l %a4,%a6
        jsr     CloseWindow(%a6)
        moveq   #0,%d2
        bra.s   close_int

no_window:
        lea     err_window(%pc),%a0
        bsr     print
        moveq   #RETURN_FAIL,%d2
        bra.s   close_int

no_intuition:
        lea     err_intuition(%pc),%a0
        bsr     print
no_dos:
        moveq   #RETURN_FAIL,%d2

close_int:
        movea.l 4.w,%a6
        move.l  %a4,%d0
        beq.s   close_dos
        movea.l %a4,%a1
        jsr     CloseLibrary(%a6)
close_dos:
        move.l  %a5,%d0
        beq.s   done
        movea.l %a5,%a1
        jsr     CloseLibrary(%a6)
done:
        move.l  %d2,%d0
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts

| Prints the NUL-terminated string at A0 through dos Write(Output()).
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

| Formats A0 with D5 (X) and D6 (Y) through exec RawDoFmt and prints it.
print_xy:
        movem.l %a2-%a3/%a6,-(%sp)
        lea     -64(%sp),%sp
        move.l  %d6,-(%sp)
        move.l  %d5,-(%sp)
        movea.l %sp,%a1
        lea     putch(%pc),%a2
        lea     8(%sp),%a3
        movea.l 4.w,%a6
        jsr     RawDoFmt(%a6)
        addq.l  #8,%sp
        movea.l %sp,%a0
        bsr.s   print
        lea     64(%sp),%sp
        movem.l (%sp)+,%a2-%a3/%a6
        rts

putch:
        move.b  %d0,(%a3)+
        rts

dosname:        .asciz  "dos.library"
intname:        .asciz  "intuition.library"
title:          .asciz  "MouseTest"
msg_ready:      .asciz  "mousetest: ready\n"
msg_active:     .asciz  "mousetest: active\n"
msg_close:      .asciz  "mousetest: close\n"
msg_break:      .asciz  "mousetest: break\n"
fmt_sel_down:   .asciz  "mousetest: select down at %ld,%ld\n"
fmt_sel_up:     .asciz  "mousetest: select up at %ld,%ld\n"
fmt_menu_down:  .asciz  "mousetest: menu down at %ld,%ld\n"
fmt_menu_up:    .asciz  "mousetest: menu up at %ld,%ld\n"
fmt_move:       .asciz  "mousetest: move at %ld,%ld\n"
err_intuition:  .asciz  "mousetest: cannot open intuition.library\n"
err_window:     .asciz  "mousetest: cannot open window\n"
        .even

        .data
        .even
newwindow:
        .word   200
        .word   100
        .word   240
        .word   160
        .byte   0
        .byte   1
        .long   IDCMP_MOUSEBUTTONS + IDCMP_MOUSEMOVE + IDCMP_CLOSEWINDOW + IDCMP_ACTIVEWINDOW
        .long   WFLG_CLOSEGADGET + WFLG_DEPTHGADGET + WFLG_DRAGBAR + WFLG_ACTIVATE + WFLG_REPORTMOUSE
        .long   0
        .long   0
        .long   title
        .long   0
        .long   0
        .word   0
        .word   0
        .word   0
        .word   0
        .word   WBENCHSCREEN
