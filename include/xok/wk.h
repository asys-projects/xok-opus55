/*
 * Xok wakeup predicates.
 *
 * Xok has no blocking interfaces.  An application that wants to sleep
 * until some condition holds downloads a wakeup predicate: a boolean
 * expression in sum-of-products form, where every term compares a
 * memory word (or the system clock) with a constant or another memory
 * word.  The kernel translates every virtual address to a physical
 * one at installation time (pinning the pages), and evaluates the
 * predicate whenever it considers scheduling the sleeping environment:
 * the environment is not run until the predicate holds.
 *
 * The language has no loops and a bounded number of terms, so its
 * evaluation is trivially safe and bounded.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_WK_H
#define XOK_WK_H

#include <stdint.h>

/* Operand kinds. */
#define WK_CONST 0		/* Immediate value. */
#define WK_MEM32 1		/* 32-bit word at a user virtual address. */
#define WK_TIME  2		/* Milliseconds since boot (low 32 bits). */
#define WK_SREG32 3		/* Word in a software region: value is
				   (region << 16) | offset; the capability
				   index used for the read check is wk_cap. */

/* Operators. All comparisons are unsigned. */
#define WK_EQ  0
#define WK_NE  1
#define WK_LT  2
#define WK_LE  3
#define WK_GT  4
#define WK_GE  5
#define WK_AND 6		/* (lhs & rhs) != 0 */
#define WK_OR  7		/* Separator: ends a product (operands ignored). */

struct wk_term
{
  uint8_t wk_op;
  uint8_t wk_lkind;
  uint8_t wk_rkind;
  uint8_t wk_cap;
  uint32_t wk_lval;
  uint32_t wk_rval;
};

#define WK_MAXTERMS 32

#endif
