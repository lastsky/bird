/*
 *	BIRD -- Linux Netlink Interface
 *
 *	(c) 1999 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <errno.h>

#define LOCAL_DEBUG

#include "nest/bird.h"
#include "nest/route.h"
#include "nest/protocol.h"
#include "nest/iface.h"
#include "lib/timer.h"
#include "lib/unix.h"
#include "lib/krt.h"
#include "lib/socket.h"

/*
 *	We need to work around namespace conflicts between us and the kernel,
 *	but I prefer this way to being forced to rename our configuration symbols.
 *	This will disappear as soon as netlink headers become part of the libc.
 */

#undef CONFIG_NETLINK
#include <linux/config.h>
#ifndef CONFIG_NETLINK
#error "Kernel not configured to support netlink"
#endif

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#ifndef MSG_TRUNC			/* FIXME: Hack to circumvent omissions in glibc includes */
#define MSG_TRUNC 0x20
#endif

#ifndef RTPROT_BIRD			/* FIXME: Kill after Alexey assigns as a number */
#define RTPROT_BIRD 13
#endif

/*
 *	Synchronous Netlink interface
 */

static int nl_sync_fd = -1;		/* Unix socket for synchronous netlink actions */
static u32 nl_sync_seq;			/* Sequence number of last request sent */

static byte *nl_rx_buffer;		/* Receive buffer */
#define NL_RX_SIZE 2048

static struct nlmsghdr *nl_last_hdr;	/* Recently received packet */
static unsigned int nl_last_size;

static void
nl_open(void)
{
  if (nl_sync_fd < 0)
    {
      nl_sync_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
      if (nl_sync_fd < 0)
	die("Unable to open rtnetlink socket: %m");
      nl_sync_seq = now;
      nl_rx_buffer = xmalloc(NL_RX_SIZE);
    }
}

static void
nl_send(struct nlmsghdr *nh)
{
  struct sockaddr_nl sa;

  memset(&sa, 0, sizeof(sa));
  sa.nl_family = AF_NETLINK;
  nh->nlmsg_pid = 0;
  nh->nlmsg_seq = ++nl_sync_seq;
  if (sendto(nl_sync_fd, nh, nh->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    die("rtnetlink sendto: %m");
  nl_last_hdr = NULL;
}

static void
nl_request_dump(int cmd)
{
  struct {
    struct nlmsghdr nh;
    struct rtgenmsg g;
  } req;
  req.nh.nlmsg_type = cmd;
  req.nh.nlmsg_len = sizeof(req);
  req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.g.rtgen_family = PF_INET;
  nl_send(&req.nh);
}

static struct nlmsghdr *
nl_get_reply(void)
{
  for(;;)
    {
      if (!nl_last_hdr)
	{
	  struct iovec iov = { nl_rx_buffer, NL_RX_SIZE };
	  struct sockaddr_nl sa;
	  struct msghdr m = { (struct sockaddr *) &sa, sizeof(sa), &iov, 1, NULL, 0, 0 };
	  int x = recvmsg(nl_sync_fd, &m, 0);
	  if (x < 0)
	    die("nl_get_reply: %m");
	  if (sa.nl_pid)		/* It isn't from the kernel */
	    {
	      DBG("Non-kernel packet\n");
	      continue;
	    }
	  nl_last_size = x;
	  nl_last_hdr = (void *) nl_rx_buffer;
	  if (m.msg_flags & MSG_TRUNC)
	    bug("nl_get_reply: got truncated reply which should be impossible");
	}
      if (NLMSG_OK(nl_last_hdr, nl_last_size))
	{
	  struct nlmsghdr *h = nl_last_hdr;
	  if (h->nlmsg_seq != nl_sync_seq)
	    {
	      log(L_WARN "nl_get_reply: Ignoring out of sequence netlink packet (%x != %x)",
		  h->nlmsg_seq, nl_sync_seq);
	      continue;
	    }
	  nl_last_hdr = NLMSG_NEXT(h, nl_last_size);
	  return h;
	}
      if (nl_last_size)
	log(L_WARN "nl_get_reply: Found packet remnant of size %d", nl_last_size);
      nl_last_hdr = NULL;
    }
}

static int
nl_error(struct nlmsghdr *h)
{
  struct nlmsgerr *e;
  int ec;

  if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)))
    {
      log(L_WARN "Netlink: Truncated error message received");
      return ENOBUFS;
    }
  e = (struct nlmsgerr *) NLMSG_DATA(h);
  ec = -e->error;
  if (ec)
    log(L_WARN "Netlink: %s", strerror(ec)); /* FIXME: Shut up? */
  return ec;
}

