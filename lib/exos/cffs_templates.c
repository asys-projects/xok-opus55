/*
 * C-FFS templates and their untrusted deterministic functions.
 *
 * Shared by the library file system and the host tool mkxnfs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>
#include <exos/udfasm.h>
#include <exos/cffs.h>

const char *const cffs_type_names[CFFS_NTYPES] = {
  [CFFS_T_DATA] = "cffs-data",
  [CFFS_T_IND] = "cffs-ind",
  [CFFS_T_DIND] = "cffs-dind",
  [CFFS_T_DIR] = "cffs-dir",
  [CFFS_T_DIRIND] = "cffs-dirind",
  [CFFS_T_SUPER] = "cffs-super",
};

/* Registers. */
#define R0  0			/* Always zero. */
#define RONE 1			/* Constant 1. */
#define RB  2			/* Inode base offset. */
#define RT  3			/* Scratch. */
#define RM  4			/* Mode. */
#define RDIR 5			/* Is a directory. */
#define RI  6			/* Loop index. */
#define RE  7			/* Loop end. */
#define RP  8			/* Pointer. */
#define RTY 9			/* Type to emit. */
#define RX  10			/* Entry offset. */
#define RXE 11			/* Entry loop end. */
#define RK  12			/* Scratch. */
#define RA  13			/* acl: op. */
#define RC  14			/* acl: child. */
#define RR  15			/* acl: result / scratch. */

#define S_IFMT  0xf000
#define S_IFDIR 0x4000

/*
 * Emit the blocks referenced by the inode at offset RB.
 */
static void
emit_inode (struct udf_asm *a, const unsigned *ids)
{
  int ldir = ua_newlabel (a), ltype = ua_newlabel (a);
  int lloop = ua_newlabel (a), lskip = ua_newlabel (a);
  int lnoind = ua_newlabel (a), lnodind = ua_newlabel (a);
  int lindt = ua_newlabel (a), lindd = ua_newlabel (a);

  /* RDIR = (mode & S_IFMT) == S_IFDIR */
  ua_op (a, UDF_LDH, RM, RB, 0, CFFS_I_MODE);
  ua_op (a, UDF_ANDI, RM, RM, 0, S_IFMT);
  ua_op (a, UDF_LI, RT, 0, 0, S_IFDIR);
  ua_op (a, UDF_LI, RDIR, 0, 0, 0);
  ua_br (a, UDF_BNE, RM, RT, ldir);
  ua_op (a, UDF_LI, RDIR, 0, 0, 1);
  ua_label (a, ldir);

  /* Direct blocks: data, or directory blocks. */
  ua_op (a, UDF_LI, RTY, 0, 0, ids[CFFS_T_DATA]);
  ua_br (a, UDF_BEQ, RDIR, R0, ltype);
  ua_op (a, UDF_LI, RTY, 0, 0, ids[CFFS_T_DIR]);
  ua_label (a, ltype);
  ua_op (a, UDF_LI, RI, 0, 0, 0);
  ua_op (a, UDF_LI, RE, 0, 0, 4 * CFFS_NDIRECT);
  ua_label (a, lloop);
  ua_op (a, UDF_ADD, RK, RB, RI, 0);
  ua_op (a, UDF_LDW, RP, RK, 0, CFFS_I_DIRECT);
  ua_br (a, UDF_BEQ, RP, R0, lskip);
  ua_op (a, UDF_EMIT, RP, RONE, RTY, 0);
  ua_label (a, lskip);
  ua_op (a, UDF_ADDI, RI, RI, 0, 4);
  ua_br (a, UDF_BLTU, RI, RE, lloop);

  /* Indirect block. */
  ua_op (a, UDF_LDW, RP, RB, 0, CFFS_I_INDIRECT);
  ua_br (a, UDF_BEQ, RP, R0, lnoind);
  ua_op (a, UDF_LI, RTY, 0, 0, ids[CFFS_T_IND]);
  ua_br (a, UDF_BEQ, RDIR, R0, lindt);
  ua_op (a, UDF_LI, RTY, 0, 0, ids[CFFS_T_DIRIND]);
  ua_label (a, lindt);
  ua_op (a, UDF_EMIT, RP, RONE, RTY, 0);
  ua_label (a, lnoind);

  /* Double indirect block (files only). */
  ua_op (a, UDF_LDW, RP, RB, 0, CFFS_I_DINDIRECT);
  ua_br (a, UDF_BEQ, RP, R0, lnodind);
  ua_br (a, UDF_BNE, RDIR, R0, lnodind);
  ua_op (a, UDF_LI, RTY, 0, 0, ids[CFFS_T_DIND]);
  ua_op (a, UDF_EMIT, RP, RONE, RTY, 0);
  ua_label (a, lnodind);
  (void) lindd;
}

