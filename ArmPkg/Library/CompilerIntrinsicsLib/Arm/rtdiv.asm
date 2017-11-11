///** @file
//
//  This code provides replacement for MSVC CRT division functions
//
//  Copyright (c) 2017, Pete Batard. All rights reserved.<BR>
//  Based on generated assembly of ReactOS' sdk/lib/crt/math/arm/__rt_###div.c,
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

  EXPORT _fltused
  EXPORT __brkdiv0

  EXPORT __rt_sdiv
  EXPORT __rt_udiv
  EXPORT __rt_udiv64
  EXPORT __rt_sdiv64

  AREA  Math, CODE, READONLY

_fltused
    dcd 0x9875

__brkdiv0
    udf #249

//
// uint64_t __rt_udiv(uint32_t divisor, uint32_t dividend)
//

__rt_udiv
  cmp         r0, #0
  beq         __brkdiv0
  push        {r3-r5,lr}
  mov         r5,r0
  mov         r4,r1
  cmp         r5,r4
  it          hi
  movhi       r0,#0
  bhi         __rt_udiv_label3
  clz         r2,r5
  clz         r3,r4
  subs        r3,r2,r3
  movs        r1,#1
  lsl         r2,r5,r3
  lsl         r3,r1,r3
  movs        r0,#0
__rt_udiv_label1
  cmp         r4,r2
  bcc         __rt_udiv_label2
  orrs        r0,r0,r3
  subs        r4,r4,r2
__rt_udiv_label2
  lsrs        r2,r2,#1
  lsrs        r3,r3,#1
  bne         __rt_udiv_label1
__rt_udiv_label3
  mov         r1,r4
  pop         {r3-r5,pc}

//
// uint64_t __rt_sdiv(int32_t divisor, int32_t dividend)
//

__rt_sdiv
  cmp         r0, #0
  beq         __brkdiv0
  push        {r4-r6,lr}
  mov         r4,r1
  ands        r6,r0,#0x80000000
  it          ne
  rsbne       r4,r4,#0
  mov         r5,r0
  rsbs        r5,r5,#0
  cmp         r5,r4
  it          hi
  movhi       r0,#0
  bhi         __rt_sdiv_label3
  clz         r2,r5
  clz         r3,r4
  subs        r3,r2,r3
  movs        r1,#1
  lsl         r2,r5,r3
  lsl         r3,r1,r3
  movs        r0,#0
__rt_sdiv_label1
  cmp         r4,r2
  bcc         __rt_sdiv_label2
  orrs        r0,r0,r3
  subs        r4,r4,r2
__rt_sdiv_label2
  lsrs        r2,r2,#1
  lsrs        r3,r3,#1
  bne         __rt_sdiv_label1
__rt_sdiv_label3
  cbz         r6,__rt_sdiv_label4
  rsbs        r4,r4,#0
__rt_sdiv_label4
  mov         r1,r4
  pop         {r4-r6,pc}

//
// typedef struct {
//   uint64_t quotient;
//   uint64_t modulus;
// } udiv64_result_t;
//
// void __rt_udiv64_internal(udiv64_result_t *result, uint64_t divisor, uint64_t dividend)
//

