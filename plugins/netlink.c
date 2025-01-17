
/* Netlink plugin for IFUP/IFDN and GW events
 *
 * Copyright (C) 2009-2011  Mårten Wikström <marten.wikstrom@keystream.se>
 * Copyright (C) 2009-2022  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>		/* IFNAMSIZ */
#include <sys/socket.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <unistd.h>

#include "finit.h"
#include "cond.h"
#include "helpers.h"
#include "plugin.h"
#include "service.h"

#define  NL_BUFSZ	4096

struct nl_request {
	struct nlmsghdr nh;
	union {
		struct rtmsg     rtm;
		struct ifinfomsg ifi;
	};
};

static int   nl_defidx;
static int   nl_ifdown;
static char *nl_buf;


static void nl_route(struct nlmsghdr *nlmsg, ssize_t len)
{
	char daddr[INET_ADDRSTRLEN];
	char gaddr[INET_ADDRSTRLEN];
	struct in_addr ind, ing;
	struct rtmsg *r;
	struct rtattr *a;
	int plen = 0;
	int dst = 0;
	int idx = 0;
	int gw = 0;
	int la;

	if (nlmsg->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg))) {
		_e("Packet too small or truncated!");
		return;
	}

	r  = NLMSG_DATA(nlmsg);
	a  = RTM_RTA(r);
	la = RTM_PAYLOAD(nlmsg);
	if (la >= len) {
		_e("Packet too large!");
		return;
	}

	while (RTA_OK(a, la)) {
		void *data = RTA_DATA(a);

		switch (a->rta_type) {
		case RTA_GATEWAY:
			gw = *((int *)data);
			//_d("GW: 0x%04x", gw);
			break;

		case RTA_DST:
			dst = *((int *)data);
			plen = r->rtm_dst_len;
			//_d("Prefix LEN: 0x%04x", plen);
			break;

		case RTA_OIF:
			idx = *((int *)data);
			//_d("IDX: 0x%04x", idx);
			break;
		}

		a = RTA_NEXT(a, la);
	}

	ind.s_addr = dst;
	ing.s_addr = gw;
	inet_ntop(AF_INET, &ind, daddr, sizeof(daddr));
	inet_ntop(AF_INET, &ing, gaddr, sizeof(gaddr));
	_d("Got gw %s dst/len %s/%d ifindex %d", gaddr, daddr, plen, idx);

	if ((!dst && !plen) && (gw || idx)) {
		if (nlmsg->nlmsg_type == RTM_DELROUTE) {
			cond_clear("net/route/default");
			nl_defidx = 0;
		} else {
			cond_set("net/route/default");
			nl_defidx = idx;
		}
	}
}

static void net_cond_set(char *ifname, char *cond, int set)
{
	char msg[MAX_ARG_LEN];

	snprintf(msg, sizeof(msg), "net/%s/%s", ifname, cond);
	if (set)
		cond_set(msg);
	else
		cond_clear(msg);
}

static int validate_ifname(const char *ifname)
{
	if (!ifname || !ifname[0])
		return 1;

	if (strnlen(ifname, IFNAMSIZ) == IFNAMSIZ)
		return 1;

	if (!strcmp(ifname, ".") || !strcmp(ifname, ".."))
		return 1;

	while (*ifname) {
		if (*ifname == '/' || *ifname == ':' || isspace(*ifname))
			return 1;
		ifname++;
	}

	return 0;
}

/*
 * Check if this interface was associated with the default route
 * previously, or if it's been removed.  If so, trigger a recheck
 * of the system default route.
 */
static void nl_check_default(char *ifname)
{
	int idx = (int)if_nametoindex(ifname);

	if ((nl_defidx > 0 && nl_defidx == idx) || (idx == 0 && errno == ENODEV))
		nl_ifdown = 1;
}

