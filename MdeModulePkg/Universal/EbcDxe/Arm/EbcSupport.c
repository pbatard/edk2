/** @file
  This module contains EBC support routines that are customized based on
  the target Arm processor.

Copyright (c) 2016, Pete Batard. All rights reserved.<BR>
Copyright (c) 2016, Linaro, Ltd. All rights reserved.<BR>
Copyright (c) 2015, The Linux Foundation. All rights reserved.<BR>
Copyright (c) 2006 - 2014, Intel Corporation. All rights reserved.<BR>

This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "EbcInt.h"
#include "EbcExecute.h"

//
// Amount of space that is not used in the stack
//
#define STACK_REMAIN_SIZE (1024 * 4)
//
// Amount of space to be used by the stack argument tracker
// Less than 2 bits are needed for every 32 bits of stack data
// and we can grow our buffer if needed, so start at 1/64th
//
#define STACK_TRACKER_SIZE  (STACK_POOL_SIZE / 64)

#pragma pack(1)
typedef struct {
  UINT32    Instr[2];
  UINT32    Magic;
  UINT32    EbcEntryPoint;
  UINT32    EbcLlEntryPoint;
} EBC_INSTRUCTION_BUFFER;
#pragma pack()

extern CONST EBC_INSTRUCTION_BUFFER       mEbcInstructionBufferTemplate;

/**
  Begin executing an EBC image.
  This is used for Ebc Thunk call.

  @return The value returned by the EBC application we're going to run.

**/
UINT64
EFIAPI
EbcLLEbcInterpret (
  VOID
  );

/**
  Begin executing an EBC image.
  This is used for Ebc image entrypoint.

  @return The value returned by the EBC application we're going to run.

**/
UINT64
EFIAPI
EbcLLExecuteEbcImageEntryPoint (
  VOID
  );

/**
  Pushes a 32 bit unsigned value to the VM stack.

  @param VmPtr  The pointer to current VM context.
  @param Arg    The value to be pushed.

**/
VOID
PushU32 (
  IN VM_CONTEXT *VmPtr,
  IN UINT32     Arg
  )
{
  //
  // Advance the VM stack down, and then copy the argument to the stack.
  // Hope it's aligned.
  //
  VmPtr->Gpr[0] -= sizeof (UINT32);
  *(UINT32 *)(UINTN)VmPtr->Gpr[0] = Arg;
}

/**
  Returns the current stack tracker entry.

  @param VmPtr  The pointer to current VM context.

  @return  The decoded stack tracker index [0x00, 0x08].

  The decoding of stack tracker entries operates as follows:
    00b                        -> 0000b
    01b                        -> 1000b
    1xb preceded by yzb        -> 0xyzb
    (e.g. 11b preceded by 10b  -> 0110b)

**/
UINT8
GetStackTrackerEntry (
  IN VM_CONTEXT *VmPtr
)
{
  UINT8 Index, PreviousIndex;

  if (VmPtr->StackTrackerIndex < 0) {
    //
    // Anything prior to tracking is considered aligned to 64 bits
    //
    return 0x00;
  }

  Index = VmPtr->StackTracker[(VmPtr->StackTrackerIndex - 1) / 4];
  Index >>= 6 - (2 * ((VmPtr->StackTrackerIndex - 1) % 4));
  Index &= 0x03;

  if (Index == 0x01) {
    Index = 0x08;
  } else if (Index & 0x02) {
    PreviousIndex = VmPtr->StackTracker[(VmPtr->StackTrackerIndex - 2) / 4];
    PreviousIndex >>= 6 - (2 * ((VmPtr->StackTrackerIndex - 2) % 4));
    Index = ((Index << 2) & 0x04) | (PreviousIndex & 0x03);
  }

  return Index;
}

