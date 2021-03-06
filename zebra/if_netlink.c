/*
 * Interface looking up by netlink.
 * Copyright (C) 1998 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#ifdef GNU_LINUX

/* The following definition is to workaround an issue in the Linux kernel
 * header files with redefinition of 'struct in6_addr' in both
 * netinet/in.h and linux/in6.h.
 * Reference - https://sourceware.org/ml/libc-alpha/2013-01/msg00599.html
 */
#define _LINUX_IN6_H

#include <linux/if_bridge.h>
#include <linux/if_link.h>
#include <net/if_arp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>

#include "linklist.h"
#include "if.h"
#include "log.h"
#include "prefix.h"
#include "connected.h"
#include "table.h"
#include "memory.h"
#include "zebra_memory.h"
#include "rib.h"
#include "thread.h"
#include "privs.h"
#include "nexthop.h"
#include "vrf.h"
#include "vrf_int.h"
#include "mpls.h"

#include "vty.h"
#include "zebra/zserv.h"
#include "zebra/zebra_ns.h"
#include "zebra/zebra_vrf.h"
#include "zebra/rt.h"
#include "zebra/redistribute.h"
#include "zebra/interface.h"
#include "zebra/debug.h"
#include "zebra/rtadv.h"
#include "zebra/zebra_ptm.h"
#include "zebra/zebra_mpls.h"
#include "zebra/kernel_netlink.h"
#include "zebra/if_netlink.h"


/* Note: on netlink systems, there should be a 1-to-1 mapping between interface
   names and ifindex values. */
static void set_ifindex(struct interface *ifp, ifindex_t ifi_index,
			struct zebra_ns *zns)
{
	struct interface *oifp;

	if (((oifp = if_lookup_by_index_per_ns(zns, ifi_index)) != NULL)
	    && (oifp != ifp)) {
		if (ifi_index == IFINDEX_INTERNAL)
			zlog_err(
				"Netlink is setting interface %s ifindex to reserved "
				"internal value %u",
				ifp->name, ifi_index);
		else {
			if (IS_ZEBRA_DEBUG_KERNEL)
				zlog_debug(
					"interface index %d was renamed from %s to %s",
					ifi_index, oifp->name, ifp->name);
			if (if_is_up(oifp))
				zlog_err(
					"interface rename detected on up interface: index %d "
					"was renamed from %s to %s, results are uncertain!",
					ifi_index, oifp->name, ifp->name);
			if_delete_update(oifp);
		}
	}
	if_set_index(ifp, ifi_index);
}

/* Utility function to parse hardware link-layer address and update ifp */
static void netlink_interface_update_hw_addr(struct rtattr **tb,
					     struct interface *ifp)
{
	int i;

	if (tb[IFLA_ADDRESS]) {
		int hw_addr_len;

		hw_addr_len = RTA_PAYLOAD(tb[IFLA_ADDRESS]);

		if (hw_addr_len > INTERFACE_HWADDR_MAX)
			zlog_warn("Hardware address is too large: %d",
				  hw_addr_len);
		else {
			ifp->hw_addr_len = hw_addr_len;
			memcpy(ifp->hw_addr, RTA_DATA(tb[IFLA_ADDRESS]),
			       hw_addr_len);

			for (i = 0; i < hw_addr_len; i++)
				if (ifp->hw_addr[i] != 0)
					break;

			if (i == hw_addr_len)
				ifp->hw_addr_len = 0;
			else
				ifp->hw_addr_len = hw_addr_len;
		}
	}
}

