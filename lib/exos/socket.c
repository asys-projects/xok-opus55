/*
 * ExOS sockets: UDP and TCP, in the application.
 *
 * Each listening or connecting socket has its own packet ring and DPF
 * filter; connections accepted from a listening socket share its ring
 * and are demultiplexed here (a second, user-level demultiplexing
 * step, as in ExOS).  Packets are processed whenever the application
 * calls into the stack, at the beginning of each time slice (a
 * context-switch add-on), and when a wakeup predicate including the
 * rings and the TCP timers fires (a wakeup-predicate add-on).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/net.h>
#include <exos/fd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NSOCK 64
#define TCP_BUFSZ 65536
#define TCP_MSS 1460
#define RING_N 64

#define SEQ_LT(a,b) ((int32_t) ((a) - (b)) < 0)
#define SEQ_LE(a,b) ((int32_t) ((a) - (b)) <= 0)
#define SEQ_GT(a,b) ((int32_t) ((a) - (b)) > 0)
#define SEQ_GE(a,b) ((int32_t) ((a) - (b)) >= 0)

enum
{ TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
  TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK,
  TCP_TIME_WAIT
};

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

struct tcp_hdr
{
  uint16_t sport, dport;
  uint32_t seq, ack;
  uint8_t off, flags;
  uint16_t win, sum, urp;
} __attribute__ ((packed));

struct udp_hdr
{
  uint16_t sport, dport, len, sum;
} __attribute__ ((packed));

struct sock
{
  int used;
  int type;
  int refs;			/* File descriptors / file table entries. */
  uint32_t lip, rip;
  uint16_t lport, rport;
  int filter;
  struct ring ring;
  struct ring *rx;		/* Ring we receive from (ours or listener's). */
  int nonblock;
  int error;
  /* TCP. */
  int state;
  uint32_t iss, snd_una, snd_nxt, snd_wnd, sbuf_seq;
  uint32_t irs, rcv_nxt;
  uint16_t mss;
  uint8_t *sbuf;
  unsigned slen;
  int fin_pending, fin_sent;
  uint8_t *rbuf;
  unsigned rhead, rlen;
  int rcv_fin;
  uint32_t rto, rtx_deadline, tw_deadline;
  int rtx_count;
  int listener;			/* Accepted connection: index of listener. */
  int acceptq[SOMAXCONN];
  int nacceptq, backlog;
  int closed;			/* Application closed it. */
  uint32_t last_adv;		/* Last advertised window edge. */
};

static struct sock socks[NSOCK];
static int in_stack;
static int net_initialised;

static uint32_t
now_ms (void)
{
  return (uint32_t) (exos_time_ns () / 1000000);
}

static void net_setup (void);

static struct sock *
sock_alloc (int type)
{
  for (int i = 0; i < NSOCK; i++)
    if (!socks[i].used)
      {
	struct sock *s = &socks[i];
	memset (s, 0, sizeof (*s));
	s->used = 1;
	s->type = type;
	s->filter = -1;
	s->ring.id = -1;
	s->listener = -1;
	s->state = TCP_CLOSED;
	s->mss = 536;
	return s;
      }
  return NULL;
}

static int
sock_index (struct sock *s)
{
  return s - socks;
}

static void
sock_free (struct sock *s)
{
  if (s->filter >= 0)
    sys_dpf_delete (EXOS_CAP, s->filter);
  if (s->ring.id >= 0)
    ring_destroy (&s->ring);
  free (s->sbuf);
  free (s->rbuf);
  if (s->listener >= 0)
    {
      struct sock *l = &socks[s->listener];
      l->refs--;
      if (l->refs <= 0 && l->closed)
	sock_free (l);
    }
  memset (s, 0, sizeof (*s));
}

static uint16_t
ephemeral_port (void)
{
  static uint16_t next;
  if (next == 0)
    next = 49152 + ((exos_time_ns () / 1000) ^ (__envid * 77)) % 16000;
  if (++next >= 65000)
    next = 49152;
  return htons (next);
}

/*
 * TCP output.
 */
static unsigned
rwin (struct sock *s)
{
  unsigned w = TCP_BUFSZ - s->rlen;
  return w > 65535 ? 65535 : w;
}

static int
tcp_send (struct sock *s, uint8_t flags, uint32_t seq, const void *data,
	  unsigned len)
{
  struct
  {
    struct tcp_hdr h;
    uint8_t opt[4];
  } __attribute__ ((packed)) seg;
  unsigned hlen = sizeof (struct tcp_hdr);
  uint32_t sum;
  struct
  {
    uint32_t src, dst;
    uint8_t zero, proto;
    uint16_t len;
  } __attribute__ ((packed)) ph;

  memset (&seg, 0, sizeof (seg));
  seg.h.sport = s->lport;
  seg.h.dport = s->rport;
  seg.h.seq = htonl (seq);
  seg.h.ack = (flags & TH_ACK) ? htonl (s->rcv_nxt) : 0;
  seg.h.flags = flags;
  seg.h.win = htons (rwin (s));
  if (flags & TH_SYN)
    {
      seg.opt[0] = 2;
      seg.opt[1] = 4;
      seg.opt[2] = TCP_MSS >> 8;
      seg.opt[3] = TCP_MSS & 0xff;
      hlen += 4;
    }
  seg.h.off = (hlen / 4) << 4;
  ph.src = netcfg->ip;
  ph.dst = s->rip;
  ph.zero = 0;
  ph.proto = IPPROTO_TCP;
  ph.len = htons (hlen + len);
  sum = in_cksum_add (&ph, sizeof (ph), 0);
  sum = in_cksum_add (&seg, hlen, sum);
  /* Data may start on an odd boundary only if hlen is odd: it is not. */
  seg.h.sum = in_cksum (data, len, sum);
  if (flags & TH_ACK)
    s->last_adv = s->rcv_nxt + rwin (s);
  return ip_output (s->rip, IPPROTO_TCP, &seg, hlen, data, len);
}

static void
tcp_set_rtx (struct sock *s)
{
  if (s->rtx_deadline == 0)
    s->rtx_deadline = now_ms () + s->rto;
}