static void nl_link(struct nlmsghdr *nlmsg, ssize_t len)
{
	char ifname[IFNAMSIZ + 1];
	struct ifinfomsg *i;
	struct rtattr *a;
	int la;

	if (nlmsg->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg))) {
		_e("Packet too small or truncated!");
		return;
	}

	i  = NLMSG_DATA(nlmsg);
	a  = (struct rtattr *)((char *)i + NLMSG_ALIGN(sizeof(struct ifinfomsg)));
	la = NLMSG_PAYLOAD(nlmsg, sizeof(struct ifinfomsg));
	if (la >= len) {
		_e("Packet too large!");
		return;
	}

	for (; RTA_OK(a, la); a = RTA_NEXT(a, la)) {
		if (a->rta_type != IFLA_IFNAME)
			continue;

		strlcpy(ifname, RTA_DATA(a), sizeof(ifname));
		if (validate_ifname(ifname)) {
			_d("Invalid interface name '%s', skipping ...", ifname);
			continue;
		}

		switch (nlmsg->nlmsg_type) {
		case RTM_NEWLINK:
			/*
			 * New interface has appeared, or interface flags has changed.
			 * Check ifi_flags here to see if the interface is UP/DOWN
			 */
			_d("%s: New link, flags 0x%x, change 0x%x", ifname, i->ifi_flags, i->ifi_change);
			net_cond_set(ifname, "exist",   1);
			net_cond_set(ifname, "up",      i->ifi_flags & IFF_UP);
			net_cond_set(ifname, "running", i->ifi_flags & IFF_RUNNING);
			if (!(i->ifi_flags & IFF_UP) || !(i->ifi_flags & IFF_RUNNING))
				nl_check_default(ifname);
			break;

		case RTM_DELLINK:
			/* NOTE: Interface has disappeared, not link down ... */
			_d("%s: Delete link", ifname);
			net_cond_set(ifname, "exist",   0);
			net_cond_set(ifname, "up",      0);
			net_cond_set(ifname, "running", 0);
			nl_check_default(ifname);
			break;

		case RTM_NEWADDR:
			_d("%s: New Address", ifname);
			break;

		case RTM_DELADDR:
			_d("%s: Deconfig Address", ifname);
			break;

		default:
			_d("%s: Msg 0x%x", ifname, nlmsg->nlmsg_type);
			break;
		}
	}
}

static int nl_parse(int sd)
{
	while (1) {
		struct nlmsghdr *nh;
		ssize_t len;
		size_t l;

		while ((len = recv(sd, nl_buf, NL_BUFSZ, 0)) < 0) {
			switch (errno) {
			case EAGAIN:	/* Nothing more right now. */
				return 0;

			case EINTR:	/* Signal */
				continue;

			case ENOBUFS:	/* netlink(7) */
				break;

			default:
				_pe("recv()");
				break;
			}

			return -1;
		}

//		_d("recv %zd bytes", len);
		l = (size_t)len;
		for (nh = (struct nlmsghdr *)nl_buf; NLMSG_OK(nh, l); nh = NLMSG_NEXT(nh, l)) {
			struct nlmsgerr *nle;

			switch (nh->nlmsg_type) {
			case NLMSG_DONE:
//				_d("Done with netlink messages.");
				return 0;

			case NLMSG_ERROR:
//				_d("Kernel netlink comm. error.");
				nle = NLMSG_DATA(nh);
				if (nle) {
					errno = -nle->error;
					_pe("Kernel netlink error %d", errno);
				}
				return -1;

			case RTM_NEWROUTE:
			case RTM_DELROUTE:
//				_d("Netlink route ...");
				nl_route(nh, len);
				break;

			case RTM_NEWLINK:
			case RTM_DELLINK:
//				_d("Netlink link ...");
				nl_link(nh, len);
				break;

			default:
				_w("unhandled netlink message, type %d", nh->nlmsg_type);
				break;
			}
		}
	}
}

