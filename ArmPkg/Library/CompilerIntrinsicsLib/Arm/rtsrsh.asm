///** @file
//
//  This code provides replacement for MSVC CRT __rt_srsh
//
//  Copyright (c) Timo Kreuzer. All rights reserved.<BR>
//
//  This program and the accompanying materials
//  are licensed and made available under the terms and conditions of the BSD License
//  which accompanies this distribution.  The full text of the license may be found at
//  http://opensource.org/licenses/bsd-license.php
//
//  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
//  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
//
//**/

  EXPORT  __rt_srsh

  AREA  Math, CODE, READONLY

//
//  int64_t __rt_srsh(int64_t  value, uint32_t shift);
//

__rt_srsh
    rsbs r3, r2, #32
    bmi __rt_srsh_label1
    lsr r0, r0, r2
    lsl r3, r1, r3
    orr r0, r0, r3
    asr r1, r1, r2
    bx  lr
__rt_srsh_label1
    cmp r2, 64
    bhs __rt_srsh_label2
    sub r3, r2, #32
    asr r0, r1, r3
    asr r1, r1, #32
    bx  lr
__rt_srsh_label2
    asr r1, r1, #32
    mov r0, r1
    bx  lr

  END
