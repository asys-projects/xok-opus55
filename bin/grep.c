/* grep: fixed strings, or simple regular expressions with ^ $ . * */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

static int iflag, vflag, nflag, cflag;

static int matchhere (const char *re, const char *s);

static int
eq (char a, char b)
{
  return iflag ? tolower ((unsigned char) a) == tolower ((unsigned char) b) : a == b;
}

static int
matchstar (char c, const char *re, const char *s)
{
  do
    {
      if (matchhere (re, s))
	return 1;
    }
  while (*s != '\0' && (*s++ == c || c == '.' || eq (s[-1], c)));
  return 0;
}

static int
matchhere (const char *re, const char *s)
{
  if (re[0] == '\0')
    return 1;
  if (re[1] == '*')
    return matchstar (re[0], re + 2, s);
  if (re[0] == '$' && re[1] == '\0')
    return *s == '\0' || *s == '\n';
  if (*s != '\0' && *s != '\n' && (re[0] == '.' || eq (re[0], *s)))
    return matchhere (re + 1, s + 1);
  return 0;
}

static int
match (const char *re, const char *s)
{
  if (re[0] == '^')
    return matchhere (re + 1, s);
  do
    {
      if (matchhere (re, s))
	return 1;
    }
  while (*s++ != '\0');
  return 0;
}

static int
grep (const char *re, FILE *f, const char *name, int multi)
{
  char *line = NULL;
  size_t cap = 0;
  long ln = 0, cnt = 0;
  while (getline (&line, &cap, f) > 0)
    {
      ln++;
      if (match (re, line) != vflag)
	{
	  cnt++;
	  if (cflag)
	    continue;
	  if (multi)
	    printf ("%s:", name);
	  if (nflag)
	    printf ("%ld:", ln);
	  fputs (line, stdout);
	  if (line[strlen (line) - 1] != '\n')
	    putchar ('\n');
	}
    }
  if (cflag)
    printf ("%s%s%ld\n", multi ? name : "", multi ? ":" : "", cnt);
  free (line);
  return cnt > 0;
}

int
main (int argc, char **argv)
{
  int c, found = 0;
  while ((c = getopt (argc, argv, "ivnc")) != -1)
    switch (c)
      {
      case 'i':
	iflag = 1;
	break;
      case 'v':
	vflag = 1;
	break;
      case 'n':
	nflag = 1;
	break;
      case 'c':
	cflag = 1;
	break;
      default:
	return 2;
      }
  if (optind >= argc)
    {
      fprintf (stderr, "usage: grep [-ivnc] pattern [file...]\n");
      return 2;
    }
  const char *re = argv[optind++];
  if (optind == argc)
    found = grep (re, stdin, "(stdin)", 0);
  for (int i = optind; i < argc; i++)
    {
      FILE *f = fopen (argv[i], "r");
      if (f == NULL)
	{
	  perror (argv[i]);
	  continue;
	}
      found |= grep (re, f, argv[i], argc - optind > 1);
      fclose (f);
    }
  return found ? 0 : 1;
}
