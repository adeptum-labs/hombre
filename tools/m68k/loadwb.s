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

| LoadWB: opens workbench.library and asks it to start the Workbench. The
| library runs the desktop in a task of its own, so this returns at once;
| it returns 20 when the library cannot be opened.

        .set    OpenLibrary, -552
        .set    CloseLibrary, -414
        .set    StartWorkbench, -42
        .set    RETURN_FAIL, 20

        .text
        .globl  _start

| A4 = workbench.library, D2 = the return code.
_start:
        movem.l %d2/%a4-%a6,-(%sp)
        movea.l 4.w,%a6
        lea     wbname(%pc),%a1
        moveq   #40,%d0
        jsr     OpenLibrary(%a6)
        movea.l %d0,%a4
        tst.l   %d0
        beq.s   failed
        suba.l  %a0,%a0
        moveq   #0,%d1
        movea.l %a4,%a6
        jsr     StartWorkbench(%a6)
        movea.l 4.w,%a6
        movea.l %a4,%a1
        jsr     CloseLibrary(%a6)
        moveq   #0,%d2
        bra.s   done
failed:
        moveq   #RETURN_FAIL,%d2
done:
        move.l  %d2,%d0
        movem.l (%sp)+,%d2/%a4-%a6
        rts

wbname: .asciz  "workbench.library"
        .even