static enum zebra_link_type netlink_to_zebra_link_type(unsigned int hwt)
{
	switch (hwt) {
	case ARPHRD_ETHER:
		return ZEBRA_LLT_ETHER;
	case ARPHRD_EETHER:
		return ZEBRA_LLT_EETHER;
	case ARPHRD_AX25:
		return ZEBRA_LLT_AX25;
	case ARPHRD_PRONET:
		return ZEBRA_LLT_PRONET;
	case ARPHRD_IEEE802:
		return ZEBRA_LLT_IEEE802;
	case ARPHRD_ARCNET:
		return ZEBRA_LLT_ARCNET;
	case ARPHRD_APPLETLK:
		return ZEBRA_LLT_APPLETLK;
	case ARPHRD_DLCI:
		return ZEBRA_LLT_DLCI;
	case ARPHRD_ATM:
		return ZEBRA_LLT_ATM;
	case ARPHRD_METRICOM:
		return ZEBRA_LLT_METRICOM;
	case ARPHRD_IEEE1394:
		return ZEBRA_LLT_IEEE1394;
	case ARPHRD_EUI64:
		return ZEBRA_LLT_EUI64;
	case ARPHRD_INFINIBAND:
		return ZEBRA_LLT_INFINIBAND;
	case ARPHRD_SLIP:
		return ZEBRA_LLT_SLIP;
	case ARPHRD_CSLIP:
		return ZEBRA_LLT_CSLIP;
	case ARPHRD_SLIP6:
		return ZEBRA_LLT_SLIP6;
	case ARPHRD_CSLIP6:
		return ZEBRA_LLT_CSLIP6;
	case ARPHRD_RSRVD:
		return ZEBRA_LLT_RSRVD;
	case ARPHRD_ADAPT:
		return ZEBRA_LLT_ADAPT;
	case ARPHRD_ROSE:
		return ZEBRA_LLT_ROSE;
	case ARPHRD_X25:
		return ZEBRA_LLT_X25;
	case ARPHRD_PPP:
		return ZEBRA_LLT_PPP;
	case ARPHRD_CISCO:
		return ZEBRA_LLT_CHDLC;
	case ARPHRD_LAPB:
		return ZEBRA_LLT_LAPB;
	case ARPHRD_RAWHDLC:
		return ZEBRA_LLT_RAWHDLC;
	case ARPHRD_TUNNEL:
		return ZEBRA_LLT_IPIP;
	case ARPHRD_TUNNEL6:
		return ZEBRA_LLT_IPIP6;
	case ARPHRD_FRAD:
		return ZEBRA_LLT_FRAD;
	case ARPHRD_SKIP:
		return ZEBRA_LLT_SKIP;
	case ARPHRD_LOOPBACK:
		return ZEBRA_LLT_LOOPBACK;
	case ARPHRD_LOCALTLK:
		return ZEBRA_LLT_LOCALTLK;
	case ARPHRD_FDDI:
		return ZEBRA_LLT_FDDI;
	case ARPHRD_SIT:
		return ZEBRA_LLT_SIT;
	case ARPHRD_IPDDP:
		return ZEBRA_LLT_IPDDP;
	case ARPHRD_IPGRE:
		return ZEBRA_LLT_IPGRE;
	case ARPHRD_PIMREG:
		return ZEBRA_LLT_PIMREG;
	case ARPHRD_HIPPI:
		return ZEBRA_LLT_HIPPI;
	case ARPHRD_ECONET:
		return ZEBRA_LLT_ECONET;
	case ARPHRD_IRDA:
		return ZEBRA_LLT_IRDA;
	case ARPHRD_FCPP:
		return ZEBRA_LLT_FCPP;
	case ARPHRD_FCAL:
		return ZEBRA_LLT_FCAL;
	case ARPHRD_FCPL:
		return ZEBRA_LLT_FCPL;
	case ARPHRD_FCFABRIC:
		return ZEBRA_LLT_FCFABRIC;
	case ARPHRD_IEEE802_TR:
		return ZEBRA_LLT_IEEE802_TR;
	case ARPHRD_IEEE80211:
		return ZEBRA_LLT_IEEE80211;
#ifdef ARPHRD_IEEE802154
	case ARPHRD_IEEE802154:
		return ZEBRA_LLT_IEEE802154;
#endif
#ifdef ARPHRD_IP6GRE
	case ARPHRD_IP6GRE:
		return ZEBRA_LLT_IP6GRE;
#endif
#ifdef ARPHRD_IEEE802154_PHY
	case ARPHRD_IEEE802154_PHY:
		return ZEBRA_LLT_IEEE802154_PHY;
#endif

	default:
		return ZEBRA_LLT_UNKNOWN;
	}
}

static void netlink_determine_zebra_iftype(char *kind, zebra_iftype_t *zif_type)
{
	*zif_type = ZEBRA_IF_OTHER;

	if (!kind)
		return;

	if (strcmp(kind, "vrf") == 0)
		*zif_type = ZEBRA_IF_VRF;
	else if (strcmp(kind, "bridge") == 0)
		*zif_type = ZEBRA_IF_BRIDGE;
	else if (strcmp(kind, "vlan") == 0)
		*zif_type = ZEBRA_IF_VLAN;
	else if (strcmp(kind, "vxlan") == 0)
		*zif_type = ZEBRA_IF_VXLAN;
	else if (strcmp(kind, "macvlan") == 0)
		*zif_type = ZEBRA_IF_MACVLAN;
}

#define parse_rtattr_nested(tb, max, rta)                                      \
	netlink_parse_rtattr((tb), (max), RTA_DATA(rta), RTA_PAYLOAD(rta))

static void netlink_vrf_change(struct nlmsghdr *h, struct rtattr *tb,
			       const char *name)
{
	struct ifinfomsg *ifi;
	struct rtattr *linkinfo[IFLA_INFO_MAX + 1];
	struct rtattr *attr[IFLA_VRF_MAX + 1];
	struct vrf *vrf;
	struct zebra_vrf *zvrf;
	u_int32_t nl_table_id;

	ifi = NLMSG_DATA(h);

	memset(linkinfo, 0, sizeof linkinfo);
	parse_rtattr_nested(linkinfo, IFLA_INFO_MAX, tb);

	if (!linkinfo[IFLA_INFO_DATA]) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug(
				"%s: IFLA_INFO_DATA missing from VRF message: %s",
				__func__, name);
		return;
	}

	memset(attr, 0, sizeof attr);
	parse_rtattr_nested(attr, IFLA_VRF_MAX, linkinfo[IFLA_INFO_DATA]);
	if (!attr[IFLA_VRF_TABLE]) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug(
				"%s: IFLA_VRF_TABLE missing from VRF message: %s",
				__func__, name);
		return;
	}

	nl_table_id = *(u_int32_t *)RTA_DATA(attr[IFLA_VRF_TABLE]);

	if (h->nlmsg_type == RTM_NEWLINK) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug("RTM_NEWLINK for VRF %s(%u) table %u", name,
				   ifi->ifi_index, nl_table_id);

		/*
		 * vrf_get is implied creation if it does not exist
		 */
		vrf = vrf_get((vrf_id_t)ifi->ifi_index,
			      name); // It would create vrf
		if (!vrf) {
			zlog_err("VRF %s id %u not created", name,
				 ifi->ifi_index);
			return;
		}

		/* Enable the created VRF. */
		if (!vrf_enable(vrf)) {
			zlog_err("Failed to enable VRF %s id %u", name,
				 ifi->ifi_index);
			return;
		}

		/*
		 * This is the only place that we get the actual kernel table_id
		 * being used.  We need it to set the table_id of the routes
		 * we are passing to the kernel.... And to throw some totally
		 * awesome parties. that too.
		 */
		zvrf = (struct zebra_vrf *)vrf->info;
		zvrf->table_id = nl_table_id;
	} else // h->nlmsg_type == RTM_DELLINK
	{
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug("RTM_DELLINK for VRF %s(%u)", name,
				   ifi->ifi_index);

		vrf = vrf_lookup_by_id((vrf_id_t)ifi->ifi_index);

		if (!vrf) {
			zlog_warn("%s: vrf not found", __func__);
			return;
		}

		vrf_delete(vrf);
	}
}