static struct nlmsghdr *
nl_get_scan(void)
{
  struct nlmsghdr *h = nl_get_reply();

  if (h->nlmsg_type == NLMSG_DONE)
    return NULL;
  if (h->nlmsg_type == NLMSG_ERROR)
    {
      nl_error(h);
      return NULL;
    }
  return h;
}

static int
nl_exchange(struct nlmsghdr *pkt)
{
  struct nlmsghdr *h;

  nl_send(pkt);
  for(;;)
    {
      h = nl_get_reply();
      if (h->nlmsg_type == NLMSG_ERROR)
	break;
      log(L_WARN "nl_exchange: Unexpected reply received");
    }
  return nl_error(h);
}

/*
 *	Netlink attributes
 */

static int nl_attr_len;

static void *
nl_checkin(struct nlmsghdr *h, int lsize)
{
  nl_attr_len = h->nlmsg_len - NLMSG_LENGTH(lsize);
  if (nl_attr_len < 0)
    {
      log(L_ERR "nl_checkin: underrun by %d bytes", -nl_attr_len);
      return NULL;
    }
  return NLMSG_DATA(h);
}

static int
nl_parse_attrs(struct rtattr *a, struct rtattr **k, int ksize)
{
  int max = ksize / sizeof(struct rtattr *);
  bzero(k, ksize);
  while (RTA_OK(a, nl_attr_len))
    {
      if (a->rta_type < max)
	k[a->rta_type] = a;
      a = RTA_NEXT(a, nl_attr_len);
    }
  if (nl_attr_len)
    {
      log(L_ERR "nl_parse_attrs: remnant of size %d", nl_attr_len);
      return 0;
    }
  else
    return 1;
}

static void
nl_add_attr_u32(struct nlmsghdr *h, unsigned maxsize, int code, u32 data)
{
  unsigned len = RTA_LENGTH(4);
  struct rtattr *a;

  if (NLMSG_ALIGN(h->nlmsg_len) + len > maxsize)
    bug("nl_add_attr32: packet buffer overflow");
  a = (struct rtattr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len));
  a->rta_type = code;
  a->rta_len = len;
  memcpy(RTA_DATA(a), &data, 4);
  h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + len;
}

static void
nl_add_attr_ipa(struct nlmsghdr *h, unsigned maxsize, int code, ip_addr ipa)
{
  unsigned len = RTA_LENGTH(sizeof(ipa));
  struct rtattr *a;

  if (NLMSG_ALIGN(h->nlmsg_len) + len > maxsize)
    bug("nl_add_attr_ipa: packet buffer overflow");
  a = (struct rtattr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len));
  a->rta_type = code;
  a->rta_len = len;
  ipa = ipa_hton(ipa);
  memcpy(RTA_DATA(a), &ipa, sizeof(ipa));
  h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + len;
}

/*
 *	Scanning of interfaces
 */

static void
nl_parse_link(struct nlmsghdr *h, int scan)
{
  struct ifinfomsg *i;
  struct rtattr *a[IFLA_STATS+1];
  int new = h->nlmsg_type == RTM_NEWLINK;
  struct iface f;
  struct iface *ifi;
  char *name;
  u32 mtu;
  unsigned int fl;

  if (!(i = nl_checkin(h, sizeof(*i))) || !nl_parse_attrs(IFLA_RTA(i), a, sizeof(a)))
    return;
  if (!a[IFLA_IFNAME] || RTA_PAYLOAD(a[IFLA_IFNAME]) < 2 ||
      !a[IFLA_MTU] || RTA_PAYLOAD(a[IFLA_MTU]) != 4)
    {
      log(L_ERR "nl_parse_link: Malformed message received");
      return;
    }
  name = RTA_DATA(a[IFLA_IFNAME]);
  memcpy(&mtu, RTA_DATA(a[IFLA_MTU]), sizeof(u32));

  ifi = if_find_by_index(i->ifi_index);
  if (!new)
    {
      DBG("KRT: IF%d(%s) goes down\n", i->ifi_index, name);
      if (ifi && !scan)
	{
	  memcpy(&f, ifi, sizeof(struct iface));
	  f.flags |= IF_ADMIN_DOWN;
	  if_update(&f);
	}
    }
  else
    {
      DBG("KRT: IF%d(%s) goes up (mtu=%d,flg=%x)\n", i->ifi_index, name, mtu, i->ifi_flags);
      if (ifi)
	memcpy(&f, ifi, sizeof(f));
      else
	{
	  bzero(&f, sizeof(f));
	  f.index = i->ifi_index;
	}
      strncpy(f.name, RTA_DATA(a[IFLA_IFNAME]), sizeof(f.name)-1);
      f.mtu = mtu;
      f.flags = 0;
      fl = i->ifi_flags;
      if (fl & IFF_UP)
	f.flags |= IF_LINK_UP;
      if (fl & IFF_POINTOPOINT)
	f.flags |= IF_UNNUMBERED | IF_MULTICAST;
      if (fl & IFF_LOOPBACK)
	f.flags |= IF_LOOPBACK | IF_IGNORE;
      if (fl & IFF_BROADCAST)
	f.flags |= IF_BROADCAST | IF_MULTICAST;
      if_update(&f);
    }
}