static void
tcp_output (struct sock *s)
{
  uint32_t wnd = s->snd_wnd ? s->snd_wnd : 1;
  uint32_t cwnd = 16 * TCP_MSS;

  if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT
      && s->state != TCP_FIN_WAIT_1 && s->state != TCP_CLOSING
      && s->state != TCP_LAST_ACK)
    return;
  if (wnd > cwnd)
    wnd = cwnd;
  for (;;)
    {
      uint32_t sent = s->snd_nxt - s->sbuf_seq;	/* Data bytes sent. */
      uint32_t inflight = s->snd_nxt - s->snd_una;
      if (sent > s->slen)
	sent = s->slen;		/* FIN was sent. */
      uint32_t unsent = s->slen - sent;
      if (unsent == 0 || inflight >= wnd)
	break;
      uint32_t n = unsent;
      if (n > s->mss)
	n = s->mss;
      if (n > wnd - inflight)
	n = wnd - inflight;
      tcp_send (s, TH_ACK | (n == unsent ? TH_PSH : 0), s->snd_nxt,
		s->sbuf + sent, n);
      s->snd_nxt += n;
      tcp_set_rtx (s);
    }
  if (s->fin_pending && !s->fin_sent
      && s->snd_nxt - s->sbuf_seq == s->slen)
    {
      tcp_send (s, TH_FIN | TH_ACK, s->snd_nxt, NULL, 0);
      s->snd_nxt++;
      s->fin_sent = 1;
      tcp_set_rtx (s);
      if (s->state == TCP_ESTABLISHED)
	s->state = TCP_FIN_WAIT_1;
      else if (s->state == TCP_CLOSE_WAIT)
	s->state = TCP_LAST_ACK;
    }
}

static void
tcp_reset_reply (uint32_t src, uint32_t dst, const struct tcp_hdr *th,
		 unsigned dlen)
{
  struct sock tmp;

  if (th->flags & TH_RST)
    return;
  memset (&tmp, 0, sizeof (tmp));
  tmp.lport = th->dport;
  tmp.rport = th->sport;
  tmp.rip = src;
  if (th->flags & TH_ACK)
    tcp_send (&tmp, TH_RST, ntohl (th->ack), NULL, 0);
  else
    {
      tmp.rcv_nxt = ntohl (th->seq) + dlen + ((th->flags & TH_SYN) ? 1 : 0);
      tcp_send (&tmp, TH_RST | TH_ACK, 0, NULL, 0);
    }
}

static void
tcp_drop (struct sock *s, int err)
{
  s->error = err;
  s->state = TCP_CLOSED;
  s->rtx_deadline = 0;
  if (s->closed)
    sock_free (s);
}

static void
tcp_parse_mss (struct sock *s, const struct tcp_hdr *th, unsigned hlen)
{
  const uint8_t *o = (const uint8_t *) th + sizeof (*th);
  const uint8_t *end = (const uint8_t *) th + hlen;

  while (o < end)
    {
      if (o[0] == 0)
	break;
      if (o[0] == 1)
	{
	  o++;
	  continue;
	}
      if (o + 1 >= end || o[1] < 2)
	break;
      if (o[0] == 2 && o[1] == 4 && o + 4 <= end)
	{
	  unsigned m = (o[2] << 8) | o[3];
	  if (m >= 64)
	    s->mss = m < TCP_MSS ? m : TCP_MSS;
	}
      o += o[1];
    }
}

static int
tcp_bufs (struct sock *s)
{
  if (s->sbuf == NULL)
    s->sbuf = malloc (TCP_BUFSZ);
  if (s->rbuf == NULL)
    s->rbuf = malloc (TCP_BUFSZ);
  return s->sbuf && s->rbuf ? 0 : -ENOMEM;
}