static int
owns_dir (struct xn_template *t, const unsigned *ids)
{
  struct udf_asm a;
  int lent, lnext;

  ua_init (&a, t->t_owns, XN_OWNS_MAX);
  lent = ua_newlabel (&a);
  lnext = ua_newlabel (&a);
  ua_op (&a, UDF_LI, RONE, 0, 0, 1);
  ua_op (&a, UDF_LI, RX, 0, 0, 0);
  ua_op (&a, UDF_LI, RXE, 0, 0, XN_BLKSIZE);
  ua_label (&a, lent);
  ua_op (&a, UDF_LDB, RT, RX, 0, 0);
  ua_br (&a, UDF_BEQ, RT, R0, lnext);
  ua_op (&a, UDF_ADDI, RB, RX, 0, CFFS_DIRENT_INODE);
  emit_inode (&a, ids);
  ua_label (&a, lnext);
  ua_op (&a, UDF_ADDI, RX, RX, 0, CFFS_DIRENT_SIZE);
  ua_br (&a, UDF_BLTU, RX, RXE, lent);
  ua_op (&a, UDF_END, 0, 0, 0, 0);
  return ua_finish (&a);
}

static int
owns_super (struct xn_template *t, const unsigned *ids)
{
  struct udf_asm a;

  ua_init (&a, t->t_owns, XN_OWNS_MAX);
  ua_op (&a, UDF_LI, RONE, 0, 0, 1);
  ua_op (&a, UDF_LI, RB, 0, 0, CFFS_DIRENT_INODE);
  emit_inode (&a, ids);
  ua_op (&a, UDF_END, 0, 0, 0, 0);
  return ua_finish (&a);
}

/* An array of CFFS_NPTRS pointers to blocks of type CHILD. */
static int
owns_ptrs (struct xn_template *t, unsigned child)
{
  struct udf_asm a;
  int lloop, lskip;

  ua_init (&a, t->t_owns, XN_OWNS_MAX);
  lloop = ua_newlabel (&a);
  lskip = ua_newlabel (&a);
  ua_op (&a, UDF_LI, RONE, 0, 0, 1);
  ua_op (&a, UDF_LI, RTY, 0, 0, child);
  ua_op (&a, UDF_LI, RI, 0, 0, 0);
  ua_op (&a, UDF_LI, RE, 0, 0, XN_BLKSIZE);
  ua_label (&a, lloop);
  ua_op (&a, UDF_LDW, RP, RI, 0, 0);
  ua_br (&a, UDF_BEQ, RP, R0, lskip);
  ua_op (&a, UDF_EMIT, RP, RONE, RTY, 0);
  ua_label (&a, lskip);
  ua_op (&a, UDF_ADDI, RI, RI, 0, 4);
  ua_br (&a, UDF_BLTU, RI, RE, lloop);
  ua_op (&a, UDF_END, 0, 0, 0, 0);
  return ua_finish (&a);
}