static int get_iflink_speed(const char *ifname)
{
	struct ifreq ifdata;
	struct ethtool_cmd ecmd;
	int sd;
	int rc;

	/* initialize struct */
	memset(&ifdata, 0, sizeof(ifdata));

	/* set interface name */
	strlcpy(ifdata.ifr_name, ifname, sizeof(ifdata.ifr_name));

	/* initialize ethtool interface */
	memset(&ecmd, 0, sizeof(ecmd));
	ecmd.cmd = ETHTOOL_GSET; /* ETHTOOL_GLINK */
	ifdata.ifr_data = (__caddr_t)&ecmd;

	/* use ioctl to get IP address of an interface */
	sd = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sd < 0) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug("Failure to read interface %s speed: %d %s",
				   ifname, errno, safe_strerror(errno));
		return 0;
	}

	/* Get the current link state for the interface */
	rc = ioctl(sd, SIOCETHTOOL, (char *)&ifdata);
	if (rc < 0) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug(
				"IOCTL failure to read interface %s speed: %d %s",
				ifname, errno, safe_strerror(errno));
		ecmd.speed_hi = 0;
		ecmd.speed = 0;
	}

	close(sd);

	return (ecmd.speed_hi << 16) | ecmd.speed;
}

static int netlink_extract_bridge_info(struct rtattr *link_data,
				       struct zebra_l2info_bridge *bridge_info)
{
	struct rtattr *attr[IFLA_BR_MAX + 1];

	memset(bridge_info, 0, sizeof(*bridge_info));
	memset(attr, 0, sizeof attr);
	parse_rtattr_nested(attr, IFLA_BR_MAX, link_data);
	if (attr[IFLA_BR_VLAN_FILTERING])
		bridge_info->vlan_aware =
			*(u_char *)RTA_DATA(attr[IFLA_BR_VLAN_FILTERING]);
	return 0;
}

static int netlink_extract_vlan_info(struct rtattr *link_data,
				     struct zebra_l2info_vlan *vlan_info)
{
	struct rtattr *attr[IFLA_VLAN_MAX + 1];
	vlanid_t vid_in_msg;

	memset(vlan_info, 0, sizeof(*vlan_info));
	memset(attr, 0, sizeof attr);
	parse_rtattr_nested(attr, IFLA_VLAN_MAX, link_data);
	if (!attr[IFLA_VLAN_ID]) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug("IFLA_VLAN_ID missing from VLAN IF message");
		return -1;
	}

	vid_in_msg = *(vlanid_t *)RTA_DATA(attr[IFLA_VLAN_ID]);
	vlan_info->vid = vid_in_msg;
	return 0;
}

static int netlink_extract_vxlan_info(struct rtattr *link_data,
				      struct zebra_l2info_vxlan *vxl_info)
{
	struct rtattr *attr[IFLA_VXLAN_MAX + 1];
	vni_t vni_in_msg;
	struct in_addr vtep_ip_in_msg;

	memset(vxl_info, 0, sizeof(*vxl_info));
	memset(attr, 0, sizeof attr);
	parse_rtattr_nested(attr, IFLA_VXLAN_MAX, link_data);
	if (!attr[IFLA_VXLAN_ID]) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug(
				"IFLA_VXLAN_ID missing from VXLAN IF message");
		return -1;
	}

	vni_in_msg = *(vni_t *)RTA_DATA(attr[IFLA_VXLAN_ID]);
	vxl_info->vni = vni_in_msg;
	if (!attr[IFLA_VXLAN_LOCAL]) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug(
				"IFLA_VXLAN_LOCAL missing from VXLAN IF message");
	} else {
		vtep_ip_in_msg =
			*(struct in_addr *)RTA_DATA(attr[IFLA_VXLAN_LOCAL]);
		vxl_info->vtep_ip = vtep_ip_in_msg;
	}

	return 0;
}

/*
 * Extract and save L2 params (of interest) for an interface. When a
 * bridge interface is added or updated, take further actions to map
 * its members. Likewise, for VxLAN interface.
 */
static void netlink_interface_update_l2info(struct interface *ifp,
					    struct rtattr *link_data, int add)
{
	if (!link_data)
		return;

	if (IS_ZEBRA_IF_BRIDGE(ifp)) {
		struct zebra_l2info_bridge bridge_info;

		netlink_extract_bridge_info(link_data, &bridge_info);
		zebra_l2_bridge_add_update(ifp, &bridge_info, add);
	} else if (IS_ZEBRA_IF_VLAN(ifp)) {
		struct zebra_l2info_vlan vlan_info;

		netlink_extract_vlan_info(link_data, &vlan_info);
		zebra_l2_vlanif_update(ifp, &vlan_info);
	} else if (IS_ZEBRA_IF_VXLAN(ifp)) {
		struct zebra_l2info_vxlan vxlan_info;

		netlink_extract_vxlan_info(link_data, &vxlan_info);
		zebra_l2_vxlanif_add_update(ifp, &vxlan_info, add);
	}
}