static void
tcp_input (struct ip_info *ip, int ringowner)
{
  const struct tcp_hdr *th = (const struct tcp_hdr *) ip->payload;
  unsigned hlen, dlen;
  uint32_t seq, ack, sum;
  const uint8_t *data;
  struct sock *s = NULL;
  struct
  {
    uint32_t src, dst;
    uint8_t zero, proto;
    uint16_t len;
  } __attribute__ ((packed)) ph;

  if (ip->plen < sizeof (*th))
    return;
  hlen = (th->off >> 4) * 4;
  if (hlen < sizeof (*th) || hlen > ip->plen)
    return;
  ph.src = ip->src;
  ph.dst = ip->dst;
  ph.zero = 0;
  ph.proto = IPPROTO_TCP;
  ph.len = htons (ip->plen);
  sum = in_cksum_add (&ph, sizeof (ph), 0);
  if (in_cksum (ip->payload, ip->plen, sum) != 0)
    return;
  dlen = ip->plen - hlen;
  data = ip->payload + hlen;
  seq = ntohl (th->seq);
  ack = ntohl (th->ack);

  for (int i = 0; i < NSOCK; i++)
    if (socks[i].used && socks[i].type == SOCK_STREAM
	&& socks[i].state != TCP_LISTEN && socks[i].state != TCP_CLOSED
	&& socks[i].lport == th->dport && socks[i].rport == th->sport
	&& socks[i].rip == ip->src)
      {
	s = &socks[i];
	break;
      }

  if (s == NULL)
    {
      /* A new connection to a listening socket? */
      struct sock *l = NULL;
      for (int i = 0; i < NSOCK; i++)
	if (socks[i].used && socks[i].state == TCP_LISTEN
	    && socks[i].lport == th->dport && !socks[i].closed)
	  l = &socks[i];
      if (l == NULL || (th->flags & (TH_SYN | TH_ACK | TH_RST)) != TH_SYN)
	{
	  tcp_reset_reply (ip->src, ip->dst, th, dlen);
	  return;
	}
      int pending = 0;
      for (int i = 0; i < NSOCK; i++)
	if (socks[i].used && socks[i].listener == sock_index (l)
	    && socks[i].state == TCP_SYN_RCVD)
	  pending++;
      if (l->nacceptq + pending >= l->backlog)
	return;			/* Drop: the client will retry. */
      s = sock_alloc (SOCK_STREAM);
      if (s == NULL || tcp_bufs (s) < 0)
	{
	  if (s)
	    sock_free (s);
	  return;
	}
      s->lip = ip->dst;
      s->rip = ip->src;
      s->lport = th->dport;
      s->rport = th->sport;
      s->listener = sock_index (l);
      l->refs++;
      s->rx = l->rx;
      s->irs = seq;
      s->rcv_nxt = seq + 1;
      s->iss = (uint32_t) exos_time_ns () ^ (sock_index (s) << 20);
      s->snd_una = s->iss;
      s->snd_nxt = s->iss + 1;
      s->sbuf_seq = s->iss + 1;
      s->snd_wnd = ntohs (th->win);
      s->rto = 1000;
      s->state = TCP_SYN_RCVD;
      tcp_parse_mss (s, th, hlen);
      tcp_send (s, TH_SYN | TH_ACK, s->iss, NULL, 0);
      tcp_set_rtx (s);
      return;
    }

  if (s->state == TCP_SYN_SENT)
    {
      if ((th->flags & TH_ACK) && ack != s->iss + 1)
	{
	  tcp_reset_reply (ip->src, ip->dst, th, dlen);
	  return;
	}
      if (th->flags & TH_RST)
	{
	  if (th->flags & TH_ACK)
	    tcp_drop (s, ECONNREFUSED);
	  return;
	}
      if (!(th->flags & TH_SYN) || !(th->flags & TH_ACK))
	return;
      s->irs = seq;
      s->rcv_nxt = seq + 1;
      s->snd_una = ack;
      s->snd_wnd = ntohs (th->win);
      tcp_parse_mss (s, th, hlen);
      s->state = TCP_ESTABLISHED;
      s->rtx_deadline = 0;
      s->rtx_count = 0;
      tcp_send (s, TH_ACK, s->snd_nxt, NULL, 0);
      tcp_output (s);
      return;
    }

  if (th->flags & TH_RST)
    {
      if (SEQ_GE (seq, s->rcv_nxt) && SEQ_LT (seq, s->rcv_nxt + 65536))
	{
	  if (s->state == TCP_SYN_RCVD)
	    sock_free (s);
	  else
	    tcp_drop (s, ECONNRESET);
	}
      return;
    }
  if (th->flags & TH_SYN)
    {
      /* Retransmitted SYN, or SYN on a synchronised connection. */
      if (s->state == TCP_SYN_RCVD)
	tcp_send (s, TH_SYN | TH_ACK, s->iss, NULL, 0);
      else
	tcp_send (s, TH_ACK, s->snd_nxt, NULL, 0);
      return;
    }
  if (!(th->flags & TH_ACK))
    return;

  /* ACK processing. */
  if (s->state == TCP_SYN_RCVD)
    {
      if (ack != s->iss + 1)
	{
	  tcp_reset_reply (ip->src, ip->dst, th, dlen);
	  return;
	}
      s->snd_una = ack;
      s->state = TCP_ESTABLISHED;
      s->rtx_deadline = 0;
      s->rtx_count = 0;
      struct sock *l = &socks[s->listener];
      if (l->used && l->state == TCP_LISTEN && l->nacceptq < SOMAXCONN)
	l->acceptq[l->nacceptq++] = sock_index (s);
      else
	{
	  tcp_reset_reply (ip->src, ip->dst, th, dlen);
	  sock_free (s);
	  return;
	}
    }
  if (SEQ_GT (ack, s->snd_una) && SEQ_LE (ack, s->snd_nxt))
    {
      uint32_t acked_data = 0;
      if (SEQ_GT (ack, s->sbuf_seq))
	{
	  acked_data = ack - s->sbuf_seq;
	  if (acked_data > s->slen)
	    acked_data = s->slen;	/* The rest is our FIN. */
	}
      if (acked_data)
	{
	  memmove (s->sbuf, s->sbuf + acked_data, s->slen - acked_data);
	  s->slen -= acked_data;
	  s->sbuf_seq += acked_data;
	}
      s->snd_una = ack;
      s->rtx_count = 0;
      s->rto = 1000;
      s->rtx_deadline = 0;
      if (s->snd_una != s->snd_nxt)
	tcp_set_rtx (s);
      if (s->fin_sent && s->snd_una == s->snd_nxt)
	{
	  if (s->state == TCP_FIN_WAIT_1)
	    s->state = TCP_FIN_WAIT_2;
	  else if (s->state == TCP_CLOSING)
	    {
	      s->state = TCP_TIME_WAIT;
	      s->tw_deadline = now_ms () + 2000;
	    }
	  else if (s->state == TCP_LAST_ACK)
	    {
	      s->state = TCP_CLOSED;
	      if (s->closed)
		{
		  sock_free (s);
		  return;
		}
	    }
	}
    }
  if (SEQ_GE (ack, s->snd_una))
    s->snd_wnd = ntohs (th->win);

  /* Data. */
  int need_ack = 0;
  if (dlen > 0)
    {
      if (seq == s->rcv_nxt && !s->rcv_fin)
	{
	  unsigned room = TCP_BUFSZ - s->rlen;
	  unsigned n = dlen < room ? dlen : room;
	  for (unsigned k = 0; k < n; k++)
	    s->rbuf[(s->rhead + s->rlen + k) % TCP_BUFSZ] = data[k];
	  s->rlen += n;
	  s->rcv_nxt += n;
	  if (n < dlen)
	    dlen = n;		/* Rest dropped: no FIN processing. */
	}
      need_ack = 1;
    }
  if ((th->flags & TH_FIN) && seq + dlen == s->rcv_nxt && !s->rcv_fin)
    {
      s->rcv_nxt++;
      s->rcv_fin = 1;
      need_ack = 1;
      switch (s->state)
	{
	case TCP_ESTABLISHED:
	  s->state = TCP_CLOSE_WAIT;
	  break;
	case TCP_FIN_WAIT_1:
	  if (s->fin_sent && s->snd_una == s->snd_nxt)
	    {
	      s->state = TCP_TIME_WAIT;
	      s->tw_deadline = now_ms () + 2000;
	    }
	  else
	    s->state = TCP_CLOSING;
	  break;
	case TCP_FIN_WAIT_2:
	  s->state = TCP_TIME_WAIT;
	  s->tw_deadline = now_ms () + 2000;
	  break;
	}
    }
  if (need_ack)
    tcp_send (s, TH_ACK, s->snd_nxt, NULL, 0);
  tcp_output (s);
}

static void
tcp_timers (void)
{
  uint32_t t = now_ms ();

  for (int i = 0; i < NSOCK; i++)
    {
      struct sock *s = &socks[i];
      if (!s->used || s->type != SOCK_STREAM)
	continue;
      if (s->state == TCP_TIME_WAIT && (int32_t) (t - s->tw_deadline) >= 0)
	{
	  s->state = TCP_CLOSED;
	  if (s->closed)
	    sock_free (s);
	  continue;
	}
      if (s->rtx_deadline && (int32_t) (t - s->rtx_deadline) >= 0)
	{
	  s->rtx_deadline = 0;
	  if (++s->rtx_count > 8)
	    {
	      if (s->state == TCP_SYN_RCVD)
		sock_free (s);
	      else
		tcp_drop (s, ETIMEDOUT);
	      continue;
	    }
	  s->rto = s->rto * 2 > 8000 ? 8000 : s->rto * 2;
	  switch (s->state)
	    {
	    case TCP_SYN_SENT:
	      tcp_send (s, TH_SYN, s->iss, NULL, 0);
	      tcp_set_rtx (s);
	      break;
	    case TCP_SYN_RCVD:
	      tcp_send (s, TH_SYN | TH_ACK, s->iss, NULL, 0);
	      tcp_set_rtx (s);
	      break;
	    default:
	      /* Go back N. */
	      s->snd_nxt = s->snd_una;
	      if (s->fin_sent && SEQ_LT (s->snd_una, s->sbuf_seq + s->slen + 1))
		s->fin_sent = 0;
	      if (s->snd_wnd == 0)
		s->snd_wnd = 1;	/* Window probe. */
	      tcp_output (s);
	      if (s->snd_una != s->snd_nxt)
		tcp_set_rtx (s);
	    }
	}
    }
}

