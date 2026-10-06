/*
 * Xok network interface layer.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_KERN_NET_H
#define XOK_KERN_NET_H

#include "kern.h"

#define NET_MAXDEV XOK_MAXNET

struct netdev;

struct netdev_ops
{
  /* Transmit a frame (copied by the driver).  -E_FULL if busy. */
  int (*xmit) (struct netdev *d, const void *frame, unsigned len);
  /* Poll for received frames and completions. */
  void (*poll) (struct netdev *d);
};

struct netdev
{
  char name[16];
  uint8_t mac[6];
  unsigned mtu;
  unsigned idx;
  bool link;
  const struct netdev_ops *ops;
  void *drv;
};

int net_register (struct netdev *d);
void net_rx (struct netdev *d, const void *frame, unsigned len);
void net_link (struct netdev *d, bool up);

#endif