static int netlink_bridge_interface(struct nlmsghdr *h, int len, ns_id_t ns_id,
				    int startup)
{
	char *name = NULL;
	struct ifinfomsg *ifi;
	struct rtattr *tb[IFLA_MAX + 1];
	struct interface *ifp;
	struct rtattr *aftb[IFLA_BRIDGE_MAX + 1];
	struct {
		u_int16_t flags;
		u_int16_t vid;
	} * vinfo;
	vlanid_t access_vlan;

	/* Fetch name and ifindex */
	ifi = NLMSG_DATA(h);
	memset(tb, 0, sizeof tb);
	netlink_parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

	if (tb[IFLA_IFNAME] == NULL)
		return -1;
	name = (char *)RTA_DATA(tb[IFLA_IFNAME]);

	/* The interface should already be known, if not discard. */
	ifp = if_lookup_by_index_per_ns(zebra_ns_lookup(ns_id), ifi->ifi_index);
	if (!ifp) {
		zlog_warn("Cannot find bridge IF %s(%u)", name, ifi->ifi_index);
		return 0;
	}
	if (!IS_ZEBRA_IF_VXLAN(ifp))
		return 0;

	/* We are only interested in the access VLAN i.e., AF_SPEC */
	if (!tb[IFLA_AF_SPEC])
		return 0;

	/* There is a 1-to-1 mapping of VLAN to VxLAN - hence
	 * only 1 access VLAN is accepted.
	 */
	memset(aftb, 0, sizeof aftb);
	parse_rtattr_nested(aftb, IFLA_BRIDGE_MAX, tb[IFLA_AF_SPEC]);
	if (!aftb[IFLA_BRIDGE_VLAN_INFO])
		return 0;

	vinfo = RTA_DATA(aftb[IFLA_BRIDGE_VLAN_INFO]);
	if (!(vinfo->flags & BRIDGE_VLAN_INFO_PVID))
		return 0;

	access_vlan = (vlanid_t)vinfo->vid;
	if (IS_ZEBRA_DEBUG_KERNEL)
		zlog_debug("Access VLAN %u for VxLAN IF %s(%u)", access_vlan,
			   name, ifi->ifi_index);
	zebra_l2_vxlanif_update_access_vlan(ifp, access_vlan);
	return 0;
}

/* Called from interface_lookup_netlink().  This function is only used
   during bootstrap. */
static int netlink_interface(struct sockaddr_nl *snl, struct nlmsghdr *h,
			     ns_id_t ns_id, int startup)
{
	int len;
	struct ifinfomsg *ifi;
	struct rtattr *tb[IFLA_MAX + 1];
	struct rtattr *linkinfo[IFLA_MAX + 1];
	struct interface *ifp;
	char *name = NULL;
	char *kind = NULL;
	char *desc = NULL;
	char *slave_kind = NULL;
	struct zebra_ns *zns;
	vrf_id_t vrf_id = VRF_DEFAULT;
	zebra_iftype_t zif_type = ZEBRA_IF_OTHER;
	zebra_slave_iftype_t zif_slave_type = ZEBRA_IF_SLAVE_NONE;
	ifindex_t bridge_ifindex = IFINDEX_INTERNAL;
	ifindex_t link_ifindex = IFINDEX_INTERNAL;

	zns = zebra_ns_lookup(ns_id);
	ifi = NLMSG_DATA(h);

	if (h->nlmsg_type != RTM_NEWLINK)
		return 0;

	len = h->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg));
	if (len < 0)
		return -1;

	/* We are interested in some AF_BRIDGE notifications. */
	if (ifi->ifi_family == AF_BRIDGE)
		return netlink_bridge_interface(h, len, ns_id, startup);

	/* Looking up interface name. */
	memset(tb, 0, sizeof tb);
	memset(linkinfo, 0, sizeof linkinfo);
	netlink_parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

#ifdef IFLA_WIRELESS
	/* check for wireless messages to ignore */
	if ((tb[IFLA_WIRELESS] != NULL) && (ifi->ifi_change == 0)) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug("%s: ignoring IFLA_WIRELESS message",
				   __func__);
		return 0;
	}
#endif /* IFLA_WIRELESS */

	if (tb[IFLA_IFNAME] == NULL)
		return -1;
	name = (char *)RTA_DATA(tb[IFLA_IFNAME]);

	if (tb[IFLA_IFALIAS])
		desc = (char *)RTA_DATA(tb[IFLA_IFALIAS]);

	if (tb[IFLA_LINKINFO]) {
		parse_rtattr_nested(linkinfo, IFLA_INFO_MAX, tb[IFLA_LINKINFO]);

		if (linkinfo[IFLA_INFO_KIND])
			kind = RTA_DATA(linkinfo[IFLA_INFO_KIND]);

#if HAVE_DECL_IFLA_INFO_SLAVE_KIND
		if (linkinfo[IFLA_INFO_SLAVE_KIND])
			slave_kind = RTA_DATA(linkinfo[IFLA_INFO_SLAVE_KIND]);
#endif

		netlink_determine_zebra_iftype(kind, &zif_type);
	}

	/* If VRF, create the VRF structure itself. */
	if (zif_type == ZEBRA_IF_VRF) {
		netlink_vrf_change(h, tb[IFLA_LINKINFO], name);
		vrf_id = (vrf_id_t)ifi->ifi_index;
	}

	if (tb[IFLA_MASTER]) {
		if (slave_kind && (strcmp(slave_kind, "vrf") == 0)) {
			zif_slave_type = ZEBRA_IF_SLAVE_VRF;
			vrf_id = *(u_int32_t *)RTA_DATA(tb[IFLA_MASTER]);
		} else if (slave_kind && (strcmp(slave_kind, "bridge") == 0)) {
			zif_slave_type = ZEBRA_IF_SLAVE_BRIDGE;
			bridge_ifindex =
				*(ifindex_t *)RTA_DATA(tb[IFLA_MASTER]);
		} else
			zif_slave_type = ZEBRA_IF_SLAVE_OTHER;
	}

	/* If linking to another interface, note it. */
	if (tb[IFLA_LINK])
		link_ifindex = *(ifindex_t *)RTA_DATA(tb[IFLA_LINK]);

	/* Add interface. */
	ifp = if_get_by_name(name, vrf_id, 0);
	set_ifindex(ifp, ifi->ifi_index, zns);
	ifp->flags = ifi->ifi_flags & 0x0000fffff;
	if (IS_ZEBRA_IF_VRF(ifp))
		SET_FLAG(ifp->status, ZEBRA_INTERFACE_VRF_LOOPBACK);
	ifp->mtu6 = ifp->mtu = *(uint32_t *)RTA_DATA(tb[IFLA_MTU]);
	ifp->metric = 0;
	ifp->speed = get_iflink_speed(name);
	ifp->ptm_status = ZEBRA_PTM_STATUS_UNKNOWN;

	if (desc)
		ifp->desc = XSTRDUP(MTYPE_TMP, desc);

	/* Set zebra interface type */
	zebra_if_set_ziftype(ifp, zif_type, zif_slave_type);

	/* Update link. */
	zebra_if_update_link(ifp, link_ifindex);

	/* Hardware type and address. */
	ifp->ll_type = netlink_to_zebra_link_type(ifi->ifi_type);
	netlink_interface_update_hw_addr(tb, ifp);

	if_add_update(ifp);

	/* Extract and save L2 interface information, take additional actions.
	 */
	netlink_interface_update_l2info(ifp, linkinfo[IFLA_INFO_DATA], 1);
	if (IS_ZEBRA_IF_BRIDGE_SLAVE(ifp))
		zebra_l2if_update_bridge_slave(ifp, bridge_ifindex);

	return 0;
}

