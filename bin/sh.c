/*
 * sh: the ExOS shell.
 *
 * Supports pipelines, redirections (<, >, >>, 2>, 2>&1), background
 * jobs (&), command lists (;, &&, ||), quoting, $VAR / $? / $$
 * expansion, '*' and '?' globbing in the last path component, comments,
 * and the builtins cd, pwd, exit, export, unset, set, wait, true, false,
 * echo, source/.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/wait.h>

#define MAXARGS 128
#define MAXLINE 1024

static int last_status;
static int interactive;

/*
 * Words.
 */
struct word
{
  char *s;
  int quoted;			/* No globbing. */
};

enum tok
{ T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG, T_LT, T_GT, T_GTGT, T_ERR,
  T_ERRGT, T_END
};

struct token
{
  enum tok t;
  struct word w;
};

static char *
xstrdup (const char *s)
{
  char *d = strdup (s);
  if (d == NULL)
    {
      fprintf (stderr, "sh: out of memory\n");
      exit (1);
    }
  return d;
}

static void
append (char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
  if (*len + n + 1 > *cap)
    {
      *cap = (*len + n + 1) * 2;
      *buf = realloc (*buf, *cap);
    }
  memcpy (*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = 0;
}

static void
expand_var (const char **pp, char **buf, size_t *len, size_t *cap)
{
  const char *p = *pp + 1;
  char name[64], num[16];
  size_t n = 0;

  if (*p == '?')
    {
      snprintf (num, sizeof (num), "%d", last_status);
      append (buf, len, cap, num, strlen (num));
      *pp = p + 1;
      return;
    }
  if (*p == '$')
    {
      snprintf (num, sizeof (num), "%d", getpid ());
      append (buf, len, cap, num, strlen (num));
      *pp = p + 1;
      return;
    }
  int brace = *p == '{';
  if (brace)
    p++;
  while ((isalnum ((unsigned char) *p) || *p == '_') && n < sizeof (name) - 1)
    name[n++] = *p++;
  name[n] = 0;
  if (brace && *p == '}')
    p++;
  if (n == 0)
    {
      append (buf, len, cap, "$", 1);
      *pp = *pp + 1;
      return;
    }
  const char *v = getenv (name);
  if (v)
    append (buf, len, cap, v, strlen (v));
  *pp = p;
}

/* Expand quotes, escapes and variables of a raw word. */
static char *
expand_word (const char *p, int *quoted)
{
  char *buf = NULL;
  size_t len = 0, cap = 0;

  *quoted = 0;
  append (&buf, &len, &cap, "", 0);
  while (*p)
    {
      if (*p == '\'')
	{
	  const char *e = strchr (p + 1, '\'');
	  if (e == NULL)
	    e = p + strlen (p);
	  append (&buf, &len, &cap, p + 1, e - p - 1);
	  p = *e ? e + 1 : e;
	  *quoted = 1;
	}
      else if (*p == '"')
	{
	  p++;
	  while (*p && *p != '"')
	    {
	      if (*p == '\\' && p[1])
		{
		  append (&buf, &len, &cap, p + 1, 1);
		  p += 2;
		}
	      else if (*p == '$')
		expand_var (&p, &buf, &len, &cap);
	      else
		append (&buf, &len, &cap, p++, 1);
	    }
	  if (*p == '"')
	    p++;
	  *quoted = 1;
	}
      else if (*p == '\\' && p[1])
	{
	  append (&buf, &len, &cap, p + 1, 1);
	  p += 2;
	  *quoted = 1;
	}
      else if (*p == '$')
	expand_var (&p, &buf, &len, &cap);
      else
	append (&buf, &len, &cap, p++, 1);
    }
  return buf;
}

/* Tokenise LINE. */
static int
tokenize (const char *line, struct token *toks, int max)
{
  int n = 0;
  const char *p = line;

  while (n < max - 1)
    {
      while (*p == ' ' || *p == '\t' || *p == '\n')
	p++;
      if (*p == 0 || *p == '#')
	break;
      struct token *t = &toks[n];
      memset (t, 0, sizeof (*t));
      if (p[0] == '|' && p[1] == '|')
	t->t = T_OR, p += 2;
      else if (p[0] == '&' && p[1] == '&')
	t->t = T_AND, p += 2;
      else if (*p == '|')
	t->t = T_PIPE, p++;
      else if (*p == ';')
	t->t = T_SEMI, p++;
      else if (*p == '&')
	t->t = T_BG, p++;
      else if (*p == '<')
	t->t = T_LT, p++;
      else if (p[0] == '>' && p[1] == '>')
	t->t = T_GTGT, p += 2;
      else if (*p == '>')
	t->t = T_GT, p++;
      else if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1')
	t->t = T_ERRGT, p += 4;
      else if (p[0] == '2' && p[1] == '>')
	t->t = T_ERR, p += 2;
      else
	{
	  /* Keep the raw word: expansion happens when the command runs,
	     so that $? reflects the previous command of the line. */
	  const char *st = p;
	  while (*p && !strchr (" \t\n|;&<>", *p))
	    {
	      if (*p == '\'' || *p == '"')
		{
		  char q = *p++;
		  while (*p && *p != q)
		    {
		      if (q == '"' && *p == '\\' && p[1])
			p++;
		      p++;
		    }
		  if (*p)
		    p++;
		}
	      else if (*p == '\\' && p[1])
		p += 2;
	      else
		p++;
	    }
	  t->t = T_WORD;
	  t->w.s = strndup (st, p - st);
	  t->w.quoted = 0;
	}
      n++;
    }
  toks[n].t = T_END;
  return n;
}

/*
 * Globbing of the last path component.
 */
static int
match (const char *pat, const char *s)
{
  for (; *pat; pat++, s++)
    {
      if (*pat == '*')
	{
	  for (const char *q = s;; q++)
	    {
	      if (match (pat + 1, q))
		return 1;
	      if (*q == 0)
		return 0;
	    }
	}
      if (*s == 0 || (*pat != '?' && *pat != *s))
	return 0;
    }
  return *s == 0;
}

static int
cmpstr (const void *a, const void *b)
{
  return strcmp (*(char *const *) a, *(char *const *) b);
}

static int
glob_word (const char *w, char **argv, int argc, int max)
{
  const char *slash = strrchr (w, '/');
  char dir[256], prefix[256];
  const char *pat = slash ? slash + 1 : w;
  int start = argc;
  DIR *d;
  struct dirent *de;

  if (!strpbrk (pat, "*?"))
    {
      argv[argc++] = xstrdup (w);
      return argc;
    }
  if (slash)
    {
      size_t n = slash - w;
      memcpy (dir, w, n);
      dir[n] = 0;
      if (n == 0)
	strcpy (dir, "/");
      snprintf (prefix, sizeof (prefix), "%.*s/", (int) n, w);
    }
  else
    {
      strcpy (dir, ".");
      prefix[0] = 0;
    }
  d = opendir (dir);
  if (d)
    {
      while ((de = readdir (d)) != NULL && argc < max - 1)
	{
	  if (de->d_name[0] == '.' && pat[0] != '.')
	    continue;
	  if (match (pat, de->d_name))
	    {
	      char *s = malloc (strlen (prefix) + strlen (de->d_name) + 1);
	      sprintf (s, "%s%s", prefix, de->d_name);
	      argv[argc++] = s;
	    }
	}
      closedir (d);
    }
  if (argc == start)
    argv[argc++] = xstrdup (w);
  else
    qsort (argv + start, argc - start, sizeof (char *), cmpstr);
  return argc;
}

/*
 * Simple commands and pipelines.
 */
struct cmd
{
  char *argv[MAXARGS];
  int argc;
  char *in, *out, *err;
  int append, err2out;
};

static int run_line (const char *line);
static int source_file (const char *path);

static int
builtin (struct cmd *c, int *status)
{
  char **argv = c->argv;

  if (c->argc == 0)
    return 0;
  if (!strcmp (argv[0], "cd"))
    {
      const char *d = c->argc > 1 ? argv[1] : getenv ("HOME");
      if (d == NULL)
	d = "/";
      *status = 0;
      if (chdir (d) < 0)
	{
	  fprintf (stderr, "cd: %s: %s\n", d, strerror (errno));
	  *status = 1;
	}
      else
	{
	  char cwd[256];
	  setenv ("PWD", getcwd (cwd, sizeof (cwd)), 1);
	}
      return 1;
    }
  if (!strcmp (argv[0], "pwd"))
    {
      char cwd[256];
      printf ("%s\n", getcwd (cwd, sizeof (cwd)));
      *status = 0;
      return 1;
    }
  if (!strcmp (argv[0], "exit"))
    {
      fflush (stdout);
      exit (c->argc > 1 ? atoi (argv[1]) : last_status);
    }
  if (!strcmp (argv[0], "export"))
    {
      for (int i = 1; i < c->argc; i++)
	{
	  char *eq = strchr (argv[i], '=');
	  if (eq)
	    {
	      *eq = 0;
	      setenv (argv[i], eq + 1, 1);
	      *eq = '=';
	    }
	}
      *status = 0;
      return 1;
    }
  if (!strcmp (argv[0], "unset"))
    {
      for (int i = 1; i < c->argc; i++)
	unsetenv (argv[i]);
      *status = 0;
      return 1;
    }
  if (!strcmp (argv[0], "set"))
    {
      extern char **environ;
      for (char **e = environ; e && *e; e++)
	printf ("%s\n", *e);
      *status = 0;
      return 1;
    }
  if (!strcmp (argv[0], "wait"))
    {
      int st;
      while (wait (&st) > 0)
	;
      *status = 0;
      return 1;
    }
  if (!strcmp (argv[0], "true") || !strcmp (argv[0], ":"))
    {
      *status = 0;
      return 1;
    }
  if (!strcmp (argv[0], "false"))
    {
      *status = 1;
      return 1;
    }
  if ((!strcmp (argv[0], "source") || !strcmp (argv[0], ".")) && c->argc > 1)
    {
      *status = source_file (argv[1]);
      return 1;
    }
  /* Assignment: VAR=value */
  if (c->argc == 1 && strchr (argv[0], '=') && argv[0][0] != '=')
    {
      char *eq = strchr (argv[0], '=');
      *eq = 0;
      setenv (argv[0], eq + 1, 1);
      *eq = '=';
      *status = 0;
      return 1;
    }
  return 0;
}

static int
redirect (struct cmd *c)
{
  int fd;

  if (c->in)
    {
      if ((fd = open (c->in, O_RDONLY)) < 0)
	{
	  fprintf (stderr, "sh: %s: %s\n", c->in, strerror (errno));
	  return -1;
	}
      dup2 (fd, 0);
      close (fd);
    }
  if (c->out)
    {
      fd = open (c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC),
		 0644);
      if (fd < 0)
	{
	  fprintf (stderr, "sh: %s: %s\n", c->out, strerror (errno));
	  return -1;
	}
      dup2 (fd, 1);
      close (fd);
    }
  if (c->err)
    {
      fd = open (c->err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0)
	return -1;
      dup2 (fd, 2);
      close (fd);
    }
  if (c->err2out)
    dup2 (1, 2);
  return 0;
}

