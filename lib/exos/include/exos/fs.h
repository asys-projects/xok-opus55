/*
 * ExOS file system interfaces: XN helpers and the C-FFS library file
 * system.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef EXOS_FS_H
#define EXOS_FS_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <xok/xn.h>
#include <exos/cffs.h>

/* XN library helpers (xnlib.c). */
int xnl_get (unsigned dev, uint32_t blk, uint32_t parent);
void *xnl_map (unsigned dev, uint32_t blk, int writable);
void xnl_unmap_all (void);
void xnl_unmap (unsigned dev, uint32_t blk);
int xnl_wait_io (int idx);
int xnl_writeback (unsigned dev, uint32_t blk, int wait);
int xnl_sync (unsigned dev);
void xnl_fork_child (void);
int xnl_alloc_block (unsigned dev, uint32_t hint);
void xnl_set_parent (uint32_t blk, uint32_t parent);
uint32_t xnl_parent (uint32_t blk);

/* An inode: the slot of the directory block (or superblock) that
   embeds it. */
struct cffs_ref
{
  uint16_t dev;
  uint16_t slot;
  uint32_t blk;
};

int cffs_mount (void);
int cffs_mounted (void);
int cffs_root (struct cffs_ref *r);
int cffs_iget (const struct cffs_ref *r, struct cffs_inode *ino);
int cffs_namei (const char *path, struct cffs_ref *r);
int cffs_lookup (const struct cffs_ref *dir, const char *name,
		 struct cffs_ref *r);
int cffs_create (const struct cffs_ref *dir, const char *name, mode_t mode,
		 uid_t uid, gid_t gid, struct cffs_ref *r);
ssize_t cffs_read (const struct cffs_ref *r, off_t off, void *buf,
		   size_t n);
ssize_t cffs_write (const struct cffs_ref *r, off_t off, const void *buf,
		    size_t n);
int cffs_truncate (const struct cffs_ref *r, off_t size);
int cffs_unlink (const struct cffs_ref *dir, const char *name, int isdir);
int cffs_rename (const struct cffs_ref *odir, const char *oname,
		 const struct cffs_ref *ndir, const char *nname);
int cffs_readdir (const struct cffs_ref *dir, uint32_t * cookie,
		  char *name, struct cffs_ref *child);
int cffs_setattr (const struct cffs_ref *r, int what, uint32_t v1,
		  uint32_t v2);
void cffs_stat (const struct cffs_ref *r, const struct cffs_inode *ino,
		struct stat *st);
int cffs_sync (void);

#define CFFS_SET_MODE  1
#define CFFS_SET_OWNER 2
#define CFFS_SET_TIMES 3

#endif