/**
  Add a single new stack tracker entry.

  @param VmPtr  The pointer to current VM context.
  @param Value  The value to be encoded.

  @retval EFI_OUT_OF_RESOURCES  Not enough memory to grow the stack tracker.
  @retval EFI_SUCCESS           The entry was added successfully.

  For reference, each 2-bit index sequence is stored as follows:
    Stack tracker byte:     byte 0   byte 1    byte 3
    Stack tracker index:  [0|1|2|3] [4|5|6|7] [8|9|...]

  Valid values are in [0x00, 0x08] and get encoded as:
    0000b -> 00b      (single 2-bit sequence)
    0001b -> 01b 10b  (dual 2-bit sequence)
    0010b -> 10b 10b  (dual 2-bit sequence)
    0011b -> 11b 10b  (dual 2-bit sequence)
    0100b -> 00b 11b  (dual 2-bit sequence)
    0101b -> 01b 11b  (dual 2-bit sequence)
    0110b -> 10b 11b  (dual 2-bit sequence)
    0111b -> 11b 11b  (dual 2-bit sequence)
    1000b -> 01b      (single 2-bit sequence)
**/
EFI_STATUS
AddStackTrackerEntry (
  IN VM_CONTEXT *VmPtr,
  IN UINT8      Value
)
{
  UINT8 i, Data;

  ASSERT (Value <= 0x08);
  if (Value == 0x08) {
    Data = 0x01;
  } else {
    Data = Value & 0x03;
  }

  for (i = 0; i < 2; i++) {
    if ((VmPtr->StackTrackerIndex / 4) >= VmPtr->StackTrackerSize) {
      //
      // Grow the stack tracker buffer
      //
      VmPtr->StackTracker = ReallocatePool(VmPtr->StackTrackerSize,
            VmPtr->StackTrackerSize*2, VmPtr->StackTracker);
      if (VmPtr->StackTracker == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      VmPtr->StackTrackerSize *= 2;
    }

    //
    // Clear bits and add our value
    //
    VmPtr->StackTracker[VmPtr->StackTrackerIndex / 4] &=
          0xFC << (6 - 2 * (VmPtr->StackTrackerIndex % 4));
    VmPtr->StackTracker[VmPtr->StackTrackerIndex / 4] |=
          Data << (6 - 2 * (VmPtr->StackTrackerIndex % 4));
    VmPtr->StackTrackerIndex++;
    if ((Value >= 0x01) && (Value <= 0x07)) {
      //
      // 4-bits needed => Append the extra 2 bit value
      //
      Data = (Value >> 2) | 0x02;
    } else {
      //
      // 2-bit encoding was enough
      //
      break;
    }
  }
  return EFI_SUCCESS;
}

/**
  Insert 'Count' number of 'Value' bytes into the stack tracker.
  This expects the current entry to be aligned to byte boundary.

  @param VmPtr  The pointer to current VM context.
  @param Value  The byte value to be inserted.
  @param Count  The number of times the value should be repeated.

  @retval EFI_OUT_OF_RESOURCES  Not enough memory to grow the stack tracker.
  @retval EFI_SUCCESS           The entries were added successfully.

**/
EFI_STATUS
AddStackTrackerBytes (
  IN VM_CONTEXT *VmPtr,
  IN UINT8      Value,
  IN INTN       Count
)
{
  UINTN i, NewSize;
  INTN UpdatedIndex;

  //
  // Byte alignement should have been sorted prior to this call
  //
  ASSERT (VmPtr->StackTrackerIndex % 4 == 0);

  UpdatedIndex = VmPtr->StackTrackerIndex + 4 * Count;
  if (UpdatedIndex >= VmPtr->StackTrackerSize) {
    //
    // Grow the stack tracker buffer
    //
    for (NewSize = VmPtr->StackTrackerSize * 2; NewSize <= UpdatedIndex;
          NewSize *= 2);
    VmPtr->StackTracker = ReallocatePool(VmPtr->StackTrackerSize, NewSize,
          VmPtr->StackTracker);
    if (VmPtr->StackTracker == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    VmPtr->StackTrackerSize = NewSize;
  }
  for (i = 0; i < Count; i++) {
    VmPtr->StackTracker[(VmPtr->StackTrackerIndex % 4) + i] = Value;
  }
  VmPtr->StackTrackerIndex += Count * 4;
  return EFI_SUCCESS;
}

/**
  Delete a single stack tracker entry.

  @param VmPtr  The pointer to current VM context.

  @retval EFI_UNSUPPORTED       The stack tracker is being underflown due
                                to unbalanced stack operations.
  @retval EFI_SUCCESS           The index was added successfully.
**/
EFI_STATUS
DelStackTrackerEntry (
  IN VM_CONTEXT *VmPtr
)
{
  UINT8 Index;

  //
  // Don't care about clearing the used bits, just update the index
  //
  VmPtr->StackTrackerIndex--;
  Index = VmPtr->StackTracker[VmPtr->StackTrackerIndex / 4];
  Index >>= 6 - (2 * (VmPtr->StackTrackerIndex % 4));

  if (Index & 0x02) {
    //
    // 4-bit sequence
    //
    VmPtr->StackTrackerIndex--;
  }
  if (VmPtr->StackTrackerIndex < 0) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

/**
  Update the stack tracker according to the latest natural and constant
  value stack manipulation operations.

  @param VmPtr         The pointer to current VM context.
  @param NaturalUnits  The number of natural values that were pushed (>0) or
                       popped (<0).
  @param ConstUnits    The number of const bytes that were pushed (>0) or
                       popped (<0).

  @retval EFI_OUT_OF_RESOURCES  Not enough memory to grow the stack tracker.
  @retval EFI_UNSUPPORTED       The stack tracker is being underflown due to
                                unbalanced stack operations.
  @retval EFI_SUCCESS           The stack tracker was updated successfully.

**/
EFI_STATUS
UpdateStackTracker (
  IN VM_CONTEXT  *VmPtr,
  IN INTN        NaturalUnits,
  IN INTN        ConstUnits
)
{
  EFI_STATUS Status;
  UINT8 LastEntry;

  //
  // Mismatched signage should already have been filtered out.
  //
  ASSERT ( ((NaturalUnits >= 0) && (ConstUnits >= 0)) ||
           ((NaturalUnits <= 0) && (ConstUnits <= 0)) )

  while (NaturalUnits < 0) {
    //
    // Add natural indexes (1000b) into our stack tracker
    // Note, we don't care if the previous entry was aligned as a non 64-bit
    // aligned entry cannot be used as a call parameter in valid EBC code.
    // This also adds the effect of re-aligning our data to 64 bytes, which
    // will help speed up tracking of local stack variables (arrays, etc.)
    //
    if ((VmPtr->StackTrackerIndex % 4 == 0) && (NaturalUnits <= -4)) {
      //
      // Optimize adding a large number of naturals, such as ones reserved
      // for local function variables/arrays. 0x55 means 4 naturals.
      //
      Status = AddStackTrackerBytes (VmPtr, 0x55, -NaturalUnits / 4);
      NaturalUnits -= 4 * (NaturalUnits / 4); // Beware of negative modulos
    } else {
      Status = AddStackTrackerEntry (VmPtr, 0x08);
      NaturalUnits++;
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (ConstUnits < 0) {
    //
    // Add constant indexes (0000b-0111b) into our stack tracker
    // For constants, we do care if the previous entry was aligned to 64 bit
    // since we need to include any existing non aligned indexes into the new
    // set of (constant) indexes we are adding. Thus, if the last index is
    // non zero (non 64-bit aligned) we just delete it and add the value to
    // our constant.
    //
    LastEntry = GetStackTrackerEntry (VmPtr);
    if ((LastEntry != 0x00) && (LastEntry != 0x08)) {
      DelStackTrackerEntry (VmPtr);
      ConstUnits -= LastEntry;
    }

    //
    // Now, add as many 64-bit indexes as we can (0000b values)
    //
    while (ConstUnits <= -8) {
      if ((ConstUnits <= -32) && (VmPtr->StackTrackerIndex % 4 == 0)) {
        //
        // Optimize adding a large number of consts, such as ones reserved
        // for local function variables/arrays. 0x00 means 4 64-bit consts.
        //
        Status = AddStackTrackerBytes (VmPtr, 0x00, -ConstUnits / 32);
        ConstUnits -= 32 * (ConstUnits / 32); // Beware of negative modulos
      } else {
        Status = AddStackTrackerEntry (VmPtr, 0x00);
        ConstUnits += 8;
      }
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    //
    // Add any remaining non 64-bit aligned bytes
    //
    if (ConstUnits % 8) {
      Status = AddStackTrackerEntry (VmPtr, (-ConstUnits) % 8);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  while ((NaturalUnits > 0) || (ConstUnits > 0)) {
    ASSERT(VmPtr->StackTrackerIndex > 0);

    //
    // Delete natural/constant items from the stack tracker
    //
    if (VmPtr->StackTrackerIndex % 4 == 0) {
      //
      // Speedup deletion for blocks of naturals/consts
      //
      while ((ConstUnits >= 32) &&
            (VmPtr->StackTracker[(VmPtr->StackTrackerIndex-1) % 4] == 0x00)) {
        //
        // Start with consts since that's what we add last
        //
        VmPtr->StackTrackerIndex -= 4;
        ConstUnits -= 32;
      }
      while ((NaturalUnits >= 4) &&
            (VmPtr->StackTracker[(VmPtr->StackTrackerIndex-1) % 4] == 0x55)) {
        VmPtr->StackTrackerIndex -= 4;
        NaturalUnits -= 4;
      }
    }

    if ((NaturalUnits == 0) && (ConstUnits == 0)) {
      //
      // May already have depleted our values through block processing above
      //
      break;
    }

    LastEntry = GetStackTrackerEntry (VmPtr);
    Status = DelStackTrackerEntry (VmPtr);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (LastEntry == 0x08) {
      if (NaturalUnits > 0) {
        //
        // Remove a natural and move on
        //
        NaturalUnits--;
        continue;
      }
      //
      // Got a natural while expecting const which may be the result of a
      // "cloaked" stack operation (eg. R1 <- R0, stack ops on R1, R0 <- R1)
      // which we monitor through the R0 delta converted to const. In this
      // case just remove 4 const for each natural we find in the tracker.
      //
      LastEntry = 0x04;
    } else if (ConstUnits <= 0) {
      //
      // Got a const while expecting a natural which may be the result of a
      // "cloaked" stack operation => Substract 1 natural unit and add 4 to
      // const units. Note that "cloaked" stack operations cannot break our
      // tracking as the enqueuing of natural parameters is not something
      // that can be concealed if one interprets the EBC specs correctly.
      //
      NaturalUnits--;
      ConstUnits += 4;
    }

    if (LastEntry == 0x00) {
      LastEntry = 0x08;
    }
    //
    // Remove a set of const bytes
    //
    if (ConstUnits >= LastEntry) {
      //
      // Enough const bytes to remove at least one stack tracker entry
      //
      ConstUnits -= LastEntry;
    } else {
      //
      // Not enough const bytes - need to add the remainder back
      //
      Status = AddStackTrackerEntry (VmPtr, LastEntry - ConstUnits);
      ConstUnits = 0;
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  ASSERT(VmPtr->StackTrackerIndex >= 0);
  return EFI_SUCCESS;
}

/**
  Update the stack tracker by computing the R0 delta.

  @param VmPtr         The pointer to current VM context.
  @param UpdatedR0     The new R0 value.

  @retval EFI_OUT_OF_RESOURCES  Not enough memory to grow the stack tracker.
  @retval EFI_UNSUPPORTED       The stack tracker is being underflown due to
                                unbalanced stack operations.
  @retval EFI_SUCCESS           The stack tracker was updated successfully.

**/
EFI_STATUS
UpdateStackTrackerFromDelta (
  IN VM_CONTEXT *VmPtr,
  IN UINTN      UpdatedR0
  )
{
  INTN StackPointerDelta;

  //
  // Check if the updated R0 is still in our original stack buffer.
  //
  if ((VmPtr->OrgStackTrackerIndex == 0) && ((UpdatedR0 < (UINTN) VmPtr->StackPool) ||
      (UpdatedR0 >= (UINTN) VmPtr->StackPool + STACK_POOL_SIZE))) {
    //
    // We are swicthing from the default stack buffer to a newly allocated
    // one. Keep track of our current stack tracker index in case we come
    // back to the original stack with unbalanced stack ops (e.g.
    // SP <- New stack; Enqueue data without dequeuing; SP <- Old SP)
    // Note that, since we are not moinitoring memory allocations, we can
    // only ever detect swicthing in and out of the default stack buffer.
    //
    VmPtr->OrgStackTrackerIndex = VmPtr->StackTrackerIndex;
    VmPtr->OrgStackPointer = (UINTN) VmPtr->Gpr[0];

    //
    // Do not track switching. Just realign the index.
    //
    VmPtr->StackTrackerIndex = 4 * ((VmPtr->StackTrackerIndex + 3) / 4);
    return EFI_SUCCESS;
  }

  if ((VmPtr->OrgStackTrackerIndex != 0) && ((UpdatedR0 >= (UINTN) VmPtr->StackPool) ||
      (UpdatedR0 < (UINTN) VmPtr->StackPool + STACK_POOL_SIZE))) {
    //
    // Coming back from a newly allocated stack to the original one
    // As we don't expect stack ops to have been properly balanced we just
    // restore the old stack tracker index.
    //
    VmPtr->StackTrackerIndex = VmPtr->OrgStackTrackerIndex;
    VmPtr->OrgStackTrackerIndex = 0;
    //
    // There's also no guarantee that the new R0 is being restored to the
    // value it held when stwiching stacks, so we use the value R0 held
    // at the time the switch was performed, to compute the delta.
    StackPointerDelta = UpdatedR0 - VmPtr->OrgStackPointer;
  } else {
    StackPointerDelta = UpdatedR0 - (UINTN) VmPtr->Gpr[0];
  }

  return UpdateStackTracker(VmPtr, 0, StackPointerDelta);
}


/**
  Begin executing an EBC image.

  This is a thunk function.

  @param  Arg1                  The 1st argument.
  @param  Arg2                  The 2nd argument.
  @param  Arg3                  The 3rd argument.
  @param  Arg4                  The 4th argument.
  @param  Arg8                  The 8th argument.
  @param  EntryPoint            The entrypoint of EBC code.
  @param  Args5_16[]            Array containing arguments #5 to #16.

  @return The value returned by the EBC application we're going to run.

**/
UINT64
EFIAPI
EbcInterpret (
  IN UINTN      Arg1,
  IN UINTN      Arg2,
  IN UINTN      Arg3,
  IN UINTN      Arg4,
  IN UINTN      EntryPoint,
  IN UINTN      Args5_16[]
  )
{
  //
  // Sanity checks for the stack tracker
  //
  ASSERT (sizeof (UINT64) == 8);
  ASSERT (sizeof (UINT32) == 4);

  //
  // Create a new VM context on the stack
  //
  VM_CONTEXT  VmContext;
  UINTN       Addr;
  EFI_STATUS  Status;
  UINTN       StackIndex;

  //
  // Get the EBC entry point
  //
  Addr = EntryPoint;

  //
  // Now clear out our context
  //
  ZeroMem ((VOID *) &VmContext, sizeof (VM_CONTEXT));

  //
  // Set the VM instruction pointer to the correct location in memory.
  //
  VmContext.Ip = (VMIP) Addr;

  //
  // Allocate the stack tracker and initialize it
  //
  VmContext.StackTrackerSize = STACK_TRACKER_SIZE;
  VmContext.StackTracker = AllocatePool(VmContext.StackTrackerSize);
  if (VmContext.StackTracker == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Add tracking for EfiMain() call just in case
  //
  VmContext.StackTracker[0] = 0x05; // 2 x UINT64, 2 x UINTN
  VmContext.StackTrackerIndex = 4;

  //
  // Initialize the stack pointer for the EBC. Get the current system stack
  // pointer and adjust it down by the max needed for the interpreter.
  //
  Status = GetEBCStack((EFI_HANDLE)(UINTN)-1, &VmContext.StackPool, &StackIndex);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  //
  // Adjust the VM's stack pointer down.
  //
  VmContext.StackTop = (UINT8*)VmContext.StackPool + STACK_REMAIN_SIZE;
  VmContext.Gpr[0] = (UINT64)(UINTN) ((UINT8*)VmContext.StackPool + STACK_POOL_SIZE);
  VmContext.HighStackBottom = (UINTN) VmContext.Gpr[0];
  VmContext.Gpr[0] -= sizeof (UINTN);

  //
  // Align the stack on a natural boundary.
  //
  VmContext.Gpr[0] &= ~(VM_REGISTER)(sizeof (UINTN) - 1);

  //
  // Put a magic value in the stack gap, then adjust down again.
  //
  *(UINTN *) (UINTN) (VmContext.Gpr[0]) = (UINTN) VM_STACK_KEY_VALUE;
  VmContext.StackMagicPtr             = (UINTN *) (UINTN) VmContext.Gpr[0];

  //
  // The stack upper to LowStackTop belongs to the VM.
  //
  VmContext.LowStackTop   = (UINTN) VmContext.Gpr[0];

  //
  // For the worst case, assume there are 4 arguments passed in registers, store
  // them to VM's stack.
  //
  PushU32 (&VmContext, (UINT32) Args5_16[11]);
  PushU32 (&VmContext, (UINT32) Args5_16[10]);
  PushU32 (&VmContext, (UINT32) Args5_16[9]);
  PushU32 (&VmContext, (UINT32) Args5_16[8]);
  PushU32 (&VmContext, (UINT32) Args5_16[7]);
  PushU32 (&VmContext, (UINT32) Args5_16[6]);
  PushU32 (&VmContext, (UINT32) Args5_16[5]);
  PushU32 (&VmContext, (UINT32) Args5_16[4]);
  PushU32 (&VmContext, (UINT32) Args5_16[3]);
  PushU32 (&VmContext, (UINT32) Args5_16[2]);
  PushU32 (&VmContext, (UINT32) Args5_16[1]);
  PushU32 (&VmContext, (UINT32) Args5_16[0]);
  PushU32 (&VmContext, (UINT32) Arg4);
  PushU32 (&VmContext, (UINT32) Arg3);
  PushU32 (&VmContext, (UINT32) Arg2);
  PushU32 (&VmContext, (UINT32) Arg1);

  //
  // Interpreter assumes 64-bit return address is pushed on the stack.
  // Arm does not do this so pad the stack accordingly.
  //
  PushU32 (&VmContext, 0x0UL);
  PushU32 (&VmContext, 0x0UL);
  PushU32 (&VmContext, 0x12345678UL);
  PushU32 (&VmContext, 0x87654321UL);

  //
  // For Arm, this is where we say our return address is
  //
  VmContext.StackRetAddr  = (UINT64) VmContext.Gpr[0];

  //
  // We need to keep track of where the EBC stack starts. This way, if the EBC
  // accesses any stack variables above its initial stack setting, then we know
  // it's accessing variables passed into it, which means the data is on the
  // VM's stack.
  // When we're called, on the stack (high to low) we have the parameters, the
  // return address, then the saved ebp. Save the pointer to the return address.
  // EBC code knows that's there, so should look above it for function parameters.
  // The offset is the size of locals (VMContext + Addr + saved ebp).
  // Note that the interpreter assumes there is a 16 bytes of return address on
  // the stack too, so adjust accordingly.
  //  VmContext.HighStackBottom = (UINTN)(Addr + sizeof (VmContext) + sizeof (Addr));
  //

  //
  // Begin executing the EBC code
  //
  EbcExecute (&VmContext);

  //
  // Return the value in R[7] unless there was an error
  //
  ReturnEBCStack(StackIndex);
  FreePool(VmContext.StackTracker);
  return (UINT64) VmContext.Gpr[7];
}


/**
  Begin executing an EBC image.

  @param  ImageHandle      image handle for the EBC application we're executing
  @param  SystemTable      standard system table passed into an driver's entry
                           point
  @param  EntryPoint       The entrypoint of EBC code.

  @return The value returned by the EBC application we're going to run.

**/
UINT64
EFIAPI
ExecuteEbcImageEntryPoint (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable,
  IN UINTN                EntryPoint
  )
{
  //
  // Sanity checks for the stack tracker
  //
  ASSERT (sizeof (UINT64) == 8);
  ASSERT (sizeof (UINT32) == 4);

  //
  // Create a new VM context on the stack
  //
  VM_CONTEXT  VmContext;
  UINTN       Addr;
  EFI_STATUS  Status;
  UINTN       StackIndex;

  //
  // Get the EBC entry point
  //
  Addr = EntryPoint;

  //
  // Now clear out our context
  //
  ZeroMem ((VOID *) &VmContext, sizeof (VM_CONTEXT));

  //
  // Save the image handle so we can track the thunks created for this image
  //
  VmContext.ImageHandle = ImageHandle;
  VmContext.SystemTable = SystemTable;

  //
  // Set the VM instruction pointer to the correct location in memory.
  //
  VmContext.Ip = (VMIP) Addr;

  //
  // Allocate and initialize the stack tracker
  //
  VmContext.StackTrackerSize = STACK_TRACKER_SIZE;
  VmContext.StackTracker = AllocatePool(VmContext.StackTrackerSize);
  if (VmContext.StackTracker == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Add tracking for EfiMain() call just in case
  //
  VmContext.StackTracker[0] = 0x05; // 2 x UINT64, 2 x UINTN
  VmContext.StackTrackerIndex = 4;

  //
  // Initialize the stack pointer for the EBC. Get the current system stack
  // pointer and adjust it down by the max needed for the interpreter.
  //

  //
  // Allocate stack pool
  //
  Status = GetEBCStack (ImageHandle, &VmContext.StackPool, &StackIndex);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  VmContext.StackTop = (UINT8*)VmContext.StackPool + STACK_REMAIN_SIZE;
  VmContext.Gpr[0] = (UINT64)(UINTN) ((UINT8*)VmContext.StackPool + STACK_POOL_SIZE);
  VmContext.HighStackBottom = (UINTN)VmContext.Gpr[0];
  VmContext.Gpr[0] -= sizeof (UINTN);

  //
  // Put a magic value in the stack gap, then adjust down again
  //
  *(UINTN *) (UINTN) (VmContext.Gpr[0]) = (UINTN) VM_STACK_KEY_VALUE;
  VmContext.StackMagicPtr             = (UINTN *) (UINTN) VmContext.Gpr[0];

  //
  // Align the stack on a natural boundary
  //  VmContext.Gpr[0] &= ~(sizeof(UINTN) - 1);
  //
  VmContext.LowStackTop   = (UINTN) VmContext.Gpr[0];
  VmContext.Gpr[0] -= sizeof (UINTN);
  *(UINTN *) (UINTN) (VmContext.Gpr[0]) = (UINTN) SystemTable;
  VmContext.Gpr[0] -= sizeof (UINTN);
  *(UINTN *) (UINTN) (VmContext.Gpr[0]) = (UINTN) ImageHandle;

  VmContext.Gpr[0] -= 16;
  VmContext.StackRetAddr  = (UINT64) VmContext.Gpr[0];
  //
  // VM pushes 16-bytes for return address. Simulate that here.
  //

  //
  // Begin executing the EBC code
  //
  EbcExecute (&VmContext);

  //
  // Return the value in R[7] unless there was an error
  //
  ReturnEBCStack(StackIndex);
  FreePool(VmContext.StackTracker);
  return (UINT64) VmContext.Gpr[7];
}


/**
  Create thunks for an EBC image entry point, or an EBC protocol service.

  @param  ImageHandle           Image handle for the EBC image. If not null, then
                                we're creating a thunk for an image entry point.
  @param  EbcEntryPoint         Address of the EBC code that the thunk is to call
  @param  Thunk                 Returned thunk we create here
  @param  Flags                 Flags indicating options for creating the thunk

  @retval EFI_SUCCESS           The thunk was created successfully.
  @retval EFI_INVALID_PARAMETER The parameter of EbcEntryPoint is not 16-bit
                                aligned.
  @retval EFI_OUT_OF_RESOURCES  There is not enough memory to created the EBC
                                Thunk.
  @retval EFI_BUFFER_TOO_SMALL  EBC_THUNK_SIZE is not larger enough.

**/
EFI_STATUS
EbcCreateThunks (
  IN EFI_HANDLE           ImageHandle,
  IN VOID                 *EbcEntryPoint,
  OUT VOID                **Thunk,
  IN  UINT32              Flags
  )
{
  EBC_INSTRUCTION_BUFFER       *InstructionBuffer;

  //
  // Check alignment of pointer to EBC code
  //
  if ((UINT32) (UINTN) EbcEntryPoint & 0x01) {
    return EFI_INVALID_PARAMETER;
  }

  InstructionBuffer = AllocatePool (sizeof (EBC_INSTRUCTION_BUFFER));
  if (InstructionBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Give them the address of our buffer we're going to fix up
  //
  *Thunk = InstructionBuffer;

  //
  // Copy whole thunk instruction buffer template
  //
  CopyMem (InstructionBuffer, &mEbcInstructionBufferTemplate,
    sizeof (EBC_INSTRUCTION_BUFFER));

  //
  // Patch EbcEntryPoint and EbcLLEbcInterpret
  //
  InstructionBuffer->EbcEntryPoint = (UINT32)EbcEntryPoint;
  if ((Flags & FLAG_THUNK_ENTRY_POINT) != 0) {
    InstructionBuffer->EbcLlEntryPoint = (UINT32)EbcLLExecuteEbcImageEntryPoint;
  } else {
    InstructionBuffer->EbcLlEntryPoint = (UINT32)EbcLLEbcInterpret;
  }

  //
  // Add the thunk to the list for this image. Do this last since the add
  // function flushes the cache for us.
  //
  EbcAddImageThunk (ImageHandle, InstructionBuffer,
    sizeof (EBC_INSTRUCTION_BUFFER));

  return EFI_SUCCESS;
}


/**
  This function is called to execute an EBC CALLEX instruction.
  The function check the callee's content to see whether it is common native
  code or a thunk to another piece of EBC code.
  If the callee is common native code, use EbcLLCAllEXASM to manipulate,
  otherwise, set the VM->IP to target EBC code directly to avoid another VM
  be startup which cost time and stack space.

  @param  VmPtr            Pointer to a VM context.
  @param  FuncAddr         Callee's address
  @param  NewStackPointer  New stack pointer after the call
  @param  FramePtr         New frame pointer after the call
  @param  Size             The size of call instruction

**/
VOID
EbcLLCALLEX (
  IN VM_CONTEXT   *VmPtr,
  IN UINTN        FuncAddr,
  IN UINTN        NewStackPointer,
  IN VOID         *FramePtr,
  IN UINT8        Size
  )
{
  CONST EBC_INSTRUCTION_BUFFER *InstructionBuffer;
  UINT8 ArgLayout;
  INTN i;

  //
  // Processor specific code to check whether the callee is a thunk to EBC.
  //
  InstructionBuffer = (EBC_INSTRUCTION_BUFFER *)FuncAddr;

  if (CompareMem (InstructionBuffer, &mEbcInstructionBufferTemplate,
        sizeof(EBC_INSTRUCTION_BUFFER) - 2 * sizeof (UINT32)) == 0) {
    //
    // The callee is a thunk to EBC, adjust the stack pointer down 16 bytes and
    // put our return address and frame pointer on the VM stack.
    // Then set the VM's IP to new EBC code.
    //
    VmPtr->Gpr[0] -= 8;
    VmWriteMemN (VmPtr, (UINTN) VmPtr->Gpr[0], (UINTN) FramePtr);
    VmPtr->FramePtr = (VOID *) (UINTN) VmPtr->Gpr[0];
    VmPtr->Gpr[0] -= 8;
    VmWriteMem64 (VmPtr, (UINTN) VmPtr->Gpr[0], (UINT64) (UINTN) (VmPtr->Ip + Size));

    VmPtr->Ip = (VMIP) InstructionBuffer->EbcEntryPoint;
  } else {
    //
    // The callee is not a thunk to EBC, call native code,
    // and get return value.
    //
    // Now, one major issue we have on Arm32 is that, if a mix of natural and
    // 64-bit arguments are stacked as parameters for a native call, we risk
    // running afoul of the AAPCS (the ARM calling convention) which mandates
    // that the first 2 to 4 arguments are passed as register, and that any
    // 64-bit argument *MUST* start on r0 or r2.
    //
    // So if, say, we have a natural parameter (32-bit) in Arg0 and a 64-bit
    // parameter in Arg1, then we must pad the first parameter to 64 bit, so
    // that Arg1 start at register r2.
    //
    // This is where our stack tracker comes into play, as it tracks EBC stack
    // manipulations and allows us to find whether each of the (potential)
    // arguments being passed to a native CALLEX is 64-bit or a natural. As
    // a reminder, and as per the specs (UEFI 2.6, section 21.9.3), 64-bit
    // or natural are the ONLY two types of arguments that are allowed to
    // be passed when performing a function call from EBC, even if it happens
    // to be a native one (in which case any <= 32-bit argument must be
    // stacked as a natural.
    //
    // Using the stack tracker, we can retreive the last 4 argument types
    // (encoded as 2 bit sequences), which we convert to a 4-bit value
    // that gives us the argument layout. Using this layout we can find out
    // if any of the Arm argument registers requires padding.
    //
    ArgLayout = 0;
    for (i = VmPtr->StackTrackerIndex - 4; i <= (VmPtr->StackTrackerIndex - 1); i++) {
      ArgLayout <<= 1;
      if ((i / 4) < 0) {
        continue;
      }

      //
      // Actual parameters will be stored as 2 bit sequences, with 0 as the
      // msb and a 0 lsb for 64-bit values or a 1 lsb for natural values.
      // Also, since there's little point in avoiding to process dual 2-bit
      // sequences (for stack values that aren't natural or 64-bit, and thus
      // can't be used as params) we also treat them as if they were 2-bit.
      //
      if (VmPtr->StackTracker[i / 4] & (1 << (2 * (3 - (i % 4)))))
        ArgLayout |= 1;
    }

    //
    // We'll use the fact that the NewStackPointer is 32-bit aligned to tell
    // the assembly which arg register needs to be padded through the lower
    // 2 bits. This avoids having to modify the function declaration just
    // for Arm. This does warrant an ASSERT, to be on the safe side.
    //
    ASSERT((NewStackPointer & 0x03) == 0);

    //
    // At this stage, ArgLayout is one of the following:
    // Arg# =  3  2  1  0
    // 0000b (64/64/64/64) -> ok
    // 0001b (64/64/64/Nl) -> padding needed for arg0 @ dword #0
    // 0010b (64/64/Nl/64) -> padding needed for arg1 @ dword #2
    // 0011b (64/64/Nl/Nl) -> ok
    // 0100b (64/Nl/64/64) -> ok
    // 0101b (64/Nl/64/Nl) -> padding needed for arg0 @ dword #0
    // 0110b (64/Nl/Nl/64) -> ok
    // 0111b (64/Nl/Nl/Nl) -> padding needed for arg2 @ dword #2 (yes, #2)
    // 1000b (Nl/64/64/64) -> ok
    // 1001b (Nl/64/64/Nl) -> padding needed for arg0 @ dword #0
    // 1010b (Nl/64/Nl/64) -> padding needed for arg1 @ dword #2
    // 1011b (Nl/64/Nl/Nl) -> ok
    // 1100b (Nl/Nl/64/64) -> ok
    // 1101b (Nl/Nl/64/Nl) -> padding needed for arg0 @ dword #0
    // 1110b (Nl/Nl/Nl/64) -> ok
    // 1111b (Nl/Nl/Nl/Nl) -> ok
    //
    if ((ArgLayout == 0x07) || ((ArgLayout & 0x07) == 0x02)) {
      NewStackPointer |= 3; // r3 will need to be padded
    } else if ((ArgLayout & 0x03) == 0x01) {
      NewStackPointer |= 1; // r1 will need to be padded
    }

    //
    // Note that we are not able to distinguish which part of the interval
    // [NewStackPointer, FramePtr] consists of stacked function arguments for
    // this call, and which part simply consists of locals in the caller's
    // stack frame. All we know is that there is an 8 byte gap at the top that
    // we can ignore.
    //
    VmPtr->Gpr[7] = EbcLLCALLEXNative (FuncAddr, NewStackPointer, FramePtr - 8);

    //
    // Advance the IP.
    //
    VmPtr->Ip += Size;
  }
}
