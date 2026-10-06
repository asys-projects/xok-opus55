/*
 * Xok hierarchically-named capabilities.
 *
 * As in Xok, capabilities are names, closer to a generalisation of
 * UNIX uids/gids than to traditional unforgeable tickets.  A
 * capability A dominates B if A's name is a prefix of B's name and
 * A's permissions are a superset of B's.  The empty name (length 0)
 * dominates every capability: it is the root capability.
 *
 * Every environment owns a small list of capabilities (kept by the
 * kernel, readable by everyone in the exported environment table).
 * Every system call that touches a protected resource takes the index
 * of the capability the caller wants to use: credentials are always
 * explicit.  Resources carry guard capabilities (ACLs): a request is
 * granted if the presented capability dominates the guard.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_CAP_H
#define XOK_CAP_H

#include <stdint.h>

#define CAP_NAMELEN 12

/* Permission bits. */
#define CAP_R     0x01		/* Read access. */
#define CAP_W     0x02		/* Write / modify access. */
#define CAP_X     0x04		/* Execute / control access. */
#define CAP_GRANT 0x08		/* May derive and grant to others. */
#define CAP_ALL   0x0f

struct cap
{
  uint8_t c_valid;
  uint8_t c_perm;
  uint8_t c_len;		/* Number of significant name bytes. */
  uint8_t c_pad;
  uint8_t c_name[CAP_NAMELEN];
};

/* Number of capability slots per environment. */
#define ENV_NCAPS 16

/* Conventional slots used by ExOS. */
#define CAP_ROOT  0		/* The env's most powerful capability. */
#define CAP_USER  1		/* The env's UNIX user credential. */

/* ExOS convention for UNIX credentials: name = { 'U', uid_hi, uid_lo } */
#define CAP_UID_TAG 'U'

static inline int
cap_dominates (const struct cap *a, const struct cap *b)
{
  if (!a->c_valid || !b->c_valid)
    return 0;
  if (a->c_len > b->c_len || a->c_len > CAP_NAMELEN)
    return 0;
  for (unsigned i = 0; i < a->c_len; i++)
    if (a->c_name[i] != b->c_name[i])
      return 0;
  return (a->c_perm & b->c_perm) == b->c_perm;
}

#endif
