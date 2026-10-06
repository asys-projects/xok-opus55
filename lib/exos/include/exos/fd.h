/*
 * ExOS file descriptors.
 *
 * As in ExOS, open files live in a file table in memory shared by all
 * the processes (so that offsets and reference counts are shared after
 * fork and exec, as UNIX requires); file descriptors are private
 * per-process indices into it.  Each open file is accessed through a
 * table of operations, which applications may override.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef EXOS_FD_H
#define EXOS_FD_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <exos/fs.h>

#define NFILE 256
#define NOFILE 64
#define FILETAB USHARED
#define FILETAB_SIZE (NFILE * 256)

/* Capability slot of the world capability, guarding shared tables. */
#define CAP_WORLD 2

#define FT_NONE   0
#define FT_CONS   1
#define FT_FILE   2
#define FT_DIR    3
#define FT_PIPE   4
#define FT_NULL   5
#define FT_ZERO   6
#define FT_SOCK   7
#define FT_NTYPES 8

struct file
{
  volatile uint32_t f_ref;
  uint32_t f_type;
  uint32_t f_flags;		/* O_* */
  volatile uint32_t f_lock;
  volatile int64_t f_off;
  union
  {
    struct
    {
      struct cffs_ref ref;
      char path[200];
    } file;
    struct
    {
      int sreg;			/* Software region holding the data. */
      int peer;			/* File table index of the other end. */
      int wend;			/* 1 for the write end. */
    } pipe;
    struct
    {
      int id;			/* Socket in the owner's net stack. */
      envid_t owner;
    } sock;
    uint8_t pad[232];
  } u;
};

struct file_ops
{
  ssize_t (*read) (struct file *f, void *buf, size_t n);
  ssize_t (*write) (struct file *f, const void *buf, size_t n);
  off_t (*lseek) (struct file *f, off_t off, int whence);
  int (*fstat) (struct file *f, struct stat *st);
  void (*close) (struct file *f);	/* Last reference gone. */
  int (*ready) (struct file *f, int write);	/* For select. */
  int (*wk) (struct file *f, int write, struct wk_term *t, int max);
};

extern struct file_ops *exos_fops[FT_NTYPES];

#define files ((struct file *) FILETAB)

struct fdtab
{
  int16_t fd[NOFILE];		/* File table index, or -1. */
  uint8_t cloexec[NOFILE];
};
extern struct fdtab __fdtab;

struct file *fd_file (int fd);
int fd_alloc_file (int type, int flags, struct file **fp);
int fd_install (struct file *f, int minfd);
void file_ref (struct file *f);
void file_unref (struct file *f);
int file_index (struct file *f);
void fd_fork_child (void);
void fd_close_all (void);
void file_lock (struct file *f);
void file_unlock (struct file *f);

/* Paths. */
int exos_abspath (const char *path, char *out, size_t n);
extern char __cwd[256];
extern mode_t __umask;

#endif
