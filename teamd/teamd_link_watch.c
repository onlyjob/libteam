/*
 *   teamd_link_watch.c - Team port link watchers
 *   Copyright (C) 2012 Jiri Pirko <jpirko@redhat.com>
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <netdb.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <net/if_arp.h>
#include <linux/filter.h>
#include <time.h>
#include <private/misc.h>
#include <team.h>

#include "teamd.h"

struct lw_common_port_priv {
	const struct teamd_link_watch *link_watch;
	struct teamd_context *ctx;
	struct teamd_port *tdport;
	bool link_up;
	bool forced_send;
	json_t *link_watch_json;
};

struct teamd_link_watch {
	const char *name;
	json_t *(*state_json)(struct teamd_context *ctx,
			      struct teamd_port *tdport,
			      struct lw_common_port_priv *common_ppriv);
	struct teamd_port_priv port_priv;
};

static int __set_sockaddr(struct sockaddr *sa, socklen_t sa_len,
			  sa_family_t family, const char *hostname)
{
	struct addrinfo *result;
	struct addrinfo hints;
	int err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	err = getaddrinfo(hostname, NULL, &hints, &result);
	if (err) {
		teamd_log_err("getaddrinfo failed: %s", gai_strerror(err));
		return -EINVAL;
	}
	if (sa_len != result->ai_addrlen) {
		/* This should not happen, so just to be safe */
		teamd_log_err("Wrong address length in result.");
		return -EINVAL;
	}
	memcpy(sa, result->ai_addr, sa_len);
	freeaddrinfo(result);
	return 0;
}

static int set_in_addr(struct in_addr *addr, const char *hostname)
{
	struct sockaddr_in sin;
	int err;

	err = __set_sockaddr((struct sockaddr *) &sin, sizeof(sin),
			     AF_INET, hostname);
	if (err)
		return err;
	memcpy(addr, &sin.sin_addr, sizeof(*addr));
	return 0;
}

static int set_sockaddr_in6(struct sockaddr_in6 *sin6, const char *hostname)
{
	int err;

	err = __set_sockaddr((struct sockaddr *) sin6, sizeof(*sin6),
			     AF_INET6, hostname);
	if (err)
		return err;
	return 0;
}

static char *__str_sockaddr(struct sockaddr *sa, socklen_t sa_len,
			    sa_family_t family)
{
	static char buf[NI_MAXHOST];
	int err;

	sa->sa_family = family;
	err = getnameinfo(sa, sa_len, buf, sizeof(buf),
			  NULL, 0, NI_NUMERICHOST);
	if (err) {
		teamd_log_err("getnameinfo failed: %s", gai_strerror(err));
		return NULL;
	}
	return buf;
}

static char *str_in_addr(struct in_addr *addr)
{
	struct sockaddr_in sin;

	memcpy(&sin.sin_addr, addr, sizeof(*addr));
	return __str_sockaddr((struct sockaddr *) &sin,
			      sizeof(sin), AF_INET);
}

static char *str_sockaddr_in6(struct sockaddr_in6 *sin6)
{
	return __str_sockaddr((struct sockaddr *) sin6,
			      sizeof(*sin6), AF_INET6);
}

static bool teamd_link_watch_link_up_differs(struct lw_common_port_priv *common_ppriv,
					     bool new_link_up)
{
	return new_link_up != common_ppriv->link_up;
}

static int teamd_link_watch_check_link_up(struct teamd_context *ctx,
					  struct teamd_port *tdport,
					  struct lw_common_port_priv *common_ppriv,
					  bool new_link_up)
{
	const char *lw_name = common_ppriv->link_watch->name;

	if (!teamd_link_watch_link_up_differs(common_ppriv, new_link_up))
		return 0;
	common_ppriv->link_up = new_link_up;
	teamd_log_info("%s: %s-link went %s.", tdport->ifname, lw_name,
		       new_link_up ? "up" : "down");
	return teamd_event_port_link_changed(ctx, tdport);
}


/*
 * Ethtool link watch
 */

struct lw_ethtool_port_priv {
	struct lw_common_port_priv common; /* must be first */
	struct timespec delay_up;
	struct timespec delay_down;
};

static struct lw_ethtool_port_priv *
lw_ethtool_ppriv_get(struct lw_common_port_priv *common_ppriv)
{
	return (struct lw_ethtool_port_priv *) common_ppriv;
}

#define LW_ETHTOOL_DELAY_CB_NAME "lw_ethtool_delay"

static int lw_ethtool_event_watch_port_changed(struct teamd_context *ctx,
					       struct teamd_port *tdport,
					       void *priv)
{
	struct lw_common_port_priv *common_ppriv = priv;
	struct lw_ethtool_port_priv *ethtool_ppriv = priv;
	bool link_up;
	struct timespec *delay;
	int err;

	if (common_ppriv->tdport != tdport ||
	    !team_is_port_changed(tdport->team_port))
		return 0;

	/*
	 * Link changed for sure, so if there is some delay in progress,
	 * cancel it before proceeding.
	 */
	teamd_loop_callback_disable(ctx, LW_ETHTOOL_DELAY_CB_NAME, priv);
	link_up = team_is_port_link_up(tdport->team_port);
	if (!teamd_link_watch_link_up_differs(common_ppriv, link_up))
		return 0;

	if (link_up) {
		if (timespec_is_zero(&ethtool_ppriv->delay_up))
			goto nodelay;
		delay = &ethtool_ppriv->delay_up;
	} else {
		if (timespec_is_zero(&ethtool_ppriv->delay_down))
			goto nodelay;
		delay = &ethtool_ppriv->delay_down;
	}

	err = teamd_loop_callback_timer_set(ctx, LW_ETHTOOL_DELAY_CB_NAME,
					    priv, NULL, delay);
	if (err) {
		teamd_log_err("Failed to set delay timer.");
		return err;
	}
	teamd_loop_callback_enable(ctx, LW_ETHTOOL_DELAY_CB_NAME, priv);
	return 0;

nodelay:
	return teamd_link_watch_check_link_up(ctx, tdport, common_ppriv,
					      link_up);
}