uint32_t
net_next_timer (void)
{
  uint32_t best = 0;
  for (int i = 0; i < NSOCK; i++)
    {
      struct sock *s = &socks[i];
      uint32_t d = 0;
      if (!s->used)
	continue;
      if (s->rtx_deadline)
	d = s->rtx_deadline;
      if (s->state == TCP_TIME_WAIT && (!d || SEQ_LT (s->tw_deadline, d)))
	d = s->tw_deadline;
      if (d && (!best || SEQ_LT (d, best)))
	best = d;
    }
  return best;
}

/*
 * Packet processing.
 */
static void
process_ring (struct sock *owner)
{
  struct ring *r = &owner->ring;

  while (ring_pending (r))
    {
      uint8_t *frame = r->bufs + r->next * RING_BUFSZ;
      unsigned len = r->flags[r->next];
      struct ip_info ip;

      if (len > RING_BUFSZ)
	len = RING_BUFSZ;
      if (owner->type == SOCK_DGRAM)
	return;			/* Datagrams are read by recvfrom. */
      if (ip_parse (frame, len, &ip) == 0 && ip.proto == IPPROTO_TCP)
	tcp_input (&ip, sock_index (owner));
      /* The segment may have closed and freed the ring's socket. */
      if (!owner->used || owner->ring.id < 0)
	return;
      r->flags[r->next] = 0;
      r->next = (r->next + 1) % r->n;
    }
}

void
net_poll (void)
{
  if (in_stack)
    return;
  in_stack = 1;
  for (int i = 0; i < NSOCK; i++)
    if (socks[i].used && socks[i].ring.id >= 0)
      process_ring (&socks[i]);
  tcp_timers ();
  in_stack = 0;
}

static void
net_cswitch (void)
{
  net_poll ();
}

static int
net_wk_addon (struct wk_term *t, int max)
{
  int n = 0;
  uint32_t timer;

  for (int i = 0; i < NSOCK && n + 2 < max; i++)
    {
      struct sock *s = &socks[i];
      if (!s->used || s->ring.id < 0)
	continue;
      if (n)
	t[n++].wk_op = WK_OR;
      memset (&t[n], 0, sizeof (t[n]));
      t[n].wk_op = WK_NE;
      t[n].wk_lkind = WK_MEM32;
      t[n].wk_lval = (uint32_t) & s->ring.flags[s->ring.next];
      t[n].wk_rkind = WK_CONST;
      t[n].wk_rval = 0;
      n++;
    }
  timer = net_next_timer ();
  if (timer && n + 2 < max)
    {
      if (n)
	t[n++].wk_op = WK_OR;
      memset (&t[n], 0, sizeof (t[n]));
      t[n].wk_op = WK_GE;
      t[n].wk_lkind = WK_TIME;
      t[n].wk_rkind = WK_CONST;
      t[n].wk_rval = timer;
      n++;
    }
  return n;
}

static void
net_exit (void)
{
  /* Give our connections a chance to deliver their data. */
  for (int round = 0; round < 50; round++)
    {
      int busy = 0;
      net_poll ();
      for (int i = 0; i < NSOCK; i++)
	if (socks[i].used && socks[i].type == SOCK_STREAM
	    && (socks[i].slen || (socks[i].fin_pending && !socks[i].fin_sent)
		|| socks[i].state == TCP_FIN_WAIT_1
		|| socks[i].state == TCP_LAST_ACK
		|| socks[i].state == TCP_CLOSING))
	  busy = 1;
      if (!busy)
	break;
      usleep (20000);
    }
}

void (*__exos_net_exit) (void);

static void
net_setup (void)
{
  if (net_initialised)
    return;
  net_initialised = 1;
  exos_add_cswitch (net_cswitch);
  exos_add_wk (net_wk_addon, net_poll);
  __exos_net_exit = net_exit;
}

/* Sleep until something happens on the network (or MS elapse). */
static void
net_sleep (uint32_t ms)
{
  struct wk_term t;

  memset (&t, 0, sizeof (t));
  t.wk_op = WK_GE;
  t.wk_lkind = WK_TIME;
  t.wk_rkind = WK_CONST;
  t.wk_rval = now_ms () + ms;
  exos_wkpred (&t, 1);
}

/*
 * File descriptor glue.
 */
static struct sock *
fd_sock (int fd, struct file **fp)
{
  struct file *f = fd_file (fd);

  if (f == NULL)
    {
      errno = EBADF;
      return NULL;
    }
  if (f->f_type != FT_SOCK)
    {
      errno = ENOTSOCK;
      return NULL;
    }
  if (f->u.sock.owner != __envid || f->u.sock.id < 0
      || f->u.sock.id >= NSOCK || !socks[f->u.sock.id].used)
    {
      /* Sockets live in the creating process' stack. */
      errno = EBADF;
      return NULL;
    }
  if (fp)
    *fp = f;
  return &socks[f->u.sock.id];
}

static int
sock_fd (struct sock *s)
{
  struct file *f;
  int fd, r;

  if ((r = fd_alloc_file (FT_SOCK, O_RDWR, &f)) < 0)
    {
      errno = -r;
      return -1;
    }
  f->u.sock.id = sock_index (s);
  f->u.sock.owner = __envid;
  if ((fd = fd_install (f, 0)) < 0)
    {
      f->f_ref = 0;
      errno = EMFILE;
      return -1;
    }
  s->refs++;
  return fd;
}

int
socket (int domain, int type, int proto)
{
  struct sock *s;
  int fd;

  if (domain != AF_INET)
    {
      errno = EAFNOSUPPORT;
      return -1;
    }
  if (type != SOCK_STREAM && type != SOCK_DGRAM)
    {
      errno = EPROTOTYPE;
      return -1;
    }
  if (net_wait_config (5000) < 0)
    {
      errno = ENETDOWN;
      return -1;
    }
  net_setup ();
  s = sock_alloc (type);
  if (s == NULL)
    {
      errno = ENFILE;
      return -1;
    }
  fd = sock_fd (s);
  if (fd < 0)
    sock_free (s);
  return fd;
}