static void
nl_parse_addr(struct nlmsghdr *h)
{
  struct ifaddrmsg *i;
  struct rtattr *a[IFA_ANYCAST+1];
  int new = h->nlmsg_type == RTM_NEWADDR;
  struct iface f;
  struct iface *ifi;

  if (!(i = nl_checkin(h, sizeof(*i))) || !nl_parse_attrs(IFA_RTA(i), a, sizeof(a)))
    return;
  if (i->ifa_family != AF_INET)
    return;
  if (!a[IFA_ADDRESS] || RTA_PAYLOAD(a[IFA_ADDRESS]) != sizeof(ip_addr) ||
      !a[IFA_LOCAL] || RTA_PAYLOAD(a[IFA_LOCAL]) != sizeof(ip_addr) ||
      (a[IFA_BROADCAST] && RTA_PAYLOAD(a[IFA_BROADCAST]) != sizeof(ip_addr)))
    {
      log(L_ERR "nl_parse_addr: Malformed message received");
      return;
    }
  if (i->ifa_flags & IFA_F_SECONDARY)
    {
      DBG("KRT: Received address message for secondary address which is not supported.\n"); /* FIXME */
      return;
    }

  ifi = if_find_by_index(i->ifa_index);
  if (!ifi)
    {
      log(L_ERR "KRT: Received address message for unknown interface %d\n", i->ifa_index);
      return;
    }
  memcpy(&f, ifi, sizeof(f));

  if (i->ifa_prefixlen > 32 || i->ifa_prefixlen == 31 ||
      (f.flags & IF_UNNUMBERED) && i->ifa_prefixlen != 32)
    {
      log(L_ERR "KRT: Invalid prefix length for interface %s: %d\n", f.name, i->ifa_prefixlen);
      new = 0;
    }

  f.ip = f.brd = f.opposite = IPA_NONE;
  if (!new)
    {
      DBG("KRT: IF%d IP address deleted\n");
      f.pxlen = 0;
    }
  else
    {
      memcpy(&f.ip, RTA_DATA(a[IFA_LOCAL]), sizeof(f.ip));
      f.ip = ipa_ntoh(f.ip);
      f.pxlen = i->ifa_prefixlen;
      if (f.flags & IF_UNNUMBERED)
	{
	  memcpy(&f.opposite, RTA_DATA(a[IFA_ADDRESS]), sizeof(f.opposite));
	  f.opposite = f.brd = ipa_ntoh(f.opposite);
	}
      else if ((f.flags & IF_BROADCAST) && a[IFA_BROADCAST])
	{
	  memcpy(&f.brd, RTA_DATA(a[IFA_BROADCAST]), sizeof(f.brd));
	  f.brd = ipa_ntoh(f.brd);
	}
      /* else a NBMA link */
      f.prefix = ipa_and(f.ip, ipa_mkmask(f.pxlen));
      DBG("KRT: IF%d IP address set to %I, net %I/%d, brd %I, opp %I\n", f.index, f.ip, f.prefix, f.pxlen, f.brd, f.opposite);
    }
  if_update(&f);
}

