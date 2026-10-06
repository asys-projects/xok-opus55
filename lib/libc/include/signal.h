#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define NSIG 32
typedef void (*sighandler_t) (int);
typedef uint32_t sigset_t;
#define SIG_DFL ((sighandler_t) 0)
#define SIG_IGN ((sighandler_t) 1)
#define SIG_ERR ((sighandler_t) -1)
struct sigaction
{
  sighandler_t sa_handler;
  sigset_t sa_mask;
  int sa_flags;
};
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
sighandler_t signal (int sig, sighandler_t h);
int sigaction (int sig, const struct sigaction *sa, struct sigaction *old);
int sigprocmask (int how, const sigset_t *set, sigset_t *old);
int kill (pid_t pid, int sig);
int raise (int sig);
int sigemptyset (sigset_t *s);
int sigfillset (sigset_t *s);
int sigaddset (sigset_t *s, int sig);
int sigdelset (sigset_t *s, int sig);
int sigismember (const sigset_t *s, int sig);
unsigned int alarm (unsigned int s);
int pause (void);
const char *strsignal (int sig);
#endif
