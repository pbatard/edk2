///** @file
//
//  This code provides low level routines that support the Virtual Machine
//  for option ROMs.
//
//  Copyright (c) 2016, Linaro, Ltd. All rights reserved.<BR>
//  Copyright (c) 2015, The Linux Foundation. All rights reserved.<BR>
//  Copyright (c) 2007 - 2014, Intel Corporation. All rights reserved.<BR>
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

    .thumb
    .syntax unified

ASM_GLOBAL ASM_PFX(EbcLLCALLEXNative)
ASM_GLOBAL ASM_PFX(EbcLLEbcInterpret)
ASM_GLOBAL ASM_PFX(EbcLLExecuteEbcImageEntryPoint)

INTERWORK_FUNC(EbcLLCALLEXNative)
INTERWORK_FUNC(EbcLLEbcInterpret)
INTERWORK_FUNC(EbcLLExecuteEbcImageEntryPoint)

ASM_GLOBAL ASM_PFX(mEbcInstructionBufferTemplate)

//****************************************************************************
// EbcLLCALLEX
//
// This function is called to execute an EBC CALLEX instruction.
// This instruction requires that we thunk out to external native
// code. For ARM, we copy the VM stack into the main stack and then pop
// the first 4 arguments off according to the ARM Procedure Call Standard
// On return, we restore the stack pointer to its original location.
//
//****************************************************************************
// UINTN EbcLLCALLEXNative(UINTN FuncAddr, UINTN NewStackPointer, VOID *FramePtr)
ASM_PFX(EbcLLCALLEXNative):
    mov     ip, r0                  // Preserve r0

    //
    // If the EBC stack frame is smaller than or equal to 16 bytes, we know there
    // are no stacked arguments #5 and beyond that we need to copy to the native
    // stack. In this case, we can perform a tail call which is much more
    // efficient, since there is no need to touch the native stack at all.
    //
    sub     r3, r2, r1              // Length = NewStackPointer - FramePtr
    cmp     r3, #16
    bgt     0f

    ldrd    r2, r3, [r1, #8]
    ldrd    r0, r1, [r1]

    bx      ip

    //
    // More than 16 bytes: we need to build the full native stack frame and copy
    // the part of the VM stack exceeding 16 bytes (which may contain stacked
    // arguments) to the native stack
    //
0:  push    {r4, lr}
    mov     r4, sp

    //
    // Ensure that the stack pointer remains 8 byte aligned,
    // even if the size of the VM stack frame is not a multiple of 8
    //
    add     r1, r1, #16             // Skip over [potential] reg params
    tst     r3, #7                  // Multiple of 8?
    beq     1f
    ldr     r3, [r2, #-4]!          // No? Then push one word
    str     r3, [sp, #-8]!          // ... but use two slots
    b       2f

1:  ldrd    r0, r3, [r2, #-8]!
    strd    r0, r3, [sp, #-8]!
2:  cmp     r2, r1
    bgt     1b

    ldrd    r2, r3, [r1, #-8]
    ldrd    r0, r1, [r1, #-16]

    blx     ip

    mov     sp, r4
    pop     {r4, pc}

//****************************************************************************
// EbcLLEbcInterpret
//
// This function is called by the thunk code to handle an Native to EBC call
// This can handle up to 16 arguments (1-4 on in r0-r3, 5-16 are on the stack)
// ip contains the Entry point that will be the first argument when
// EBCInterpret is called.
//
//****************************************************************************
ASM_PFX(EbcLLEbcInterpret):
    stmdb   sp!, {r4, lr}

    // push the entry point and the address of args #5 - #16 onto the stack
    add     r4, sp, #8
    str     ip, [sp, #-8]!
    str     r4, [sp, #4]

    // call C-code
    bl      ASM_PFX(EbcInterpret)

    add     sp, sp, #8
    ldmia   sp!, {r4, pc}

//****************************************************************************
// EbcLLExecuteEbcImageEntryPoint
//
// This function is called by the thunk code to handle the image entry point
// ip contains the Entry point that will be the third argument when
// ExecuteEbcImageEntryPoint is called.
//
//****************************************************************************
ASM_PFX(EbcLLExecuteEbcImageEntryPoint):
    ldr     r2, [ip, #12]

    // tail call to C code
    b       ASM_PFX(ExecuteEbcImageEntryPoint)

//****************************************************************************
// mEbcInstructionBufferTemplate
//****************************************************************************
    .section    ".rodata", "a"
    .align      2
    .arm
ASM_PFX(mEbcInstructionBufferTemplate):
    adr     ip, .
    ldr     pc, 0f

    //
    // Add a magic code here to help the VM recognize the thunk.
    //
    udf     #0xEBC

    .long   0                       // EBC_ENTRYPOINT_SIGNATURE
0:  .long   0                       // EBC_LL_EBC_ENTRYPOINT_SIGNATURE