/*
 * acl helper: RR = 1 if the credential may perform access NEED (4 =
 * read, 2 = write) on the inode at RB, following UNIX owner/other
 * permission bits.  The root credential (empty name) may do anything.
 * Credentials of users are named { 'U', uid >> 8, uid & 0xff }.
 */
static void
acl_perm (struct udf_asm *a, int need)
{
  int lother = ua_newlabel (a), lcheck = ua_newlabel (a);
  int lyes = ua_newlabel (a), ldone = ua_newlabel (a);

  ua_op (a, UDF_CRED, RT, 0, 0, -1);
  ua_br (a, UDF_BEQ, RT, R0, lyes);	/* Root. */
  ua_op (a, UDF_LI, RK, 0, 0, 3);
  ua_br (a, UDF_BNE, RT, RK, lother);
  ua_op (a, UDF_CRED, RT, 0, 0, 0);
  ua_op (a, UDF_LI, RK, 0, 0, 'U');
  ua_br (a, UDF_BNE, RT, RK, lother);
  ua_op (a, UDF_LDW, RP, RB, 0, CFFS_I_UID);
  ua_op (a, UDF_LI, RK, 0, 0, 8);
  ua_op (a, UDF_SHR, RK, RP, RK, 0);
  ua_op (a, UDF_ANDI, RK, RK, 0, 0xff);
  ua_op (a, UDF_CRED, RT, 0, 0, 1);
  ua_br (a, UDF_BNE, RT, RK, lother);
  ua_op (a, UDF_ANDI, RK, RP, 0, 0xff);
  ua_op (a, UDF_CRED, RT, 0, 0, 2);
  ua_br (a, UDF_BNE, RT, RK, lother);
  /* Owner: bits 8..6. */
  ua_op (a, UDF_LDH, RM, RB, 0, CFFS_I_MODE);
  ua_op (a, UDF_LI, RK, 0, 0, 6);
  ua_op (a, UDF_SHR, RM, RM, RK, 0);
  ua_br (a, UDF_JMP, 0, 0, lcheck);
  ua_label (a, lother);
  ua_op (a, UDF_LDH, RM, RB, 0, CFFS_I_MODE);
  ua_label (a, lcheck);
  ua_op (a, UDF_ANDI, RM, RM, 0, need);
  ua_op (a, UDF_LI, RR, 0, 0, 0);
  ua_br (a, UDF_BEQ, RM, R0, ldone);
  ua_label (a, lyes);
  ua_op (a, UDF_LI, RR, 0, 0, 1);
  ua_label (a, ldone);
}

/*
 * Find, in a directory block, the used entry whose inode references
 * block RC: on success jump to FOUND with RB = inode base; otherwise
 * fall through.
 */
static void
find_child (struct udf_asm *a, int found)
{
  int lent = ua_newlabel (a), lnext = ua_newlabel (a);
  int lptr = ua_newlabel (a);

  ua_op (a, UDF_LI, RX, 0, 0, 0);
  ua_op (a, UDF_LI, RXE, 0, 0, XN_BLKSIZE);
  ua_label (a, lent);
  ua_op (a, UDF_LDB, RT, RX, 0, 0);
  ua_br (a, UDF_BEQ, RT, R0, lnext);
  ua_op (a, UDF_ADDI, RB, RX, 0, CFFS_DIRENT_INODE);
  /* Pointers are contiguous: direct[12], indirect, dindirect. */
  ua_op (a, UDF_LI, RI, 0, 0, 0);
  ua_op (a, UDF_LI, RE, 0, 0, 4 * (CFFS_NDIRECT + 2));
  ua_label (a, lptr);
  ua_op (a, UDF_ADD, RK, RB, RI, 0);
  ua_op (a, UDF_LDW, RP, RK, 0, CFFS_I_DIRECT);
  ua_br (a, UDF_BEQ, RP, RC, found);
  ua_op (a, UDF_ADDI, RI, RI, 0, 4);
  ua_br (a, UDF_BLTU, RI, RE, lptr);
  ua_label (a, lnext);
  ua_op (a, UDF_ADDI, RX, RX, 0, CFFS_DIRENT_SIZE);
  ua_br (a, UDF_BLTU, RX, RXE, lent);
}

