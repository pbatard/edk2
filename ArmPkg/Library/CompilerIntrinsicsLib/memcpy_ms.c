//------------------------------------------------------------------------------
//
// Copyright (c) 2017, Pete Batard. All rights reserved.<BR>
//
// This program and the accompanying materials are licensed and made
// available under the terms and conditions of the BSD License which
// accompanies this distribution.  The full text of the license may be
// found at http://opensource.org/licenses/bsd-license.php
//
// THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
// WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR
// IMPLIED.
//
//------------------------------------------------------------------------------

#include <stddef.h>        // For size_t

void* memcpy(void *, const void *, size_t);
#pragma intrinsic(memcpy)
#pragma function(memcpy)
void* memcpy(void *dest, const void *src, size_t n)
{
  unsigned char *d = dest;
  unsigned char const *s = src;

  while (n--)
    *d++ = *s++;

  return dest;
}
