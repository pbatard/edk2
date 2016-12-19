/** @file
  This module contains the stack tracking routines used for parameter
  processing and alignment on ARM.

Copyright (c) 2016, Pete Batard. All rights reserved.<BR>

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
// Amount of space to be used by the stack argument tracker
// In most cases, less than 2 bits should be used for every 32 bits of
// stack data and we can grow our buffer if needed, so start at 1/64th
//
#define STACK_TRACKER_SIZE      (STACK_POOL_SIZE / 64)
#define STACK_TRACKER_SIZE_MAX  (STACK_POOL_SIZE / 8)

//
// Stack tracking data structure, used to compute parameter alignment.
//
typedef struct {
  UINT8     *Data;              ///< stack tracker data buffer
  INTN      Size;               ///< size of the stack tracker data buffer, in bytes
  INTN      Index;              ///< current stack tracker index, in 1/4th bytes
  INTN      OrgIndex;           ///< copy of the index, used on stack buffer switch
  UINTN     OrgStackPointer;    ///< copy of the stack pointer, used on stack buffer switch
} EBC_STACK_TRACKER;

/**
  Allocate a stack tracker structure and initialize it.

  @param VmPtr         The pointer to current VM context.

  @retval EFI_OUT_OF_RESOURCES  Not enough memory to set the stack tracker.
  @retval EFI_SUCCESS           The stack tracker was initialized successfully.

**/
EFI_STATUS
AllocateStackTracker (
  IN VM_CONTEXT *VmPtr
  )
{
  EBC_STACK_TRACKER *StackTracker;

  StackTracker = AllocateZeroPool(sizeof(EBC_STACK_TRACKER));
  if (StackTracker == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  StackTracker->Size = STACK_TRACKER_SIZE;
  StackTracker->Data = AllocatePool(StackTracker->Size);
  if (StackTracker->Data == NULL) {
    FreePool(StackTracker);
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Add tracking for EfiMain() call just in case
  //
  StackTracker->Data[0] = 0x05; // 2 x UINT64, 2 x UINTN
  StackTracker->Index = 4;

  VmPtr->StackTracker = (VOID*) StackTracker;

  return EFI_SUCCESS;
}

/**
  Free a stack tracker structure.

  @param VmPtr         The pointer to current VM context.
**/
VOID
FreeStackTracker (
  IN VM_CONTEXT *VmPtr
  )
{
  EBC_STACK_TRACKER *StackTracker;

  StackTracker = (EBC_STACK_TRACKER*) VmPtr->StackTracker;

  FreePool(StackTracker->Data);
  FreePool(StackTracker);
  VmPtr->StackTracker = NULL;
}

/**
  Return the argument layout for the current function call, according
  to the current stack tracking data.
  The first argument is at bit 0, with the 16th parameter at bit 15,
  with a bit set to 1 if an argument is 64 bit, 0 otherwise.

  @param VmPtr         The pointer to current VM context.

  @return              A 16 bit argument layout.
**/
UINT16
GetArgLayout (
  IN VM_CONTEXT *VmPtr
  )
{
  EBC_STACK_TRACKER *StackTracker;
  UINT16 ArgLayout, Mask;
  INTN Index;

  StackTracker = (EBC_STACK_TRACKER*) VmPtr->StackTracker;

  ASSERT (StackTracker->Data != NULL);
  ASSERT (StackTracker->Index >= 0);

  // One major issue we have on Arm is that, if a mix of natural and
  // 64-bit arguments are stacked as parameters for a native call, we risk
  // running afoul of the AAPCS (the ARM calling convention) which mandates
  // that the first 2 to 4 arguments are passed as register, and that any
  // 64-bit argument *MUST* start either on an even register or at an 8-byte
  // aligned address.
  //
  // So if, for instance, we have a natural parameter (32-bit) in Arg0 and
  // a 64-bit parameter in Arg1, then, after we copy the data onto r0, we
  // must skip r1 so that Arg1 starts at register r2. Similarly, we may have
  // to skip words on stack for 64-bit parameter alignment.
  //
  // This is where our stack tracker comes into play, as it tracks EBC stack
  // manipulations and allows us to find whether each of the (potential)
  // arguments being passed to a native CALLEX is 64-bit or natural. As
  // a reminder, and as per the specs (UEFI 2.6, section 21.9.3), 64-bit
  // or natural are the ONLY types of arguments that are allowed when
  // performing EBC function calls, inluding native ones (in which case any
  // argument of 32 bit or less must be stacked as a natural).
  //
  // Through the stack tracker, we can retreive the last 16 argument types
  // (decoded from the 2 bit sequences), which we convert to a 16-bit value
  // that gives us the argument layout.
  //
  ArgLayout = 0;
  Mask = 1;
  for (Index = StackTracker->Index - 1; Index >= StackTracker->Index - 16; Index--) {
    if ((Index / 4) < 0) {
      break; // Don't underflow the tracker.
    }
    //
    // Actual function parameters are stored as 2 bit sequences in our
    // tracker, with 00b indicating a 64-bit parameter and 01b a natural.
    // When converting this to ArgLayout, we set the relevant arg position
    // bit to 1, for a 64-bit arg, or 0, for a natural.
    // Also, since there's little point in avoiding to process 4-bit
    // sequences (for stack values that aren't natural or 64-bit, and thus
    // can't be used as arguments) we also process them as 2-bit.
    //
    if ((StackTracker->Data[Index / 4] & (1 << (2 * (3 - (Index % 4))))) == 0) {
      ArgLayout |= Mask;
    }
    Mask <<= 1;
  }
  return ArgLayout;
}

/**
  Returns the current stack tracker entry.

  @param StackTracker   A pointer to the stack tracker struct.

  @return  The decoded stack tracker index [0x00, 0x08].

  The decoding of stack tracker entries operates as follows:
    00b                        -> 0000b
    01b                        -> 1000b
    1xb preceded by yzb        -> 0xyzb
    (e.g. 11b preceded by 10b  -> 0110b)

**/
UINT8
GetStackTrackerEntry (
  IN EBC_STACK_TRACKER *StackTracker
  )
{
  UINT8 Index, PreviousIndex;

  if (StackTracker->Index < 0) {
    //
    // Anything prior to tracking is considered aligned to 64 bits
    //
    return 0x00;
  }

  Index = StackTracker->Data[(StackTracker->Index - 1) / 4];
  Index >>= 6 - (2 * ((StackTracker->Index - 1) % 4));
  Index &= 0x03;

  if (Index == 0x01) {
    Index = 0x08;
  } else if ((Index & 0x02) != 0) {
    PreviousIndex = StackTracker->Data[(StackTracker->Index - 2) / 4];
    PreviousIndex >>= 6 - (2 * ((StackTracker->Index - 2) % 4));
    Index = ((Index << 2) & 0x04) | (PreviousIndex & 0x03);
  }

  return Index;
}

/**
  Add a single new stack tracker entry.

  @param StackTracker   A pointer to the stack tracker struct.
  @param Value          The value to be encoded.

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
  IN EBC_STACK_TRACKER *StackTracker,
  IN UINT8             Value
  )
{
  UINT8 Pass, Data;

  ASSERT (Value <= 0x08);
  if (Value == 0x08) {
    Data = 0x01;
  } else {
    Data = Value & 0x03;
  }

  for (Pass = 0; Pass < 2; Pass++) {
    if ((StackTracker->Index / 4) >= StackTracker->Size) {
      //
      // Grow the stack tracker buffer
      //
      StackTracker->Data = ReallocatePool(StackTracker->Size,
            StackTracker->Size*2, StackTracker->Data);
      if (StackTracker->Data == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }
      StackTracker->Size *= 2;
      ASSERT (StackTracker->Size <= STACK_TRACKER_SIZE_MAX);
    }

    //
    // Clear bits and add our value
    //
    StackTracker->Data[StackTracker->Index / 4] &=
          0xFC << (6 - 2 * (StackTracker->Index % 4));
    StackTracker->Data[StackTracker->Index / 4] |=
          Data << (6 - 2 * (StackTracker->Index % 4));
    StackTracker->Index++;
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

  @param StackTracker   A pointer to the stack tracker struct.
  @param Value          The byte value to be inserted.
  @param Count          The number of times the value should be repeated.

  @retval EFI_OUT_OF_RESOURCES  Not enough memory to grow the stack tracker.
  @retval EFI_SUCCESS           The entries were added successfully.

**/
EFI_STATUS
AddStackTrackerBytes (
  IN EBC_STACK_TRACKER *StackTracker,
  IN UINT8             Value,
  IN INTN              Count
  )
{
  UINTN ByteNum, NewSize;
  INTN UpdatedIndex;

  //
  // Byte alignement should have been sorted prior to this call
  //
  ASSERT (StackTracker->Index % 4 == 0);

  UpdatedIndex = StackTracker->Index + 4 * Count;
  if (UpdatedIndex >= StackTracker->Size) {
    //
    // Grow the stack tracker buffer
    //
    for (NewSize = StackTracker->Size * 2; NewSize <= UpdatedIndex;
      NewSize *= 2);
    ASSERT (NewSize <= STACK_TRACKER_SIZE_MAX);
    StackTracker->Data = ReallocatePool(StackTracker->Size, NewSize,
          StackTracker->Data);
    if (StackTracker->Data == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    StackTracker->Size = NewSize;
  }
  for (ByteNum = 0; ByteNum < Count; ByteNum++) {
    StackTracker->Data[(StackTracker->Index / 4) + ByteNum] = Value;
  }
  StackTracker->Index = UpdatedIndex;
  return EFI_SUCCESS;
}

/**
  Delete a single stack tracker entry.

  @param StackTracker   A pointer to the stack tracker struct.

  @retval EFI_UNSUPPORTED       The stack tracker is being underflown due
                                to unbalanced stack operations.
  @retval EFI_SUCCESS           The index was added successfully.
**/
EFI_STATUS
DelStackTrackerEntry (
  IN EBC_STACK_TRACKER *StackTracker
  )
{
  UINT8 Index;

  ASSERT (StackTracker->Index > 0);

  //
  // Don't care about clearing the used bits, just update the index
  //
  StackTracker->Index--;
  Index = StackTracker->Data[StackTracker->Index / 4];
  Index >>= 6 - (2 * (StackTracker->Index % 4));

  if ((Index & 0x02) != 0) {
    //
    // 4-bit sequence
    //
    StackTracker->Index--;
  }
  if (StackTracker->Index < 0) {
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
  EBC_STACK_TRACKER *StackTracker;

  StackTracker = (EBC_STACK_TRACKER*) VmPtr->StackTracker;

  //
  // Mismatched signage should already have been filtered out.
  //
  ASSERT ( ((NaturalUnits >= 0) && (ConstUnits >= 0)) ||
           ((NaturalUnits <= 0) && (ConstUnits <= 0)) );

  while (NaturalUnits < 0) {
    //
    // Add natural indexes (1000b) into our stack tracker
    // Note, we don't care if the previous entry was aligned as a non 64-bit
    // aligned entry cannot be used as a call parameter in valid EBC code.
    // This also adds the effect of re-aligning our data to 64 bytes, which
    // will help speed up tracking of local stack variables (arrays, etc.)
    //
    if ((StackTracker->Index % 4 == 0) && (NaturalUnits <= -4)) {
      //
      // Optimize adding a large number of naturals, such as ones reserved
      // for local function variables/arrays. 0x55 means 4 naturals.
      //
      Status = AddStackTrackerBytes (StackTracker, 0x55, -NaturalUnits / 4);
      NaturalUnits -= 4 * (NaturalUnits / 4); // Beware of negative modulos
    } else {
      Status = AddStackTrackerEntry (StackTracker, 0x08);
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
    LastEntry = GetStackTrackerEntry (StackTracker);
    if ((LastEntry != 0x00) && (LastEntry != 0x08)) {
      DelStackTrackerEntry (StackTracker);
      ConstUnits -= LastEntry;
    }

    //
    // Now, add as many 64-bit indexes as we can (0000b values)
    //
    while (ConstUnits <= -8) {
      if ((ConstUnits <= -32) && (StackTracker->Index % 4 == 0)) {
        //
        // Optimize adding a large number of consts, such as ones reserved
        // for local function variables/arrays. 0x00 means 4 64-bit consts.
        //
        Status = AddStackTrackerBytes (StackTracker, 0x00, -ConstUnits / 32);
        ConstUnits -= 32 * (ConstUnits / 32); // Beware of negative modulos
      } else {
        Status = AddStackTrackerEntry (StackTracker, 0x00);
        ConstUnits += 8;
      }
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }

    //
    // Add any remaining non 64-bit aligned bytes
    //
    if ((ConstUnits % 8) != 0) {
      Status = AddStackTrackerEntry (StackTracker, (-ConstUnits) % 8);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  while ((NaturalUnits > 0) || (ConstUnits > 0)) {
    ASSERT(StackTracker->Index > 0);

    //
    // Delete natural/constant items from the stack tracker
    //
    if (StackTracker->Index % 4 == 0) {
      //
      // Speedup deletion for blocks of naturals/consts
      //
      while ((ConstUnits >= 32) &&
            (StackTracker->Data[(StackTracker->Index-1) % 4] == 0x00)) {
        //
        // Start with consts since that's what we add last
        //
        StackTracker->Index -= 4;
        ConstUnits -= 32;
      }
      while ((NaturalUnits >= 4) &&
            (StackTracker->Data[(StackTracker->Index-1) % 4] == 0x55)) {
        StackTracker->Index -= 4;
        NaturalUnits -= 4;
      }
    }

    if ((NaturalUnits == 0) && (ConstUnits == 0)) {
      //
      // May already have depleted our values through block processing above
      //
      break;
    }

    LastEntry = GetStackTrackerEntry (StackTracker);
    Status = DelStackTrackerEntry (StackTracker);
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
      Status = AddStackTrackerEntry (StackTracker, LastEntry - ConstUnits);
      ConstUnits = 0;
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  ASSERT(StackTracker->Index >= 0);
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
  EBC_STACK_TRACKER *StackTracker;

  StackTracker = (EBC_STACK_TRACKER*) VmPtr->StackTracker;

  //
  // Check if the updated R0 is still in our original stack buffer.
  //
  if ((StackTracker->OrgIndex == 0) && ((UpdatedR0 < (UINTN) VmPtr->StackPool) ||
      (UpdatedR0 >= (UINTN) VmPtr->StackPool + STACK_POOL_SIZE))) {
    //
    // We are swicthing from the default stack buffer to a newly allocated
    // one. Keep track of our current stack tracker index in case we come
    // back to the original stack with unbalanced stack ops (e.g.
    // SP <- New stack; Enqueue data without dequeuing; SP <- Old SP)
    // Note that, since we are not moinitoring memory allocations, we can
    // only ever detect swicthing in and out of the default stack buffer.
    //
    StackTracker->OrgIndex = StackTracker->Index;
    StackTracker->OrgStackPointer = (UINTN) VmPtr->Gpr[0];

    //
    // Do not track switching. Just realign the index.
    //
    StackTracker->Index = 4 * ((StackTracker->Index + 3) / 4);
    return EFI_SUCCESS;
  }

  if ((StackTracker->OrgIndex != 0) && ((UpdatedR0 >= (UINTN) VmPtr->StackPool) ||
      (UpdatedR0 < (UINTN) VmPtr->StackPool + STACK_POOL_SIZE))) {
    //
    // Coming back from a newly allocated stack to the original one
    // As we don't expect stack ops to have been properly balanced we just
    // restore the old stack tracker index.
    //
    StackTracker->Index = StackTracker->OrgIndex;
    StackTracker->OrgIndex = 0;
    //
    // There's also no guarantee that the new R0 is being restored to the
    // value it held when stwiching stacks, so we use the value R0 held
    // at the time the switch was performed, to compute the delta.
    StackPointerDelta = UpdatedR0 - StackTracker->OrgStackPointer;
  } else {
    StackPointerDelta = UpdatedR0 - (UINTN) VmPtr->Gpr[0];
  }

  return UpdateStackTracker(VmPtr, 0, StackPointerDelta);
}
