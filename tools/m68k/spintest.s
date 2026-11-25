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

| Keeps the processor to itself until something ends it: prints a line, takes
| Forbid unless its argument starts with n, and loops.

        .set    Forbid, -132
        .set    CloseLibrary, -414
        .set    OpenLibrary, -552
        .set    Write, -48
        .set    Output, -60

        .set    RETURN_FAIL, 20

        .text
        .globl  _start

_start:
        movem.l %d2-%d3/%a5-%a6,-(%sp)
        moveq   #0,%d3
        move.b  (%a0),%d3
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
        move.l  %d3,-(%sp)
        moveq   #message_end-message,%d3
        jsr     Write(%a6)
        move.l  (%sp)+,%d3
        movea.l 4.w,%a6
        cmpi.b  #'n',%d3
        beq.s   spin
        jsr     Forbid(%a6)
spin:   bra.s   spin

fail:   moveq   #RETURN_FAIL,%d0
        movem.l (%sp)+,%d2-%d3/%a5-%a6
        rts

dosname:        .asciz  "dos.library"
message:        .ascii  "spintest: spinning\n"
message_end:
        .even