static int
attach_filter (struct sock *s, uint8_t proto, uint32_t srcip, uint16_t sport)
{
  struct dpf_atom a[8];
  int n, r;

  if (s->ring.id < 0 && (r = ring_create (&s->ring, RING_N)) < 0)
    return r;
  s->rx = &s->ring;
  n = dpf_build_ip (a, proto, netcfg->ip, s->lport, srcip, sport);
  r = sys_dpf_insert (EXOS_CAP, a, n, s->ring.id);
  if (r < 0)
    return r == -E_CONFLICT ? -EADDRINUSE : -EIO;
  s->filter = r;
  return 0;
}

int
bind (int fd, const struct sockaddr *a, socklen_t len)
{
  const struct sockaddr_in *sin = (const struct sockaddr_in *) a;
  struct sock *s = fd_sock (fd, NULL);
  int r;

  if (s == NULL)
    return -1;
  if (len < sizeof (*sin) || sin->sin_family != AF_INET)
    {
      errno = EINVAL;
      return -1;
    }
  if (s->lport)
    {
      errno = EINVAL;
      return -1;
    }
  s->lport = sin->sin_port ? sin->sin_port : ephemeral_port ();
  s->lip = netcfg->ip;
  if (s->type == SOCK_DGRAM)
    {
      r = attach_filter (s, IPPROTO_UDP, 0, 0);
      if (r < 0)
	{
	  s->lport = 0;
	  errno = -r;
	  return -1;
	}
    }
  return 0;
}

int
listen (int fd, int backlog)
{
  struct sock *s = fd_sock (fd, NULL);
  int r;

  if (s == NULL)
    return -1;
  if (s->type != SOCK_STREAM || s->lport == 0)
    {
      errno = EINVAL;
      return -1;
    }
  if (s->state == TCP_LISTEN)
    return 0;
  r = attach_filter (s, IPPROTO_TCP, 0, 0);
  if (r < 0)
    {
      errno = -r;
      return -1;
    }
  s->backlog = backlog <= 0 || backlog > SOMAXCONN ? SOMAXCONN : backlog;
  s->state = TCP_LISTEN;
  return 0;
}

int
accept (int fd, struct sockaddr *a, socklen_t *len)
{
  struct file *f;
  struct sock *s = fd_sock (fd, &f), *c;
  int nfd;

  if (s == NULL)
    return -1;
  if (s->state != TCP_LISTEN)
    {
      errno = EINVAL;
      return -1;
    }
  for (;;)
    {
      net_poll ();
      if (s->nacceptq > 0)
	break;
      if ((f->f_flags & O_NONBLOCK) || s->nonblock)
	{
	  errno = EAGAIN;
	  return -1;
	}
      net_sleep (1000);
      if (__proc->sigpending)
	{
	  errno = EINTR;
	  return -1;
	}
    }
  c = &socks[s->acceptq[0]];
  memmove (s->acceptq, s->acceptq + 1, (--s->nacceptq) * sizeof (int));
  nfd = sock_fd (c);
  if (nfd < 0)
    return -1;
  if (a && len && *len >= sizeof (struct sockaddr_in))
    {
      struct sockaddr_in *sin = (struct sockaddr_in *) a;
      memset (sin, 0, sizeof (*sin));
      sin->sin_family = AF_INET;
      sin->sin_port = c->rport;
      sin->sin_addr.s_addr = c->rip;
      *len = sizeof (*sin);
    }
  return nfd;
}

int
connect (int fd, const struct sockaddr *a, socklen_t len)
{
  const struct sockaddr_in *sin = (const struct sockaddr_in *) a;
  struct file *f;
  struct sock *s = fd_sock (fd, &f);
  int r;

  if (s == NULL)
    return -1;
  if (len < sizeof (*sin) || sin->sin_family != AF_INET)
    {
      errno = EINVAL;
      return -1;
    }
  if (s->type == SOCK_DGRAM)
    {
      if (s->lport == 0)
	{
	  struct sockaddr_in me = {.sin_family = AF_INET };
	  if (bind (fd, (struct sockaddr *) &me, sizeof (me)) < 0)
	    return -1;
	}
      s->rip = sin->sin_addr.s_addr;
      s->rport = sin->sin_port;
      return 0;
    }
  if (s->state != TCP_CLOSED)
    {
      errno = EISCONN;
      return -1;
    }
  s->rip = sin->sin_addr.s_addr;
  s->rport = sin->sin_port;
  if (s->lport == 0)
    s->lport = ephemeral_port ();
  s->lip = netcfg->ip;
  if ((r = tcp_bufs (s)) < 0 || (r = attach_filter (s, IPPROTO_TCP, s->rip,
						    s->rport)) < 0)
    {
      errno = -r;
      return -1;
    }
  s->iss = (uint32_t) exos_time_ns () ^ (sock_index (s) << 20);
  s->snd_una = s->iss;
  s->snd_nxt = s->iss + 1;
  s->sbuf_seq = s->iss + 1;
  s->rto = 1000;
  s->state = TCP_SYN_SENT;
  tcp_send (s, TH_SYN, s->iss, NULL, 0);
  tcp_set_rtx (s);
  for (;;)
    {
      net_poll ();
      if (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT)
	return 0;
      if (s->state == TCP_CLOSED)
	{
	  errno = s->error ? s->error : ECONNREFUSED;
	  return -1;
	}
      net_sleep (1000);
    }
}

static ssize_t
tcp_write (struct sock *s, int nonblock, const void *buf, size_t n)
{
  size_t done = 0;

  while (done < n)
    {
      net_poll ();
      if (s->error)
	{
	  errno = s->error;
	  return done ? (ssize_t) done : -1;
	}
      if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
	{
	  errno = s->state == TCP_SYN_SENT ? ENOTCONN : EPIPE;
	  return done ? (ssize_t) done : -1;
	}
      size_t room = TCP_BUFSZ - s->slen;
      if (room == 0)
	{
	  if (nonblock)
	    break;
	  in_stack = 1;
	  tcp_output (s);
	  in_stack = 0;
	  net_sleep (500);
	  continue;
	}
      size_t m = n - done < room ? n - done : room;
      memcpy (s->sbuf + s->slen, (const char *) buf + done, m);
      s->slen += m;
      done += m;
      in_stack = 1;
      tcp_output (s);
      in_stack = 0;
    }
  if (done == 0 && n)
    {
      errno = EAGAIN;
      return -1;
    }
  return done;
}