void
krt_if_scan(struct krt_proto *p)
{
  struct nlmsghdr *h;

  if_start_update();

  nl_request_dump(RTM_GETLINK);
  while (h = nl_get_scan())
    if (h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK)
      nl_parse_link(h, 1);
    else
      log(L_DEBUG "nl_scan_ifaces: Unknown packet received (type=%d)", h->nlmsg_type);

  nl_request_dump(RTM_GETADDR);
  while (h = nl_get_scan())
    if (h->nlmsg_type == RTM_NEWADDR || h->nlmsg_type == RTM_DELADDR)
      nl_parse_addr(h);
    else
      log(L_DEBUG "nl_scan_ifaces: Unknown packet received (type=%d)", h->nlmsg_type);

  if_end_update();
}

/*
 *	Routes
 */

int					/* FIXME: Check use of this function in krt.c */
krt_capable(rte *e)
{
  rta *a = e->attrs;

  if (a->cast != RTC_UNICAST)	/* FIXME: For IPv6, we might support anycasts as well */
    return 0;
  switch (a->dest)
    {
    case RTD_ROUTER:
    case RTD_DEVICE:
    case RTD_BLACKHOLE:
    case RTD_UNREACHABLE:
    case RTD_PROHIBIT:
      break;
    default:
      return 0;
    }
  return 1;
}

static void
nl_send_route(rte *e, int new)
{
  net *net = e->net;
  rta *a = e->attrs;
  struct {
    struct nlmsghdr h;
    struct rtmsg r;
    char buf[128];
  } r;
  struct nlmsghdr *reply;

  DBG("nl_send_route(%I/%d,new=%d)\n", net->n.prefix, net->n.pxlen, new);

  bzero(&r.h, sizeof(r.h));
  bzero(&r.r, sizeof(r.r));
  r.h.nlmsg_type = new ? RTM_NEWROUTE : RTM_DELROUTE;
  r.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
  r.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (new ? NLM_F_CREATE|NLM_F_REPLACE : 0);
  /* FIXME: Do we really need to process ACKs? */

  r.r.rtm_family = AF_INET;
  r.r.rtm_dst_len = net->n.pxlen;
  r.r.rtm_tos = 0;	/* FIXME: Non-zero TOS? */
  r.r.rtm_table = RT_TABLE_MAIN;	/* FIXME: Other tables? */
  r.r.rtm_protocol = RTPROT_BIRD;
  r.r.rtm_scope = RT_SCOPE_UNIVERSE;	/* FIXME: Other scopes? */
  nl_add_attr_ipa(&r.h, sizeof(r), RTA_DST, net->n.prefix);
  switch (a->dest)
    {
    case RTD_ROUTER:
      r.r.rtm_type = RTN_UNICAST;
      nl_add_attr_ipa(&r.h, sizeof(r), RTA_GATEWAY, a->gw);
      break;
    case RTD_DEVICE:
      r.r.rtm_type = RTN_UNICAST;
      nl_add_attr_u32(&r.h, sizeof(r), RTA_OIF, a->iface->index);
      break;
    case RTD_BLACKHOLE:
      r.r.rtm_type = RTN_BLACKHOLE;
      break;
    case RTD_UNREACHABLE:
      r.r.rtm_type = RTN_UNREACHABLE;
      break;
    case RTD_PROHIBIT:
      r.r.rtm_type = RTN_PROHIBIT;
      break;
    default:
      bug("krt_capable inconsistent with nl_send_route");
    }

  nl_exchange(&r.h);
}

void
krt_set_notify(struct proto *p, net *n, rte *new, rte *old)
{
  if (old && old->attrs->source == RTS_DEVICE)	/* Device routes are left to the kernel */
    old = NULL;
  if (new && new->attrs->source == RTS_DEVICE)
    new = NULL;
  if (old && new && old->attrs->tos == new->attrs->tos)
    {
      /* FIXME: Priorities should be identical as well, but we don't use them yet. */
      nl_send_route(new, 1);
    }
  else
    {
      if (old)
	{
	  if (!old->attrs->iface || (old->attrs->iface->flags & IF_UP))
	    nl_send_route(old, 0);
	  /* else the kernel has already flushed it */
	}
      if (new)
	nl_send_route(new, 1);
    }
}

struct iface *
krt_temp_iface(struct krt_proto *p, unsigned index)
{
  struct iface *i, *j;

  WALK_LIST(i, p->scan.temp_ifs)
    if (i->index == index)
      return i;
  i = mb_allocz(p->p.pool, sizeof(struct iface));
  if (j = if_find_by_index(index))
    strcpy(i->name, j->name);
  else
    strcpy(i->name, "?");
  i->index = index;
  add_tail(&p->scan.temp_ifs, &i->n);
  return i;
}