static void
run_child (struct cmd *c)
{
  int st;

  signal (SIGINT, SIG_DFL);
  if (redirect (c) < 0)
    _exit (1);
  if (builtin (c, &st))
    {
      fflush (stdout);
      _exit (st);
    }
  execvp (c->argv[0], c->argv);
  fprintf (stderr, "sh: %s: %s\n", c->argv[0],
	   errno == ENOENT ? "command not found" : strerror (errno));
  _exit (errno == ENOENT ? 127 : 126);
}

/* Run a pipeline of N commands; returns the status of the last one. */
static int
run_pipeline (struct cmd *cmds, int n, int bg)
{
  pid_t pids[16];
  int prev = -1, status = 0;

  if (n == 1 && !bg && !cmds[0].in && !cmds[0].out && !cmds[0].err)
    {
      if (builtin (&cmds[0], &status))
	return status;
    }
  fflush (stdout);
  for (int i = 0; i < n; i++)
    {
      int pfd[2] = { -1, -1 };
      if (i < n - 1 && pipe (pfd) < 0)
	{
	  perror ("sh: pipe");
	  return 1;
	}
      pid_t pid = fork ();
      if (pid < 0)
	{
	  perror ("sh: fork");
	  return 1;
	}
      if (pid == 0)
	{
	  if (prev >= 0)
	    {
	      dup2 (prev, 0);
	      close (prev);
	    }
	  if (pfd[1] >= 0)
	    {
	      dup2 (pfd[1], 1);
	      close (pfd[1]);
	      close (pfd[0]);
	    }
	  run_child (&cmds[i]);
	}
      pids[i] = pid;
      if (prev >= 0)
	close (prev);
      if (pfd[1] >= 0)
	close (pfd[1]);
      prev = pfd[0];
    }
  if (bg)
    {
      printf ("[%d]\n", pids[n - 1]);
      return 0;
    }
  for (int i = 0; i < n; i++)
    {
      int st;
      if (waitpid (pids[i], &st, 0) == pids[i] && i == n - 1)
	status = WIFEXITED (st) ? WEXITSTATUS (st) : 128 + WTERMSIG (st);
    }
  return status;
}

