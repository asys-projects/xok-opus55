/*
 * Xok capabilities.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"

/* The root capability: empty name, all permissions. */
const struct cap cap_root = {
  .c_valid = 1,
  .c_perm = CAP_ALL,
  .c_len = 0,
};

struct cap *
env_cap (struct env *e, unsigned k)
{
  if (k >= ENV_NCAPS)
    return NULL;
  if (!e->info->e_caps[k].c_valid)
    return NULL;
  return (struct cap *) &e->info->e_caps[k];
}

/*
 * Check whether the capability C grants the operations PERM on a
 * resource guarded by GUARD: C's name must be a prefix of the guard's
 * name (C dominates the guard's name), and both C and the guard must
 * allow PERM.  An invalid guard can be accessed only with the root
 * capability.
 */
int
cap_grants (const struct cap *c, const struct cap *guard, unsigned perm)
{
  struct cap g;

  memcpy (&g, guard, sizeof (g));
  if ((c->c_perm & perm) != perm)
    return 0;
  if (!g.c_valid)
    return c->c_len == 0;
  if ((g.c_perm & perm) != perm && c->c_len != 0)
    return 0;
  g.c_perm = 0;
  return cap_dominates (c, &g);
}

/*
 * Check that capability K of E grants PERM on GUARD.
 */
int
cap_check (struct env *e, unsigned k, const struct cap *guard, unsigned perm)
{
  struct cap *c = env_cap (e, k);

  if (c == NULL)
    return -E_CAP_INVALID;
  return cap_grants (c, guard, perm) ? 0 : -E_CAP_INSUFF;
}