/* Request for specific interface or address information from the kernel */
static int netlink_request_intf_addr(struct zebra_ns *zns, int family, int type,
				     u_int32_t filter_mask)
{
	struct {
		struct nlmsghdr n;
		struct ifinfomsg ifm;
		char buf[256];
	} req;

	/* Form the request, specifying filter (rtattr) if needed. */
	memset(&req, 0, sizeof(req));
	req.n.nlmsg_type = type;
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.ifm.ifi_family = family;

	/* Include filter, if specified. */
	if (filter_mask)
		addattr32(&req.n, sizeof(req), IFLA_EXT_MASK, filter_mask);

	return netlink_request(&zns->netlink_cmd, &req.n);
}

/* Interface lookup by netlink socket. */
int interface_lookup_netlink(struct zebra_ns *zns)
{
	int ret;

	/* Get interface information. */
	ret = netlink_request_intf_addr(zns, AF_PACKET, RTM_GETLINK, 0);
	if (ret < 0)
		return ret;
	ret = netlink_parse_info(netlink_interface, &zns->netlink_cmd, zns, 0,
				 1);
	if (ret < 0)
		return ret;

	/* Get interface information - for bridge interfaces. */
	ret = netlink_request_intf_addr(zns, AF_BRIDGE, RTM_GETLINK,
					RTEXT_FILTER_BRVLAN);
	if (ret < 0)
		return ret;
	ret = netlink_parse_info(netlink_interface, &zns->netlink_cmd, zns, 0,
				 0);
	if (ret < 0)
		return ret;

	/* Get interface information - for bridge interfaces. */
	ret = netlink_request_intf_addr(zns, AF_BRIDGE, RTM_GETLINK,
					RTEXT_FILTER_BRVLAN);
	if (ret < 0)
		return ret;
	ret = netlink_parse_info(netlink_interface, &zns->netlink_cmd, zns, 0,
				 0);
	if (ret < 0)
		return ret;

	/* Get IPv4 address of the interfaces. */
	ret = netlink_request_intf_addr(zns, AF_INET, RTM_GETADDR, 0);
	if (ret < 0)
		return ret;
	ret = netlink_parse_info(netlink_interface_addr, &zns->netlink_cmd, zns,
				 0, 1);
	if (ret < 0)
		return ret;

	/* Get IPv6 address of the interfaces. */
	ret = netlink_request_intf_addr(zns, AF_INET6, RTM_GETADDR, 0);
	if (ret < 0)
		return ret;
	ret = netlink_parse_info(netlink_interface_addr, &zns->netlink_cmd, zns,
				 0, 1);
	if (ret < 0)
		return ret;

	return 0;
}

int kernel_interface_set_master(struct interface *master,
				struct interface *slave)
{
	struct zebra_ns *zns = zebra_ns_lookup(NS_DEFAULT);

	struct {
		struct nlmsghdr n;
		struct ifinfomsg ifa;
		char buf[NL_PKT_BUF_SIZE];
	} req;

	memset(&req, 0, sizeof req);

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_SETLINK;
	req.n.nlmsg_pid = zns->netlink_cmd.snl.nl_pid;

	req.ifa.ifi_index = slave->ifindex;

	addattr_l(&req.n, sizeof req, IFLA_MASTER, &master->ifindex, 4);
	addattr_l(&req.n, sizeof req, IFLA_LINK, &slave->ifindex, 4);

	return netlink_talk(netlink_talk_filter, &req.n, &zns->netlink_cmd, zns,
			    0);
}

/* Interface address modification. */
static int netlink_address(int cmd, int family, struct interface *ifp,
			   struct connected *ifc)
{
	int bytelen;
	struct prefix *p;

	struct {
		struct nlmsghdr n;
		struct ifaddrmsg ifa;
		char buf[NL_PKT_BUF_SIZE];
	} req;

	struct zebra_ns *zns = zebra_ns_lookup(NS_DEFAULT);

	p = ifc->address;
	memset(&req, 0, sizeof req - NL_PKT_BUF_SIZE);

	bytelen = (family == AF_INET ? 4 : 16);

	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = cmd;
	req.n.nlmsg_pid = zns->netlink_cmd.snl.nl_pid;

	req.ifa.ifa_family = family;

	req.ifa.ifa_index = ifp->ifindex;

	addattr_l(&req.n, sizeof req, IFA_LOCAL, &p->u.prefix, bytelen);

	if (family == AF_INET) {
		if (CONNECTED_PEER(ifc)) {
			p = ifc->destination;
			addattr_l(&req.n, sizeof req, IFA_ADDRESS, &p->u.prefix,
				  bytelen);
		} else if (cmd == RTM_NEWADDR && ifc->destination) {
			p = ifc->destination;
			addattr_l(&req.n, sizeof req, IFA_BROADCAST,
				  &p->u.prefix, bytelen);
		}
	}