static void
free_cmds (struct cmd *cmds, int n)
{
  for (int i = 0; i < n; i++)
    for (int j = 0; j < cmds[i].argc; j++)
      free (cmds[i].argv[j]);
}

static int
run_tokens (struct token *toks, int ntok)
{
  struct cmd cmds[16];
  int ncmd = 0, i = 0;
  enum tok cond = T_SEMI;
  int status = last_status;

  memset (cmds, 0, sizeof (cmds));
  while (i <= ntok)
    {
      struct cmd *c = &cmds[ncmd];
      struct token *t = &toks[i];

      switch (t->t)
	{
	case T_WORD:
	  if (c->argc < MAXARGS - 1)
	    {
	      int quoted;
	      char *w = expand_word (t->w.s, &quoted);
	      if (quoted)
		c->argv[c->argc++] = w;
	      else
		{
		  c->argc = glob_word (w, c->argv, c->argc, MAXARGS);
		  free (w);
		}
	    }
	  i++;
	  break;
	case T_LT:
	case T_GT:
	case T_GTGT:
	case T_ERR:
	  if (toks[i + 1].t != T_WORD)
	    {
	      fprintf (stderr, "sh: syntax error\n");
	      free_cmds (cmds, ncmd + 1);
	      return 2;
	    }
	  {
	    int q;
	    char *w = expand_word (toks[i + 1].w.s, &q);
	    free (toks[i + 1].w.s);
	    toks[i + 1].w.s = w;
	  }
	  if (t->t == T_LT)
	    c->in = toks[i + 1].w.s;
	  else if (t->t == T_ERR)
	    c->err = toks[i + 1].w.s;
	  else
	    {
	      c->out = toks[i + 1].w.s;
	      c->append = t->t == T_GTGT;
	    }
	  i += 2;
	  break;
	case T_ERRGT:
	  c->err2out = 1;
	  i++;
	  break;
	case T_PIPE:
	  if (ncmd >= 15)
	    {
	      fprintf (stderr, "sh: pipeline too long\n");
	      free_cmds (cmds, ncmd + 1);
	      return 2;
	    }
	  ncmd++;
	  i++;
	  break;
	default:		/* ; & && || END */
	  {
	    int bg = t->t == T_BG;
	    int run = cond == T_SEMI || cond == T_BG
	      || (cond == T_AND && status == 0)
	      || (cond == T_OR && status != 0);
	    if (cmds[0].argc > 0 && run)
	      {
		status = run_pipeline (cmds, ncmd + 1, bg);
		last_status = status;
	      }
	    free_cmds (cmds, ncmd + 1);
	    memset (cmds, 0, sizeof (cmds));
	    ncmd = 0;
	    cond = t->t;
	    i++;
	    break;
	  }
	}
    }
  return status;
}

