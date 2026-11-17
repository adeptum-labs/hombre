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

| The Version command of the test disk: prints the version of
| hombretest.library, which dos finds in LIBS: and exec loads.
|
|     Hombre test version <lib_Version>
|
| Returns 0, or 20 when dos.library or hombretest.library does not open.

        .set    OpenLibrary, -552
        .set    CloseLibrary, -414
        .set    PutStr, -948
        .set    VPrintf, -954
        .set    LIB_VERSION, 20
        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A5 = dos.library, A4 = hombretest.library, D2 = the return code.
_start:
        movem.l %d2/%a4-%a6,-(%sp)
        movea.l 4.w,%a6
        lea     dosname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a5
        lea     testname(%pc),%a1
        moveq   #0,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a4
        move.l  %a5,%d0
        beq.s   failed
        move.l  %a4,%d0
        beq.s   failed

        moveq   #0,%d0
        move.w  LIB_VERSION(%a4),%d0
        move.l  %d0,-(%sp)              | the argument array: one long
        lea     format(%pc),%a0
        move.l  %a0,%d1
        move.l  %sp,%d2
        movea.l %a5,%a6
        jsr     VPrintf(%a6)
        addq.l  #4,%sp
        moveq   #0,%d2
        bra.s   close

failed:
        moveq   #RETURN_FAIL,%d2
        move.l  %a5,%d0
        beq.s   close                   | without dos there is nowhere to print
        lea     failure(%pc),%a0
        move.l  %a0,%d1
        movea.l %a5,%a6
        jsr     PutStr(%a6)

close:
        movea.l 4.w,%a6
        move.l  %a4,%d0
        beq.s   closedos
        movea.l %a4,%a1
        jsr     CloseLibrary(%a6)
closedos:
        move.l  %a5,%d0
        beq.s   done
        movea.l %a5,%a1
        jsr     CloseLibrary(%a6)
done:
        move.l  %d2,%d0
        movem.l (%sp)+,%d2/%a4-%a6
        rts

dosname:        .asciz  "dos.library"
testname:       .asciz  "hombretest.library"
format:         .asciz  "Hombre test version %ld\n"
failure:        .asciz  "Version: cannot open hombretest.library\n"
        .even
