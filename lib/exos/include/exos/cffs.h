/*
 * C-FFS: ExOS's library file system on XN.
 *
 * As in Ganger and Kaashoek's co-locating fast file system, inodes are
 * embedded in directory entries (no separate inode table), and the
 * blocks of small files are allocated close to their directory.  The
 * file system is protected by XN through the templates (and UDFs)
 * defined in cffs_templates.c:
 *
 *   cffs-super   the superblock (root of the tree), embeds the root
 *                directory's inode.
 *   cffs-dir     directory block: 16 entries with embedded inodes.
 *   cffs-dirind  indirect block of a directory: pointers to dir blocks.
 *   cffs-ind     indirect block of a file: pointers to data blocks.
 *   cffs-dind    double indirect block: pointers to cffs-ind blocks.
 *   cffs-data    file data.
 *
 * This header is shared by the library and by the host tool mkxnfs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef EXOS_CFFS_H
#define EXOS_CFFS_H

#include <stdint.h>
#include <xok/xn.h>

#define CFFS_MAGIC   0x43464653	/* "CFFS" */
#define CFFS_VERSION 1
#define CFFS_ROOTNAME "cffs"

#define CFFS_NDIRECT 12
#define CFFS_NPTRS   (XN_BLKSIZE / 4)	/* Pointers per indirect block. */
#define CFFS_NAMELEN 123

struct cffs_inode
{
  uint16_t i_mode;
  uint16_t i_nlink;
  uint32_t i_uid;
  uint32_t i_gid;
  uint32_t i_size;
  uint32_t i_atime;
  uint32_t i_mtime;
  uint32_t i_ctime;
  uint32_t i_direct[CFFS_NDIRECT];
  uint32_t i_indirect;
  uint32_t i_dindirect;
  uint32_t i_spare[11];
};

#define CFFS_I_MODE     0
#define CFFS_I_UID      4
#define CFFS_I_SIZE     12
#define CFFS_I_DIRECT   28
#define CFFS_I_INDIRECT 76
#define CFFS_I_DINDIRECT 80

struct cffs_dirent
{
  uint8_t d_used;
  uint8_t d_namelen;
  uint16_t d_pad;
  char d_name[CFFS_NAMELEN + 1];
  struct cffs_inode d_inode;
};

#define CFFS_DIRENT_SIZE 256
#define CFFS_DIRENT_INODE 128
#define CFFS_DIRENTS (XN_BLKSIZE / CFFS_DIRENT_SIZE)

struct cffs_super
{
  uint32_t s_magic;
  uint32_t s_version;
  uint32_t s_ctime;
  uint32_t s_pad[29];
  struct cffs_inode s_root;	/* At offset 128, like a dirent's inode. */
};

/* Type indices in the template array. */
enum
{
  CFFS_T_DATA = 0,
  CFFS_T_IND,
  CFFS_T_DIND,
  CFFS_T_DIR,
  CFFS_T_DIRIND,
  CFFS_T_SUPER,
  CFFS_NTYPES
};

extern const char *const cffs_type_names[CFFS_NTYPES];

/*
 * Build the templates.  IDS gives the XN type id of each template (the
 * owns-udfs emit child type ids).  Returns 0 on success.
 */
int cffs_build_templates (struct xn_template *t, const unsigned *ids);

#endif
