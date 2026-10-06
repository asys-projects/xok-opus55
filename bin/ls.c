#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

static int lflag, aflag;

static void
mode_str (mode_t m, char *s)
{
  s[0] = S_ISDIR (m) ? 'd' : S_ISCHR (m) ? 'c' : S_ISFIFO (m) ? 'p' : '-';
  const char *rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; i++)
    s[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
  if (m & S_ISVTX)
    s[9] = (m & 1) ? 't' : 'T';
  s[10] = 0;
}

static void
show (const char *path, const char *name)
{
  struct stat st;
  if (!lflag)
    {
      printf ("%s\n", name);
      return;
    }
  if (stat (path, &st) < 0)
    {
      printf ("?????????? %s\n", name);
      return;
    }
  char m[12], t[32];
  mode_str (st.st_mode, m);
  time_t mt = st.st_mtime;
  strftime (t, sizeof (t), "%Y-%m-%d %H:%M", gmtime (&mt));
  printf ("%s %2d %4u %4u %8ld %s %s\n", m, (int) st.st_nlink, st.st_uid,
	  st.st_gid, (long) st.st_size, t, name);
}

static int
cmp (const void *a, const void *b)
{
  return strcmp (*(char *const *) a, *(char *const *) b);
}

static int
list (const char *path)
{
  struct stat st;
  if (stat (path, &st) < 0)
    {
      fprintf (stderr, "ls: %s: %s\n", path, strerror (errno));
      return 1;
    }
  if (!S_ISDIR (st.st_mode))
    {
      show (path, path);
      return 0;
    }
  DIR *d = opendir (path);
  if (d == NULL)
    {
      fprintf (stderr, "ls: %s: %s\n", path, strerror (errno));
      return 1;
    }
  char *names[1024];
  int n = 0;
  struct dirent *de;
  while ((de = readdir (d)) != NULL && n < 1024)
    if (aflag || de->d_name[0] != '.')
      names[n++] = strdup (de->d_name);
  closedir (d);
  qsort (names, n, sizeof (char *), cmp);
  for (int i = 0; i < n; i++)
    {
      char full[512];
      snprintf (full, sizeof (full), "%s/%s", path, names[i]);
      show (full, names[i]);
      free (names[i]);
    }
  return 0;
}

int
main (int argc, char **argv)
{
  int c, r = 0;
  while ((c = getopt (argc, argv, "la")) != -1)
    {
      if (c == 'l')
	lflag = 1;
      else if (c == 'a')
	aflag = 1;
      else
	return 2;
    }
  if (optind == argc)
    return list (".");
  for (int i = optind; i < argc; i++)
    {
      if (argc - optind > 1)
	printf ("%s:\n", argv[i]);
      r |= list (argv[i]);
    }
  return r;
}