/*
 * acl helper: RR = 1 if the credential is the owner of the inode at RB
 * (or root).
 */
static void
acl_owner (struct udf_asm *a)
{
  int lno = ua_newlabel (a), lyes = ua_newlabel (a), ldone = ua_newlabel (a);

  ua_op (a, UDF_CRED, RT, 0, 0, -1);
  ua_br (a, UDF_BEQ, RT, R0, lyes);
  ua_op (a, UDF_LI, RK, 0, 0, 3);
  ua_br (a, UDF_BNE, RT, RK, lno);
  ua_op (a, UDF_CRED, RT, 0, 0, 0);
  ua_op (a, UDF_LI, RK, 0, 0, 'U');
  ua_br (a, UDF_BNE, RT, RK, lno);
  ua_op (a, UDF_LDW, RP, RB, 0, CFFS_I_UID);
  ua_op (a, UDF_LI, RK, 0, 0, 8);
  ua_op (a, UDF_SHR, RK, RP, RK, 0);
  ua_op (a, UDF_ANDI, RK, RK, 0, 0xff);
  ua_op (a, UDF_CRED, RT, 0, 0, 1);
  ua_br (a, UDF_BNE, RT, RK, lno);
  ua_op (a, UDF_ANDI, RK, RP, 0, 0xff);
  ua_op (a, UDF_CRED, RT, 0, 0, 2);
  ua_br (a, UDF_BNE, RT, RK, lno);
  ua_label (a, lyes);
  ua_op (a, UDF_LI, RR, 0, 0, 1);
  ua_br (a, UDF_JMP, 0, 0, ldone);
  ua_label (a, lno);
  ua_op (a, UDF_LI, RR, 0, 0, 0);
  ua_label (a, ldone);
}

/*
 * acl for directory blocks (and, with SUPER set, for the superblock,
 * which embeds the root directory inode at the first entry's inode
 * offset).
 *
 *  READ/WRITE of a child block: permission on the inode pointing to it.
 *  READ/LOCK of the block itself: the superblock is public; otherwise
 *    defer to the parent (directory permission).
 *  Modifications (each must stay within one entry):
 *    - creating (old entry unused), deleting (new entry unused) or
 *      renaming (inode unchanged) need directory write permission:
 *      defer to the parent;
 *    - changing an inode's owner or mode needs ownership;
 *    - other inode changes (size, block pointers, times) need write
 *      permission on the file; freeing blocks can also be done with
 *      directory write permission (unlink).
 */
