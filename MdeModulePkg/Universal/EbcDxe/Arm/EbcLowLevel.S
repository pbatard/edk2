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

ASM_GLOBAL ASM_PFX(EbcLLCALLEXNativeArm)
ASM_GLOBAL ASM_PFX(EbcLLEbcInterpret)
ASM_GLOBAL ASM_PFX(EbcLLExecuteEbcImageEntryPoint)
ASM_GLOBAL ASM_PFX(CopyMem);

INTERWORK_FUNC(EbcLLCALLEXNativeArm)
INTERWORK_FUNC(EbcLLEbcInterpret)
INTERWORK_FUNC(EbcLLExecuteEbcImageEntryPoint)

ASM_GLOBAL ASM_PFX(mEbcInstructionBufferTemplate)

//****************************************************************************
// EbcLLCALLEX
//
// This function is called to execute an EBC CALLEX instruction.
// This instruction requires that we thunk out to external native code
// and can handle up to 16 arguments (1-4 in r0-r3, 5-16 on the stack).
// Due to the ARM Procedure Call Standard, we may have to align 64-bit
// arguments to an even register or dword aligned stack address, which
// is where we the ArgLayout parameter comes into play.
// Note that the hard limit for up to 16 arguments below is self imposed
// (for space and speed) and does not stem for an actual limitation.
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
    // align) stack parameters before we invoke the native call.
    //
0:  push    {r7, lr}
    mov     r7, sp                  // Preserve original sp
    push    {r0-r3}                 // Preserve filled register arguments

    //
    // Because arguments are stacked in reverse order, we first need to
    // compute how much space we need for the stack, including realigments.
    // NB: Since we limit the number or args to 16, an alternative would be
    // to just move SP 14x8 bytes down, for 14 non register 64-bit args max,
    // but the computation is fast enough to allow us to preserve space.
    //
    mov     r0, ip                  // r0 = arg walker
    mov     r1, r6                  // r1 = arg counter
    sub     r2, sp, #4              // r2 = local stack
    mov     r3, r5                  // r3 = arg layout
0:  add     r1, r1, #1              // Increment our arg counter
    lsrs    r3, r3, #1
    bcs     1f                      // Is the current argument 64-bit?
    add     r0, r0, #4              // No? Move up our EBC VM stack...
    sub     r2, r2, #4              // and reserve space of stack for 32-bit
    b       2f
1:  add     r0, r0, #8              // Yes? Move up our EBC VM stack...
    sub     r2, r2, #8              // and reserve space on stack for 64-bit
    tst     r2, #7
    beq     2f
    sub     r2, r2, #4
2:  cmp     r1, #15                 // More than 16 arguments processed?
    bge     0f
    cmp     r4, r0                  // Reached the EBC stack frame?
    bgt     0b
0:  and     r2, r2, #0xfffffff8
    mov     sp, r2

    //
    // Same as what we just did above to compute space, except this time
    // we duplicate EBC data into the local stack.
    // ip = arg walker
    // r4 = top of EBC stack frame
    // r5 = arg layout
    // r6 = arg counter
    //
0:  add     r6, r6, #1              // Increment our arg counter
    lsrs    r5, r5, #1
    bcs     1f                      // Is the current argument 64 bit?
    ldr     r3, [ip], #4            // No? Then just copy it into SP
    str     r3, [sp], #4
    b       2f
1:  mov     r3, sp
    tst     r3, #7                  // Yes. Is SP aligned to 8 bytes?
    beq     1f
    add     sp, sp, #4              // No? Realign.
1:  ldr     r0, [ip], #4            // EBC stack may not be aligned for ldrd...
    ldr     r1, [ip], #4
    strd    r0, r1, [sp], #8        // ...but the local stack is.
2:  cmp     r6, #15                 // More than 16 arguments processed?
    bge     0f
    cmp     r4, ip                  // Reached the EBC stack frame?
    bgt     0b

0:  mov     sp, r2                  // Restore SP
    ldrd    r0, r1, [r7, #-16]      // Restore r0-r3 from what we pushed earlier
    ldrd    r2, r3, [r7, #-8]
    ldr     ip, [r7, #8]            // Set the target address in ip
    blx     ip
    mov     sp, r7                  // Restore the stack, dequeue and return
    pop     {r7, ip}
    pop     {r3, r4-r6}             // Destack with r3, as r0 may contain a return value
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