static int lw_ethtool_callback_delay(struct teamd_context *ctx, int events,
				     void *priv)
{
	struct lw_common_port_priv *common_ppriv = priv;
	struct teamd_port *tdport;
	bool link_up;

	tdport = common_ppriv->tdport;
	link_up = team_is_port_link_up(tdport->team_port);
	return teamd_link_watch_check_link_up(ctx, tdport, common_ppriv,
					      link_up);
}

static int lw_ethtool_load_options(struct teamd_context *ctx,
				   struct teamd_port *tdport,
				   struct lw_ethtool_port_priv *ethtool_ppriv)
{
	json_t *link_watch_json = ethtool_ppriv->common.link_watch_json;
	int err;
	int tmp;

	err = json_unpack(link_watch_json, "{s:i}", "delay_up", &tmp);
	if (!err) {
		if (tmp < 0) {
			teamd_log_err("\"delay_up\" must not be negative number.");
			return -EINVAL;
		}
		teamd_log_dbg("delay_up \"%d\".", tmp);
		ms_to_timespec(&ethtool_ppriv->delay_up, tmp);
	}
	err = json_unpack(link_watch_json, "{s:i}", "delay_down", &tmp);
	if (!err) {
		if (tmp < 0) {
			teamd_log_err("\"delay_down\" must not be negative number.");
			return -EINVAL;
		}
		teamd_log_dbg("delay_down \"%d\".", tmp);
		ms_to_timespec(&ethtool_ppriv->delay_down, tmp);
	}
	return 0;
}

static const struct teamd_event_watch_ops lw_ethtool_port_watch_ops = {
	.port_changed = lw_ethtool_event_watch_port_changed,
};

static int lw_ethtool_port_added(struct teamd_context *ctx,
				 struct teamd_port *tdport,
				 void *priv, void *creator_priv)
{
	int err;

	err = lw_ethtool_load_options(ctx, tdport, priv);
	if (err) {
		teamd_log_err("Failed to load options.");
		return err;
	}
	err = teamd_loop_callback_timer_add(ctx, LW_ETHTOOL_DELAY_CB_NAME,
					    priv, lw_ethtool_callback_delay);
	if (err) {
		teamd_log_err("Failed add delay callback timer");
		return err;
	}
	err = teamd_event_watch_register(ctx, &lw_ethtool_port_watch_ops, priv);
	if (err) {
		teamd_log_err("Failed to register event watch.");
		goto delay_callback_del;
	}
	return 0;

delay_callback_del:
	teamd_loop_callback_del(ctx, LW_ETHTOOL_DELAY_CB_NAME, priv);
	return err;
}

static void lw_ethtool_port_removed(struct teamd_context *ctx,
				    struct teamd_port *tdport,
				    void *priv, void *creator_priv)
{
	teamd_event_watch_unregister(ctx, &lw_ethtool_port_watch_ops, priv);
	teamd_loop_callback_del(ctx, LW_ETHTOOL_DELAY_CB_NAME, priv);
}

static json_t *lw_ethtool_state_json(struct teamd_context *ctx,
				     struct teamd_port *tdport,
				     struct lw_common_port_priv *common_ppriv)
{
	struct lw_ethtool_port_priv *ethtool_ppriv;

	ethtool_ppriv = lw_ethtool_ppriv_get(common_ppriv);
	return json_pack("{s:i, s:i}",
			 "delay_up",
			 timespec_to_ms(&ethtool_ppriv->delay_up),
			 "delay_down",
			 timespec_to_ms(&ethtool_ppriv->delay_up));
}

static const struct teamd_link_watch teamd_link_watch_ethtool = {
	.name			= "ethtool",
	.state_json		= lw_ethtool_state_json,
	.port_priv = {
		.init		= lw_ethtool_port_added,
		.fini		= lw_ethtool_port_removed,
		.priv_size	= sizeof(struct lw_ethtool_port_priv),
	},
};


/*
 * Generic periodic send/receive link watch "template"
 */

struct lw_psr_port_priv;

struct lw_psr_ops {
	int (*sock_open)(struct lw_psr_port_priv *psr_ppriv);
	void (*sock_close)(struct lw_psr_port_priv *psr_ppriv);
	int (*load_options)(struct teamd_context *ctx,
			    struct teamd_port *tdport,
			    struct lw_psr_port_priv *psr_ppriv);
	int (*send)(struct lw_psr_port_priv *psr_ppriv);
	int (*receive)(struct lw_psr_port_priv *psr_ppriv);
};

struct lw_psr_port_priv {
	struct lw_common_port_priv common; /* must be first */
	const struct lw_psr_ops *ops;
	struct timespec interval;
	struct timespec init_wait;
	unsigned int missed_max;
	int sock;
	unsigned int missed;
	bool reply_received;
};

static struct lw_psr_port_priv *
lw_psr_ppriv_get(struct lw_common_port_priv *common_ppriv)
{
	return (struct lw_psr_port_priv *) common_ppriv;
}

static int lw_psr_callback_periodic(struct teamd_context *ctx, int events,
				    void *priv)
{
	struct lw_common_port_priv *common_ppriv = priv;
	struct lw_psr_port_priv *psr_ppriv = priv;
	struct teamd_port *tdport = common_ppriv->tdport;
	bool link_up = common_ppriv->link_up;
	int err;

	if (psr_ppriv->reply_received) {
		link_up = true;
		psr_ppriv->missed = 0;
	} else {
		psr_ppriv->missed++;
		if (psr_ppriv->missed > psr_ppriv->missed_max && link_up) {
			teamd_log_dbg("%s: Missed %u replies (max %u).",
				      tdport->ifname, psr_ppriv->missed,
				      psr_ppriv->missed_max);
			link_up = false;
		}
	}
	err = teamd_link_watch_check_link_up(ctx, tdport,
					     common_ppriv, link_up);
	if (err)
		return err;
	psr_ppriv->reply_received = false;

	return psr_ppriv->ops->send(psr_ppriv);
}

static int lw_psr_callback_socket(struct teamd_context *ctx, int events,
				  void *priv)
{
	struct lw_psr_port_priv *psr_ppriv = priv;

	return psr_ppriv->ops->receive(psr_ppriv);
}

static const struct timespec lw_psr_default_init_wait = { 0, 1 };

