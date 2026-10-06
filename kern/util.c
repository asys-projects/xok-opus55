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

/*
 * Kernel heap: a checked front end to NUX's kmem allocator.  Every
 * allocation is recorded, so that frees with the wrong size or of
 * unknown pointers are caught immediately.
 */
#define KM_TRACK 8192
static struct
{
  vaddr_t va;
  size_t size;
} kmtab[KM_TRACK];

void *
kmalloc (size_t size)
{
  vaddr_t va = kmem_alloc (0, size);

  if (va == 0 || va == VADDR_INVALID)
    return NULL;
  for (unsigned i = 0; i < KM_TRACK; i++)
    if (kmtab[i].va == 0)
      {
	kmtab[i].va = va;
	kmtab[i].size = size;
	return (void *) va;
      }
  return (void *) va;
}

void
kfree (void *p, size_t size)
{
  vaddr_t va = (vaddr_t) p;

  for (unsigned i = 0; i < KM_TRACK; i++)
    if (kmtab[i].va == va)
      {
	if (kmtab[i].size != size)
	  kpanic ("kfree(%p): size %u, allocated %u", p, (unsigned) size,
		  (unsigned) kmtab[i].size);
	kmtab[i].va = 0;
	kmem_free (0, va, size);
	return;
      }
  kpanic ("kfree(%p, %u): not allocated", p, (unsigned) size);
}