	/* p is now either ifc->address or ifc->destination */
	req.ifa.ifa_prefixlen = p->prefixlen;

	if (CHECK_FLAG(ifc->flags, ZEBRA_IFA_SECONDARY))
		SET_FLAG(req.ifa.ifa_flags, IFA_F_SECONDARY);

	if (ifc->label)
		addattr_l(&req.n, sizeof req, IFA_LABEL, ifc->label,
			  strlen(ifc->label) + 1);

	return netlink_talk(netlink_talk_filter, &req.n, &zns->netlink_cmd, zns,
			    0);
}

int kernel_address_add_ipv4(struct interface *ifp, struct connected *ifc)
{
	return netlink_address(RTM_NEWADDR, AF_INET, ifp, ifc);
}

int kernel_address_delete_ipv4(struct interface *ifp, struct connected *ifc)
{
	return netlink_address(RTM_DELADDR, AF_INET, ifp, ifc);
}

int netlink_interface_addr(struct sockaddr_nl *snl, struct nlmsghdr *h,
			   ns_id_t ns_id, int startup)
{
	int len;
	struct ifaddrmsg *ifa;
	struct rtattr *tb[IFA_MAX + 1];
	struct interface *ifp;
	void *addr;
	void *broad;
	u_char flags = 0;
	char *label = NULL;
	struct zebra_ns *zns;

	zns = zebra_ns_lookup(ns_id);
	ifa = NLMSG_DATA(h);

	if (ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6)
		return 0;

	if (h->nlmsg_type != RTM_NEWADDR && h->nlmsg_type != RTM_DELADDR)
		return 0;

	len = h->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	if (len < 0)
		return -1;

	memset(tb, 0, sizeof tb);
	netlink_parse_rtattr(tb, IFA_MAX, IFA_RTA(ifa), len);

	ifp = if_lookup_by_index_per_ns(zns, ifa->ifa_index);
	if (ifp == NULL) {
		zlog_err(
			"netlink_interface_addr can't find interface by index %d",
			ifa->ifa_index);
		return -1;
	}

	if (IS_ZEBRA_DEBUG_KERNEL) /* remove this line to see initial ifcfg */
	{
		char buf[BUFSIZ];
		zlog_debug("netlink_interface_addr %s %s flags 0x%x:",
			   nl_msg_type_to_str(h->nlmsg_type), ifp->name,
			   ifa->ifa_flags);
		if (tb[IFA_LOCAL])
			zlog_debug("  IFA_LOCAL     %s/%d",
				   inet_ntop(ifa->ifa_family,
					     RTA_DATA(tb[IFA_LOCAL]), buf,
					     BUFSIZ),
				   ifa->ifa_prefixlen);
		if (tb[IFA_ADDRESS])
			zlog_debug("  IFA_ADDRESS   %s/%d",
				   inet_ntop(ifa->ifa_family,
					     RTA_DATA(tb[IFA_ADDRESS]), buf,
					     BUFSIZ),
				   ifa->ifa_prefixlen);
		if (tb[IFA_BROADCAST])
			zlog_debug("  IFA_BROADCAST %s/%d",
				   inet_ntop(ifa->ifa_family,
					     RTA_DATA(tb[IFA_BROADCAST]), buf,
					     BUFSIZ),
				   ifa->ifa_prefixlen);
		if (tb[IFA_LABEL] && strcmp(ifp->name, RTA_DATA(tb[IFA_LABEL])))
			zlog_debug("  IFA_LABEL     %s",
				   (char *)RTA_DATA(tb[IFA_LABEL]));

		if (tb[IFA_CACHEINFO]) {
			struct ifa_cacheinfo *ci = RTA_DATA(tb[IFA_CACHEINFO]);
			zlog_debug("  IFA_CACHEINFO pref %d, valid %d",
				   ci->ifa_prefered, ci->ifa_valid);
		}
	}

	/* logic copied from iproute2/ip/ipaddress.c:print_addrinfo() */
	if (tb[IFA_LOCAL] == NULL)
		tb[IFA_LOCAL] = tb[IFA_ADDRESS];
	if (tb[IFA_ADDRESS] == NULL)
		tb[IFA_ADDRESS] = tb[IFA_LOCAL];

	/* local interface address */
	addr = (tb[IFA_LOCAL] ? RTA_DATA(tb[IFA_LOCAL]) : NULL);

	/* is there a peer address? */
	if (tb[IFA_ADDRESS]
	    && memcmp(RTA_DATA(tb[IFA_ADDRESS]), RTA_DATA(tb[IFA_LOCAL]),
		      RTA_PAYLOAD(tb[IFA_ADDRESS]))) {
		broad = RTA_DATA(tb[IFA_ADDRESS]);
		SET_FLAG(flags, ZEBRA_IFA_PEER);
	} else
		/* seeking a broadcast address */
		broad = (tb[IFA_BROADCAST] ? RTA_DATA(tb[IFA_BROADCAST])
					   : NULL);

	/* addr is primary key, SOL if we don't have one */
	if (addr == NULL) {
		zlog_debug("%s: NULL address", __func__);
		return -1;
	}

	/* Flags. */
	if (ifa->ifa_flags & IFA_F_SECONDARY)
		SET_FLAG(flags, ZEBRA_IFA_SECONDARY);

	/* Label */
	if (tb[IFA_LABEL])
		label = (char *)RTA_DATA(tb[IFA_LABEL]);

	if (ifp && label && strcmp(ifp->name, label) == 0)
		label = NULL;

	/* Register interface address to the interface. */
	if (ifa->ifa_family == AF_INET) {
		if (h->nlmsg_type == RTM_NEWADDR)
			connected_add_ipv4(ifp, flags, (struct in_addr *)addr,
					   ifa->ifa_prefixlen,
					   (struct in_addr *)broad, label);
		else
			connected_delete_ipv4(
				ifp, flags, (struct in_addr *)addr,
				ifa->ifa_prefixlen, (struct in_addr *)broad);
	}
	if (ifa->ifa_family == AF_INET6) {
		if (h->nlmsg_type == RTM_NEWADDR) {
			/* Only consider valid addresses; we'll not get a
			 * notification from
			 * the kernel till IPv6 DAD has completed, but at init
			 * time, Quagga
			 * does query for and will receive all addresses.
			 */
			if (!(ifa->ifa_flags
			      & (IFA_F_DADFAILED | IFA_F_TENTATIVE)))
				connected_add_ipv6(ifp, flags,
						   (struct in6_addr *)addr,
						   ifa->ifa_prefixlen, label);
		} else
			connected_delete_ipv6(ifp, (struct in6_addr *)addr,
					      ifa->ifa_prefixlen);
	}

	return 0;
}