static int nl_request(int sd, unsigned int seq, int type)
{
	struct nl_request *nlr = (struct nl_request *)nl_buf;

	memset(nlr, 0, sizeof(struct nl_request));
	nlr->nh.nlmsg_type  = type;
	nlr->nh.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
	nlr->nh.nlmsg_seq   = seq;
	nlr->nh.nlmsg_pid   = 1;

	switch (type) {
	case RTM_GETROUTE:
//		_d("RTM_GETROUTE");
		nlr->rtm.rtm_family = AF_INET;
		nlr->rtm.rtm_table  = RT_TABLE_MAIN;
		nlr->nh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
		break;

	case RTM_GETLINK:
//		_d("RTM_GETLINK");
		nlr->ifi.ifi_family = AF_UNSPEC;
		nlr->ifi.ifi_change = 0xFFFFFFFF;
		nlr->nh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ifinfomsg));
		break;

	default:
		_w("Cannot resync, unhandled message type %d", type);
		return -1;
	}

	if (send(sd, nlr, nlr->nh.nlmsg_len, 0) < 0)
		return 1;

	return nl_parse(sd);
}

static void nl_resync_routes(int sd, unsigned int seq)
{
	if (nl_request(sd, seq, RTM_GETROUTE))
		_pe("Failed netlink route request");
}

static void nl_resync_ifaces(int sd, unsigned int seq)
{
	if (nl_request(sd, seq, RTM_GETLINK))
		_pe("Failed netlink link request");
}

/*
 * We've potentially lost netlink events, let's resync with kernel.
 */
static void nl_resync(int all)
{
	unsigned int seq = 0;
	int sd;

	sd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (sd < 0) {
		_pe("netlink socket");
		return;
	}

	if (all) {
		_d("============================ RESYNC =================================");
		/* this doesn't update conditions, and thus does not stop services */
		cond_deassert("net/");

		nl_resync_ifaces(sd, seq++);
		nl_resync_routes(sd, seq++);

		/* delayed update after we've corrected things */
		service_step_all(SVC_TYPE_ANY);
		_d("=========================== RESYNCED ================================");
	} else
		nl_resync_routes(sd, seq++);

	close(sd);
}

static void nl_callback(void *arg, int sd, int events)
{
	if (nl_parse(sd) < 0) {
		if (errno == ENOBUFS) {	/* netlink(7) */
			_w("busy system, resynchronizing with kernel.");
			nl_resync(1);
			return;
		}
	}

	/*
	 * Linux doesn't send route changes when interfaces go down, so
	 * we need to check ourselves, e.g. for loss of default route.
	 */
	if (nl_ifdown) {
		_d("interface down, checking default route.");
		if (nl_defidx > 0) {
			nl_defidx = 0;
			nl_resync(0);
			if (nl_defidx <= 0) {
				cond_clear("net/route/default");
				nl_defidx = 0;
			}
		}

		nl_ifdown = 0;
	}
}

static void nl_reconf(void *arg)
{
	cond_reassert("net/");
}

static plugin_t plugin = {
	.name = __FILE__,
	.hook[HOOK_SVC_RECONF] = { .cb = nl_reconf },
	.io = {
		.cb    = nl_callback,
		.flags = PLUGIN_IO_READ,
	},
};

PLUGIN_INIT(plugin_init)
{
	struct sockaddr_nl sa;
	int sd;

	sd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (sd < 0) {
		_pe("socket()");
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_IPV4_ROUTE | RTMGRP_LINK;
	sa.nl_pid    = getpid();

	if (bind(sd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		_pe("bind()");
		close(sd);
		return;
	}

	nl_buf = malloc(NL_BUFSZ);
	if (!nl_buf) {
		_pe("malloc()");
		close(sd);
		return;
	}

	plugin.io.fd = sd;
	plugin_register(&plugin);
}

PLUGIN_EXIT(plugin_exit)
{
	plugin_unregister(&plugin);
	close(plugin.io.fd);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
