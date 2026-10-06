/*
 * Xok system call error codes.  System calls return a non-negative
 * value on success and the negated error code on failure.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_ERROR_H
#define XOK_ERROR_H

#define E_UNSPEC     1		/* Unspecified error. */
#define E_BAD_ENV    2		/* Environment does not exist. */
#define E_INVAL      3		/* Invalid parameter. */
#define E_NO_MEM     4		/* Out of memory. */
#define E_NO_FREE_ENV 5		/* No free environment slot. */
#define E_CAP_INVALID 6		/* Capability index out of range or invalid. */
#define E_CAP_INSUFF 7		/* Capability does not dominate the guard. */
#define E_FAULT      8		/* Bad user address. */
#define E_NOT_FOUND  9		/* Object not found. */
#define E_EXISTS     10		/* Object already exists. */
#define E_BUSY       11		/* Resource busy. */
#define E_AGAIN      12		/* Try again (non-blocking operation). */
#define E_NO_QUANTUM 13		/* Quantum slot not free / not allocated. */
#define E_NOSYS      14		/* Unknown system call. */
#define E_RANGE      15		/* Out of range. */
#define E_IPC_BLOCKED 16	/* Target not accepting IPC. */
#define E_FULL       17		/* Ring / queue full. */
#define E_NO_DEV     18		/* No such device. */
#define E_IO         19		/* I/O error. */
#define E_UDF        20		/* UDF evaluation failed (illegal op). */
#define E_BOGUS_UPDATE 21	/* UDF verification of a modification failed. */
#define E_NOT_FREE   22		/* Requested block/page not free. */
#define E_NOT_INCORE 23		/* Block not in buffer cache registry. */
#define E_TAINTED    24		/* Block tainted: write would break ordering. */
#define E_ACCESS     25		/* Access denied by acl-uf. */
#define E_CONFLICT   26		/* Filter overlaps an existing one. */
#define E_PINNED     27		/* Page pinned by kernel or device. */
#define E_MAXERROR   28

#endif