static int lw_psr_load_options(struct teamd_context *ctx,
			       struct teamd_port *tdport,
			       struct lw_psr_port_priv *psr_ppriv)
{
	json_t *link_watch_json = psr_ppriv->common.link_watch_json;
	int err;
	int tmp;

	err = json_unpack(link_watch_json, "{s:i}", "interval", &tmp);
	if (err) {
		teamd_log_err("Failed to get \"interval\" link-watch option.");
		return -EINVAL;
	}
	teamd_log_dbg("interval \"%d\".", tmp);
	ms_to_timespec(&psr_ppriv->interval, tmp);

	err = json_unpack(link_watch_json, "{s:i}", "init_wait", &tmp);
	if (!err)
		ms_to_timespec(&psr_ppriv->init_wait, tmp);
	/* if init_wait is set to 0, use default_init_wait */
	if (err || !tmp)
		psr_ppriv->init_wait = lw_psr_default_init_wait;
	teamd_log_dbg("init_wait \"%d\".", timespec_to_ms(&psr_ppriv->init_wait));

	err = json_unpack(link_watch_json, "{s:i}", "missed_max", &tmp);
	if (err) {
		teamd_log_err("Failed to get \"missed_max\" link-watch option.");
		return -EINVAL;
	}
	if (tmp < 0) {
		teamd_log_err("\"missed_max\" must not be negative number.");
		return -EINVAL;
	}
	teamd_log_dbg("missed_max \"%d\".", tmp);
	psr_ppriv->missed_max = tmp;

	return 0;
}

#define LW_PERIODIC_CB_NAME "lw_periodic"
#define LW_SOCKET_CB_NAME "lw_socket"

static int lw_psr_port_added(struct teamd_context *ctx,
			     struct teamd_port *tdport,
			     void *priv, void *creator_priv)
{
	struct lw_psr_port_priv *psr_ppriv = priv;
	int err;

	err = lw_psr_load_options(ctx, tdport, psr_ppriv);
	if (err) {
		teamd_log_err("Failed to load options.");
		return err;
	}

	err = psr_ppriv->ops->load_options(ctx, tdport, psr_ppriv);
	if (err) {
		teamd_log_err("Failed to load options.");
		return err;
	}

	err = psr_ppriv->ops->sock_open(psr_ppriv);
	if (err) {
		teamd_log_err("Failed to create socket.");
		return err;
	}

	err = teamd_loop_callback_fd_add(ctx, LW_SOCKET_CB_NAME, psr_ppriv,
					 lw_psr_callback_socket,
					 psr_ppriv->sock,
					 TEAMD_LOOP_FD_EVENT_READ);
	if (err) {
		teamd_log_err("Failed add socket callback.");
		goto close_sock;
	}

	err = teamd_loop_callback_timer_add_set(ctx, LW_PERIODIC_CB_NAME,
						psr_ppriv,
						lw_psr_callback_periodic,
						&psr_ppriv->interval,
						&psr_ppriv->init_wait);
	if (err) {
		teamd_log_err("Failed add callback timer");
		goto socket_callback_del;
	}

	err = team_set_port_user_linkup_enabled(ctx->th, tdport->ifindex, true);
	if (err) {
		teamd_log_err("%s: Failed to enable user linkup.",
			      tdport->ifname);
		goto periodic_callback_del;
	}

	teamd_loop_callback_enable(ctx, LW_SOCKET_CB_NAME, psr_ppriv);
	teamd_loop_callback_enable(ctx, LW_PERIODIC_CB_NAME, psr_ppriv);
	return 0;

periodic_callback_del:
	teamd_loop_callback_del(ctx, LW_PERIODIC_CB_NAME, psr_ppriv);
socket_callback_del:
	teamd_loop_callback_del(ctx, LW_SOCKET_CB_NAME, psr_ppriv);
close_sock:
	psr_ppriv->ops->sock_close(psr_ppriv);
	return err;
}

static void lw_psr_port_removed(struct teamd_context *ctx,
				struct teamd_port *tdport,
				void *priv, void *creator_priv)
{
	struct lw_psr_port_priv *psr_ppriv = priv;

	teamd_loop_callback_del(ctx, LW_PERIODIC_CB_NAME, psr_ppriv);
	teamd_loop_callback_del(ctx, LW_SOCKET_CB_NAME, psr_ppriv);
	psr_ppriv->ops->sock_close(psr_ppriv);
}


/*
 * ARP ping link watch
 */

struct lw_ap_port_priv {
	union {
		struct lw_common_port_priv common;
		struct lw_psr_port_priv psr;
	} start; /* must be first */
	struct in_addr src;
	struct in_addr dst;
	bool validate_active;
	bool validate_inactive;
	bool send_always;
	bool vlanid_in_use;
	unsigned short vlanid;
};

static struct lw_ap_port_priv *
lw_ap_ppriv_get(struct lw_psr_port_priv *psr_ppriv)
{
	return (struct lw_ap_port_priv *) psr_ppriv;
}

#define OFFSET_ARP_OP_CODE					\
	in_struct_offset(struct arphdr, ar_op)

static struct sock_filter arp_rpl_flt[] = {
	BPF_STMT(BPF_LD + BPF_H + BPF_ABS, OFFSET_ARP_OP_CODE),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARPOP_REPLY, 1, 0),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARPOP_REQUEST, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, (u_int) -1),
	BPF_STMT(BPF_RET + BPF_K, 0),
};

static const struct sock_fprog arp_rpl_fprog = {
	.len = ARRAY_SIZE(arp_rpl_flt),
	.filter = arp_rpl_flt,
};

static struct sock_filter arp_novlan_rpl_flt[] = {
	BPF_STMT(BPF_LD + BPF_B + BPF_ABS, SKF_AD_OFF + SKF_AD_VLAN_TAG_PRESENT),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0, 0, 4),
	BPF_STMT(BPF_LD + BPF_H + BPF_ABS, OFFSET_ARP_OP_CODE),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARPOP_REPLY, 1, 0),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARPOP_REQUEST, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, (u_int) -1),
	BPF_STMT(BPF_RET + BPF_K, 0),
};

static const struct sock_fprog arp_novlan_rpl_fprog = {
	.len = ARRAY_SIZE(arp_novlan_rpl_flt),
	.filter = arp_novlan_rpl_flt,
};

