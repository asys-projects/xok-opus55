/*
 * Small library routines missing from NUX's libec.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "kern.h"

int
strcmp (const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return (unsigned char) *a - (unsigned char) *b;
}