static int
run_line (const char *line)
{
  struct token toks[256];
  int n = tokenize (line, toks, 256);
  int r = run_tokens (toks, n);

  for (int i = 0; i < n; i++)
    if (toks[i].t == T_WORD)
      free (toks[i].w.s);
  return r;
}

static int
source_file (const char *path)
{
  FILE *f = fopen (path, "r");
  char line[MAXLINE];
  int r = 0;

  if (f == NULL)
    {
      fprintf (stderr, "sh: %s: %s\n", path, strerror (errno));
      return 127;
    }
  while (fgets (line, sizeof (line), f))
    r = run_line (line);
  fclose (f);
  return r;
}

static void
reap (void)
{
  int st;
  pid_t p;
  while ((p = waitpid (-1, &st, WNOHANG)) > 0)
    if (interactive)
      printf ("[%d] done (%d)\n", p, WEXITSTATUS (st));
}

int
main (int argc, char **argv)
{
  char line[MAXLINE];

  if (argc > 2 && !strcmp (argv[1], "-c"))
    return run_line (argv[2]);
  if (argc > 1)
    return source_file (argv[1]);

  interactive = isatty (0);
  if (interactive)
    signal (SIGINT, SIG_IGN);
  for (;;)
    {
      if (interactive)
	{
	  char cwd[256];
	  reap ();
	  printf ("%s# ", getcwd (cwd, sizeof (cwd)));
	  fflush (stdout);
	}
      if (fgets (line, sizeof (line), stdin) == NULL)
	break;
      run_line (line);
    }
  return last_status;
}
