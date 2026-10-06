/*
 * XN: Xok's protected, extensible stable storage.
 *
 * XN multiplexes disks among untrusted library file systems (libFSes)
 * at the granularity of disk blocks.  It does not know any file system
 * format: libFSes describe their metadata types with templates, each
 * carrying untrusted deterministic functions (UDFs) written in a small
 * pseudo-RISC language interpreted safely by the kernel:
 *
 *   owns-udf(meta)   the set of <block, count, type> extents the
 *                    metadata block points to.  Deterministic: it can
 *                    only read the metadata block.
 *   acl-uf(meta, op, credential)
 *                    approves (1), denies (0) or defers (2) an operation
 *                    on the block or on one of its children; deferred
 *                    checks are re-run at the parent.
 *   size-uf(meta)    size of the structure in bytes.
 *
 * XN verifies every metadata modification by running owns-udf on the
 * old and on the proposed new contents: the result must be unchanged
 * (modify), grow by exactly the allocated blocks (alloc), or shrink by
 * exactly the freed blocks (free).  Determinism makes the verification
 * inductive: libFSes track their own blocks, XN merely checks that they
 * do so correctly.
 *
 * XN also keeps the buffer cache registry (mapping disk blocks to the
 * physical pages caching them, exported read-only to applications), the
 * free map, the type catalogue and the root catalogue, and enforces the
 * ordering rules needed for crash consistency: a block pointing to an
 * uninitialised block is tainted and cannot be written; a freed block is
 * not reused until its parent, without the pointer, has reached the disk.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_XN_H
#define XOK_XN_H

#include <stdint.h>
#include <xok/cap.h>

#define XN_BLKSIZE   4096
#define XN_MAGIC     0x584e4653	/* "XNFS" */
#define XN_VERSION   1

#define XN_MAXTYPES  32
#define XN_MAXROOTS  64
#define XN_NAMELEN   32

/*
 * On-disk layout:
 *   block 0                 superblock
 *   typecat_start ...       one block per type template
 *   rootcat_start           root catalogue (one block)
 *   freemap_start ...       free map (1 bit per block, 1 = free)
 *   data_start ...          blocks managed by libFSes
 */
struct xn_super
{
  uint32_t s_magic;
  uint32_t s_version;
  uint32_t s_nblocks;
  uint32_t s_typecat_start;
  uint32_t s_rootcat_start;
  uint32_t s_freemap_start;
  uint32_t s_freemap_nblocks;
  uint32_t s_data_start;
  uint32_t s_clean;		/* Cleanly synchronised. */
  uint32_t s_gen;		/* Mount count. */
};

/*
 * UDF instructions.
 */
struct udf_insn
{
  uint8_t op;
  uint8_t a, b, c;
  int32_t imm;
};

enum
{
  UDF_END = 0,			/* Stop (owns: done; acl/size: return 0). */
  UDF_LI,			/* r[a] = imm */
  UDF_MOV,			/* r[a] = r[b] */
  UDF_ADD,			/* r[a] = r[b] + r[c] */
  UDF_SUB,			/* r[a] = r[b] - r[c] */
  UDF_MUL,			/* r[a] = r[b] * r[c] */
  UDF_DIVU,			/* r[a] = r[b] / r[c] (fault if 0) */
  UDF_REMU,			/* r[a] = r[b] % r[c] (fault if 0) */
  UDF_AND,			/* r[a] = r[b] & r[c] */
  UDF_OR,			/* r[a] = r[b] | r[c] */
  UDF_XOR,			/* r[a] = r[b] ^ r[c] */
  UDF_SHL,			/* r[a] = r[b] << (r[c] & 31) */
  UDF_SHR,			/* r[a] = r[b] >> (r[c] & 31) */
  UDF_ADDI,			/* r[a] = r[b] + imm */
  UDF_ANDI,			/* r[a] = r[b] & imm */
  UDF_LDB,			/* r[a] = meta[r[b] + imm] (8 bits) */
  UDF_LDH,			/* r[a] = meta[r[b] + imm] (16 bits, LE) */
  UDF_LDW,			/* r[a] = meta[r[b] + imm] (32 bits, LE) */
  /* Loads read the current contents if c == 0; in acl-ufs, c == 1
     reads the proposed new contents of the block being modified. */
  UDF_JMP,			/* pc = imm */
  UDF_BEQ,			/* if r[a] == r[b] pc = imm */
  UDF_BNE,			/* if r[a] != r[b] pc = imm */
  UDF_BLTU,			/* if r[a] <  r[b] pc = imm (unsigned) */
  UDF_BGEU,			/* if r[a] >= r[b] pc = imm (unsigned) */
  UDF_EMIT,			/* owns: emit <r[a], r[b] blocks, type r[c]> */
  UDF_RET,			/* acl/size: return r[a] */
  UDF_ARG,			/* acl: r[a] = arg[imm] */
  UDF_CRED,			/* acl: r[a] = credential byte imm (-1: length,
				   -2: permissions) */
  UDF_DOMN,			/* acl: r[a] = credential dominates the name of
				   r[c] bytes at meta[r[b]] */
  UDF_NOPS
};

