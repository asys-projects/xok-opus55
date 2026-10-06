/*
 * CMOS real-time clock: wall clock time at boot.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"

static unsigned
cmos_read (unsigned reg)
{
  outb (0x70, reg);
  return inb (0x71);
}

static unsigned
bcd (unsigned v, bool isbcd)
{
  return isbcd ? (v & 0xf) + (v >> 4) * 10 : v;
}

static uint64_t
days_from_civil (int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint64_t) (era * 146097 + (int) doe - 719468);
}

uint64_t
rtc_read_unix (void)
{
  unsigned s, m, h, d, mo, y, regb;
  bool isbcd;

  /* Wait for the end of an update cycle. */
  for (int i = 0; i < 100000 && (cmos_read (0x0a) & 0x80); i++)
    ;
  s = cmos_read (0x00);
  m = cmos_read (0x02);
  h = cmos_read (0x04);
  d = cmos_read (0x07);
  mo = cmos_read (0x08);
  y = cmos_read (0x09);
  regb = cmos_read (0x0b);
  isbcd = !(regb & 0x04);
  s = bcd (s, isbcd);
  m = bcd (m, isbcd);
  h = bcd (h & 0x7f, isbcd) + (!(regb & 0x02) && (h & 0x80) ? 12 : 0);
  d = bcd (d, isbcd);
  mo = bcd (mo, isbcd);
  y = bcd (y, isbcd) + 2000;
  return days_from_civil (y, mo, d) * 86400 + h * 3600 + m * 60 + s;
}
