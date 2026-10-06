#ifndef _UNISTD_H
#define _UNISTD_H
#include <sys/types.h>
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
ssize_t read (int fd, void *buf, size_t n);
ssize_t write (int fd, const void *buf, size_t n);
int close (int fd);
off_t lseek (int fd, off_t off, int whence);
int dup (int fd);
int dup2 (int fd, int nfd);
int pipe (int fds[2]);
pid_t fork (void);
int execv (const char *path, char *const argv[]);
int execve (const char *path, char *const argv[], char *const envp[]);
int execvp (const char *file, char *const argv[]);
void _exit (int status) __attribute__ ((noreturn));
pid_t getpid (void);
pid_t getppid (void);
uid_t getuid (void);
uid_t geteuid (void);
gid_t getgid (void);
gid_t getegid (void);
int setuid (uid_t uid);
int setgid (gid_t gid);
unsigned int sleep (unsigned int s);
int usleep (useconds_t us);
int chdir (const char *path);
char *getcwd (char *buf, size_t size);
int unlink (const char *path);
int rmdir (const char *path);
int link (const char *a, const char *b);
int access (const char *path, int mode);
int isatty (int fd);
int ftruncate (int fd, off_t len);
int truncate (const char *path, off_t len);
int fsync (int fd);
void sync (void);
void *sbrk (intptr_t inc);
int gethostname (char *name, size_t len);
long sysconf (int name);
int getopt (int argc, char *const argv[], const char *opts);
extern char *optarg;
extern int optind, opterr, optopt;
extern char **environ;
#define _SC_PAGESIZE 1
#define _SC_NPROCESSORS_ONLN 2
#define _SC_CLK_TCK 3
#endif