static struct sock_filter arp_vlan_rpl_flt[] = {
	BPF_STMT(BPF_LD + BPF_B + BPF_ABS, SKF_AD_OFF + SKF_AD_VLAN_TAG_PRESENT),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0, 6, 0),
	BPF_STMT(BPF_LD + BPF_B + BPF_ABS, SKF_AD_OFF + SKF_AD_VLAN_TAG),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0xffff, 0, 4), /* 0xffff will be replaced by vland id */
	BPF_STMT(BPF_LD + BPF_H + BPF_ABS, OFFSET_ARP_OP_CODE),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARPOP_REPLY, 1, 0),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ARPOP_REQUEST, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, (u_int) -1),
	BPF_STMT(BPF_RET + BPF_K, 0),
};

/* this hack replaces vlanid value in filter code */
#define SET_FILTER_VLANID(fprog, vlanid) (fprog)->filter[3].k = vlanid

static const struct sock_fprog arp_vlan_rpl_fprog = {
	.len = ARRAY_SIZE(arp_vlan_rpl_flt),
	.filter = arp_vlan_rpl_flt,
};

static int lw_ap_sock_open(struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_ap_port_priv *ap_ppriv = lw_ap_ppriv_get(psr_ppriv);
	struct sock_fprog fprog;
	struct sock_filter arp_vlan_rpl_flt[ARRAY_SIZE(arp_vlan_rpl_flt)];

	if (ap_ppriv->vlanid_in_use) {
		memcpy(&arp_vlan_rpl_flt, arp_vlan_rpl_fprog.filter,
		       sizeof(arp_vlan_rpl_flt));
		fprog = arp_vlan_rpl_fprog;
		fprog.filter = arp_vlan_rpl_flt;
		SET_FILTER_VLANID(&fprog, ap_ppriv->vlanid);
	} else {
		fprog = arp_novlan_rpl_fprog;
	}
	return teamd_packet_sock_open(&psr_ppriv->sock,
				      psr_ppriv->common.tdport->ifindex,
				      htons(ETH_P_ARP), &fprog, &arp_rpl_fprog);
}

static void lw_ap_sock_close(struct lw_psr_port_priv *psr_ppriv)
{
	close(psr_ppriv->sock);
}

static int lw_ap_load_options(struct teamd_context *ctx,
			      struct teamd_port *tdport,
			      struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_ap_port_priv *ap_ppriv = lw_ap_ppriv_get(psr_ppriv);
	json_t *link_watch_json = psr_ppriv->common.link_watch_json;
	char *host;
	int tmp;
	int err;

	err = json_unpack(link_watch_json, "{s:s}", "source_host", &host);
	if (err) {
		teamd_log_err("Failed to get \"source_host\" link-watch option.");
		return -EINVAL;
	}
	err = set_in_addr(&ap_ppriv->src, host);
	if (err)
		return err;
	teamd_log_dbg("source address \"%s\".",
		      str_in_addr(&ap_ppriv->src));

	err = json_unpack(link_watch_json, "{s:s}", "target_host", &host);
	if (err) {
		teamd_log_err("Failed to get \"target_host\" link-watch option.");
		return -EINVAL;
	}
	err = set_in_addr(&ap_ppriv->dst, host);
	if (err)
		return err;
	teamd_log_dbg("target address \"%s\".", str_in_addr(&ap_ppriv->dst));

	err = json_unpack(link_watch_json, "{s:b}",  "validate_active", &tmp);
	ap_ppriv->validate_active = err ? false : !!tmp;
	teamd_log_dbg("validate_active \"%d\".", ap_ppriv->validate_active);

	err = json_unpack(link_watch_json, "{s:b}",  "validate_inactive", &tmp);
	ap_ppriv->validate_inactive = err ? false : !!tmp;
	teamd_log_dbg("validate_inactive \"%d\".", ap_ppriv->validate_inactive);

	err = json_unpack(link_watch_json, "{s:b}",  "send_always", &tmp);
	ap_ppriv->send_always = err ? false : !!tmp;
	teamd_log_dbg("send_always \"%d\".", ap_ppriv->send_always);

	err = json_unpack(link_watch_json, "{s:i}", "vlanid", &tmp);
	if (!err) {
		if (tmp < 0 || tmp >= 4096) {
			teamd_log_err("Wrong \"vlanid\" option value.");
			return -EINVAL;
		}
		ap_ppriv->vlanid_in_use = true;
		ap_ppriv->vlanid = tmp;
		teamd_log_dbg("vlan id \"%u\".", ap_ppriv->vlanid);
	}
	err = set_in_addr(&ap_ppriv->dst, host);
	if (err)
		return err;
	teamd_log_dbg("target address \"%s\".", str_in_addr(&ap_ppriv->dst));

	return 0;
}

static int __get_port_curr_hwaddr(struct lw_psr_port_priv *psr_ppriv,
				  struct sockaddr_ll *addr, size_t expected_len)
{
	struct team_ifinfo *ifinfo = psr_ppriv->common.tdport->team_ifinfo;
	size_t port_hwaddr_len = team_get_ifinfo_hwaddr_len(ifinfo);
	char *port_hwaddr = team_get_ifinfo_hwaddr(ifinfo);
	int err;

	err = teamd_getsockname_hwaddr(psr_ppriv->sock, addr, expected_len);
	if (err)
		return err;
	if ((addr->sll_halen != port_hwaddr_len) ||
	    (expected_len && expected_len != port_hwaddr_len)) {
		teamd_log_err("Unexpected length of hw address.");
		return -ENOTSUP;
	}
	memcpy(addr->sll_addr, port_hwaddr, addr->sll_halen);
	return 0;
}

struct arp_packet {
	struct arphdr			ah;
	unsigned char			sender_mac[ETH_ALEN];
	struct in_addr			sender_ip;
	unsigned char			target_mac[ETH_ALEN];
	struct in_addr			target_ip;
} __attribute__((packed));

struct __vlan_hdr {
	__be16 h_vlan_TCI;
	__be16 h_vlan_encapsulated_proto;
};

struct arp_vlan_packet {
	struct __vlan_hdr		vlanh;
	struct arp_packet		ap;
} __attribute__((packed));

