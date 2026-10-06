#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <stddef.h>
#include <stdint.h>
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int mode_t;
typedef long off_t;
typedef long ssize_t;
typedef unsigned int ino_t;
typedef unsigned int dev_t;
typedef unsigned int nlink_t;
typedef long time_t;
typedef long suseconds_t;
typedef unsigned long useconds_t;
typedef long blksize_t;
typedef long blkcnt_t;
typedef int clockid_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef char *caddr_t;
#endif