static ssize_t
tcp_read (struct sock *s, int nonblock, void *buf, size_t n, int peek)
{
  for (;;)
    {
      net_poll ();
      if (s->rlen > 0)
	{
	  size_t m = n < s->rlen ? n : s->rlen;
	  for (size_t k = 0; k < m; k++)
	    ((char *) buf)[k] = s->rbuf[(s->rhead + k) % TCP_BUFSZ];
	  if (!peek)
	    {
	      s->rhead = (s->rhead + m) % TCP_BUFSZ;
	      s->rlen -= m;
	      /* Window update if the window grew a lot. */
	      if (SEQ_GE (s->rcv_nxt + rwin (s), s->last_adv + TCP_BUFSZ / 4))
		{
		  in_stack = 1;
		  tcp_send (s, TH_ACK, s->snd_nxt, NULL, 0);
		  in_stack = 0;
		}
	    }
	  return m;
	}
      if (s->rcv_fin || s->state == TCP_CLOSED)
	{
	  if (s->error && s->error != ECONNRESET)
	    {
	      errno = s->error;
	      return -1;
	    }
	  return 0;
	}
      if (nonblock)
	{
	  errno = EAGAIN;
	  return -1;
	}
      net_sleep (1000);
      if (__proc->sigpending)
	{
	  errno = EINTR;
	  return -1;
	}
    }
}

/* UDP. */
static ssize_t
udp_send (struct sock *s, const void *buf, size_t n, uint32_t ip,
	  uint16_t port)
{
  struct udp_hdr uh;
  struct
  {
    uint32_t src, dst;
    uint8_t zero, proto;
    uint16_t len;
  } __attribute__ ((packed)) ph;
  uint32_t sum;
  int r;

  if (n > 1472)
    {
      errno = EMSGSIZE;
      return -1;
    }
  uh.sport = s->lport;
  uh.dport = port;
  uh.len = htons (sizeof (uh) + n);
  uh.sum = 0;
  ph.src = netcfg->ip;
  ph.dst = ip;
  ph.zero = 0;
  ph.proto = IPPROTO_UDP;
  ph.len = uh.len;
  sum = in_cksum_add (&ph, sizeof (ph), 0);
  sum = in_cksum_add (&uh, sizeof (uh), sum);
  uh.sum = in_cksum (buf, n, sum);
  if (uh.sum == 0)
    uh.sum = 0xffff;
  r = ip_output (ip, IPPROTO_UDP, &uh, sizeof (uh), buf, n);
  if (r < 0)
    {
      errno = -r;
      return -1;
    }
  return n;
}

static ssize_t
udp_recv (struct sock *s, int nonblock, void *buf, size_t n,
	  struct sockaddr_in *from, int peek)
{
  for (;;)
    {
      struct ring *r = &s->ring;
      while (ring_pending (r))
	{
	  uint8_t *frame = r->bufs + r->next * RING_BUFSZ;
	  unsigned len = r->flags[r->next];
	  struct ip_info ip;
	  ssize_t res = -1;

	  if (len > RING_BUFSZ)
	    len = RING_BUFSZ;
	  if (ip_parse (frame, len, &ip) == 0 && ip.proto == IPPROTO_UDP
	      && ip.plen >= sizeof (struct udp_hdr))
	    {
	      const struct udp_hdr *uh = (const struct udp_hdr *) ip.payload;
	      unsigned dl = ntohs (uh->len);
	      if (dl >= sizeof (*uh) && dl <= ip.plen)
		{
		  dl -= sizeof (*uh);
		  res = dl < n ? dl : n;
		  memcpy (buf, uh + 1, res);
		  if (from)
		    {
		      memset (from, 0, sizeof (*from));
		      from->sin_family = AF_INET;
		      from->sin_port = uh->sport;
		      from->sin_addr.s_addr = ip.src;
		    }
		}
	    }
	  if (res >= 0 && peek)
	    return res;
	  r->flags[r->next] = 0;
	  r->next = (r->next + 1) % r->n;
	  if (res >= 0)
	    return res;
	}
      if (nonblock)
	{
	  errno = EAGAIN;
	  return -1;
	}
      net_sleep (1000);
      if (__proc->sigpending)
	{
	  errno = EINTR;
	  return -1;
	}
    }
}

ssize_t
sendto (int fd, const void *buf, size_t n, int flags,
	const struct sockaddr *a, socklen_t len)
{
  struct file *f;
  struct sock *s = fd_sock (fd, &f);
  const struct sockaddr_in *sin = (const struct sockaddr_in *) a;

  if (s == NULL)
    return -1;
  if (s->type == SOCK_STREAM)
    return tcp_write (s, (f->f_flags & O_NONBLOCK) || (flags & MSG_DONTWAIT),
		      buf, n);
  if (s->lport == 0)
    {
      struct sockaddr_in me = {.sin_family = AF_INET };
      if (bind (fd, (struct sockaddr *) &me, sizeof (me)) < 0)
	return -1;
    }
  if (sin)
    return udp_send (s, buf, n, sin->sin_addr.s_addr, sin->sin_port);
  if (!s->rport)
    {
      errno = EDESTADDRREQ;
      return -1;
    }
  return udp_send (s, buf, n, s->rip, s->rport);
}

ssize_t
send (int fd, const void *buf, size_t n, int flags)
{
  return sendto (fd, buf, n, flags, NULL, 0);
}

