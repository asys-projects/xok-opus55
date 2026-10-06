/* md5sum (RFC 1321). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

struct md5
{
  uint32_t a, b, c, d;
  uint64_t len;
  uint8_t buf[64];
  unsigned n;
};

static const uint32_t K[64] = {
  0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
  0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
  0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
  0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
  0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
  0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
  0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
  0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
  0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
  0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
  0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const int S[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7,
  12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11,
  16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10,
  15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static void
block (struct md5 *m, const uint8_t *p)
{
  uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d;
  for (int i = 0; i < 16; i++)
    w[i] = p[4 * i] | (p[4 * i + 1] << 8) | (p[4 * i + 2] << 16)
      | ((uint32_t) p[4 * i + 3] << 24);
  for (int i = 0; i < 64; i++)
    {
      uint32_t f;
      int g;
      if (i < 16)
	f = (b & c) | (~b & d), g = i;
      else if (i < 32)
	f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
      else if (i < 48)
	f = b ^ c ^ d, g = (3 * i + 5) % 16;
      else
	f = c ^ (b | ~d), g = (7 * i) % 16;
      uint32_t t = d;
      d = c;
      c = b;
      uint32_t x = a + f + K[i] + w[g];
      b = b + ((x << S[i]) | (x >> (32 - S[i])));
      a = t;
    }
  m->a += a, m->b += b, m->c += c, m->d += d;
}

static void
update (struct md5 *m, const uint8_t *p, size_t n)
{
  m->len += n;
  while (n--)
    {
      m->buf[m->n++] = *p++;
      if (m->n == 64)
	{
	  block (m, m->buf);
	  m->n = 0;
	}
    }
}

static void
final (struct md5 *m, uint8_t out[16])
{
  uint64_t bits = m->len * 8;
  uint8_t pad = 0x80, z = 0;
  update (m, &pad, 1);
  while (m->n != 56)
    update (m, &z, 1);
  for (int i = 0; i < 8; i++)
    {
      uint8_t b = bits >> (8 * i);
      update (m, &b, 1);
    }
  uint32_t v[4] = { m->a, m->b, m->c, m->d };
  for (int i = 0; i < 16; i++)
    out[i] = v[i / 4] >> (8 * (i % 4));
}

static int
sum (FILE *f, const char *name)
{
  struct md5 m = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, {0}, 0 };
  static uint8_t buf[8192];
  size_t n;
  uint8_t d[16];
  while ((n = fread (buf, 1, sizeof (buf), f)) > 0)
    update (&m, buf, n);
  final (&m, d);
  for (int i = 0; i < 16; i++)
    printf ("%02x", d[i]);
  printf ("  %s\n", name);
  return 0;
}

int
main (int argc, char **argv)
{
  if (argc == 1)
    return sum (stdin, "-");
  int r = 0;
  for (int i = 1; i < argc; i++)
    {
      FILE *f = fopen (argv[i], "r");
      if (f == NULL)
	{
	  perror (argv[i]);
	  r = 1;
	  continue;
	}
      sum (f, argv[i]);
      fclose (f);
    }
  return r;
}
