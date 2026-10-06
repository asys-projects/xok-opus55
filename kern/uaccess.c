/*
 * Access to the current environment's memory.
 *
 * The current environment's address space is always loaded when the
 * kernel handles one of its system calls.  NUX's user access routines
 * catch faults: Xok never resolves faults on behalf of applications,
 * so a fault simply makes the access fail (-E_FAULT); the library OS
 * is responsible for making its buffers present and writable.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "kern.h"

static bool
user_range_ok (uaddr_t a, size_t len)
{
  if (len == 0)
    return true;
  if (a + len < a)
    return false;
  return a + len <= UTOP;
}

int
copyin (void *dst, uaddr_t src, size_t len)
{
  if (len == 0)
    return 0;
  if (!user_range_ok (src, len))
    return -E_FAULT;
  return cpu_useraccess_copyfrom (dst, src, len, NULL) ? 0 : -E_FAULT;
}

int
copyout (uaddr_t dst, const void *src, size_t len)
{
  if (len == 0)
    return 0;
  if (!user_range_ok (dst, len) || dst >= UXOK_BASE)
    return -E_FAULT;
  return cpu_useraccess_copyto (dst, (void *) src, len, NULL) ? 0 : -E_FAULT;
}

int
copyin_str (char *dst, uaddr_t src, size_t max)
{
  for (size_t i = 0; i < max; i++)
    {
      if (copyin (dst + i, src + i, 1) < 0)
	return -E_FAULT;
      if (dst[i] == '\0')
	return i;
    }
  return -E_RANGE;
}
