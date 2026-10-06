/*
 * Xok console: output goes to NUX's console (serial and VGA/frame
 * buffer); input comes from the serial line (COM1) and the PS/2
 * keyboard, and is queued for sys_cgetc.  The count of characters
 * received is published in the sysinfo page so that applications can
 * sleep on console input with a wakeup predicate.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "kern.h"
#include "dev.h"

#define COM1 0x3f8
#define COM1_IRQ 4
#define KBD_IRQ 1
#define KBD_DATA 0x60
#define KBD_STATUS 0x64

#define CONSBUFSZ 512

static struct
{
  uint8_t buf[CONSBUFSZ];
  uint32_t rpos, wpos;
} cons;

void
cons_intr (int c)
{
  if (c == 0)
    return;
  if (cons.wpos - cons.rpos >= CONSBUFSZ)
    return;
  cons.buf[cons.wpos++ % CONSBUFSZ] = c;
  sysinfo->si_cons_in++;
}

int
cons_getc (void)
{
  cons_poll ();
  if (cons.rpos == cons.wpos)
    return -E_AGAIN;
  return cons.buf[cons.rpos++ % CONSBUFSZ];
}

/*
 * Serial port.
 */
static bool serial_present;

static void
serial_rx (void)
{
  if (!serial_present)
    return;
  while (inb (COM1 + 5) & 0x01)
    {
      int c = inb (COM1);
      if (c == '\r')
	c = '\n';
      else if (c == 0x7f)
	c = '\b';
      cons_intr (c);
    }
}

/*
 * PS/2 keyboard (scan code set 1).
 */
#define SHIFT 1
#define CTL   2
#define CAPS  4
#define E0ESC 8

static const uint8_t normalmap[128] = {
  0, 0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
  0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};

static const uint8_t shiftmap[128] = {
  0, 0x1b, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
  0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static unsigned kbd_shift;

static void
kbd_rx (void)
{
  while (inb (KBD_STATUS) & 0x01)
    {
      unsigned st = inb (KBD_STATUS);
      uint8_t data = inb (KBD_DATA);
      int c;

      if (st & 0x20)
	continue;		/* Mouse data. */
      if (data == 0xe0)
	{
	  kbd_shift |= E0ESC;
	  continue;
	}
      if (data & 0x80)
	{
	  data &= 0x7f;
	  if (data == 0x2a || data == 0x36)
	    kbd_shift &= ~SHIFT;
	  else if (data == 0x1d)
	    kbd_shift &= ~CTL;
	  kbd_shift &= ~E0ESC;
	  continue;
	}
      if (kbd_shift & E0ESC)
	{
	  kbd_shift &= ~E0ESC;
	  if (data == 0x1d)
	    kbd_shift |= CTL;
	  continue;
	}
      if (data == 0x2a || data == 0x36)
	{
	  kbd_shift |= SHIFT;
	  continue;
	}
      if (data == 0x1d)
	{
	  kbd_shift |= CTL;
	  continue;
	}
      if (data == 0x3a)
	{
	  kbd_shift ^= CAPS;
	  continue;
	}
      c = (kbd_shift & SHIFT) ? shiftmap[data] : normalmap[data];
      if ((kbd_shift & CAPS) && c >= 'a' && c <= 'z')
	c -= 'a' - 'A';
      else if ((kbd_shift & CAPS) && c >= 'A' && c <= 'Z')
	c += 'a' - 'A';
      if ((kbd_shift & CTL) && c >= 'a' && c <= 'z')
	c -= 'a' - 1;
      cons_intr (c);
    }
}

void
cons_poll (void)
{
  serial_rx ();
  kbd_rx ();
}

static void
cons_irq (unsigned irq, void *arg)
{
  cons_poll ();
}

void
cons_init (void)
{
  /* The serial port is initialised by NUX; enable RX interrupts. */
  outb (COM1 + 7, 0x5a);
  serial_present = inb (COM1 + 7) == 0x5a;
  if (serial_present)
    {
      outb (COM1 + 1, 0x01);	/* IER: received data available. */
      outb (COM1 + 4, 0x0b);	/* MCR: DTR, RTS, OUT2 (IRQ enable). */
      (void) inb (COM1 + 2);
      (void) inb (COM1);
      irq_register (COM1_IRQ, cons_irq, NULL);
    }
  /* Flush the keyboard controller and enable keyboard IRQs. */
  while (inb (KBD_STATUS) & 0x01)
    (void) inb (KBD_DATA);
  irq_register (KBD_IRQ, cons_irq, NULL);
}