#define UDF_NREGS 16
#define UDF_MAXINSNS 256
#define UDF_MAXSTEPS 2000000
#define UDF_MAXEMIT 4096

/* acl-uf arguments. */
#define XN_ARG_OP    0		/* XN_OP_* */
#define XN_ARG_OFF   1		/* Modification offset. */
#define XN_ARG_LEN   2		/* Modification length. */
#define XN_ARG_CHILD 3		/* Child block concerned (or 0). */
#define XN_NARGS     4

#define XN_OP_READ   1		/* Map / read. */
#define XN_OP_WRITE  2		/* Map writable / write. */
#define XN_OP_ALLOC  3		/* Allocate children in this block. */
#define XN_OP_FREE   4		/* Free children of this block. */
#define XN_OP_MODIFY 5		/* Modify this metadata block. */
#define XN_OP_LOCK   6

/* acl-uf results. */
#define XN_ACL_DENY  0
#define XN_ACL_ALLOW 1
#define XN_ACL_DEFER 2		/* Ask the parent (op becomes READ/WRITE). */

/* Template flags. */
#define XN_TF_DATA   0x01	/* Data: no owns, can be mapped writable. */

/* A type template (one disk block in the type catalogue). */
#define XN_OWNS_MAX 128
#define XN_ACL_MAX  256
#define XN_SIZE_MAX 32

struct xn_template
{
  char t_name[XN_NAMELEN];
  uint32_t t_flags;
  uint16_t t_nowns;		/* Instructions in each UDF. */
  uint16_t t_nacl;
  uint16_t t_nsize;
  uint16_t t_pad;
  struct udf_insn t_owns[XN_OWNS_MAX];
  struct udf_insn t_acl[XN_ACL_MAX];
  struct udf_insn t_size[XN_SIZE_MAX];
};

/* Root catalogue entry. */
struct xn_root
{
  char r_name[XN_NAMELEN];
  uint32_t r_blk;
  uint32_t r_nblocks;
  uint32_t r_type;
  uint32_t r_flags;		/* XN_RF_* */
  struct cap r_guard;		/* Checked when acl-uf defers at the root. */
};

#define XN_RF_USED 1
#define XN_RF_TEMPORARY 2	/* Not persistent: no ordering constraints. */

/* Metadata modification. */
#define XN_MAXMODS 8
#define XN_MODMAX  1024

struct xn_mod
{
  uint16_t m_off;
  uint16_t m_len;
  uint32_t m_data;		/* User address of m_len bytes. */
};

struct xn_op
{
  uint32_t o_child;		/* First child block (alloc/free). */
  uint32_t o_nchild;		/* Number of contiguous children. */
  uint32_t o_ctype;		/* Child type (alloc). */
  uint32_t o_nmods;
  struct xn_mod o_mods[XN_MAXMODS];
};

/*
 * Buffer cache registry entry, exported read-only at UBC.
 */
struct bc_entry
{
  uint16_t bc_dev;
  uint16_t bc_type;		/* Template id, or XN_TYPE_UNKNOWN. */
  uint32_t bc_blk;
  uint32_t bc_ppn;		/* Physical page, 0 if not in core. */
  volatile uint32_t bc_state;	/* BC_* */
  uint32_t bc_parent;		/* Parent block, or XN_NOPARENT. */
  int32_t bc_locker;		/* Env holding the lock, or 0. */
  uint16_t bc_refcnt;		/* User mappings. */
  uint16_t bc_taint;		/* Uninitialised children. */
  uint32_t bc_lastuse;		/* Ticks: approximate LRU. */
};

#define XN_TYPE_UNKNOWN 0xffff
#define XN_NOPARENT 0xffffffffu

#define BC_USED      0x0001	/* Entry in use. */
#define BC_VALID     0x0002	/* Page holds the block's contents. */
#define BC_DIRTY     0x0004	/* Modified since last written. */
#define BC_IN_TRANSIT 0x0008	/* Disk I/O in progress. */
#define BC_UNINIT    0x0010	/* Allocated, never written to disk. */
#define BC_ROOT      0x0020	/* Block of a root extent. */
#define BC_ERROR     0x0040	/* Last I/O failed. */
#define BC_LOCKED    0x0080	/* Locked by bc_locker. */
#define BC_META      0x0100	/* Metadata (template has owns). */

#define XN_NBC 16384		/* Registry entries. */

#define xn_registry ((volatile struct bc_entry *) UBC)
/* Free map of disk D: bitmap of 1 bit per block, 1 = free. */
#define xn_freemap(_d) ((volatile uint32_t *) (UXNFREE + (_d) * 0x400000))
/* Catalogues of disk D: struct xn_super, then templates, then roots. */
#define XN_CAT_SIZE 0x40000
#define xn_cat_super(_d) ((volatile struct xn_super *) (UXNCAT + (_d) * XN_CAT_SIZE))
#define xn_cat_types(_d) ((volatile struct xn_template *) (UXNCAT + (_d) * XN_CAT_SIZE + 4096))
#define xn_cat_roots(_d) ((volatile struct xn_root *) (UXNCAT + (_d) * XN_CAT_SIZE + 4096 * (1 + XN_MAXTYPES)))

#endif
