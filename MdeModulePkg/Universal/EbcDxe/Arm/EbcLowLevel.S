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
ASM_GLOBAL ASM_PFX(CopyMem);

INTERWORK_FUNC(EbcLLCALLEXNative)
INTERWORK_FUNC(EbcLLEbcInterpret)
INTERWORK_FUNC(EbcLLExecuteEbcImageEntryPoint)

ASM_GLOBAL ASM_PFX(mEbcInstructionBufferTemplate)

//****************************************************************************
// EbcLLCALLEX
//
// This function is called to execute an EBC CALLEX instruction.
// This instruction requires that we thunk out to external native
// code. For ARM, we copy the first VM stack arguments into registers,
// according to the ARM Procedure Call Standard, possibly padding one
// of the r1 or r3 registers as needed. Then depending on whether there
// exist any potential arguments left, we either perform a tail call,
// or we copy the remaining arguments into the main stack, call, and
// then restore the stack pointer to its original location.
//
//****************************************************************************
// UINTN EbcLLCALLEXNative(UINTN FuncAddr, UINTN NewStackPointer, VOID *FramePtr)
ASM_PFX(EbcLLCALLEXNative):

    and     r3, r1, #0x3            // Isolate padding value into r3 and
    and     r1, r1, #0xfffffffc     // realign NewStackPointer to 32-bit
    mov     ip, r1                  // Use ip as our argument walker
    push    {r0, r4}                // Preserve r0, r4
    mov     r4, r2                  // Keep a copy of FramePtr

    ldr     r0, [ip], #4            // fill in r0 argument
    cmp     r3, #1                  // fill in or pad r1 as needed
    beq     0f
    ldr     r1, [ip], #4
    b       1f
0:  mov     r1, #0
1:  ldr     r2, [ip], #4            // fill in r2 argument
    cmp     r3, #3                  // fill in or pad r3 as needed
    beq     0f
    ldr     r3, [ip], #4
    b       1f
0:  mov     r3, #0
1:  subs    r4, r4, ip              // Space required for non-register args
    bgt     0f                      // More args?
    pop     {ip}                    // No => perform a tail call
    pop     {r4}
    bx      ip

0:  push    {r5, lr}                // Yes => copy to stack
    mov     r5, sp                  // Preserve original sp
    push    {r0-r3}                 // Preserve our already filled register arguments
    sub     sp, sp, r4              // Allocate required space on the native stack
    mov     r0, sp
    and     r0, r0, #0xfffffff8     // Ensure that the stack pointer is 8 byte aligned
    mov     sp, r0
    mov     r1, ip
    mov     r2, r4
    bl      ASM_PFX(CopyMem)        // Duplicate stack args
    ldrd    r0, r1, [r5, #-16]      // Restore r0-r4 from what we pushed earlier
    ldrd    r2, r3, [r5, #-8]
    ldr     ip, [r5, #8]            // Set the target address in ip
    blx     ip
    mov     sp, r5                  // Restore the stack, dequeue and return
    pop     {r5, ip}
    pop     {r3, r4}                // Destack with r3, as r0 may contain a return value
    mov     pc, ip

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
    .cfi_startproc
    push    {r4, lr}
    .cfi_def_cfa_offset     8
    .cfi_offset             4, -8
    .cfi_offset             14, -4

    // push the entry point and the address of args #5 - #16 onto the stack
    add     r4, sp, #8
    .cfi_def_cfa_offset     16
    strd    ip, r4, [sp, #-8]!

    // call C-code
    ldr     ip, [ip, #20]
    blx     ip

    add     sp, sp, #8
    .cfi_def_cfa_offset     8
    pop     {r4, pc}
    .cfi_endproc

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