static int
acl_dir (struct xn_template *t, int super)
{
  struct udf_asm a;
  int lmod, lself, lfound, lread, lallow, ldeny, ldefer, lchk, lchanged;
  int lcmp, lnext, lneedown, lnoperm;

  ua_init (&a, t->t_acl, XN_ACL_MAX);
  lmod = ua_newlabel (&a);
  lself = ua_newlabel (&a);
  lfound = ua_newlabel (&a);
  lread = ua_newlabel (&a);
  lallow = ua_newlabel (&a);
  ldeny = ua_newlabel (&a);
  ldefer = ua_newlabel (&a);
  lchk = ua_newlabel (&a);
  lchanged = ua_newlabel (&a);
  lcmp = ua_newlabel (&a);
  lnext = ua_newlabel (&a);
  lneedown = ua_newlabel (&a);
  lnoperm = ua_newlabel (&a);

  ua_op (&a, UDF_ARG, RA, 0, 0, XN_ARG_OP);
  ua_op (&a, UDF_ARG, RC, 0, 0, XN_ARG_CHILD);
  ua_op (&a, UDF_LI, RT, 0, 0, XN_OP_ALLOC);
  ua_br (&a, UDF_BGEU, RA, RT, lmod);

  /* READ / WRITE. */
  ua_br (&a, UDF_BEQ, RC, R0, lself);
  if (super)
    ua_op (&a, UDF_LI, RB, 0, 0, CFFS_DIRENT_INODE);
  else
    {
      find_child (&a, lfound);
      ua_br (&a, UDF_JMP, 0, 0, ldeny);
    }
  ua_label (&a, lfound);
  ua_op (&a, UDF_LI, RT, 0, 0, XN_OP_READ);
  ua_br (&a, UDF_BEQ, RA, RT, lread);
  acl_perm (&a, 2);
  ua_br (&a, UDF_JMP, 0, 0, lchk);
  ua_label (&a, lread);
  acl_perm (&a, 4);
  ua_label (&a, lchk);
  ua_br (&a, UDF_BEQ, RR, R0, ldeny);
  ua_br (&a, UDF_JMP, 0, 0, lallow);

  ua_label (&a, lself);
  if (super)
    {
      ua_op (&a, UDF_LI, RT, 0, 0, XN_OP_READ);
      ua_br (&a, UDF_BEQ, RA, RT, lallow);
      ua_op (&a, UDF_LI, RB, 0, 0, CFFS_DIRENT_INODE);
      acl_perm (&a, 2);
      ua_br (&a, UDF_BEQ, RR, R0, ldeny);
      ua_br (&a, UDF_JMP, 0, 0, lallow);
    }
  else
    ua_br (&a, UDF_JMP, 0, 0, ldefer);

  /* Modifications. */
  ua_label (&a, lmod);
  ua_op (&a, UDF_LI, RT, 0, 0, XN_OP_LOCK);
  ua_br (&a, UDF_BEQ, RA, RT, lself);
  ua_op (&a, UDF_ARG, RK, 0, 0, XN_ARG_OFF);
  ua_op (&a, UDF_ANDI, RT, RK, 0, CFFS_DIRENT_SIZE - 1);
  ua_op (&a, UDF_ARG, RE, 0, 0, XN_ARG_LEN);
  ua_op (&a, UDF_ADD, RI, RT, RE, 0);
  ua_op (&a, UDF_LI, RP, 0, 0, CFFS_DIRENT_SIZE);
  ua_br (&a, UDF_BLTU, RP, RI, ldeny);	/* Spans two entries. */
  ua_op (&a, UDF_SUB, RX, RK, RT, 0);	/* Entry base. */
  if (super)
    {
      /* Only the root inode, in the first 256 bytes. */
      ua_br (&a, UDF_BNE, RX, R0, ldeny);
      ua_op (&a, UDF_LI, RP, 0, 0, CFFS_DIRENT_INODE);
      ua_br (&a, UDF_BLTU, RT, RP, ldeny);
    }
  else
    {
      ua_op (&a, UDF_LDB, RT, RX, 0, 0);
      ua_br (&a, UDF_BEQ, RT, R0, ldefer);	/* Create. */
      ua_op (&a, UDF_LDB, RT, RX, 1, 0);
      ua_br (&a, UDF_BEQ, RT, R0, ldefer);	/* Delete. */
    }
  /* Has the inode changed? */
  ua_op (&a, UDF_ADDI, RB, RX, 0, CFFS_DIRENT_INODE);
  ua_op (&a, UDF_LI, RI, 0, 0, 0);
  ua_op (&a, UDF_LI, RE, 0, 0, sizeof (struct cffs_inode));
  ua_label (&a, lcmp);
  ua_op (&a, UDF_ADD, RK, RB, RI, 0);
  ua_op (&a, UDF_LDW, RP, RK, 0, 0);
  ua_op (&a, UDF_LDW, RT, RK, 1, 0);
  ua_br (&a, UDF_BNE, RP, RT, lchanged);
  ua_op (&a, UDF_ADDI, RI, RI, 0, 4);
  ua_br (&a, UDF_BLTU, RI, RE, lcmp);
  /* Unchanged inode: rename. */
  ua_br (&a, UDF_JMP, 0, 0, super ? ldeny : ldefer);

  ua_label (&a, lchanged);
  ua_op (&a, UDF_LDW, RP, RB, 0, CFFS_I_UID);
  ua_op (&a, UDF_LDW, RT, RB, 1, CFFS_I_UID);
  ua_br (&a, UDF_BNE, RP, RT, lneedown);
  ua_op (&a, UDF_LDH, RP, RB, 0, CFFS_I_MODE);
  ua_op (&a, UDF_LDH, RT, RB, 1, CFFS_I_MODE);
  ua_br (&a, UDF_BNE, RP, RT, lneedown);
  acl_perm (&a, 2);
  ua_br (&a, UDF_BNE, RR, R0, lallow);
  ua_br (&a, UDF_JMP, 0, 0, lnoperm);
  ua_label (&a, lneedown);
  acl_owner (&a);
  ua_br (&a, UDF_BNE, RR, R0, lallow);
  ua_br (&a, UDF_JMP, 0, 0, ldeny);
  ua_label (&a, lnoperm);
  ua_op (&a, UDF_LI, RT, 0, 0, XN_OP_FREE);
  ua_br (&a, UDF_BNE, RA, RT, ldeny);
  ua_br (&a, UDF_JMP, 0, 0, super ? ldeny : ldefer);

  ua_label (&a, ldefer);
  ua_op (&a, UDF_LI, RR, 0, 0, XN_ACL_DEFER);
  ua_op (&a, UDF_RET, RR, 0, 0, 0);
  ua_label (&a, ldeny);
  ua_op (&a, UDF_LI, RR, 0, 0, XN_ACL_DENY);
  ua_op (&a, UDF_RET, RR, 0, 0, 0);
  ua_label (&a, lallow);
  ua_op (&a, UDF_LI, RR, 0, 0, XN_ACL_ALLOW);
  ua_op (&a, UDF_RET, RR, 0, 0, 0);
  (void) lnext;
  return ua_finish (&a);
}

