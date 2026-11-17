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

| hombretest.library, a library of the test disk with nothing in it but
| the four standard vectors. exec finds the Resident below by its match
| tag, makes the library from the init table and calls init, which keeps
| the seglist in the extended library structure.

        .set    LIB_FLAGS, 14
        .set    LIB_OPENCNT, 32
        .set    LIB_SIZE, 34
        .set    LIBF_DELEXP_BIT, 3
        .set    LIB_SEGLIST, LIB_SIZE
        .set    HOMBRETEST_SIZE, LIB_SIZE + 4
        .set    RTC_MATCHWORD, 0x4AFC
        .set    RTF_AUTOINIT, 0x80
        .set    NT_LIBRARY, 9

        .text
        .globl  _start

| Not a program: running the file as a command fails with -1.
_start:
        moveq   #-1,%d0
        rts

romtag:
        .word   RTC_MATCHWORD
        .long   romtag
        .long   endskip
        .byte   RTF_AUTOINIT
        .byte   1                       | version
        .byte   NT_LIBRARY
        .byte   0                       | priority
        .long   libname
        .long   idstring
        .long   inittable
endskip:

libname:        .asciz  "hombretest.library"
idstring:       .asciz  "hombretest 1.0 (Hombre test disk)"
        .even

inittable:
        .long   HOMBRETEST_SIZE
        .long   vectors
        .long   0                       | no InitStruct table
        .long   init

vectors:
        .long   open, close, expunge, reserved
        .long   -1

| D0 = the library, A0 = the seglist, A6 = ExecBase. Returns the library.
init:
        movea.l %d0,%a1
        move.l  %a0,LIB_SEGLIST(%a1)
        rts

| A6 = the library. The library is never expunged, so Open only counts.
open:
        addq.w  #1,LIB_OPENCNT(%a6)
        bclr    #LIBF_DELEXP_BIT,LIB_FLAGS(%a6)
        move.l  %a6,%d0
        rts

close:
        subq.w  #1,LIB_OPENCNT(%a6)
expunge:
reserved:
        moveq   #0,%d0
        rts
