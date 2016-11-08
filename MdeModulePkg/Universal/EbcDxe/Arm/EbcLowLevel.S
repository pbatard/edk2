///** @file
//
//  This code provides low level routines that support the Virtual Machine
//  for option ROMs.
//
//  Copyright (c) 2016, Pete Batard. All rights reserved.<BR>
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

ASM_GLOBAL ASM_PFX(EbcLLCALLEXNativeArm)
ASM_GLOBAL ASM_PFX(EbcLLEbcInterpret)
ASM_GLOBAL ASM_PFX(EbcLLExecuteEbcImageEntryPoint)

INTERWORK_FUNC(EbcLLCALLEXNativeArm)
INTERWORK_FUNC(EbcLLEbcInterpret)
INTERWORK_FUNC(EbcLLExecuteEbcImageEntryPoint)

ASM_GLOBAL ASM_PFX(mEbcInstructionBufferTemplate)

//****************************************************************************
// EbcLLCALLEX
//
// This function is called to execute an EBC CALLEX instruction.
// This instruction requires that we thunk out to external native code.
// Note that due to the ARM Procedure Call Standard, we may have to align
// 64-bit arguments to an even register or dword aligned stack address,
// which is what the extra ArgLayout parameter is used for.
// Also, to optimize for speed, we arbitrarily limit to 16 the maximum
// number of arguments a native call can have.
//
//****************************************************************************
// UINTN EbcLLCALLEXNativeArm(UINTN FuncAddr, UINTN NewStackPointer,
//                            VOID *FramePtr, UINT16 ArgLayout)
ASM_PFX(EbcLLCALLEXNativeArm):

    mov     ip, r1                  // Use ip as our argument walker
    push    {r0, r4-r6}
    mov     r4, r2                  // Keep a copy of FramePtr
    mov     r5, r3                  // Keep a copy of ArgLayout
    mov     r6, #2                  // Arg counter (2 for r0 and r2)

    //
    // Process the register arguments, skipping r1 and r3
    // as needed, according to the argument layout.
    //
    lsrs    r5, r5, #1
    bcc     0f                      // Is our next argument 64-bit?
    ldr     r0, [ip], #4            // Yes => fill in r0-r1
    ldr     r1, [ip], #4
    b       1f
0:  ldr     r0, [ip], #4            // No => fill in r0
    lsrs    r5, r5, #1
    bcs     2f                      // Is our next argument 64-bit?
    ldr     r1, [ip], #4            // No => fill in r1
    add     r6, r6, #1              // Increment arg counter for r1
1:  lsrs    r5, r5, #1
    bcc     0f                      // Is our next argument 64-bit?
2:  ldr     r2, [ip], #4            // Yes => fill in r2-r3
    ldr     r3, [ip], #4
    b       1f
0:  ldr     r2, [ip], #4            // No => fill in r2
    tst     r5, #1
    bne     1f                      // Is our next argument 64-bit?
    ldr     r3, [ip], #4            // No => fill in r3
    lsr     r5, r5, #1
    add     r6, r6, #1              // Increment arg counter for r3
1:  cmp     r4, ip
    bgt     0f                      // More args?
    pop     {ip}                    // No => perform a tail call
    pop     {r4-r6}
    bx      ip

    //
    // Cannot perform a tail call => We need to properly enqueue (and
    // align) all EBC stack parameters before we invoke the native call.
    //
0:  push    {r7-r10, lr}
    mov     r10, sp                 // Preserve original sp
    sub     r7, r10, #116           // Space for 14 64-bit args (+1 word)
    and     r7, r7, #0xfffffff8     // Start with an aligned stack
    mov     sp, r7

    //
    // Duplicate EBC data onto the local stack:
    // ip = EBC arg walker
    // r4 = top of EBC stack frame
    // r5 = arg layout
    // r6 = arg counter
    // r7 = local stack pointer
    //
0:  add     r6, r6, #1              // Increment the arg counter
    lsrs    r5, r5, #1
    bcs     1f                      // Is the current argument 64 bit?
    ldr     r8, [ip], #4            // No? Then just copy it onstack
    str     r8, [r7], #4
    b       2f
1:  tst     r7, #7                  // Yes. Is SP aligned to 8 bytes?
    beq     1f
    add     r7, r7, #4              // No? Realign.
1:  ldr     r8, [ip], #4            // EBC stack may not be aligned for ldrd...
    ldr     r9, [ip], #4
    strd    r8, r9, [r7], #8        // ...but the local stack is.
2:  cmp     r6, #16                 // More than 16 arguments processed?
    bge     0f
    cmp     r4, ip                  // Reached the top of the EBC stack frame?
    bgt     0b

0:  ldr     ip, [r10, #20]          // Set the target address in ip
    blx     ip
    mov     sp, r10                 // Restore the stack, dequeue and return
    pop     {r7-r10, ip}
    pop     {r3, r4-r6}             // Destack with r3, as r0 may contain a return value
    mov     pc, ip

//****************************************************************************
// EbcLLEbcInterpret
//
// This function is called by the thunk code to handle a Native to EBC call
// This can handle up to 16 arguments (args 1-2/1-4 in r0-r3, rest onstack)
// ip contains the Entry point that will be the first argument when
// EBCInterpret is called.
//
//****************************************************************************
ASM_PFX(EbcLLEbcInterpret):

    stmdb   sp!, {r4, lr}

    // push the entry point and the address of non register args on the stack
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
    .long   0                       // EBC_CALL_SIGNATURE