int
cffs_build_templates (struct xn_template *t, const unsigned *ids)
{
  int n;

  memset (t, 0, sizeof (struct xn_template) * CFFS_NTYPES);
  for (int i = 0; i < CFFS_NTYPES; i++)
    strncpy (t[i].t_name, cffs_type_names[i], XN_NAMELEN - 1);

  t[CFFS_T_DATA].t_flags = XN_TF_DATA;

  if ((n = owns_ptrs (&t[CFFS_T_IND], ids[CFFS_T_DATA])) < 0)
    return -1;
  t[CFFS_T_IND].t_nowns = n;
  if ((n = owns_ptrs (&t[CFFS_T_DIND], ids[CFFS_T_IND])) < 0)
    return -1;
  t[CFFS_T_DIND].t_nowns = n;
  if ((n = owns_ptrs (&t[CFFS_T_DIRIND], ids[CFFS_T_DIR])) < 0)
    return -1;
  t[CFFS_T_DIRIND].t_nowns = n;

  if ((n = owns_dir (&t[CFFS_T_DIR], ids)) < 0)
    return -1;
  t[CFFS_T_DIR].t_nowns = n;
  if ((n = acl_dir (&t[CFFS_T_DIR], 0)) < 0)
    return -1;
  t[CFFS_T_DIR].t_nacl = n;

  if ((n = owns_super (&t[CFFS_T_SUPER], ids)) < 0)
    return -1;
  t[CFFS_T_SUPER].t_nowns = n;
  if ((n = acl_dir (&t[CFFS_T_SUPER], 1)) < 0)
    return -1;
  t[CFFS_T_SUPER].t_nacl = n;
  return 0;
}