static void
nl_parse_route(struct krt_proto *p, struct nlmsghdr *h, int scan)
{
  struct rtmsg *i;
  struct rtattr *a[RTA_CACHEINFO+1];
  int new = h->nlmsg_type == RTM_NEWROUTE;
  ip_addr dst;
  rta ra;
  rte *e;
  net *net;
  u32 oif;
  int src;

  if (!(i = nl_checkin(h, sizeof(*i))) || !nl_parse_attrs(RTM_RTA(i), a, sizeof(a)))
    return;
  if (i->rtm_family != AF_INET)
    return;
  if ((a[RTA_DST] && RTA_PAYLOAD(a[RTA_DST]) != sizeof(ip_addr)) ||
      (a[RTA_OIF] && RTA_PAYLOAD(a[RTA_OIF]) != 4) ||
      (a[RTA_GATEWAY] && RTA_PAYLOAD(a[RTA_GATEWAY]) != sizeof(ip_addr)))
    {
      log(L_ERR "nl_parse_route: Malformed message received");
      return;
    }

  if (i->rtm_table != RT_TABLE_MAIN)	/* FIXME: What about other tables? */
    return;
  if (i->rtm_tos != 0)			/* FIXME: What about TOS? */
    return;

  if (scan && !new)
    {
      DBG("KRT: Ignoring route deletion\n");
      return;
    }

  if (a[RTA_DST])
    {
      memcpy(&dst, RTA_DATA(a[RTA_DST]), sizeof(dst));
      dst = ipa_ntoh(dst);
    }
  else
    dst = IPA_NONE;
  if (a[RTA_OIF])
    memcpy(&oif, RTA_DATA(a[RTA_OIF]), sizeof(oif));
  else
    oif = ~0;

  DBG("Got %I/%d, type=%d, oif=%d\n", dst, i->rtm_dst_len, i->rtm_type, oif);

  switch (i->rtm_protocol)
    {
    case RTPROT_REDIRECT:
      src = KRT_SRC_REDIRECT;
      break;
    case RTPROT_KERNEL:
      DBG("Route originated in kernel, ignoring\n");
      return;
    case RTPROT_BIRD:
      if (!scan)
	{
	  DBG("Echo of our own route, ignoring\n");
	  return;
	}
      src = KRT_SRC_BIRD;
      break;
    default:
      src = KRT_SRC_ALIEN;
    }

  net = net_get(&master_table, 0, dst, i->rtm_dst_len);
  ra.proto = &p->p;
  ra.source = RTS_INHERIT;
  ra.scope = SCOPE_UNIVERSE;	/* FIXME: Use kernel scope? */
  ra.cast = RTC_UNICAST;
  ra.tos = ra.flags = ra.aflags = 0;
  ra.from = IPA_NONE;
  ra.gw = IPA_NONE;
  ra.iface = NULL;
  ra.attrs = NULL;

  switch (i->rtm_type)
    {
    case RTN_UNICAST:
      if (oif == ~0U)
	{
	  log(L_ERR "KRT: Mysterious route with no OIF (%I/%d)", net->n.prefix, net->n.pxlen);
	  return;
	}
      if (a[RTA_GATEWAY])
	{
	  neighbor *ng;
	  ra.dest = RTD_ROUTER;
	  memcpy(&ra.gw, RTA_DATA(a[RTA_GATEWAY]), sizeof(ra.gw));
	  ra.gw = ipa_ntoh(ra.gw);
	  ng = neigh_find(&p->p, &ra.gw, 0);
	  if (ng)
	    ra.iface = ng->iface;
	  else
	    /* FIXME: Remove this warning? */
	    log(L_WARN "Kernel told us to use non-neighbor %I for %I/%d", ra.gw, net->n.prefix, net->n.pxlen);
	}
      else
	{
	  ra.dest = RTD_DEVICE;
	  ra.iface = krt_temp_iface(p, oif);
	}
      break;
    case RTN_BLACKHOLE:
      ra.dest = RTD_BLACKHOLE;
      break;
    case RTN_UNREACHABLE:
      ra.dest = RTD_UNREACHABLE;
      break;
    case RTN_PROHIBIT:
      ra.dest = RTD_PROHIBIT;
      break;
    /* FIXME: What about RTN_THROW? */
    default:
      DBG("KRT: Ignoring route with type=%d\n", i->rtm_type);
      return;
    }
  e = rte_get_temp(&ra);
  e->net = net;
  e->u.krt_sync.src = src;
  if (scan)
    krt_got_route(p, e);
  else
    krt_got_route_async(p, e, new);
}