int netlink_link_change(struct sockaddr_nl *snl, struct nlmsghdr *h,
			ns_id_t ns_id, int startup)
{
	int len;
	struct ifinfomsg *ifi;
	struct rtattr *tb[IFLA_MAX + 1];
	struct rtattr *linkinfo[IFLA_MAX + 1];
	struct interface *ifp;
	char *name = NULL;
	char *kind = NULL;
	char *desc = NULL;
	char *slave_kind = NULL;
	struct zebra_ns *zns;
	vrf_id_t vrf_id = VRF_DEFAULT;
	zebra_iftype_t zif_type = ZEBRA_IF_OTHER;
	zebra_slave_iftype_t zif_slave_type = ZEBRA_IF_SLAVE_NONE;
	ifindex_t bridge_ifindex = IFINDEX_INTERNAL;
	ifindex_t link_ifindex = IFINDEX_INTERNAL;


	zns = zebra_ns_lookup(ns_id);
	ifi = NLMSG_DATA(h);

	if (!(h->nlmsg_type == RTM_NEWLINK || h->nlmsg_type == RTM_DELLINK)) {
		/* If this is not link add/delete message so print warning. */
		zlog_warn("netlink_link_change: wrong kernel message %d",
			  h->nlmsg_type);
		return 0;
	}

	len = h->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg));
	if (len < 0)
		return -1;

	/* We are interested in some AF_BRIDGE notifications. */
	if (ifi->ifi_family == AF_BRIDGE)
		return netlink_bridge_interface(h, len, ns_id, startup);

	/* Looking up interface name. */
	memset(tb, 0, sizeof tb);
	memset(linkinfo, 0, sizeof linkinfo);
	netlink_parse_rtattr(tb, IFLA_MAX, IFLA_RTA(ifi), len);

#ifdef IFLA_WIRELESS
	/* check for wireless messages to ignore */
	if ((tb[IFLA_WIRELESS] != NULL) && (ifi->ifi_change == 0)) {
		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug("%s: ignoring IFLA_WIRELESS message",
				   __func__);
		return 0;
	}
