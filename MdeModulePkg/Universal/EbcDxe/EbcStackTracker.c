/** @file
  This module contains dummy function calls, for platforms that do not
  require stack tracking.

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
UpdateStackTracker(
  IN VM_CONTEXT *VmPtr,
  IN INTN       NaturalUnits,
  IN INTN       ConstUnits
  )
{
  return EFI_SUCCESS;
}

/**
  Update the stack tracker by computing the R0 delta.

  @param VmPtr         The pointer to current VM context.
  @param NewR0         The new R0 value.

  @retval EFI_OUT_OF_RESOURCES  Not enough memory to grow the stack tracker.
  @retval EFI_UNSUPPORTED       The stack tracker is being underflown due to
                                unbalanced stack operations.
  @retval EFI_SUCCESS           The stack tracker was updated successfully.

**/
EFI_STATUS
UpdateStackTrackerFromDelta (
  IN VM_CONTEXT *VmPtr,
  IN UINTN      NewR0
  )
{
  return EFI_SUCCESS;
}