void
krt_scan_fire(struct krt_proto *p)
{
  struct nlmsghdr *h;

  nl_request_dump(RTM_GETROUTE);
  while (h = nl_get_scan())
    if (h->nlmsg_type == RTM_NEWROUTE || h->nlmsg_type == RTM_DELROUTE)
      nl_parse_route(p, h, 1);
    else
      log(L_DEBUG "nl_scan_fire: Unknown packet received (type=%d)", h->nlmsg_type);
}

/*
 *	Asynchronous Netlink interface
 */

static sock *nl_async_sk;		/* BIRD socket for asynchronous notifications */
static byte *nl_async_rx_buffer;	/* Receive buffer */

static void
nl_async_msg(struct krt_proto *p, struct nlmsghdr *h)
{
  switch (h->nlmsg_type)
    {
    case RTM_NEWROUTE:
    case RTM_DELROUTE:
      DBG("KRT: Received async route notification (%d)\n", h->nlmsg_type);
      nl_parse_route(p, h, 0);
      break;
    case RTM_NEWLINK:
    case RTM_DELLINK:
      DBG("KRT: Received async link notification (%d)\n", h->nlmsg_type);
      nl_parse_link(h, 0);
      break;
    case RTM_NEWADDR:
    case RTM_DELADDR:
      DBG("KRT: Received async address notification (%d)\n", h->nlmsg_type);
      nl_parse_addr(h);
      break;
    default:
      DBG("KRT: Received unknown async notification (%d)\n", h->nlmsg_type);
    }
}

static int
nl_async_hook(sock *sk, int size)
{
  struct krt_proto *p = sk->data;
  struct iovec iov = { nl_async_rx_buffer, NL_RX_SIZE };
  struct sockaddr_nl sa;
  struct msghdr m = { (struct sockaddr *) &sa, sizeof(sa), &iov, 1, NULL, 0, 0 };
  struct nlmsghdr *h;
  int x;
  unsigned int len;

  nl_last_hdr = NULL;		/* Discard packets accidentally remaining in the rxbuf */
  x = recvmsg(sk->fd, &m, 0);
  if (x < 0)
    {
      if (errno != EWOULDBLOCK)
	log(L_ERR "Netlink recvmsg: %m");
      return 0;
    }
  if (sa.nl_pid)		/* It isn't from the kernel */
    {
      DBG("Non-kernel packet\n");
      return 1;
    }
  h = (void *) nl_async_rx_buffer;
  len = x;
  if (m.msg_flags & MSG_TRUNC)
    {
      log(L_WARN "Netlink got truncated asynchronous message");
      return 1;
    }
  while (NLMSG_OK(h, len))
    {
      nl_async_msg(p, h);
      h = NLMSG_NEXT(h, len);
    }
  if (len)
    log(L_WARN "nl_async_hook: Found packet remnant of size %d", len);
  return 1;
}

static void
nl_open_async(struct krt_proto *p)
{
  sock *sk;
  struct sockaddr_nl sa;
  int fd;

  DBG("KRT: Opening async netlink socket\n");

  fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (fd < 0)
    {
      log(L_ERR "Unable to open secondary rtnetlink socket: %m");
      return;
    }

  bzero(&sa, sizeof(sa));
  sa.nl_family = AF_NETLINK;
  sa.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;
  if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0)
    {
      log(L_ERR "Unable to bind secondary rtnetlink socket: %m");
      return;
    }

  sk = nl_async_sk = sk_new(p->p.pool);
  sk->type = SK_MAGIC;
  sk->data = p;
  sk->rx_hook = nl_async_hook;
  sk->fd = fd;
  if (sk_open(sk))
    bug("Netlink: sk_open failed");

  if (!nl_async_rx_buffer)
    nl_async_rx_buffer = xmalloc(NL_RX_SIZE);
}

/*
 *	Interface to the UNIX krt module
 */

void
krt_scan_preconfig(struct krt_config *x)
{
  x->scan.async = 1;
  /* FIXME: Use larger defaults for scanning times? */
}

void
krt_scan_start(struct krt_proto *p)
{
  init_list(&p->scan.temp_ifs);
  nl_open();
  if (KRT_CF->scan.async)
    nl_open_async(p);
}

void
krt_scan_shutdown(struct krt_proto *p)
{
}