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


| Opens a screen in the HOMBRE mode cybergraphics.library finds best for
| 1280x800 in 24-bit, locks its bitmap, writes a red-green gradient
| straight into the pixels and prints what it did, then waits two seconds
| and closes the screen.
|
| Returns 0, or 20 when a library, the mode, the screen or the lock fails.

        .set    CloseLibrary, -414
        .set    RawDoFmt, -522
        .set    OpenLibrary, -552

        .set    Write, -48
        .set    Output, -60
        .set    Delay, -198

        .set    CloseScreen, -66
        .set    OpenScreenTagList, -612

        .set    BestCModeIDTagList, -60
        .set    LockBitMapTagList, -168
        .set    UnLockBitMap, -174

        .set    sc_BitMap, 184

        .set    SA_Depth, 0x80000025
        .set    SA_DisplayID, 0x80000032
        .set    CYBRBIDTG_Depth, 0x80050000
        .set    CYBRBIDTG_NominalWidth, 0x80050001
        .set    CYBRBIDTG_NominalHeight, 0x80050002
        .set    LBMI_PIXFMT, 0x84001004
        .set    LBMI_BYTESPERROW, 0x84001006
        .set    LBMI_BASEADDRESS, 0x84001007
        .set    PIXFMT_ARGB32, 11
        .set    INVALID_ID, 0xFFFFFFFF

        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A5 = dos.library, A4 = intuition.library, A2 = cybergraphics.library,
| A3 = the screen, D2 = return code.
_start:
        movem.l %d2-%d7/%a2-%a6,-(%sp)
        moveq   #RETURN_FAIL,%d2
        suba.l  %a2,%a2
        suba.l  %a4,%a4
        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a5
        tst.l   %d0
        beq     done
        lea     intname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a4
        tst.l   %d0
        beq     close_libs
        lea     cgxname(%pc),%a1
        moveq   #41,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a2
        tst.l   %d0
        bne.s   have_cgx
        lea     err_cgx(%pc),%a0
        bsr     print
        bra     close_libs

have_cgx:
        lea     modetags,%a0
        movea.l %a2,%a6
        jsr     BestCModeIDTagList(%a6)
        move.l  %d0,%d5
        cmpi.l  #INVALID_ID,%d0
        beq     no_mode
        move.l  %d5,screen_id
        lea     fmt_mode(%pc),%a0
        bsr     print_long

        suba.l  %a0,%a0
        lea     screentags,%a1
        movea.l %a4,%a6
        jsr     OpenScreenTagList(%a6)
        movea.l %d0,%a3
        tst.l   %d0
        beq     no_screen

        lea     sc_BitMap(%a3),%a0
        lea     locktags,%a1
        movea.l %a2,%a6
        jsr     LockBitMapTagList(%a6)
        move.l  %d0,%d7
        beq     no_lock
        cmpi.l  #PIXFMT_ARGB32,pixfmt
        bne     unlock
        lea     msg_locked(%pc),%a0
        bsr     print

| 256 columns of rising red over 200 rows of rising green.
        movea.l base,%a0
        move.l  rowbytes,%d6
        moveq   #0,%d4
row:
        movea.l %a0,%a1
        moveq   #0,%d3
column:
        move.l  %d3,%d0
        swap    %d0
        move.w  %d4,%d1
        lsl.w   #8,%d1
        move.w  %d1,%d0
        ori.l   #0xFF000040,%d0
        move.l  %d0,(%a1)+
        addq.w  #1,%d3
        cmpi.w  #256,%d3
        bne.s   column
        adda.l  %d6,%a0
        addq.w  #1,%d4
        cmpi.w  #200,%d4
        bne.s   row

unlock:
        movea.l %d7,%a0
        movea.l %a2,%a6
        jsr     UnLockBitMap(%a6)
        lea     msg_done(%pc),%a0
        bsr     print
        moveq   #0,%d2
        move.l  #100,%d1
        movea.l %a5,%a6
        jsr     Delay(%a6)
        bra.s   close_screen

no_lock:
        lea     err_lock(%pc),%a0
        bsr     print
close_screen:
        movea.l %a3,%a0
        movea.l %a4,%a6
        jsr     CloseScreen(%a6)
        bra.s   close_libs

no_screen:
        lea     err_screen(%pc),%a0
        bsr     print
        bra.s   close_libs

no_mode:
        lea     err_mode(%pc),%a0
        bsr     print

close_libs:
        movea.l 4.w,%a6
        move.l  %a2,%d0
        beq.s   1f
        movea.l %a2,%a1
        jsr     CloseLibrary(%a6)
1:      move.l  %a4,%d0
        beq.s   2f
        movea.l %a4,%a1
        jsr     CloseLibrary(%a6)
2:      movea.l %a5,%a1
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

| Formats A0 with the long in D5 through exec RawDoFmt and prints it.
print_long:
        movem.l %a2-%a3/%a6,-(%sp)
        lea     -64(%sp),%sp
        move.l  %d5,-(%sp)
        movea.l %sp,%a1
        lea     putch(%pc),%a2
        lea     4(%sp),%a3
        movea.l 4.w,%a6
        jsr     RawDoFmt(%a6)
        lea     4(%sp),%sp
        movea.l %sp,%a0
        bsr     print
        lea     64(%sp),%sp
        movem.l (%sp)+,%a2-%a3/%a6
        rts

putch:
        move.b  %d0,(%a3)+
        rts

dosname:        .asciz  "dos.library"
intname:        .asciz  "intuition.library"
cgxname:        .asciz  "cybergraphics.library"
fmt_mode:       .asciz  "cgxtest: mode %lx\n"
msg_locked:     .asciz  "cgxtest: locked ARGB32\n"
msg_done:       .asciz  "cgxtest: done\n"
err_cgx:        .asciz  "cgxtest: cannot open cybergraphics.library\n"
err_mode:       .asciz  "cgxtest: no mode\n"
err_screen:     .asciz  "cgxtest: cannot open the screen\n"
err_lock:       .asciz  "cgxtest: cannot lock the bitmap\n"
        .even

        .data
        .even
modetags:
        .long   CYBRBIDTG_Depth, 24
        .long   CYBRBIDTG_NominalWidth, 1280
        .long   CYBRBIDTG_NominalHeight, 800
        .long   0, 0
screentags:
        .long   SA_DisplayID
screen_id:
        .long   0
        .long   SA_Depth, 24
        .long   0, 0
locktags:
        .long   LBMI_BASEADDRESS, base
        .long   LBMI_BYTESPERROW, rowbytes
        .long   LBMI_PIXFMT, pixfmt
        .long   0, 0
base:           .long   0
rowbytes:       .long   0
pixfmt:         .long   0