static int lw_ap_send(struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_ap_port_priv *ap_ppriv = lw_ap_ppriv_get(psr_ppriv);
	int err;
	struct sockaddr_ll ll_my;
	struct sockaddr_ll ll_bcast;
	struct arp_packet ap;

	if (!(psr_ppriv->common.forced_send || ap_ppriv->send_always))
		return 0;

	err = __get_port_curr_hwaddr(psr_ppriv, &ll_my, 0);
	if (err)
		return err;
	ll_bcast = ll_my;
	memset(ll_bcast.sll_addr, 0xFF, ll_bcast.sll_halen);

	memset(&ap, 0, sizeof(ap));
	ap.ah.ar_hrd = htons(ll_my.sll_hatype);
	ap.ah.ar_pro = htons(ETH_P_IP);
	ap.ah.ar_hln = ll_my.sll_halen;
	ap.ah.ar_pln = 4;
	ap.ah.ar_op = htons(ARPOP_REQUEST);

	memcpy(ap.sender_mac, ll_my.sll_addr, sizeof(ap.sender_mac));
	ap.sender_ip = ap_ppriv->src;
	memcpy(ap.target_mac, ll_bcast.sll_addr, sizeof(ap.target_mac));
	ap.target_ip = ap_ppriv->dst;

	if (ap_ppriv->vlanid_in_use) {
		struct arp_vlan_packet avp;
		avp.ap = ap;
		avp.vlanh.h_vlan_encapsulated_proto = htons(ETH_P_ARP);
		avp.vlanh.h_vlan_TCI = htons(ap_ppriv->vlanid);
		ll_bcast.sll_protocol = htons(ETH_P_8021Q);
		return teamd_sendto(psr_ppriv->sock, &avp, sizeof(avp),
				    0, (struct sockaddr *) &ll_bcast,
				    sizeof(ll_bcast));
	} else {
		return teamd_sendto(psr_ppriv->sock, &ap, sizeof(ap),
				    0, (struct sockaddr *) &ll_bcast,
				    sizeof(ll_bcast));
	}
}

static int lw_ap_receive(struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_common_port_priv *common_ppriv = &psr_ppriv->common;
	struct lw_ap_port_priv *ap_ppriv = lw_ap_ppriv_get(psr_ppriv);
	int err;
	struct sockaddr_ll ll_my;
	struct sockaddr_ll ll_from;
	struct arp_packet ap;
	bool port_enabled;

	err = teamd_recvfrom(psr_ppriv->sock, &ap, sizeof(ap), 0,
			     (struct sockaddr *) &ll_from, sizeof(ll_from));
	if (err <= 0)
		return err;

	err = teamd_port_enabled(common_ppriv->ctx, common_ppriv->tdport,
				 &port_enabled);
	if (err)
		return err;

	if ((port_enabled && ap_ppriv->validate_active) ||
	    (!port_enabled && ap_ppriv->validate_inactive)) {
		err = __get_port_curr_hwaddr(psr_ppriv, &ll_my, 0);
		if (err)
			return err;

		if (ap.ah.ar_hrd != htons(ll_my.sll_hatype) ||
		    ap.ah.ar_pro != htons(ETH_P_IP) ||
		    ap.ah.ar_hln != ll_my.sll_halen ||
		    ap.ah.ar_pln != 4) {
			return 0;
		}

		if ((ap_ppriv->src.s_addr != ap.target_ip.s_addr ||
		     ap_ppriv->dst.s_addr != ap.sender_ip.s_addr) &&
		    (ap_ppriv->dst.s_addr != ap.target_ip.s_addr ||
		     ap_ppriv->src.s_addr != ap.sender_ip.s_addr))
			return 0;
	}

	psr_ppriv->reply_received = true;
	return 0;
}

static const struct lw_psr_ops lw_psr_ops_ap = {
	.sock_open		= lw_ap_sock_open,
	.sock_close		= lw_ap_sock_close,
	.load_options		= lw_ap_load_options,
	.send			= lw_ap_send,
	.receive		= lw_ap_receive,
};

static int lw_ap_port_added(struct teamd_context *ctx,
			    struct teamd_port *tdport,
			    void *priv, void *creator_priv)
{
	struct lw_ap_port_priv *ap_ppriv = priv;
	struct lw_psr_port_priv *psr_ppriv = &ap_ppriv->start.psr;

	psr_ppriv->ops = &lw_psr_ops_ap;
	return lw_psr_port_added(ctx, tdport, priv, creator_priv);
}

static json_t *lw_ap_state_json(struct teamd_context *ctx,
				struct teamd_port *tdport,
				struct lw_common_port_priv *common_ppriv)
{
	struct lw_psr_port_priv *psr_ppriv = lw_psr_ppriv_get(common_ppriv);
	struct lw_ap_port_priv *ap_ppriv = lw_ap_ppriv_get(psr_ppriv);
	static char src[NI_MAXHOST];
	static char dst[NI_MAXHOST];

	strcpy(src, str_in_addr(&ap_ppriv->src));
	strcpy(dst, str_in_addr(&ap_ppriv->dst));
	return json_pack("{s:s, s:s, s:i, s:i, s:b, s:b, s:b, s:i, s:i}",
			 "source_host", src,
			 "target_host", dst,
			 "interval", timespec_to_ms(&psr_ppriv->interval),
			 "init_wait", timespec_to_ms(&psr_ppriv->init_wait),
			 "validate_active", ap_ppriv->validate_active,
			 "validate_inactive", ap_ppriv->validate_inactive,
			 "send_always", ap_ppriv->send_always,
			 "missed_max", psr_ppriv->missed_max,
			 "missed", psr_ppriv->missed);
}

static const struct teamd_link_watch teamd_link_watch_arp_ping = {
	.name			= "arp_ping",
	.state_json		= lw_ap_state_json,
	.port_priv = {
		.init		= lw_ap_port_added,
		.fini		= lw_psr_port_removed,
		.priv_size	= sizeof(struct lw_ap_port_priv),
	},
};


/*
 * IPV6 NS/NA ping link watch
 */

struct lw_nsnap_port_priv {
	union {
		struct lw_common_port_priv common;
		struct lw_psr_port_priv psr;
	} start; /* must be first */
	int tx_sock;
	struct sockaddr_in6 dst;
};