ssize_t
recvfrom (int fd, void *buf, size_t n, int flags, struct sockaddr *a,
	  socklen_t *len)
{
  struct file *f;
  struct sock *s = fd_sock (fd, &f);
  int nb;

  if (s == NULL)
    return -1;
  nb = (f->f_flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
  if (s->type == SOCK_STREAM)
    return tcp_read (s, nb, buf, n, flags & MSG_PEEK);
  if (s->lport == 0)
    {
      errno = EINVAL;
      return -1;
    }
  if (len)
    *len = sizeof (struct sockaddr_in);
  return udp_recv (s, nb, buf, n, (struct sockaddr_in *) a, flags & MSG_PEEK);
}

ssize_t
recv (int fd, void *buf, size_t n, int flags)
{
  return recvfrom (fd, buf, n, flags, NULL, NULL);
}

int
shutdown (int fd, int how)
{
  struct sock *s = fd_sock (fd, NULL);
  if (s == NULL)
    return -1;
  if (s->type == SOCK_STREAM && how != SHUT_RD)
    {
      s->fin_pending = 1;
      in_stack = 1;
      tcp_output (s);
      in_stack = 0;
    }
  return 0;
}

int
setsockopt (int fd, int level, int opt, const void *v, socklen_t len)
{
  return fd_sock (fd, NULL) ? 0 : -1;
}

int
getsockopt (int fd, int level, int opt, void *v, socklen_t *len)
{
  struct sock *s = fd_sock (fd, NULL);
  if (s == NULL)
    return -1;
  if (level == SOL_SOCKET && opt == SO_ERROR && *len >= sizeof (int))
    {
      *(int *) v = s->error;
      *len = sizeof (int);
      return 0;
    }
  memset (v, 0, *len);
  return 0;
}

static int
sockname (int fd, struct sockaddr *a, socklen_t *len, int peer)
{
  struct sock *s = fd_sock (fd, NULL);
  struct sockaddr_in *sin = (struct sockaddr_in *) a;

  if (s == NULL)
    return -1;
  if (*len < sizeof (*sin))
    {
      errno = EINVAL;
      return -1;
    }
  memset (sin, 0, sizeof (*sin));
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = peer ? s->rip : netcfg->ip;
  sin->sin_port = peer ? s->rport : s->lport;
  *len = sizeof (*sin);
  return 0;
}

int
getsockname (int fd, struct sockaddr *a, socklen_t *len)
{
  return sockname (fd, a, len, 0);
}

int
getpeername (int fd, struct sockaddr *a, socklen_t *len)
{
  return sockname (fd, a, len, 1);
}

/* File operations for FT_SOCK. */
static struct sock *
file_sock (struct file *f)
{
  if (f->u.sock.owner != __envid || f->u.sock.id < 0
      || f->u.sock.id >= NSOCK || !socks[f->u.sock.id].used)
    return NULL;
  return &socks[f->u.sock.id];
}

static ssize_t
sock_fread (struct file *f, void *buf, size_t n)
{
  struct sock *s = file_sock (f);
  ssize_t r;

  if (s == NULL)
    return -EBADF;
  if (s->type == SOCK_STREAM)
    r = tcp_read (s, f->f_flags & O_NONBLOCK, buf, n, 0);
  else
    r = udp_recv (s, f->f_flags & O_NONBLOCK, buf, n, NULL, 0);
  return r < 0 ? -errno : r;
}

static ssize_t
sock_fwrite (struct file *f, const void *buf, size_t n)
{
  struct sock *s = file_sock (f);
  ssize_t r;

  if (s == NULL)
    return -EBADF;
  if (s->type == SOCK_STREAM)
    r = tcp_write (s, f->f_flags & O_NONBLOCK, buf, n);
  else if (s->rport)
    r = udp_send (s, buf, n, s->rip, s->rport);
  else
    {
      errno = EDESTADDRREQ;
      r = -1;
    }
  return r < 0 ? -errno : r;
}

static void
sock_fclose (struct file *f)
{
  struct sock *s = file_sock (f);

  if (s == NULL)
    return;
  if (--s->refs > 0)
    return;
  s->closed = 1;
  if (s->type == SOCK_STREAM)
    {
      switch (s->state)
	{
	case TCP_ESTABLISHED:
	case TCP_CLOSE_WAIT:
	case TCP_SYN_RCVD:
	  s->fin_pending = 1;
	  in_stack = 1;
	  tcp_output (s);
	  in_stack = 0;
	  return;
	case TCP_LISTEN:
	  /* Pending connections go away with the listener. */
	  for (int i = 0; i < s->nacceptq; i++)
	    {
	      struct sock *c = &socks[s->acceptq[i]];
	      tcp_send (c, TH_RST | TH_ACK, c->snd_nxt, NULL, 0);
	      sock_free (c);
	    }
	  s->nacceptq = 0;
	  s->state = TCP_CLOSED;
	  if (s->refs > 0)
	    return;
	  /* Connections still using our ring keep it alive. */
	  for (int i = 0; i < NSOCK; i++)
	    if (socks[i].used && socks[i].listener == sock_index (s))
	      return;
	  break;
	case TCP_FIN_WAIT_1:
	case TCP_FIN_WAIT_2:
	case TCP_CLOSING:
	case TCP_LAST_ACK:
	case TCP_TIME_WAIT:
	  return;
	}
    }
  sock_free (s);
}

static int
sock_fstat (struct file *f, struct stat *st)
{
  memset (st, 0, sizeof (*st));
  st->st_mode = S_IFSOCK | 0666;
  return 0;
}

static int
sock_ready (struct file *f, int write)
{
  struct sock *s = file_sock (f);

  if (s == NULL)
    return 1;
  net_poll ();
  if (s->type == SOCK_DGRAM)
    return write || ring_pending (&s->ring);
  if (s->state == TCP_LISTEN)
    return !write && s->nacceptq > 0;
  if (write)
    return s->state == TCP_CLOSED || s->error || s->slen < TCP_BUFSZ;
  return s->rlen > 0 || s->rcv_fin || s->state == TCP_CLOSED || s->error;
}

static struct file_ops sock_ops = {
  .read = sock_fread,
  .write = sock_fwrite,
  .fstat = sock_fstat,
  .close = sock_fclose,
  .ready = sock_ready,
};

__attribute__ ((constructor))
     static void sock_register (void)
{
  exos_fops[FT_SOCK] = &sock_ops;
}

/*
 * select / poll: check readiness; sleep on pipes, the console, the
 * network rings and timers.
 */
static int
fd_ready (int fd, int write)
{
  struct file *f = fd_file (fd);
  if (f == NULL)
    return -1;
  if (exos_fops[f->f_type] && exos_fops[f->f_type]->ready)
    return exos_fops[f->f_type]->ready (f, write);
  return 1;
}

static void
fd_sleep (struct pollfd *fds, nfds_t n, int timeout_ms)
{
  struct wk_term t[WK_MAXTERMS];
  int nt = 0;
  uint32_t cons = sysinfo_page->si_cons_in;

  for (nfds_t i = 0; i < n; i++)
    {
      struct file *f = fd_file (fds[i].fd);
      if (f == NULL || nt + 4 >= WK_MAXTERMS - 4)
	continue;
      if (f->f_type == FT_CONS && (fds[i].events & POLLIN))
	{
	  if (nt)
	    t[nt++].wk_op = WK_OR;
	  memset (&t[nt], 0, sizeof (t[nt]));
	  t[nt].wk_op = WK_NE;
	  t[nt].wk_lkind = WK_MEM32;
	  t[nt].wk_lval = (uint32_t) & sysinfo_page->si_cons_in;
	  t[nt].wk_rkind = WK_CONST;
	  t[nt].wk_rval = cons;
	  nt++;
	}
      else if (exos_fops[f->f_type] && exos_fops[f->f_type]->wk)
	{
	  if (nt)
	    t[nt++].wk_op = WK_OR;
	  int k = exos_fops[f->f_type]->wk (f, (fds[i].events & POLLOUT) != 0,
					    t + nt, WK_MAXTERMS - nt - 4);
	  if (k == 0)
	    nt--;
	  nt += k;
	}
    }
  if (nt)
    t[nt++].wk_op = WK_OR;
  memset (&t[nt], 0, sizeof (t[nt]));
  t[nt].wk_op = WK_GE;
  t[nt].wk_lkind = WK_TIME;
  t[nt].wk_rkind = WK_CONST;
  t[nt].wk_rval = now_ms () + (timeout_ms >= 0 && timeout_ms < 1000
			       ? timeout_ms : 1000);
  nt++;
  exos_wkpred (t, nt);
}

int
poll (struct pollfd *fds, nfds_t n, int timeout)
{
  uint32_t deadline = now_ms () + (timeout > 0 ? timeout : 0);

  for (;;)
    {
      int count = 0;
      for (nfds_t i = 0; i < n; i++)
	{
	  int r, w;
	  fds[i].revents = 0;
	  if (fds[i].fd < 0)
	    continue;
	  r = (fds[i].events & POLLIN) ? fd_ready (fds[i].fd, 0) : 0;
	  w = (fds[i].events & POLLOUT) ? fd_ready (fds[i].fd, 1) : 0;
	  if (r < 0 || w < 0)
	    fds[i].revents = POLLNVAL;
	  else
	    fds[i].revents = (r ? POLLIN : 0) | (w ? POLLOUT : 0);
	  if (fds[i].revents)
	    count++;
	}
      if (count || timeout == 0)
	return count;
      if (timeout > 0 && (int32_t) (now_ms () - deadline) >= 0)
	return 0;
      if (__proc->sigpending)
	{
	  errno = EINTR;
	  return -1;
	}
      fd_sleep (fds, n, timeout > 0 ? (int) (deadline - now_ms ()) : -1);
    }
}

int
select (int nfd, fd_set *rs, fd_set *ws, fd_set *es, struct timeval *tv)
{
  struct pollfd pf[FD_SETSIZE];
  int n = 0, r, timeout = -1;

  if (nfd > FD_SETSIZE)
    nfd = FD_SETSIZE;
  for (int fd = 0; fd < nfd; fd++)
    {
      short ev = 0;
      if (rs && FD_ISSET (fd, rs))
	ev |= POLLIN;
      if (ws && FD_ISSET (fd, ws))
	ev |= POLLOUT;
      if (ev)
	{
	  pf[n].fd = fd;
	  pf[n].events = ev;
	  n++;
	}
    }
  if (tv)
    timeout = tv->tv_sec * 1000 + tv->tv_usec / 1000;
  r = poll (pf, n, timeout);
  if (r < 0)
    return r;
  if (rs)
    FD_ZERO (rs);
  if (ws)
    FD_ZERO (ws);
  if (es)
    FD_ZERO (es);
  r = 0;
  for (int i = 0; i < n; i++)
    {
      if ((pf[i].revents & (POLLIN | POLLNVAL)) && rs)
	FD_SET (pf[i].fd, rs), r++;
      if ((pf[i].revents & POLLOUT) && ws)
	FD_SET (pf[i].fd, ws), r++;
    }
  return r;
}

/*
 * DNS: a minimal resolver for A records.
 */
int h_errno;

struct hostent *
gethostbyname (const char *name)
{
  static struct hostent he;
  static char *addrs[2];
  static struct in_addr addr;
  static char hname[256];
  uint8_t q[512], r[512];
  int fd, n = 0;

  strlcpy (hname, name, sizeof (hname));
  he.h_name = hname;
  he.h_aliases = NULL;
  he.h_addrtype = AF_INET;
  he.h_length = 4;
  he.h_addr_list = addrs;
  addrs[0] = (char *) &addr;
  addrs[1] = NULL;
  if (inet_aton (name, &addr))
    return &he;
  if (!strcmp (name, "localhost") || !strcmp (name, "xok"))
    {
      addr.s_addr = netcfg->ip;
      return &he;
    }
  if (net_wait_config (5000) < 0 || netcfg->dns == 0)
    {
      h_errno = 1;
      return NULL;
    }
  /* Query. */
  uint16_t id = (uint16_t) exos_time_ns ();
  q[n++] = id >> 8;
  q[n++] = id;
  q[n++] = 0x01;		/* RD */
  q[n++] = 0;
  q[n++] = 0, q[n++] = 1;	/* QDCOUNT */
  q[n++] = 0, q[n++] = 0, q[n++] = 0, q[n++] = 0, q[n++] = 0, q[n++] = 0;
  for (const char *p = name; *p;)
    {
      size_t l = strcspn (p, ".");
      if (l == 0 || l > 63 || n + l + 6 > sizeof (q))
	{
	  h_errno = 1;
	  return NULL;
	}
      q[n++] = l;
      memcpy (q + n, p, l);
      n += l;
      p += l;
      if (*p == '.')
	p++;
    }
  q[n++] = 0;
  q[n++] = 0, q[n++] = 1;	/* A */
  q[n++] = 0, q[n++] = 1;	/* IN */
  fd = socket (AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return NULL;
  struct sockaddr_in srv = {.sin_family = AF_INET,.sin_port = htons (53) };
  srv.sin_addr.s_addr = netcfg->dns;
  for (int tries = 0; tries < 3; tries++)
    {
      struct pollfd pf = {.fd = fd,.events = POLLIN };
      sendto (fd, q, n, 0, (struct sockaddr *) &srv, sizeof (srv));
      if (poll (&pf, 1, 2000) <= 0)
	continue;
      int len = recv (fd, r, sizeof (r), 0);
      if (len < 12 || r[0] != (id >> 8) || r[1] != (id & 0xff))
	continue;
      int an = (r[6] << 8) | r[7];
      int off = 12;
      /* Skip the question. */
      while (off < len && r[off])
	off += (r[off] & 0xc0) ? 1 : r[off] + 1;
      off += 5;
      for (int i = 0; i < an && off + 12 <= len; i++)
	{
	  if ((r[off] & 0xc0) == 0xc0)
	    off += 2;
	  else
	    {
	      while (off < len && r[off])
		off += r[off] + 1;
	      off++;
	    }
	  int type = (r[off] << 8) | r[off + 1];
	  int rdlen = (r[off + 8] << 8) | r[off + 9];
	  off += 10;
	  if (type == 1 && rdlen == 4 && off + 4 <= len)
	    {
	      memcpy (&addr, r + off, 4);
	      close (fd);
	      return &he;
	    }
	  off += rdlen;
	}
    }
  close (fd);
  h_errno = 1;
  return NULL;
}