#endif /* IFLA_WIRELESS */

	if (tb[IFLA_IFNAME] == NULL)
		return -1;
	name = (char *)RTA_DATA(tb[IFLA_IFNAME]);

	if (tb[IFLA_LINKINFO]) {
		parse_rtattr_nested(linkinfo, IFLA_INFO_MAX, tb[IFLA_LINKINFO]);

		if (linkinfo[IFLA_INFO_KIND])
			kind = RTA_DATA(linkinfo[IFLA_INFO_KIND]);

#if HAVE_DECL_IFLA_INFO_SLAVE_KIND
		if (linkinfo[IFLA_INFO_SLAVE_KIND])
			slave_kind = RTA_DATA(linkinfo[IFLA_INFO_SLAVE_KIND]);
#endif

		netlink_determine_zebra_iftype(kind, &zif_type);
	}

	/* If linking to another interface, note it. */
	if (tb[IFLA_LINK])
		link_ifindex = *(ifindex_t *)RTA_DATA(tb[IFLA_LINK]);

	if (tb[IFLA_IFALIAS]) {
		desc = (char *)RTA_DATA(tb[IFLA_IFALIAS]);
	}

	/* If VRF, create or update the VRF structure itself. */
	if (zif_type == ZEBRA_IF_VRF) {
		netlink_vrf_change(h, tb[IFLA_LINKINFO], name);
		vrf_id = (vrf_id_t)ifi->ifi_index;
	}

	/* See if interface is present. */
	ifp = if_lookup_by_name_per_ns(zns, name);

	if (ifp) {
		if (ifp->desc)
			XFREE(MTYPE_TMP, ifp->desc);
		if (desc)
			ifp->desc = XSTRDUP(MTYPE_TMP, desc);
	}

	if (h->nlmsg_type == RTM_NEWLINK) {
		if (tb[IFLA_MASTER]) {
			if (slave_kind && (strcmp(slave_kind, "vrf") == 0)) {
				zif_slave_type = ZEBRA_IF_SLAVE_VRF;
				vrf_id =
					*(u_int32_t *)RTA_DATA(tb[IFLA_MASTER]);
			} else if (slave_kind
				   && (strcmp(slave_kind, "bridge") == 0)) {
				zif_slave_type = ZEBRA_IF_SLAVE_BRIDGE;
				bridge_ifindex =
					*(ifindex_t *)RTA_DATA(tb[IFLA_MASTER]);
			} else
				zif_slave_type = ZEBRA_IF_SLAVE_OTHER;
		}

		if (ifp == NULL
		    || !CHECK_FLAG(ifp->status, ZEBRA_INTERFACE_ACTIVE)) {
			/* Add interface notification from kernel */
			if (IS_ZEBRA_DEBUG_KERNEL)
				zlog_debug(
					"RTM_NEWLINK ADD for %s(%u) vrf_id %u type %d "
					"sl_type %d master %u flags 0x%x",
					name, ifi->ifi_index, vrf_id, zif_type,
					zif_slave_type, bridge_ifindex,
					ifi->ifi_flags);

			if (ifp == NULL) {
				/* unknown interface */
				ifp = if_get_by_name(name, vrf_id, 0);
			} else {
				/* pre-configured interface, learnt now */
				if (ifp->vrf_id != vrf_id)
					if_update_to_new_vrf(ifp, vrf_id);
			}

			/* Update interface information. */
			set_ifindex(ifp, ifi->ifi_index, zns);
			ifp->flags = ifi->ifi_flags & 0x0000fffff;
			if (IS_ZEBRA_IF_VRF(ifp))
				SET_FLAG(ifp->status,
					 ZEBRA_INTERFACE_VRF_LOOPBACK);
			ifp->mtu6 = ifp->mtu = *(int *)RTA_DATA(tb[IFLA_MTU]);
			ifp->metric = 0;
			ifp->ptm_status = ZEBRA_PTM_STATUS_UNKNOWN;

			/* Set interface type */
			zebra_if_set_ziftype(ifp, zif_type, zif_slave_type);

			/* Update link. */
			zebra_if_update_link(ifp, link_ifindex);

			netlink_interface_update_hw_addr(tb, ifp);

			/* Inform clients, install any configured addresses. */
			if_add_update(ifp);

			/* Extract and save L2 interface information, take
			 * additional actions. */
			netlink_interface_update_l2info(
				ifp, linkinfo[IFLA_INFO_DATA], 1);
			if (IS_ZEBRA_IF_BRIDGE_SLAVE(ifp))
				zebra_l2if_update_bridge_slave(ifp,
							       bridge_ifindex);
		} else if (ifp->vrf_id != vrf_id) {
			/* VRF change for an interface. */
			if (IS_ZEBRA_DEBUG_KERNEL)
				zlog_debug(
					"RTM_NEWLINK vrf-change for %s(%u) "
					"vrf_id %u -> %u flags 0x%x",
					name, ifp->ifindex, ifp->vrf_id, vrf_id,
					ifi->ifi_flags);

			if_handle_vrf_change(ifp, vrf_id);
		} else {
			int was_bridge_slave;

			/* Interface update. */
			if (IS_ZEBRA_DEBUG_KERNEL)
				zlog_debug(
					"RTM_NEWLINK update for %s(%u) "
					"sl_type %d master %u flags 0x%x",
					name, ifp->ifindex, zif_slave_type,
					bridge_ifindex, ifi->ifi_flags);

			set_ifindex(ifp, ifi->ifi_index, zns);
			ifp->mtu6 = ifp->mtu = *(int *)RTA_DATA(tb[IFLA_MTU]);
			ifp->metric = 0;

			/* Update interface type - NOTE: Only slave_type can
			 * change. */
			was_bridge_slave = IS_ZEBRA_IF_BRIDGE_SLAVE(ifp);
			zebra_if_set_ziftype(ifp, zif_type, zif_slave_type);

			netlink_interface_update_hw_addr(tb, ifp);

			if (if_is_no_ptm_operative(ifp)) {
				ifp->flags = ifi->ifi_flags & 0x0000fffff;
				if (!if_is_no_ptm_operative(ifp)) {
					if (IS_ZEBRA_DEBUG_KERNEL)
						zlog_debug(
							"Intf %s(%u) has gone DOWN",
							name, ifp->ifindex);
					if_down(ifp);
				} else if (if_is_operative(ifp)) {
					/* Must notify client daemons of new
					 * interface status. */
					if (IS_ZEBRA_DEBUG_KERNEL)
						zlog_debug(
							"Intf %s(%u) PTM up, notifying clients",
							name, ifp->ifindex);
					zebra_interface_up_update(ifp);
				}
			} else {
				ifp->flags = ifi->ifi_flags & 0x0000fffff;
				if (if_is_operative(ifp)) {
					if (IS_ZEBRA_DEBUG_KERNEL)
						zlog_debug(
							"Intf %s(%u) has come UP",
							name, ifp->ifindex);
					if_up(ifp);
				}
			}

			/* Extract and save L2 interface information, take
			 * additional actions. */
			netlink_interface_update_l2info(
				ifp, linkinfo[IFLA_INFO_DATA], 0);
			if (IS_ZEBRA_IF_BRIDGE_SLAVE(ifp) || was_bridge_slave)
				zebra_l2if_update_bridge_slave(ifp,
							       bridge_ifindex);
		}
	} else {
		/* Delete interface notification from kernel */
		if (ifp == NULL) {
			zlog_warn("RTM_DELLINK for unknown interface %s(%u)",
				  name, ifi->ifi_index);
			return 0;
		}

		if (IS_ZEBRA_DEBUG_KERNEL)
			zlog_debug("RTM_DELLINK for %s(%u)", name,
				   ifp->ifindex);

		UNSET_FLAG(ifp->status, ZEBRA_INTERFACE_VRF_LOOPBACK);

		/* Special handling for bridge or VxLAN interfaces. */
		if (IS_ZEBRA_IF_BRIDGE(ifp))
			zebra_l2_bridge_del(ifp);
		else if (IS_ZEBRA_IF_VXLAN(ifp))
			zebra_l2_vxlanif_del(ifp);

		if (!IS_ZEBRA_IF_VRF(ifp))
			if_delete_update(ifp);
	}

	return 0;
}

/* Interface information read by netlink. */
void interface_list(struct zebra_ns *zns)
{
	interface_lookup_netlink(zns);
}

#endif /* GNU_LINUX */