static struct lw_nsnap_port_priv *
lw_nsnap_ppriv_get(struct lw_psr_port_priv *psr_ppriv)
{
	return (struct lw_nsnap_port_priv *) psr_ppriv;
}

static int icmp6_sock_open(int *sock_p)
{
	int sock;
	struct icmp6_filter flt;
	int ret;
	int err;
	int val;

	sock = socket(PF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (sock == -1) {
		teamd_log_err("Failed to create ICMP6 socket.");
		return -errno;
	}

	ICMP6_FILTER_SETBLOCKALL(&flt);
	ret = setsockopt(sock, IPPROTO_ICMPV6, ICMP6_FILTER, &flt, sizeof(flt));
	if (ret == -1) {
		teamd_log_err("Failed to setsockopt ICMP6_FILTER.");
		err = -errno;
		goto close_sock;
	}

	val = 255;
	ret = setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
			 &val, sizeof(val));
	if (ret == -1) {
		teamd_log_err("Failed to setsockopt IPV6_MULTICAST_HOPS.");
		err = -errno;
		goto close_sock;
	}

	*sock_p = sock;
	return 0;
close_sock:
	close(sock);
	return err;
}

#define OFFSET_NEXT_HEADER					\
	in_struct_offset(struct ip6_hdr, ip6_nxt)
#define OFFSET_NA_TYPE						\
	sizeof (struct ip6_hdr) +				\
	in_struct_offset(struct nd_neighbor_advert, nd_na_type)

static struct sock_filter na_flt[] = {
	BPF_STMT(BPF_LD + BPF_B + BPF_ABS, OFFSET_NEXT_HEADER),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, IPPROTO_ICMPV6, 0, 3),
	BPF_STMT(BPF_LD + BPF_B + BPF_ABS, OFFSET_NA_TYPE),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ND_NEIGHBOR_ADVERT, 0, 1),
	BPF_STMT(BPF_RET + BPF_K, (u_int) -1),
	BPF_STMT(BPF_RET + BPF_K, 0),
};

static const struct sock_fprog na_fprog = {
	.len = ARRAY_SIZE(na_flt),
	.filter = na_flt,
};

static int lw_nsnap_sock_open(struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_nsnap_port_priv *nsnap_ppriv = lw_nsnap_ppriv_get(psr_ppriv);
	int err;

	/*
	 * We use two sockets here. NS packets are send through ICMP6 socket.
	 * With this socket, unfortunately, kernel does not provide a way to
	 * deliver incoming ICMP6 packet on inactive ports into userspace.
	 * So we use packet socket to get these packets.
	 */
	err = teamd_packet_sock_open(&psr_ppriv->sock,
				     psr_ppriv->common.tdport->ifindex,
				     htons(ETH_P_IPV6), &na_fprog, NULL);
	if (err)
		return err;
	err = icmp6_sock_open(&nsnap_ppriv->tx_sock);
	if (err)
		goto close_packet_sock;
	return 0;
close_packet_sock:
	close(psr_ppriv->sock);
	return err;
}

static void lw_nsnap_sock_close(struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_nsnap_port_priv *nsnap_ppriv = lw_nsnap_ppriv_get(psr_ppriv);

	close(nsnap_ppriv->tx_sock);
	close(psr_ppriv->sock);
}

static int lw_nsnap_load_options(struct teamd_context *ctx,
				 struct teamd_port *tdport,
				 struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_nsnap_port_priv *nsnap_ppriv = lw_nsnap_ppriv_get(psr_ppriv);
	json_t *link_watch_json = psr_ppriv->common.link_watch_json;
	char *host;
	int err;

	err = json_unpack(link_watch_json, "{s:s}", "target_host", &host);
	if (err) {
		teamd_log_err("Failed to get \"target_host\" link-watch option.");
		return -EINVAL;
	}
	err = set_sockaddr_in6(&nsnap_ppriv->dst, host);
	if (err)
		return err;
	teamd_log_dbg("target address \"%s\".",
		      str_sockaddr_in6(&nsnap_ppriv->dst));

	return 0;
}

static void compute_multi_in6_addr(struct in6_addr *addr)
{
	addr->s6_addr32[0] = htonl(0xFF020000);
	addr->s6_addr32[1] = 0;
	addr->s6_addr32[2] = htonl(0x1);
	addr->s6_addr32[3] |= htonl(0xFF000000);
}

struct ns_packet {
	struct nd_neighbor_solicit	nsh;
	struct nd_opt_hdr		opt;
	unsigned char			hwaddr[ETH_ALEN];
};

static int lw_nsnap_send(struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_nsnap_port_priv *nsnap_ppriv = lw_nsnap_ppriv_get(psr_ppriv);
	int err;
	struct sockaddr_ll ll_my;
	struct sockaddr_in6 sendto_addr;
	struct ns_packet nsp;

	err = teamd_getsockname_hwaddr(psr_ppriv->sock, &ll_my,
				       sizeof(nsp.hwaddr));
	if (err)
		return err;

	memset(&nsp, 0, sizeof(nsp));

	/* setup ICMP6 header */
	nsp.nsh.nd_ns_type = ND_NEIGHBOR_SOLICIT;
	nsp.nsh.nd_ns_cksum = 0; /* kernel computes this */
	nsp.nsh.nd_ns_target = nsnap_ppriv->dst.sin6_addr;
	nsp.opt.nd_opt_type = ND_OPT_SOURCE_LINKADDR;
	nsp.opt.nd_opt_len = 1; /* 8 bytes */
	memcpy(nsp.hwaddr, ll_my.sll_addr, sizeof(nsp.hwaddr));

	sendto_addr = nsnap_ppriv->dst;
	compute_multi_in6_addr(&sendto_addr.sin6_addr);
	sendto_addr.sin6_scope_id = psr_ppriv->common.tdport->ifindex;
	err = teamd_sendto(nsnap_ppriv->tx_sock, &nsp, sizeof(nsp), 0,
			   (struct sockaddr *) &sendto_addr,
			   sizeof(sendto_addr));
	return err;
}

struct na_packet {
	struct ip6_hdr			ip6h;
	struct nd_neighbor_advert	nah;
	struct nd_opt_hdr		opt;
	unsigned char			hwaddr[ETH_ALEN];
};