__rt_udiv64_internal
  orrs        r1,r2,r3
  beq         __brkdiv0
  push        {r4-r8,lr}
  mov         r7,r3
  mov         r6,r2
  mov         r4,r0
  ldrd        r0,r5,[sp,#0x18]
  cmp         r7,r5
  bcc         __rt_udiv64_internal_label2
  bhi         __rt_udiv64_internal_label1
  cmp         r6,r0
  bls         __rt_udiv64_internal_label2
__rt_udiv64_internal_label1
  movs        r3,#0
  strd        r3,r3,[r4]
  b           __rt_udiv64_internal_label8
__rt_udiv64_internal_label2
  clz         r2,r7
  cmp         r2,#0x20
  bne         __rt_udiv64_internal_label3
  clz         r3,r6
  add         r2,r2,r3
__rt_udiv64_internal_label3
  clz         r1,r5 ;
  cmp         r1,#0x20
  bne         __rt_udiv64_internal_label4
  clz         r3,r0
  add         r1,r1,r3
__rt_udiv64_internal_label4
  subs        r1,r2,r1
  rsb         r3,r1,#0x20
  lsr         r3,r6,r3
  lsl         r2,r7,r1
  orrs        r2,r2,r3
  sub         r3,r1,#0x20
  lsl         r3,r6,r3
  orrs        r2,r2,r3
  lsl         r7,r6,r1
  sub         r3,r1,#0x20
  movs        r6,#1
  lsls        r6,r6,r3
  movs        r3,#1
  mov         lr,#0
  lsl         r1,r3,r1
  mov         r8,lr
__rt_udiv64_internal_label5
  cmp         r5,r2
  bcc         __rt_udiv64_internal_label7
  bhi         __rt_udiv64_internal_label6
  cmp         r0,r7
  bcc         __rt_udiv64_internal_label7
__rt_udiv64_internal_label6
  subs        r0,r0,r7
  sbcs        r5,r5,r2
  orr         lr,lr,r1
  orr         r8,r8,r6
__rt_udiv64_internal_label7
  lsls        r3,r2,#0x1F
  orr         r7,r3,r7,lsr #1
  lsls        r3,r6,#0x1F
  orr         r1,r3,r1,lsr #1
  lsrs        r6,r6,#1
  lsrs        r2,r2,#1
  orrs        r3,r1,r6
  bne         __rt_udiv64_internal_label5
  strd        lr,r8,[r4]
__rt_udiv64_internal_label8
  str         r5,[r4,#0xC]
  str         r0,[r4,#8]
  pop         {r4-r8,pc}

//
// {int64_t, int64_t} __rt_sdiv64(int64_t divisor, int64_t dividend)
//

__rt_sdiv64
  push        {r4-r6,lr}
  sub         sp,sp,#0x18
  and         r6,r1,#0x80000000
  movs        r4,r6
  mov         r5,r0
  beq         __rt_sdiv64_label1
  movs        r0,#0
  rsbs        r2,r2,#0
  sbc         r3,r0,r3
__rt_sdiv64_label1
  movs        r4,r6
  beq         __rt_sdiv64_label2
  movs        r0,#0
  rsbs        r5,r5,#0
  sbc         r1,r0,r1
__rt_sdiv64_label2
  str         r2,[sp]
  str         r3,[sp,#4]
  mov         r3,r1
  mov         r2,r5
  add         r0,sp,#8
  bl          __rt_udiv64_internal
  movs        r3,r6
  beq         __rt_sdiv64_label3
  ldrd        r3,r2,[sp,#0x10]
  movs        r1,#0
  rsbs        r3,r3,#0
  sbcs        r1,r1,r2
  b           __rt_sdiv64_label4
__rt_sdiv64_label3
  ldrd        r3,r1,[sp,#0x10]
__rt_sdiv64_label4
  mov         r2,r3
  ldr         r0,[sp,#8]
  mov         r3,r1
  ldr         r1,[sp,#0xC]
  add         sp,sp,#0x18
  pop         {r4-r6,pc}

//
// {uint64_t, uint64_t} __rt_udiv64(uint64_t divisor, uint64_t dividend)
//

__rt_udiv64
  push        {r4,r5,lr}
  sub         sp,sp,#0x1C
  mov         r4,r2
  mov         r2,r0
  mov         r5,r3
  add         r0,sp,#8
  mov         r3,r1
  str         r4,[sp]
  str         r5,[sp,#4]
  bl          __rt_udiv64_internal
  ldrd        r2,r3,[sp,#0x10]
  ldrd        r0,r1,[sp,#8]
  add         sp,sp,#0x1C
  pop         {r4,r5,pc}

  END
