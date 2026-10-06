/*
 * Xok block device layer.
 *
 * Disk drivers register block devices; requests (struct bio) are
 * asynchronous, described by a list of physical memory segments, and
 * complete through a callback (called with the kernel lock held, from
 * interrupt or polling context).  XN and the raw disk interface sit on
 * top of this layer.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_KERN_BLK_H
#define XOK_KERN_BLK_H

#include "kern.h"

#define SECTOR_SIZE 512
#define BIO_MAXSEGS 16

struct bio_seg
{
  paddr_t pa;
  uint32_t len;			/* Multiple of 512, does not cross a page. */
};

struct bio
{
  uint64_t sector;
  uint32_t nsect;
  bool write;
  unsigned nsegs;
  struct bio_seg seg[BIO_MAXSEGS];
  int error;
  void (*done) (struct bio *b);
  void *arg;
  uint32_t arg2;
  struct bio *next;
};

struct blkdev;

struct blkdev_ops
{
  /* Start the request; the device is idle. */
  int (*start) (struct blkdev *d, struct bio *b);
  /* Check for completion (polling); returns true if the active
     request completed. */
  bool (*poll) (struct blkdev *d);
};

struct blkdev
{
  char name[16];
  uint64_t nsectors;
  const struct blkdev_ops *ops;
  void *drv;
  unsigned idx;
  struct bio *qhead, *qtail;
  struct bio *active;
  uint64_t nreads, nwrites, nerrors;
};

#define MAXBLKDEV 4

int blk_register (struct blkdev *d);
struct blkdev *blk_get (unsigned idx);
unsigned blk_count (void);
void blk_submit (struct blkdev *d, struct bio *b);
void blk_complete (struct blkdev *d, int error);
int blk_rw_sync (struct blkdev *d, uint64_t sector, uint32_t nsect,
		 void *buf, bool write);
struct bio *bio_alloc (void);
void bio_free (struct bio *b);
int bio_add_page (struct bio *b, pfn_t pfn, uint32_t off, uint32_t len);

#endif