static int lw_nsnap_receive(struct lw_psr_port_priv *psr_ppriv)
{
	struct lw_nsnap_port_priv *nsnap_ppriv = lw_nsnap_ppriv_get(psr_ppriv);
	struct na_packet nap;
	struct sockaddr_ll ll_from;
	int err;

	err = teamd_recvfrom(psr_ppriv->sock, &nap, sizeof(nap), 0,
			     (struct sockaddr *) &ll_from, sizeof(ll_from));
	if (err <= 0)
		return err;

	/* check IPV6 header */
	if (nap.ip6h.ip6_vfc != 0x60 /* IPV6 */ ||
	    nap.ip6h.ip6_plen != htons(sizeof(nap) - sizeof(nap.ip6h)) ||
	    nap.ip6h.ip6_nxt != IPPROTO_ICMPV6 ||
	    nap.ip6h.ip6_hlim != 255 /* Do not route */ ||
	    memcmp(&nap.ip6h.ip6_src, &nsnap_ppriv->dst.sin6_addr,
		   sizeof(struct in6_addr)))
		return 0;

	/* check ICMP6 header */
	if (nap.nah.nd_na_type != ND_NEIGHBOR_ADVERT ||
	    nap.opt.nd_opt_type != ND_OPT_TARGET_LINKADDR ||
	    nap.opt.nd_opt_len != 1 /* 8 bytes */)
		return 0;

	psr_ppriv->reply_received = true;
	return 0;
}

static const struct lw_psr_ops lw_psr_ops_nsnap = {
	.sock_open		= lw_nsnap_sock_open,
	.sock_close		= lw_nsnap_sock_close,
	.load_options		= lw_nsnap_load_options,
	.send			= lw_nsnap_send,
	.receive		= lw_nsnap_receive,
};

static int lw_nsnap_port_added(struct teamd_context *ctx,
			       struct teamd_port *tdport,
			       void *priv, void *creator_priv)
{
	struct lw_psr_port_priv *psr_port_priv = priv;

	psr_port_priv->ops = &lw_psr_ops_nsnap;
	return lw_psr_port_added(ctx, tdport, priv, creator_priv);
}

static json_t *lw_nsnap_state_json(struct teamd_context *ctx,
				   struct teamd_port *tdport,
				   struct lw_common_port_priv *common_ppriv)
{
	struct lw_psr_port_priv *psr_ppriv = lw_psr_ppriv_get(common_ppriv);
	struct lw_nsnap_port_priv *nsnap_ppriv = lw_nsnap_ppriv_get(psr_ppriv);
	return json_pack("{s:s, s:i, s:i, s:i, s:i}",
			 "target_host", str_sockaddr_in6(&nsnap_ppriv->dst),
			 "interval", timespec_to_ms(&psr_ppriv->interval),
			 "init_wait", timespec_to_ms(&psr_ppriv->init_wait),
			 "missed_max", psr_ppriv->missed_max,
			 "missed", psr_ppriv->missed);
}

static const struct teamd_link_watch teamd_link_watch_nsnap = {
	.name			= "nsna_ping",
	.state_json		= lw_nsnap_state_json,
	.port_priv = {
		.init		= lw_nsnap_port_added,
		.fini		= lw_psr_port_removed,
		.priv_size	= sizeof(struct lw_nsnap_port_priv),
	},
};


/*
 * General link watch code
 */
static const struct teamd_link_watch *teamd_link_watch_list[] = {
	&teamd_link_watch_ethtool,
	&teamd_link_watch_arp_ping,
	&teamd_link_watch_nsnap,
};

#define TEAMD_LINK_WATCH_LIST_SIZE ARRAY_SIZE(teamd_link_watch_list)

/*
 * For port priv identification purposes
 */
#define LW_PORT_PRIV_CREATOR_PRIV (&teamd_link_watch_list)

static const struct teamd_link_watch *teamd_find_link_watch(const char *link_watch_name)
{
	int i;

	for (i = 0; i < TEAMD_LINK_WATCH_LIST_SIZE; i++) {
		if (strcmp(teamd_link_watch_list[i]->name, link_watch_name) == 0)
			return teamd_link_watch_list[i];
	}
	return NULL;
}

bool teamd_link_watch_port_up(struct teamd_context *ctx,
			      struct teamd_port *tdport)
{
	struct lw_common_port_priv *common_ppriv;
	bool link;

	if (!tdport)
		return true;
	link = true;
	teamd_for_each_port_priv_by_creator(common_ppriv, tdport,
					    LW_PORT_PRIV_CREATOR_PRIV) {
		link = common_ppriv->link_up;
		if (link)
			return link;
	}
	return link;
}

static int teamd_link_watch_refresh_user_linkup(struct teamd_context *ctx,
						struct teamd_port *tdport)
{
	bool link;
	bool cur_link;
	int err;

	if (!teamd_port_present(ctx, tdport))
		return 0;

	link = teamd_link_watch_port_up(ctx, tdport);
	err = team_get_port_user_linkup(ctx->th, tdport->ifindex,
					&cur_link);
	if (!err && link == cur_link)
		return 0;
	err = team_set_port_user_linkup(ctx->th, tdport->ifindex,
					link);
	if (err)
		return err;
	return 0;
}

static int link_watch_load_one_json_obj(struct teamd_context *ctx,
					struct teamd_port *tdport,
					json_t *link_watch_obj)
{
	int ret;
	int err;
	const char *link_watch_name;
	const struct teamd_link_watch *link_watch;
	struct lw_common_port_priv *common_ppriv;

	ret = json_unpack(link_watch_obj, "{s:s}", "name", &link_watch_name);
	if (ret) {
		teamd_log_err("%s: Failed to get link watch name.",
			      tdport->ifname);
		return -EINVAL;
	}
	link_watch = teamd_find_link_watch(link_watch_name);
	if (!link_watch) {
		teamd_log_err("No link_watch named \"%s\" available.",
			      link_watch_name);
		return -EINVAL;
	}
	err = teamd_port_priv_create_and_get((void **) &common_ppriv, tdport,
					     &link_watch->port_priv,
					     LW_PORT_PRIV_CREATOR_PRIV);
	if (err)
		return err;
	common_ppriv->link_watch = link_watch;
	common_ppriv->ctx = ctx;
	common_ppriv->tdport = tdport;
	common_ppriv->link_watch_json = link_watch_obj;
	return 0;
}

