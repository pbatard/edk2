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

//
// TODO: remove those
//
VOID WaitForKey(VOID)
{
	UINTN Event;
	gST->ConIn->Reset(gST->ConIn, FALSE);
	gST->BootServices->WaitForEvent(1, &gST->ConIn->WaitForKey, &Event);
}

VOID PrintStr(CHAR16* Str)
{
	gST->ConOut->OutputString(gST->ConOut, Str);
}

VOID PrintHex(CHAR16* Str, UINT32 val)
{
	CHAR16 a[] = L"0123456789ABCDEF";
	CHAR16 r[] = L"0x12345678\r\n";
	INTN i, d;
	UINT32 n = val, m = 0x0FFFFFFF;

	for (i = 0; i < 8; i++) {
		d = n >> (4 * (7 - i));
		r[i + 2] = a[d];
		n &= m;
		m >>= 4;
	}

	PrintStr(Str);
	PrintStr(r);
}

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

**/
UINT8
GetStackTrackerIndex (
  IN VM_CONTEXT *VmPtr
)
{
  UINT8 Index, IndexPrev;

  if (VmPtr->StackTrackerIndex < 0) {
    // Anything prior to tracking is considered aligned to 64 bits
    return 0x00;
  }

  Index = VmPtr->StackTracker[(VmPtr->StackTrackerIndex - 1) / 4];
  Index >>= 6 - (2 * ((VmPtr->StackTrackerIndex - 1) % 4));
  Index &= 0x03;

  // Decoding operates as follows:
  // 00b                        -> 0000b
  // 01b                        -> 1000b
  // 1Xb preceded by YZb        -> 0XYZb
  // (e.g. 11b preceded by 10b  -> 0110b)
  //
  // Note that, in accordance with the UEFI specs, when CALLEX to native is
  // invoked, then the *ONLY* valid values allowed for the stacked function 
  // parameters are 0000b (for a 64 bit parameter) or 1000b (native length,
  // which is used for standard native, or any argument that is 32-bit or
  // smaller).
  // Therefore any encouter with 0001b to 0111b as part of CALLEX argument
  // processing must be rejected as unsupported code, as it signifies a
  // programming error in the EBC application.
  // 
  if (Index == 0x01) {
    Index = 0x08;
  } else if (Index & 0x02) {
    IndexPrev = VmPtr->StackTracker[(VmPtr->StackTrackerIndex - 2) / 4];
    IndexPrev >>= 6 - (2 * ((VmPtr->StackTrackerIndex - 2) % 4));
    Index = ((Index << 2) & 0x04) | (IndexPrev & 0x03);
  }

//  PrintHex(L"Get: ", Index);
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
**/
EFI_STATUS
AddStackTrackerIndex (
  IN VM_CONTEXT *VmPtr,
  IN UINT8 Index
)
{
  UINT8 i, Data;

  // Valid values are [0x00, 0x08], which get encoded as:
  // 0000b -> 00b      (single 2-bit sequence)
  // 0001b -> 01b 10b  (dual 2-bit sequence)
  // 0010b -> 10b 10b  (dual 2-bit sequence)
  // 0011b -> 11b 10b  (dual 2-bit sequence)
  // 0100b -> 00b 11b  (dual 2-bit sequence)
  // 0101b -> 01b 11b  (dual 2-bit sequence)
  // 0110b -> 10b 11b  (dual 2-bit sequence)
  // 0111b -> 11b 11b  (dual 2-bit sequence)
  // 1000b -> 01b      (single 2-bit sequence)
  ASSERT (Index <= 0x08);
  if (Index == 0x08) {
    Data = 0x01;
  } else {
    Data = Index & 0x03;
  }

  for (i = 0; i < 2; i++) {
    if ((VmPtr->StackTrackerIndex / 4) >= VmPtr->StackTrackerSize) {
      // Grow the stack tracker buffer
      VmPtr->StackTracker = ReallocatePool(VmPtr->StackTrackerSize, VmPtr->StackTrackerSize*2, VmPtr->StackTracker);
      if (VmPtr->StackTracker == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      VmPtr->StackTrackerSize *= 2;
    }
    // Ensure that we clear bits we don't use yet
    VmPtr->StackTracker[VmPtr->StackTrackerIndex / 4] &= 0xFC << (6 - 2 * (VmPtr->StackTrackerIndex % 4));
    VmPtr->StackTracker[VmPtr->StackTrackerIndex / 4] |= Data << (6 - 2 * (VmPtr->StackTrackerIndex % 4));
    VmPtr->StackTrackerIndex++;
    if ((Index >= 0x01) && (Index <= 0x07)) {
      // 4-bits needed => Append the extra 2 bit value
      Data = (Index >> 2) | 0x02;
    } else {
      // 2-bit encoding was enough
      break;
    }
  }
  return EFI_SUCCESS;
}

/**
  Insert 'Count' number of 'Value' bytes into the stack tracker
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
  IN UINT8 Value,
  IN INTN Count
)
{
  UINTN i, NewSize;
  INTN UpdatedIndex;

  // Byte alignement should have been sorted prior to this call
  ASSERT (VmPtr->StackTrackerIndex % 4 == 0);

  UpdatedIndex = VmPtr->StackTrackerIndex + 4 * Count;
  if (UpdatedIndex >= VmPtr->StackTrackerSize) {
    // Grow the stack tracker buffer
    for (NewSize = VmPtr->StackTrackerSize * 2; NewSize <= UpdatedIndex; NewSize *= 2);
    VmPtr->StackTracker = ReallocatePool(VmPtr->StackTrackerSize, NewSize, VmPtr->StackTracker);
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

  @retval EFI_UNSUPPORTED       The stack tracker is being underflown due to
                                unbalanced stack operations.
  @retval EFI_SUCCESS           The index was added successfully.
**/
EFI_STATUS
DelStackTrackerIndex (
  IN VM_CONTEXT *VmPtr
)
{
  UINT8 Index;

  // We don't care about clearing the used bits, just update the index
  VmPtr->StackTrackerIndex--;
  Index = VmPtr->StackTracker[VmPtr->StackTrackerIndex / 4];
  Index >>= 6 - (2 * (VmPtr->StackTrackerIndex % 4));

  if (Index & 0x02) {
    // Dual sequence
    VmPtr->StackTrackerIndex--;
  }
  if (VmPtr->StackTrackerIndex < 0) {
PrintStr(L"DelStackTrackerIndex - underflow\r\n");
WaitForKey();
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
  IN VM_CONTEXT *VmPtr,
  IN INT64 NaturalUnits,
  IN INT64 ConstUnits
)
{
  EFI_STATUS Status;
  UINT8 LastIndex;

  // Mismatched signage should already have been filtered out.
  ASSERT ( ((NaturalUnits >= 0) && (ConstUnits >= 0)) ||
           ((NaturalUnits <= 0) && (ConstUnits <= 0)) )

  while (NaturalUnits < 0) {
    // Add natural indexes (1000b) into our stack tracker
    // Note, we don't care if the previous entry was aligned as a non 64-bit
    // aligned entry cannot be used as a call parameter in valid EBC code.
    // This also adds the effect of re-aligning our data to 64 bytes, which
    // will help speed up tracking of local stack variables (arrays, etc.)
    if ((VmPtr->StackTrackerIndex % 4 == 0) && (NaturalUnits <= -4)) {
      // Optimize adding of a large number of naturals, such as the ones
      // reserved for local function variables/arrays. 0x55 = 4 naturals.
      Status = AddStackTrackerBytes (VmPtr, 0x55, -1 * (INTN)(NaturalUnits / 4));
      NaturalUnits -= 4 * (NaturalUnits / 4); // Beware of negative modulos
    } else {
      Status = AddStackTrackerIndex (VmPtr, 0x08);
      NaturalUnits++;
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (ConstUnits < 0) {
    // Add constant indexes (0000b-0111b) into our stack tracker
    // For constants, we do care if the previous entry was aligned to 64 bit
    // since we need to include any existing non aligned indexes into the new
    // set of (constant) indexes we are adding. Thus, if the last index is
    // non zero (non 64-bit aligned) we just delete it and add the value to
    // our constant.
    LastIndex = GetStackTrackerIndex (VmPtr);
    if ((LastIndex != 0x00) && (LastIndex != 0x08)) {
      DelStackTrackerIndex (VmPtr);
      ConstUnits -= LastIndex;
    }
    // Now, add as many 64-bit indexes as we can (0000b values)
    while (ConstUnits <= -8) {
      if ((ConstUnits <= -32) && (VmPtr->StackTrackerIndex % 4 == 0)) {
        // Optimize adding of a large number of consts, such as the ones
        // reserved for local function variables/arrays. 0x00 = 4 64-bit consts.
        Status = AddStackTrackerBytes (VmPtr, 0x00, -1 * (INTN)(ConstUnits / 32));
        ConstUnits -= 32 * (ConstUnits / 32); // Beware of negative modulos
      } else {
        Status = AddStackTrackerIndex (VmPtr, 0x00);
        ConstUnits += 8;
      }
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
    // Add any remaining non 64-bit aligned bytes
    if (ConstUnits % 8) {
      Status = AddStackTrackerIndex (VmPtr, (-ConstUnits) % 8);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  while ((NaturalUnits > 0) || (ConstUnits > 0)) {
    // Delete natural/constant items from the stack tracker
    LastIndex = GetStackTrackerIndex (VmPtr);
    Status = DelStackTrackerIndex (VmPtr);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (LastIndex == 0x08) {
      if (NaturalUnits > 0) {
        // Remove a natural and move on
        NaturalUnits--;
        continue;
      }
      // Got a natural while expecting const which may be the result of a
      // "cloaked" stack operation (eg. R1 <- R0, stack ops on R1, R0 <- R1)
      // which we monitor through the R0 delta converted to const. In this
      // case just remove 4 const for each natural we find in the tracker.
      LastIndex = 0x04;
    } else if (ConstUnits <= 0) {
       // Got a const while expecting a natural which may be the result of a
       // "cloaked" stack operation => Substract 1 natural unit and add 4 to
       // const units. Note that "cloaked" stack operations cannot break our
       // tracking as the enqueuing of natural parameters is not something
       // that can be concealed if one interprets the EBC specs correctly.
       NaturalUnits--;
       ConstUnits += 4;
    }
    if (LastIndex == 0x00) {
      LastIndex = 0x08;
    }
    // Remove a set of const bytes
    if (ConstUnits >= LastIndex) {
      // Enough const bytes to remove the whole stack tracker entry
      ConstUnits -= LastIndex;
    } else {
      // Not enough const bytes - need to add the remainder back
      Status = AddStackTrackerIndex (VmPtr, LastIndex - ConstUnits);
      ConstUnits = 0;
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  return EFI_SUCCESS;
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
  // Add tracking for EfiMain() call just in case
  VmContext.StackTracker[0] = 0x05; // 2 x UINT64, 2 x UINTN
  VmContext.StackTrackerIndex = 4;

  //
  // Initialize the stack pointer for the EBC. Get the current system stack
  // pointer and adjust it down by the max needed for the interpreter.
  //

  //
  // Adjust the VM's stack pointer down.
  //

  Status = GetEBCStack((EFI_HANDLE)(UINTN)-1, &VmContext.StackPool, &StackIndex);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  // Reserve space at the bottom of the allocated stack for the stack argument tracker
  VmContext.StackTop = (UINT8*)VmContext.StackPool + STACK_REMAIN_SIZE;
  VmContext.Gpr[0] = (UINT32) ((UINT8*)VmContext.StackPool + STACK_POOL_SIZE);
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
  // Add tracking for EfiMain() call just in case
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
  VmContext.Gpr[0] = (UINT32)((UINT8*)VmContext.StackPool + STACK_POOL_SIZE);
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
  UINT32 BackupData, *ArgPtr = (UINT32*) NewStackPointer;
  UINT8 ArgLayout;
  INTN i, Padding = 0;

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
    // This is where our stack tracker comes into play, which tracks EBC stack
    // manipulations and allows us to find whether each of the (potential)
    // arguments being passed to a native CALLEX is 64-bit or a natural (since
    // are the ONLY two types of arguments that can be passed to a native
    // call, as per the UEFI/EBC specs).
    //
    // Using the stack tarcker, we can retreive the last 4 argument types
    // (encode as 2 bit sequences), which we convert to a 4-bit value (with 
    // each bit set for natural, cleared for 64-bit) that provides us with
    // an argument layout we can use to determine where padding is needed.
    //
    ArgLayout = 0;
    for (i = VmPtr->StackTrackerIndex - 4; i <= (VmPtr->StackTrackerIndex - 1); i++) {
      ArgLayout <<= 1;
      if ((i / 4) < 0) // We may attempt to read before the stack, which is ok
        continue;
      // NB: There's little point in trying to detect dual 2-bit sequences
      // here (used for stack tracked values that aren't natural or 64-bit)
      // even if they could be used to detect the end of function parameters.
      // Since those can't apply to actual function parameters (unless the
      // EBC code is in breach of the specs), it won't matter if we attempt
      // to process them.
      if (VmPtr->StackTracker[i / 4] & (1 << (2 * (3 - (i % 4)))))
        ArgLayout |= 1;
    }

	// TODO: validate each sequence
    // At this stage, ArgLayout is one of the following
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
    if ((ArgLayout == 0x07) || ((ArgLayout & 0x07) == 0x02))
      Padding = 3; // dword # to be zeroed
    else if ((ArgLayout & 0x03) == 0x01)
      Padding = 1; // dword # to be zeroed

    // Padding is only needed if this actually belongs to stacked data
    if ((UINTN) &ArgPtr[Padding] > (UINTN) FramePtr)
      Padding = 0;
    // Since we have at most one arg we need to shift, just shift the whole
    // stack 4 bytes left, up to our argument, and then fill a zero in.
    // TODO: Don't modify the stack - pad the registers in the assembly instead
    if (Padding) {
      // Preserve the bytes we are about to override
      BackupData = ArgPtr[-1];
      for (i = 0; i < Padding; i++)
        ArgPtr[i - 1] = ArgPtr[i];
      ArgPtr[i - 1] = 0;
    }

    //
    // Note that we are not able to distinguish which part of the interval
    // [NewStackPointer, FramePtr] consists of stacked function arguments for
    // this call, and which part simply consists of locals in the caller's
    // stack frame. All we know is that there is an 8 byte gap at the top that
    // we can ignore.
    //
    VmPtr->Gpr[7] = EbcLLCALLEXNative (FuncAddr,
        NewStackPointer - (Padding ? 4 : 0), FramePtr - 8);

    // Restore the stack data to its original content
    if (Padding) {
      for (i = Padding - 1; i >= 0 ; i--)
        ArgPtr[i] = ArgPtr[i - 1];
      // Restore overridden bytes
      ArgPtr[-1] = BackupData;
    }

    //
    // Advance the IP.
    //
    VmPtr->Ip += Size;
  }
}
