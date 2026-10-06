/*
 * Power management: ACPI soft-off and reset.
 *
 * The FADT gives the PM1 control blocks and the reset register; the
 * S5 sleep type is found in the DSDT's \_S5 package.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include <nux/apxh.h>

static uint32_t pm1a_cnt, pm1b_cnt;
static uint16_t slp_typa, slp_typb;
static bool s5_valid;
static uint8_t reset_space, reset_value;
static uint64_t reset_addr;
static bool reset_valid;

static void *
map_table (uint64_t pa, uint32_t * len)
{
  uint8_t *hdr = kva_physmap (pa, 8, HAL_PTE_P);
  uint32_t l = *(uint32_t *) (hdr + 4);
  kva_unmap (hdr, 8);
  if (l < 36 || l > 1024 * 1024)
    return NULL;
  *len = l;
  return kva_physmap (pa, l, HAL_PTE_P);
}

static void
parse_dsdt (uint64_t pa)
{
  uint32_t len;
  uint8_t *t = map_table (pa, &len);

  if (t == NULL)
    return;
  for (uint32_t i = 36; i + 12 < len; i++)
    {
      if (memcmp (t + i, "_S5_", 4) != 0)
	continue;
      /* NameOp _S5_ PackageOp PkgLength NumElements ... */
      if (t[i - 1] != 0x08 && !(t[i - 2] == 0x08 && t[i - 1] == '\\'))
	continue;
      uint8_t *p = t + i + 4;
      if (*p != 0x12)
	continue;
      p++;
      p += ((*p & 0xc0) >> 6) + 1;	/* PkgLength. */
      p++;			/* NumElements. */
      if (*p == 0x0a)
	p++;
      slp_typa = *p++;
      if (*p == 0x0a)
	p++;
      slp_typb = *p;
      s5_valid = true;
      break;
    }
  kva_unmap (t, len);
}

static void
parse_fadt (uint64_t pa)
{
  uint32_t len;
  uint8_t *f = map_table (pa, &len);

  if (f == NULL)
    return;
  pm1a_cnt = *(uint32_t *) (f + 64);
  pm1b_cnt = *(uint32_t *) (f + 68);
  if (len >= 129 && (*(uint32_t *) (f + 112) & (1 << 10)))
    {
      reset_space = f[116];
      reset_addr = *(uint64_t *) (f + 120);
      reset_value = f[128];
      reset_valid = true;
    }
  parse_dsdt (*(uint32_t *) (f + 40));
  kva_unmap (f, len);
}

void
power_init (void)
{
  const struct apxh_pltdesc *pd = hal_pltinfo ();
  uint8_t *rsdp;
  uint64_t sdt;
  bool xsdt;
  uint32_t len;
  uint8_t *t;

  if (pd == NULL || pd->type != PLT_ACPI)
    return;
  rsdp = kva_physmap (pd->pltptr, 36, HAL_PTE_P);
  xsdt = rsdp[15] >= 2 && *(uint64_t *) (rsdp + 24) != 0;
  sdt = xsdt ? *(uint64_t *) (rsdp + 24) : *(uint32_t *) (rsdp + 16);
  kva_unmap (rsdp, 36);

  t = map_table (sdt, &len);
  if (t == NULL)
    return;
  for (uint32_t off = 36; off < len; off += xsdt ? 8 : 4)
    {
      uint64_t pa = xsdt ? *(uint64_t *) (t + off) : *(uint32_t *) (t + off);
      uint8_t *h = kva_physmap (pa, 8, HAL_PTE_P);
      bool facp = memcmp (h, "FACP", 4) == 0;
      kva_unmap (h, 8);
      if (facp)
	parse_fadt (pa);
    }
  kva_unmap (t, len);
  kprintf ("acpi: PM1a_CNT %x S5 %s (%x) reset %s\n", pm1a_cnt,
	   s5_valid ? "found" : "not found", slp_typa,
	   reset_valid ? "register" : "keyboard controller");
}

void __attribute__ ((noreturn)) power_reset (int poweroff)
{
  kprintf ("xok: %s\n", poweroff ? "power off" : "reboot");
  if (poweroff)
    {
      if (s5_valid && pm1a_cnt)
	{
	  outw (pm1a_cnt, (slp_typa << 10) | (1 << 13));
	  if (pm1b_cnt)
	    outw (pm1b_cnt, (slp_typb << 10) | (1 << 13));
	}
      /* Known emulator ports. */
      outw (0x604, 0x2000);
      outw (0xb004, 0x2000);
      kprintf ("xok: power off failed, halting\n");
      while (1)
	asm volatile ("cli; hlt");
    }

  if (reset_valid && reset_space == 1)
    outb (reset_addr, reset_value);
  outb (0x64, 0xfe);
  outb (0xcf9, 0x06);
  /* Triple fault. */
  {
    struct
    {
      uint16_t lim;
      uint32_t base;
    } __attribute__ ((packed)) idt = { 0, 0 };
    asm volatile ("lidt %0; int3"::"m" (idt));
  }
  while (1)
    asm volatile ("cli; hlt");
}