static int link_watch_load_json_obj(struct teamd_context *ctx,
				    struct teamd_port *tdport,
				    json_t *link_watch_obj)
{
	size_t i;
	int err;

	if (!json_is_array(link_watch_obj))
		return link_watch_load_one_json_obj(ctx, tdport,
						    link_watch_obj);
	for (i = 0; i < json_array_size(link_watch_obj); i++) {
		json_t *obj = json_array_get(link_watch_obj, i);

		err = link_watch_load_one_json_obj(ctx, tdport, obj);
		if (err)
			return err;
	}
	return 0;
}

static int link_watch_event_watch_port_added(struct teamd_context *ctx,
					     struct teamd_port *tdport,
					     void *priv)
{
	int ret;
	int err;
	json_t *link_watch_obj;

	ret = json_unpack(ctx->config_json, "{s:{s:{s:o}}}", "ports",
			  tdport->ifname, "link_watch", &link_watch_obj);
	if (!ret) {
		teamd_log_dbg("%s: Got link watch from port config.",
			      tdport->ifname);
		err = link_watch_load_json_obj(ctx, tdport, link_watch_obj);
		if (err)
			return err;
	}

	ret = json_unpack(ctx->config_json, "{s:o}", "link_watch",
			  &link_watch_obj);
	if (!ret) {
		teamd_log_dbg("Got link watch from global config.");
		err = link_watch_load_json_obj(ctx, tdport, link_watch_obj);
		if (err)
			return err;
	}

	if (!teamd_get_first_port_priv_by_creator(tdport,
						  LW_PORT_PRIV_CREATOR_PRIV))
		teamd_log_info("%s: Using no link watch.", tdport->ifname);
	return 0;
}

static int link_watch_event_watch_port_link_changed(struct teamd_context *ctx,
						    struct teamd_port *tdport,
						    void *priv)
{
	return teamd_link_watch_refresh_user_linkup(ctx, tdport);
}

static void __set_forced_send_for_port(struct teamd_port *tdport,
				       bool forced_send)
{
	struct lw_common_port_priv *common_ppriv;

	teamd_for_each_port_priv_by_creator(common_ppriv, tdport,
					    LW_PORT_PRIV_CREATOR_PRIV) {
		common_ppriv->forced_send = forced_send;
	}
}

static int link_watch_refresh_forced_send(struct teamd_context *ctx)
{
	struct teamd_port *tdport;
	bool port_enabled;
	int enabled_port_count = 0;
	int err;

	teamd_for_each_tdport(tdport, ctx) {
		err = teamd_port_enabled(ctx, tdport, &port_enabled);
		if (err)
			return err;
		__set_forced_send_for_port(tdport, port_enabled);
		if (port_enabled)
			enabled_port_count++;
	}

	/*
	 * In case no ports are enabled, set forced_send to true for all
	 * ports. That enforces active linkwatch approach to regain link
	 * on some port again.
	 */
	if (enabled_port_count == 0) {
		teamd_for_each_tdport(tdport, ctx)
			__set_forced_send_for_port(tdport, true);
	}
	return 0;
}

static int link_watch_enabled_option_changed(struct teamd_context *ctx,
					     struct team_option *option,
					     void *priv)
{
	return link_watch_refresh_forced_send(ctx);
}

static const struct teamd_event_watch_ops link_watch_port_watch_ops = {
	.port_added = link_watch_event_watch_port_added,
	.port_link_changed = link_watch_event_watch_port_link_changed,
	.option_changed = link_watch_enabled_option_changed,
	.option_changed_match_name = "enabled",
};

static json_t *__fill_lw_instance(struct teamd_context *ctx,
				  struct teamd_port *tdport,
				  struct lw_common_port_priv *common_ppriv)
{
	const struct teamd_link_watch *lw = common_ppriv->link_watch;
	json_t *lwinfo_json;
	json_t *state_json;

	if (!lw->state_json)
		lwinfo_json = json_object();
	else
		lwinfo_json = lw->state_json(ctx, tdport, common_ppriv);

	state_json = json_pack("{s:s, s:b, s:o}",
			       "name", lw->name,
			       "up", common_ppriv->link_up,
			       "info", lwinfo_json);
	if (!state_json)
		json_decref(lwinfo_json);
	return state_json;
}

static int link_watch_state_per_port_dump(struct teamd_context *ctx,
					  struct teamd_port *tdport,
					  json_t **pstate_json, void *priv)
{
	struct lw_common_port_priv *common_ppriv;
	int err;
	json_t *state_json;
	json_t *array_json;
	json_t *instance_json;
	bool link;

	array_json = json_array();
	if (!array_json)
		return -ENOMEM;

	teamd_for_each_port_priv_by_creator(common_ppriv, tdport,
					    LW_PORT_PRIV_CREATOR_PRIV) {
		instance_json = __fill_lw_instance(ctx, tdport, common_ppriv);
		if (!instance_json)
			goto errout;
		err = json_array_append_new(array_json, instance_json);
		if (err)
			goto errout;
	}
	link = teamd_link_watch_port_up(ctx, tdport);
	state_json = json_pack("{s:o, s:b}",
			       "list", array_json,
			       "up", link);
	if (!state_json)
		goto errout;
	*pstate_json = state_json;
	return 0;

errout:
	json_decref(array_json);
	return -ENOMEM;
}

static const struct teamd_state_json_ops link_watch_state_ops = {
	.per_port_dump = link_watch_state_per_port_dump,
	.name = "link_watches",
};

int teamd_link_watch_init(struct teamd_context *ctx)
{
	int err;

	err = teamd_event_watch_register(ctx, &link_watch_port_watch_ops, NULL);
	if (err) {
		teamd_log_err("Failed to register event watch.");
		return err;
	}
	err = teamd_state_json_register(ctx, &link_watch_state_ops, ctx);
	if (err)
		goto event_watch_unregister;
	return 0;

event_watch_unregister:
	teamd_event_watch_unregister(ctx, &link_watch_port_watch_ops, NULL);
	return err;
}

void teamd_link_watch_fini(struct teamd_context *ctx)
{
	teamd_event_watch_unregister(ctx, &link_watch_port_watch_ops, NULL);
}
