/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-linux-platform.c - Linux kernel & udev network configuration layer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2012-2013 Red Hat, Inc.
 */
#include <config.h>

#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <linux/if_arp.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <netlink/netlink.h>
#include <netlink/object.h>
#include <netlink/cache.h>
#include <netlink/route/link.h>
#include <netlink/route/link/vlan.h>
#include <netlink/route/addr.h>
#include <netlink/route/route.h>

#include "nm-linux-platform.h"
#include "nm-logging.h"

/* This is only included for the translation of VLAN flags */
#include "nm-setting-vlan.h"

#define debug(...) nm_log_dbg (LOGD_PLATFORM, __VA_ARGS__)
#define warning(...) nm_log_warn (LOGD_PLATFORM, __VA_ARGS__)
#define error(...) nm_log_err (LOGD_PLATFORM, __VA_ARGS__)

typedef struct {
	struct nl_sock *nlh;
	struct nl_sock *nlh_event;
	struct nl_cache *link_cache;
	struct nl_cache *address_cache;
	struct nl_cache *route_cache;
	GIOChannel *event_channel;
	guint event_id;
} NMLinuxPlatformPrivate;

#define NM_LINUX_PLATFORM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_LINUX_PLATFORM, NMLinuxPlatformPrivate))

G_DEFINE_TYPE (NMLinuxPlatform, nm_linux_platform, NM_TYPE_PLATFORM)

void
nm_linux_platform_setup (void)
{
	nm_platform_setup (NM_TYPE_LINUX_PLATFORM);
}

/******************************************************************/

/* libnl library workarounds and additions */

/* Automatic deallocation of local variables */
#define auto_nl_object __attribute__((cleanup(put_nl_object)))
static void
put_nl_object (void *ptr)
{
	struct nl_object **object = ptr;

	if (object && *object) {
		nl_object_put (*object);
		*object = NULL;
	}
}

#define auto_nl_addr __attribute__((cleanup(put_nl_addr)))
static void
put_nl_addr (void *ptr)
{
	struct nl_addr **object = ptr;

	if (object && *object) {
		nl_addr_put (*object);
		*object = NULL;
	}
}

/* libnl doesn't use const where due */
#define nl_addr_build(family, addr, addrlen) nl_addr_build (family, (gpointer) addr, addrlen)

/* rtnl_addr_set_prefixlen fails to update the nl_addr prefixlen */
static void
nm_rtnl_addr_set_prefixlen (struct rtnl_addr *rtnladdr, int plen)
{
	struct nl_addr *nladdr;

	rtnl_addr_set_prefixlen (rtnladdr, plen);

	nladdr = rtnl_addr_get_local (rtnladdr);
	if (nladdr)
		nl_addr_set_prefixlen (nladdr, plen);
}
#define rtnl_addr_set_prefixlen nm_rtnl_addr_set_prefixlen

typedef enum {
	LINK,
	IP4_ADDRESS,
	IP6_ADDRESS,
	IP4_ROUTE,
	IP6_ROUTE,
	N_TYPES
} ObjectType;

typedef enum {
	ADDED,
	CHANGED,
	REMOVED,
	N_STATUSES
} ObjectStatus;

static ObjectType
object_type_from_nl_object (const struct nl_object *object)
{
	g_assert (object);

	if (!strcmp (nl_object_get_type (object), "route/link"))
		return LINK;
	else if (!strcmp (nl_object_get_type (object), "route/addr")) {
		switch (rtnl_addr_get_family ((struct rtnl_addr *) object)) {
		case AF_INET:
			return IP4_ADDRESS;
		case AF_INET6:
			return IP6_ADDRESS;
		default:
			g_assert_not_reached ();
		}
	} else if (!strcmp (nl_object_get_type (object), "route/route")) {
		switch (rtnl_route_get_family ((struct rtnl_route *) object)) {
		case AF_INET:
			return IP4_ROUTE;
		case AF_INET6:
			return IP6_ROUTE;
		default:
			g_assert_not_reached ();
		}
	} else
		g_assert_not_reached ();
}

/* libnl inclues LINK_ATTR_FAMILY in oo_id_attrs of link_obj_ops and thus
 * refuses to search for items that lack this attribute. I believe this is a
 * bug or a bad design at the least. Address family is not an identifying
 * attribute of a network interface and IMO is not an attribute of a network
 * interface at all.
 */
static struct nl_object *
nm_nl_cache_search (struct nl_cache *cache, struct nl_object *needle)
{
	if (object_type_from_nl_object (needle) == LINK)
		rtnl_link_set_family ((struct rtnl_link *) needle, AF_UNSPEC);

	return nl_cache_search (cache, needle);
}
#define nl_cache_search nm_nl_cache_search

/* Ask the kernel for an object identical (as in nl_cache_identical) to the
 * needle argument. This is a kernel counterpart for nl_cache_search.
 *
 * libnl 3.2 doesn't seem to provide such functionality.
 */
static struct nl_object *
get_kernel_object (struct nl_sock *sock, struct nl_object *needle)
{

	switch (object_type_from_nl_object (needle)) {
	case LINK:
		{
			struct nl_object *kernel_object;
			int ifindex = rtnl_link_get_ifindex ((struct rtnl_link *) needle);
			const char *name = rtnl_link_get_name ((struct rtnl_link *) needle);
			int nle;

			nle = rtnl_link_get_kernel (sock, ifindex, name, (struct rtnl_link **) &kernel_object);
			switch (nle) {
			case -NLE_SUCCESS:
				return kernel_object;
			case -NLE_NODEV:
				return NULL;
			default:
				error ("Netlink error: %s", nl_geterror (nle));
				return NULL;
			}
		}
	default:
		/* Fallback to a one-time cache allocation. */
		{
			struct nl_cache *cache;
			struct nl_object *object;
			int nle;

			nle = nl_cache_alloc_and_fill (
					nl_cache_ops_lookup (nl_object_get_type (needle)),
					sock, &cache);
			g_return_val_if_fail (!nle, NULL);
			object = nl_cache_search (cache, needle);

			nl_cache_free (cache);
			return object;
		}
	}
}

/* libnl 3.2 doesn't seem to provide such a generic way to add libnl-route objects. */
static gboolean
add_kernel_object (struct nl_sock *sock, struct nl_object *object)
{
	switch (object_type_from_nl_object (object)) {
	case LINK:
		return rtnl_link_add (sock, (struct rtnl_link *) object, NLM_F_CREATE);
	case IP4_ADDRESS:
	case IP6_ADDRESS:
		return rtnl_addr_add (sock, (struct rtnl_addr *) object, NLM_F_CREATE);
	case IP4_ROUTE:
	case IP6_ROUTE:
		return rtnl_route_add (sock, (struct rtnl_route *) object, NLM_F_CREATE);
	default:
		g_assert_not_reached ();
	}
}

/* libnl 3.2 doesn't seem to provide such a generic way to delete libnl-route objects. */
static int
delete_kernel_object (struct nl_sock *sock, struct nl_object *object)
{
	switch (object_type_from_nl_object (object)) {
	case LINK:
		return rtnl_link_delete (sock, (struct rtnl_link *) object);
	case IP4_ADDRESS:
	case IP6_ADDRESS:
		return rtnl_addr_delete (sock, (struct rtnl_addr *) object, 0);
	case IP4_ROUTE:
	case IP6_ROUTE:
		return rtnl_route_delete (sock, (struct rtnl_route *) object, 0);
	default:
		g_assert_not_reached ();
	}
}

/******************************************************************/

/* Object type specific utilities */

static const char *
type_to_string (NMLinkType type)
{
	switch (type) {
	case NM_LINK_TYPE_DUMMY:
		return "dummy";
	case NM_LINK_TYPE_BRIDGE:
		return "bridge";
	case NM_LINK_TYPE_BOND:
		return "bond";
	case NM_LINK_TYPE_TEAM:
		return "team";
	default:
		g_warning ("Wrong type: %d", type);
		return NULL;
	}
}

static NMLinkType
link_extract_type (struct rtnl_link *rtnllink)
{
	const char *type;

	if (!rtnllink)
		return NM_LINK_TYPE_NONE;

	type = rtnl_link_get_type (rtnllink);

	if (!type)
		switch (rtnl_link_get_arptype (rtnllink)) {
		case ARPHRD_LOOPBACK:
			return NM_LINK_TYPE_LOOPBACK;
		case ARPHRD_ETHER:
			return NM_LINK_TYPE_ETHERNET;
		default:
			return NM_LINK_TYPE_GENERIC;
		}
	else if (!g_strcmp0 (type, "dummy"))
		return NM_LINK_TYPE_DUMMY;
	else if (!g_strcmp0 (type, "bridge"))
		return NM_LINK_TYPE_BRIDGE;
	else if (!g_strcmp0 (type, "bond"))
		return NM_LINK_TYPE_BOND;
	else if (!g_strcmp0 (type, "team"))
		return NM_LINK_TYPE_TEAM;
	else
		return NM_LINK_TYPE_UNKNOWN;
}

static void
link_init (NMPlatformLink *info, struct rtnl_link *rtnllink)
{
	memset (info, 0, sizeof (*info));

	g_assert (rtnllink);

	info->ifindex = rtnl_link_get_ifindex (rtnllink);
	strcpy (info->name, rtnl_link_get_name (rtnllink));
	info->type = link_extract_type (rtnllink);
	info->up = !!(rtnl_link_get_flags (rtnllink) & IFF_UP);
	info->connected = !!(rtnl_link_get_flags (rtnllink) & IFF_LOWER_UP);
	info->arp = !(rtnl_link_get_flags (rtnllink) & IFF_NOARP);
	info->master = rtnl_link_get_master (rtnllink);
}

/* Hack: Empty bridges and bonds have IFF_LOWER_UP flag and therefore they break
 * the carrier detection. This hack makes nm-platform think they don't have the
 * IFF_LOWER_UP flag. This seems to also apply to bonds (specifically) with all
 * slaves down.
 *
 * Note: This is still a bit racy but when NetworkManager asks for enslaving a slave,
 * nm-platform will do that synchronously and will immediately ask for both master
 * and slave information after the enslaving request. After the synchronous call, the
 * master carrier is already updated with the slave carrier in mind.
 *
 * https://bugzilla.redhat.com/show_bug.cgi?id=910348
 */
static void
hack_empty_master_iff_lower_up (NMPlatform *platform, struct nl_object *object)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	struct rtnl_link *rtnllink;
	int ifindex;
	struct nl_object *slave;

	if (!object)
		return;
	if (strcmp (nl_object_get_type (object), "route/link"))
		return;

	rtnllink = (struct rtnl_link *) object;

	ifindex = rtnl_link_get_ifindex (rtnllink);

	switch (link_extract_type (rtnllink)) {
	case NM_LINK_TYPE_BRIDGE:
	case NM_LINK_TYPE_BOND:
		for (slave = nl_cache_get_first (priv->link_cache); slave; slave = nl_cache_get_next (slave)) {
			struct rtnl_link *rtnlslave = (struct rtnl_link *) slave;
			if (rtnl_link_get_master (rtnlslave) == ifindex
					&& rtnl_link_get_flags (rtnlslave) & IFF_LOWER_UP)
				return;
		}
		break;
	default:
		return;
	}

	rtnl_link_unset_flags (rtnllink, IFF_LOWER_UP);
}

static void
init_ip4_address (NMPlatformIP4Address *address, struct rtnl_addr *rtnladdr)
{
	struct nl_addr *nladdr = rtnl_addr_get_local (rtnladdr);

	g_assert (nladdr);

	memset (address, 0, sizeof (*address));

	address->ifindex = rtnl_addr_get_ifindex (rtnladdr);
	address->plen = rtnl_addr_get_prefixlen (rtnladdr);
	g_assert (nl_addr_get_len (nladdr) == sizeof (address->address));
	memcpy (&address->address, nl_addr_get_binary_addr (nladdr), sizeof (address->address));
}

static void
init_ip6_address (NMPlatformIP6Address *address, struct rtnl_addr *rtnladdr)
{
	struct nl_addr *nladdr = rtnl_addr_get_local (rtnladdr);

	memset (address, 0, sizeof (*address));

	address->ifindex = rtnl_addr_get_ifindex (rtnladdr);
	address->plen = rtnl_addr_get_prefixlen (rtnladdr);
	g_assert (nl_addr_get_len (nladdr) == sizeof (address->address));
	memcpy (&address->address, nl_addr_get_binary_addr (nladdr), sizeof (address->address));
}

static void
init_ip4_route (NMPlatformIP4Route *route, struct rtnl_route *rtnlroute)
{
	struct nl_addr *dst, *gw;
	struct rtnl_nexthop *nexthop;

	g_assert (rtnl_route_get_nnexthops (rtnlroute) == 1);
	nexthop = rtnl_route_nexthop_n (rtnlroute, 0);
	dst = rtnl_route_get_dst (rtnlroute);
	gw = rtnl_route_nh_get_gateway (nexthop);

	memset (route, 0, sizeof (*route));
	route->ifindex = rtnl_route_nh_get_ifindex (nexthop);
	route->plen = nl_addr_get_prefixlen (dst);
	memcpy (&route->network, nl_addr_get_binary_addr (dst), sizeof (route->network));
	if (gw)
		memcpy (&route->gateway, nl_addr_get_binary_addr (gw), sizeof (route->gateway));
	route->metric = rtnl_route_get_priority (rtnlroute);
}

static void
init_ip6_route (NMPlatformIP6Route *route, struct rtnl_route *rtnlroute)
{
	struct nl_addr *dst, *gw;
	struct rtnl_nexthop *nexthop;

	g_assert (rtnl_route_get_nnexthops (rtnlroute) == 1);
	nexthop = rtnl_route_nexthop_n (rtnlroute, 0);
	dst = rtnl_route_get_dst (rtnlroute);
	gw = rtnl_route_nh_get_gateway (nexthop);

	memset (route, 0, sizeof (*route));
	route->ifindex = rtnl_route_nh_get_ifindex (nexthop);
	route->plen = nl_addr_get_prefixlen (dst);
	memcpy (&route->network, nl_addr_get_binary_addr (dst), sizeof (route->network));
	if (gw)
		memcpy (&route->gateway, nl_addr_get_binary_addr (gw), sizeof (route->gateway));
	route->metric = rtnl_route_get_priority (rtnlroute);
}

/******************************************************************/

/* Object and cache manipulation */

static const char *signal_by_type_and_status[N_TYPES][N_STATUSES] = {
	{ NM_PLATFORM_LINK_ADDED, NM_PLATFORM_LINK_CHANGED, NM_PLATFORM_LINK_REMOVED },
	{ NM_PLATFORM_IP4_ADDRESS_ADDED, NM_PLATFORM_IP4_ADDRESS_CHANGED, NM_PLATFORM_IP4_ADDRESS_REMOVED },
	{ NM_PLATFORM_IP6_ADDRESS_ADDED, NM_PLATFORM_IP6_ADDRESS_CHANGED, NM_PLATFORM_IP6_ADDRESS_REMOVED },
	{ NM_PLATFORM_IP4_ROUTE_ADDED, NM_PLATFORM_IP4_ROUTE_CHANGED, NM_PLATFORM_IP4_ROUTE_REMOVED },
	{ NM_PLATFORM_IP6_ROUTE_ADDED, NM_PLATFORM_IP6_ROUTE_CHANGED, NM_PLATFORM_IP6_ROUTE_REMOVED }
};

static struct nl_cache *
choose_cache (NMPlatform *platform, struct nl_object *object)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);

	switch (object_type_from_nl_object (object)) {
	case LINK:
		return priv->link_cache;
	case IP4_ADDRESS:
	case IP6_ADDRESS:
		return priv->address_cache;
	case IP4_ROUTE:
	case IP6_ROUTE:
		return priv->route_cache;
	default:
		g_assert_not_reached ();
	}
}

static void
announce_object (NMPlatform *platform, const struct nl_object *object, ObjectStatus status)
{
	ObjectType object_type = object_type_from_nl_object (object);
	const char *sig = signal_by_type_and_status[object_type][status];

	switch (object_type) {
	case LINK:
		{
			NMPlatformLink device;

			link_init (&device, (struct rtnl_link *) object);
			g_signal_emit_by_name (platform, sig, &device);
		}
		return;
	case IP4_ADDRESS:
		{
			NMPlatformIP4Address address;

			init_ip4_address (&address, (struct rtnl_addr *) object);
			g_signal_emit_by_name (platform, sig, &address);
		}
		return;
	case IP6_ADDRESS:
		{
			NMPlatformIP6Address address;

			init_ip6_address (&address, (struct rtnl_addr *) object);
			g_signal_emit_by_name (platform, sig, &address);
		}
		return;
	case IP4_ROUTE:
		{
			NMPlatformIP4Route route;

			init_ip4_route (&route, (struct rtnl_route *) object);
			g_signal_emit_by_name (platform, sig, &route);
		}
		return;
	case IP6_ROUTE:
		{
			NMPlatformIP6Route route;

			init_ip6_route (&route, (struct rtnl_route *) object);
			g_signal_emit_by_name (platform, sig, &route);
		}
		return;
	default:
		error ("Announcing object: object type unknown: %d", object_type);
	}
}

static gboolean
process_nl_error (NMPlatform *platform, int nle)
{
	/* NLE_EXIST is considered equivalent to success to avoid race conditions. You
	 * never know when something sends an identical object just before
	 * NetworkManager, e.g. from a dispatcher script.
	 */
	switch (nle) {
	case -NLE_SUCCESS:
	case -NLE_EXIST:
		return FALSE;
	default:
		error ("Netlink error: %s", nl_geterror (nle));
		return TRUE;
	}
}

static struct nl_object * build_rtnl_link (int ifindex, const char *name, NMLinkType type);

static gboolean
refresh_object (NMPlatform *platform, struct nl_object *object, int nle)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	auto_nl_object struct nl_object *cached_object = NULL;
	auto_nl_object struct nl_object *kernel_object = NULL;
	struct nl_cache *cache;

	if (process_nl_error (platform, nle))
		return FALSE;

	cache = choose_cache (platform, object);
	cached_object = nl_cache_search (choose_cache (platform, object), object);
	kernel_object = get_kernel_object (priv->nlh, object);

	g_return_val_if_fail (kernel_object, FALSE);

	hack_empty_master_iff_lower_up (platform, kernel_object);

	if (cached_object) {
		nl_cache_remove (cached_object);
		nle = nl_cache_add (cache, kernel_object);
		g_return_val_if_fail (!nle, 0);
	} else {
		nle = nl_cache_add (cache, kernel_object);
		g_return_val_if_fail (!nle, FALSE);
	}

	announce_object (platform, kernel_object, cached_object ? CHANGED : ADDED);

	/* Refresh the master device (even on enslave/release) */
	if (object_type_from_nl_object (kernel_object) == LINK) {
		int kernel_master = rtnl_link_get_master ((struct rtnl_link *) kernel_object);
		int cached_master = cached_object ? rtnl_link_get_master ((struct rtnl_link *) cached_object) : 0;
		struct nl_object *master_object;

		if (kernel_master) {
			master_object = build_rtnl_link (kernel_master, NULL, NM_LINK_TYPE_NONE);
			refresh_object (platform, master_object, 0);
			nl_object_put (master_object);
		}
		if (cached_master && cached_master != kernel_master) {
			master_object = build_rtnl_link (cached_master, NULL, NM_LINK_TYPE_NONE);
			refresh_object (platform, master_object, 0);
			nl_object_put (master_object);
		}
	}

	return TRUE;
}

/* Decreases the reference count if @obj for convenience */
static gboolean
add_object (NMPlatform *platform, struct nl_object *obj)
{
	auto_nl_object struct nl_object *object = obj;
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);

	return refresh_object (platform, object, add_kernel_object (priv->nlh, object));
}

/* Decreases the reference count if @obj for convenience */
static gboolean
delete_object (NMPlatform *platform, struct nl_object *obj)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	auto_nl_object struct nl_object *object = obj;
	auto_nl_object struct nl_object *cached_object;

	cached_object = nl_cache_search (choose_cache (platform, object), object);
	g_assert (cached_object);

	if (process_nl_error (platform, delete_kernel_object (priv->nlh, cached_object)))
		return FALSE;

	nl_cache_remove (cached_object);
	announce_object (platform, cached_object, REMOVED);

	return TRUE;
}

static void
ref_object (struct nl_object *obj, void *data)
{
	struct nl_object **out = data;

	nl_object_get (obj);
	*out = obj;
}

/* This function does all the magic to avoid race conditions caused
 * by concurrent usage of synchronous commands and an asynchronous cache. This
 * might be a nice future addition to libnl but it requires to do all operations
 * through the cache manager. In this case, nm-linux-platform serves as the
 * cache manager instead of the one provided by libnl.
 */
static int
event_notification (struct nl_msg *msg, gpointer user_data)
{
	NMPlatform *platform = NM_PLATFORM (user_data);
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	struct nl_cache *cache;
	auto_nl_object struct nl_object *object = NULL;
	auto_nl_object struct nl_object *cached_object = NULL;
	auto_nl_object struct nl_object *kernel_object = NULL;
	int event;
	int nle;

	event = nlmsg_hdr (msg)->nlmsg_type;
	nl_msg_parse (msg, ref_object, &object);
	g_return_val_if_fail (object, NL_OK);

	cache = choose_cache (platform, object);
	cached_object = nl_cache_search (cache, object);
	kernel_object = get_kernel_object (priv->nlh, object);

	debug ("netlink event (type %d)", event);

	hack_empty_master_iff_lower_up (platform, kernel_object);

	/* Removed object */
	switch (event) {
	case RTM_DELLINK:
	case RTM_DELADDR:
		/* Ignore inconsistent deletion
		 *
		 * Quick external deletion and addition can be occasionally
		 * seen as just a change.
		 */
		if (kernel_object)
			return NL_OK;
		/* Ignore internal deletion */
		if (!cached_object)
			return NL_OK;

		nl_cache_remove (cached_object);
		announce_object (platform, cached_object, REMOVED);

		return NL_OK;
	case RTM_NEWLINK:
	case RTM_NEWADDR:
		/* Ignore inconsistent addition or change (kernel will send a good one)
		 *
		 * Quick sequence of RTM_NEWLINK notifications can be occasionally
		 * collapsed to just one addition or deletion, depending of whether we
		 * already have the object in cache.
		 */
		if (!kernel_object)
			return NL_OK;
		/* Handle external addition */
		if (!cached_object) {
			nle = nl_cache_add (cache, kernel_object);
			if (nle) {
				error ("netlink cache error: %s", nl_geterror (nle));
				return NL_OK;
			}
			announce_object (platform, kernel_object, ADDED);
			return NL_OK;
		}
		/* Ignore non-change
		 *
		 * This also catches notifications for internal addition or change, unless
		 * another action occured very soon after it.
		 */
		if (!nl_object_diff (kernel_object, cached_object))
			return NL_OK;
		/* Handle external change */
		nl_cache_remove (cached_object);
		nle = nl_cache_add (cache, kernel_object);
		if (nle) {
			error ("netlink cache error: %s", nl_geterror (nle));
			return NL_OK;
		}
		announce_object (platform, kernel_object, CHANGED);

		return NL_OK;
	default:
		error ("Unknown netlink event: %d", event);
		return NL_OK;
	}
}

/******************************************************************/

static gboolean
sysctl_set (NMPlatform *platform, const char *path, const char *value)
{
	int fd, len, nwrote, tries;
	char *actual;

	g_return_val_if_fail (path != NULL, FALSE);
	g_return_val_if_fail (value != NULL, FALSE);

	fd = open (path, O_WRONLY | O_TRUNC);
	if (fd == -1) {
		error ("sysctl: failed to open '%s': (%d) %s",
				path, errno, strerror (errno));
		return FALSE;
	}

	debug ("sysctl: setting '%s' to '%s'", path, value);

	/* Most sysfs and sysctl options don't care about a trailing LF, while some
	 * (like infiniband) do.  So always add the LF.  Also, neither sysfs nor
	 * sysctl support partial writes so the LF must be added to the string we're
	 * about to write.
	 */
	actual = g_strdup_printf ("%s\n", value);

	/* Try to write the entire value three times if a partial write occurs */
	len = strlen (actual);
	for (tries = 0, nwrote = 0; tries < 3 && nwrote != len; tries++) {
		errno = 0;
		nwrote = write (fd, actual, len);
		if (nwrote == -1) {
			if (errno == EINTR) {
				error ("sysctl: interrupted, will try again");
				continue;
			}
			break;
		}
	}
	if (nwrote != len && errno != EEXIST) {
		error ("sysctl: failed to set '%s' to '%s': (%d) %s",
		             path, value, errno, strerror (errno));
	}

	g_free (actual);
	close (fd);
	return (nwrote == len);
}

static char *
sysctl_get (NMPlatform *platform, const char *path)
{
	GError *error = NULL;
	char *contents;

	if (!g_file_get_contents (path, &contents, NULL, &error)) {
		error ("error reading %s: %s", path, error->message);
		return NULL;
	}

	return g_strstrip (contents);
}

/******************************************************************/

static GArray *
link_get_all (NMPlatform *platform)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	GArray *links = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformLink), nl_cache_nitems (priv->link_cache));
	NMPlatformLink device;
	struct nl_object *object;

	for (object = nl_cache_get_first (priv->link_cache); object; object = nl_cache_get_next (object)) {
		link_init (&device, (struct rtnl_link *) object);
		g_array_append_val (links, device);
	}

	return links;
}

static struct nl_object *
build_rtnl_link (int ifindex, const char *name, NMLinkType type)
{
	struct rtnl_link *rtnllink;
	int nle;

	rtnllink = rtnl_link_alloc ();
	g_assert (rtnllink);
	if (ifindex)
		rtnl_link_set_ifindex (rtnllink, ifindex);
	if (name)
		rtnl_link_set_name (rtnllink, name);
	if (type) {
		nle = rtnl_link_set_type (rtnllink, type_to_string (type));
		g_assert (!nle);
	}

	return (struct nl_object *) rtnllink;
}

static gboolean
link_add (NMPlatform *platform, const char *name, NMLinkType type)
{
	if (type == NM_LINK_TYPE_BOND) {
		/* When the kernel loads the bond module, either via explicit modprobe
		 * or automatically in response to creating a bond master, it will also
		 * create a 'bond0' interface.  Since the bond we're about to create may
		 * or may not be named 'bond0' prevent potential confusion about a bond
		 * that the user didn't want by telling the bonding module not to create
		 * bond0 automatically.
		 */
		if (!g_file_test ("/sys/class/net/bonding_masters", G_FILE_TEST_EXISTS))
			system ("modprobe bonding max_bonds=0");
	}

	return add_object (platform, build_rtnl_link (0, name, type));
}

static gboolean
link_change (NMPlatform *platform, int ifindex, struct rtnl_link *change)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	auto_nl_object struct rtnl_link *orig;

	orig = rtnl_link_get (priv->link_cache, ifindex);

	if (!orig) {
		debug ("link not found: %d", ifindex);
		platform->error = NM_PLATFORM_ERROR_NOT_FOUND;
		return FALSE;
	}

	return refresh_object (platform, (struct nl_object *) orig,
			rtnl_link_change (priv->nlh, orig, change, 0));
}

static gboolean
link_delete (NMPlatform *platform, int ifindex)
{
	return delete_object (platform, build_rtnl_link (ifindex, NULL, NM_LINK_TYPE_NONE));
}

static int
link_get_ifindex (NMPlatform *platform, const char *ifname)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);

	return rtnl_link_name2i (priv->link_cache, ifname);
}

static struct rtnl_link *
link_get (NMPlatform *platform, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	struct rtnl_link *rtnllink = rtnl_link_get (priv->link_cache, ifindex);

	if (!rtnllink)
		platform->error = NM_PLATFORM_ERROR_NOT_FOUND;

	return rtnllink;
}

static const char *
link_get_name (NMPlatform *platform, int ifindex)
{
	auto_nl_object struct rtnl_link *rtnllink = link_get (platform, ifindex);

	return rtnllink ? rtnl_link_get_name (rtnllink) : NULL;
}

static NMLinkType
link_get_type (NMPlatform *platform, int ifindex)
{
	auto_nl_object struct rtnl_link *rtnllink = link_get (platform, ifindex);

	return link_extract_type (rtnllink);
}

static guint32
link_get_flags (NMPlatform *platform, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	auto_nl_object struct rtnl_link *rtnllink;

	rtnllink = rtnl_link_get (priv->link_cache, ifindex);

	if (!rtnllink) {
		debug ("link not found: %d", ifindex);
		platform->error = NM_PLATFORM_ERROR_NOT_FOUND;
		return IFF_NOARP;
	}

	return rtnl_link_get_flags (rtnllink);
}

static gboolean
link_is_up (NMPlatform *platform, int ifindex)
{
	return !!(link_get_flags (platform, ifindex) & IFF_UP);
}

static gboolean
link_is_connected (NMPlatform *platform, int ifindex)
{
	return !!(link_get_flags (platform, ifindex) & IFF_LOWER_UP);
}

static gboolean
link_uses_arp (NMPlatform *platform, int ifindex)
{
	return !(link_get_flags (platform, ifindex) & IFF_NOARP);
}

static gboolean
link_change_flags (NMPlatform *platform, int ifindex, unsigned int flags, gboolean value)
{
	auto_nl_object struct rtnl_link *change;

	change = rtnl_link_alloc ();
	g_return_val_if_fail (change != NULL, FALSE);

	if (value)
		rtnl_link_set_flags (change, flags);
	else
		rtnl_link_unset_flags (change, flags);

	return link_change (platform, ifindex, change);
}

static gboolean
link_set_up (NMPlatform *platform, int ifindex)
{
	return link_change_flags (platform, ifindex, IFF_UP, TRUE);
}

static gboolean
link_set_down (NMPlatform *platform, int ifindex)
{
	return link_change_flags (platform, ifindex, IFF_UP, FALSE);
}

static gboolean
link_set_arp (NMPlatform *platform, int ifindex)
{
	return link_change_flags (platform, ifindex, IFF_NOARP, FALSE);
}

static gboolean
link_set_noarp (NMPlatform *platform, int ifindex)
{
	return link_change_flags (platform, ifindex, IFF_NOARP, TRUE);
}

static gboolean
ethtool_get (const char *name, gpointer edata)
{
	struct ifreq ifr;
	int fd;

	memset (&ifr, 0, sizeof (ifr));
	strncpy (ifr.ifr_name, name, IFNAMSIZ);
	ifr.ifr_data = edata;

	fd = socket (PF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		error ("ethtool: Could not open socket.");
		return FALSE;
	}

	if (ioctl (fd, SIOCETHTOOL, &ifr) < 0) {
		debug ("ethtool: Request failed: %s", strerror (errno));
		close (fd);
		return FALSE;
	}

	close (fd);
	return TRUE;
}

static gboolean
link_supports_carrier_detect (NMPlatform *platform, int ifindex)
{
	const char *name = nm_platform_link_get_name (ifindex);
	struct ethtool_cmd edata = { .cmd = ETHTOOL_GLINK };

	/* We ignore the result. If the ETHTOOL_GLINK call succeeded, then we
	 * assume the device supports carrier-detect, otherwise we assume it
	 * doesn't.
	 *
	 * We don't use ETHTOOL_GLINK for carrier detect itself, so this can
	 * be regarded as a hack. Instead, kernel should be able to report
	 * carrier detection capability via netlink.
	 */
	return name && ethtool_get (name, &edata);
}

#define NETIF_F_VLAN_CHALLENGED (1 << 10)

static gboolean
link_supports_vlans (NMPlatform *platform, int ifindex)
{
	auto_nl_object struct rtnl_link *rtnllink = link_get (platform, ifindex);
	const char *name = nm_platform_link_get_name (ifindex);
	struct {
		struct ethtool_gfeatures features;
		struct ethtool_get_features_block features_block;
	} edata = { .features = { .cmd = ETHTOOL_GFEATURES, .size = 1 } };

	/* Only ARPHRD_ETHER links can possibly support VLANs. */
	if (!rtnllink || rtnl_link_get_arptype (rtnllink) != ARPHRD_ETHER)
		return FALSE;

	if (!name || !ethtool_get (name, &edata))
		return FALSE;

	return !(edata.features.features[0].active & NETIF_F_VLAN_CHALLENGED);
}

static gboolean
link_refresh (NMPlatform *platform, int ifindex, int nle)
{
	auto_nl_object struct nl_object *object = build_rtnl_link (ifindex, NULL, NM_LINK_TYPE_NONE);

	return refresh_object (platform, object, nle);
}

static gboolean
link_enslave (NMPlatform *platform, int master, int slave)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);

	return link_refresh (platform, slave, rtnl_link_enslave_ifindex (priv->nlh, master, slave));
}

static gboolean
link_release (NMPlatform *platform, int master, int slave)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);

	return link_refresh (platform, slave, rtnl_link_release_ifindex (priv->nlh, slave));
}

static int
link_get_master (NMPlatform *platform, int slave)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	auto_nl_object struct rtnl_link *rtnllink;
	int result;

	rtnllink = rtnl_link_get (priv->link_cache, slave);
	g_assert (rtnllink);

	result = rtnl_link_get_master (rtnllink);
	g_assert (result >= 0);

	return result;
}

static char *
link_option_path (int master, const char *category, const char *option)
{
	const char *name = nm_platform_link_get_name (master);
   
	if (!name || !category || !option)
		return NULL;

	return g_strdup_printf ("/sys/class/net/%s/%s/%s", name, category, option);
}

static gboolean
link_set_option (int master, const char *category, const char *option, const char *value)
{
	auto_g_free char *path = link_option_path (master, category, option);

	return path && nm_platform_sysctl_set (path, value);
}

static char *
link_get_option (int master, const char *category, const char *option)
{
	auto_g_free char *path = link_option_path (master, category, option);

	return path ? nm_platform_sysctl_get (path) : NULL;
}

static const char *
master_category (NMPlatform *platform, int master)
{
	switch (link_get_type (platform, master)) {
	case NM_LINK_TYPE_BRIDGE:
		return "bridge";
	case NM_LINK_TYPE_BOND:
		return "bonding";
	default:
		g_assert_not_reached ();
	}
}

static const char *
slave_category (NMPlatform *platform, int slave)
{
	int master = link_get_master (platform, slave);

	if (master) {
		platform->error = NM_PLATFORM_ERROR_NOT_SLAVE;
		return NULL;
	}

	switch (link_get_type (platform, master)) {
	case NM_LINK_TYPE_BRIDGE:
		return "brport";
	default:
		g_assert_not_reached ();
	}
}

static gboolean
master_set_option (NMPlatform *platform, int master, const char *option, const char *value)
{
	return link_set_option (master, master_category (platform, master), option, value);
}

static char *
master_get_option (NMPlatform *platform, int master, const char *option)
{
	return link_get_option (master, master_category (platform, master), option);
}

static gboolean
slave_set_option (NMPlatform *platform, int slave, const char *option, const char *value)
{
	return link_set_option (slave, slave_category (platform, slave), option, value);
}

static char *
slave_get_option (NMPlatform *platform, int slave, const char *option)
{
	return link_get_option (slave, slave_category (platform, slave), option);
}

/******************************************************************/

static int
ip_address_mark_all (NMPlatform *platform, int family, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	struct nl_object *object;
	int count = 0;

	for (object = nl_cache_get_first (priv->address_cache); object; object = nl_cache_get_next (object)) {
		nl_object_unmark (object);
		if (rtnl_addr_get_family ((struct rtnl_addr *) object) != family)
			continue;
		if (rtnl_addr_get_ifindex ((struct rtnl_addr *) object) != ifindex)
			continue;
		nl_object_mark (object);
		count++;
	}

	return count;
}

static GArray *
ip4_address_get_all (NMPlatform *platform, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	GArray *addresses;
	NMPlatformIP4Address address;
	struct nl_object *object;
	int count;

	count = ip_address_mark_all (platform, AF_INET, ifindex);
	addresses = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformIP4Address), count);

	for (object = nl_cache_get_first (priv->address_cache); object; object = nl_cache_get_next (object)) {
		if (nl_object_is_marked (object)) {
			init_ip4_address (&address, (struct rtnl_addr *) object);
			g_array_append_val (addresses, address);
			nl_object_unmark (object);
		}
	}

	return addresses;
}

static GArray *
ip6_address_get_all (NMPlatform *platform, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	GArray *addresses;
	NMPlatformIP6Address address;
	struct nl_object *object;
	int count;

	count = ip_address_mark_all (platform, AF_INET6, ifindex);
	addresses = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformIP6Address), count);

	for (object = nl_cache_get_first (priv->address_cache); object; object = nl_cache_get_next (object)) {
		if (nl_object_is_marked (object)) {
			init_ip6_address (&address, (struct rtnl_addr *) object);
			g_array_append_val (addresses, address);
			nl_object_unmark (object);
		}
	}

	return addresses;
}

static struct nl_object *
build_rtnl_addr (int family, int ifindex, gconstpointer addr, int plen)
{
	struct rtnl_addr *rtnladdr = rtnl_addr_alloc ();
	int addrlen = family == AF_INET ? sizeof (in_addr_t) : sizeof (struct in6_addr);
	auto_nl_addr struct nl_addr *nladdr = nl_addr_build (family, addr, addrlen);
	int nle;

	g_assert (rtnladdr && nladdr);

	rtnl_addr_set_ifindex (rtnladdr, ifindex);
	nle = rtnl_addr_set_local (rtnladdr, nladdr);
	g_assert (!nle);
	rtnl_addr_set_prefixlen (rtnladdr, plen);

	return (struct nl_object *) rtnladdr;
}

static gboolean
ip4_address_add (NMPlatform *platform, int ifindex, in_addr_t addr, int plen)
{
	return add_object (platform, build_rtnl_addr (AF_INET, ifindex, &addr, plen));
}

static gboolean
ip6_address_add (NMPlatform *platform, int ifindex, struct in6_addr addr, int plen)
{
	return add_object (platform, build_rtnl_addr (AF_INET6, ifindex, &addr, plen));
}

static gboolean
ip4_address_delete (NMPlatform *platform, int ifindex, in_addr_t addr, int plen)
{
	return delete_object (platform, build_rtnl_addr (AF_INET, ifindex, &addr, plen));
}

static gboolean
ip6_address_delete (NMPlatform *platform, int ifindex, struct in6_addr addr, int plen)
{
	return delete_object (platform, build_rtnl_addr (AF_INET6, ifindex, &addr, plen));
}

static gboolean
ip_address_exists (NMPlatform *platform, int family, int ifindex, gconstpointer addr, int plen)
{
	auto_nl_object struct nl_object *object = build_rtnl_addr (family, ifindex, addr, plen);
	auto_nl_object struct nl_object *cached_object = nl_cache_search (choose_cache (platform, object), object);

	return !!cached_object;
}

static gboolean
ip4_address_exists (NMPlatform *platform, int ifindex, in_addr_t addr, int plen)
{
	return ip_address_exists (platform, AF_INET, ifindex, &addr, plen);
}

static gboolean
ip6_address_exists (NMPlatform *platform, int ifindex, struct in6_addr addr, int plen)
{
	return ip_address_exists (platform, AF_INET6, ifindex, &addr, plen);
}

/******************************************************************/

static int
ip_route_mark_all (NMPlatform *platform, int family, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	struct nl_object *object;
	int count = 0;

	for (object = nl_cache_get_first (priv->route_cache); object; object = nl_cache_get_next (object)) {
		struct rtnl_route *rtnlroute = (struct rtnl_route *) object;
		struct rtnl_nexthop *nexthop;

		nl_object_unmark (object);
		if (rtnl_route_get_type (rtnlroute) != RTN_UNICAST)
			continue;
		if (rtnl_route_get_table (rtnlroute) != RT_TABLE_MAIN)
			continue;
		if (rtnl_route_get_family (rtnlroute) != family)
			continue;
		if (rtnl_route_get_nnexthops (rtnlroute) != 1)
			continue;
		nexthop = rtnl_route_nexthop_n (rtnlroute, 0);
		if (rtnl_route_nh_get_ifindex (nexthop) != ifindex)
			continue;
		nl_object_mark (object);
		count++;
	}

	return count;
}

static GArray *
ip4_route_get_all (NMPlatform *platform, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	GArray *routes;
	NMPlatformIP4Route route;
	struct nl_object *object;
	int count = 0;

	count = ip_route_mark_all (platform, AF_INET, ifindex);
	routes = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformIP4Route), count);

	for (object = nl_cache_get_first (priv->route_cache); object; object = nl_cache_get_next (object)) {
		if (nl_object_is_marked (object)) {
			init_ip4_route (&route, (struct rtnl_route *) object);
			g_array_append_val (routes, route);
			nl_object_unmark (object);
		}
	}

	return routes;
}

static GArray *
ip6_route_get_all (NMPlatform *platform, int ifindex)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	GArray *routes;
	NMPlatformIP6Route route;
	struct nl_object *object;
	int count;

	count = ip_route_mark_all (platform, AF_INET6, ifindex);
	routes = g_array_sized_new (TRUE, TRUE, sizeof (NMPlatformIP6Route), count);

	for (object = nl_cache_get_first (priv->route_cache); object; object = nl_cache_get_next (object)) {
		if (nl_object_is_marked (object)) {
			init_ip6_route (&route, (struct rtnl_route *) object);
			g_array_append_val (routes, route);
			nl_object_unmark (object);
		}
	}

	return routes;
}

static struct nl_object *
build_rtnl_route (int family, int ifindex, gconstpointer network, int plen, gconstpointer gateway, int metric, int mss)
{
	struct rtnl_route *rtnlroute = rtnl_route_alloc ();
	struct rtnl_nexthop *nexthop = rtnl_route_nh_alloc ();
	int addrlen = (family == AF_INET) ? sizeof (in_addr_t) : sizeof (struct in6_addr);
	auto_nl_addr struct nl_addr *dst = nl_addr_build (family, network, addrlen);
	auto_nl_addr struct nl_addr *gw = nl_addr_build (family, gateway, addrlen);

	g_assert (rtnlroute && dst && gw && nexthop);

	nl_addr_set_prefixlen (dst, plen);

	rtnl_route_set_table (rtnlroute, RT_TABLE_MAIN);
	rtnl_route_set_tos (rtnlroute, 0);
	rtnl_route_set_dst (rtnlroute, dst);
	rtnl_route_set_priority (rtnlroute, metric);

	rtnl_route_nh_set_ifindex (nexthop, ifindex);
	if (!nl_addr_iszero (gw))
		rtnl_route_nh_set_gateway (nexthop, gw);
	rtnl_route_add_nexthop (rtnlroute, nexthop);

	if (mss > 0)
		rtnl_route_set_metric (rtnlroute, RTAX_ADVMSS, mss);

	return (struct nl_object *) rtnlroute;
}

static gboolean
ip4_route_add (NMPlatform *platform, int ifindex, in_addr_t network, int plen, in_addr_t gateway, int metric, int mss)
{
	return add_object (platform, build_rtnl_route (AF_INET, ifindex, &network, plen, &gateway, metric, mss));
}

static gboolean
ip6_route_add (NMPlatform *platform, int ifindex, struct in6_addr network, int plen, struct in6_addr gateway, int metric, int mss)
{
	return add_object (platform, build_rtnl_route (AF_INET6, ifindex, &network, plen, &gateway, metric, mss));
}

static gboolean
ip4_route_delete (NMPlatform *platform, int ifindex, in_addr_t network, int plen,
		in_addr_t gateway, int metric)
{
	return delete_object (platform, build_rtnl_route (AF_INET, ifindex, &network, plen, &gateway, metric, 0));
}

static gboolean
ip6_route_delete (NMPlatform *platform, int ifindex, struct in6_addr network, int plen,
		struct in6_addr gateway, int metric)
{
	return delete_object (platform, build_rtnl_route (AF_INET6, ifindex, &network, plen, &gateway, metric, 0));
}

static gboolean
ip_route_exists (NMPlatform *platform, int family, int ifindex, gpointer network, int plen,
		gpointer gateway, int metric)
{
	auto_nl_object struct nl_object *object = build_rtnl_route (
			family, ifindex, network, plen, gateway, metric, 0);
	auto_nl_object struct nl_object *cached_object = nl_cache_search (
			choose_cache (platform, object), object);

	return !!cached_object;
}

static gboolean
ip4_route_exists (NMPlatform *platform, int ifindex, in_addr_t network, int plen,
		in_addr_t gateway, int metric)
{
	return ip_route_exists (platform, AF_INET, ifindex, &network, plen, &gateway, metric);
}

static gboolean
ip6_route_exists (NMPlatform *platform, int ifindex, struct in6_addr network, int plen,
		struct in6_addr gateway, int metric)
{
	return ip_route_exists (platform, AF_INET6, ifindex, &network, plen, &gateway, metric);
}

/******************************************************************/

#define EVENT_CONDITIONS      ((GIOCondition) (G_IO_IN | G_IO_PRI))
#define ERROR_CONDITIONS      ((GIOCondition) (G_IO_ERR | G_IO_NVAL))
#define DISCONNECT_CONDITIONS ((GIOCondition) (G_IO_HUP))

static int
verify_source (struct nl_msg *msg, gpointer user_data)
{
	struct ucred *creds = nlmsg_get_creds (msg);

	if (!creds || creds->pid || creds->uid || creds->gid) {
		if (creds)
			warning ("netlink: received non-kernel message (pid %d uid %d gid %d)",
					creds->pid, creds->uid, creds->gid);
		else
			warning ("netlink: received message without credentials");
		return NL_STOP;
	}

	return NL_OK;
}

static gboolean
event_handler (GIOChannel *channel,
		GIOCondition io_condition,
		gpointer user_data)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (user_data);
	int nle;

	nle = nl_recvmsgs_default (priv->nlh_event);
	if (nle)
		error ("Failed to retrieve incoming events: %s", nl_geterror (nle));
	return TRUE;
}

static struct nl_sock *
setup_socket (gboolean event, gpointer user_data)
{
	struct nl_sock *sock;
	int nle;

	sock = nl_socket_alloc ();
	g_return_val_if_fail (sock, NULL);

	/* Only ever accept messages from kernel */
	nle = nl_socket_modify_cb (sock, NL_CB_MSG_IN, NL_CB_CUSTOM, verify_source, user_data);
	g_assert (!nle);

	/* Dispatch event messages (event socket only) */
	if (event) {
		nl_socket_modify_cb (sock, NL_CB_VALID, NL_CB_CUSTOM, event_notification, user_data);
		nl_socket_disable_seq_check (sock);
	}

	nle = nl_connect (sock, NETLINK_ROUTE);
	g_assert (!nle);
	nle = nl_socket_set_passcred (sock, 1);
	g_assert (!nle);

	return sock;
}

/******************************************************************/

static void
nm_linux_platform_init (NMLinuxPlatform *platform)
{
}

static gboolean
setup (NMPlatform *platform)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (platform);
	int channel_flags;
	gboolean status;
	int nle;

	/* Initialize netlink socket for requests */
	priv->nlh = setup_socket (FALSE, platform);
	g_assert (priv->nlh);
	debug ("Netlink socket for requests established: %d", nl_socket_get_local_port (priv->nlh));

	/* Initialize netlink socket for events */
	priv->nlh_event = setup_socket (TRUE, platform);
	g_assert (priv->nlh_event);
	/* The default buffer size wasn't enough for the testsuites. It might just
	 * as well happen with NetworkManager itself. For now let's hope 128KB is
	 * good enough.
	 */
	nle = nl_socket_set_buffer_size (priv->nlh_event, 131072, 0);
	g_assert (!nle);
	nle = nl_socket_add_memberships (priv->nlh_event,
			RTNLGRP_LINK,
			RTNLGRP_IPV4_IFADDR, RTNLGRP_IPV6_IFADDR,
			NULL);
	g_assert (!nle);
	debug ("Netlink socket for events established: %d", nl_socket_get_local_port (priv->nlh_event));

	priv->event_channel = g_io_channel_unix_new (nl_socket_get_fd (priv->nlh_event));
	g_io_channel_set_encoding (priv->event_channel, NULL, NULL);
	g_io_channel_set_close_on_unref (priv->event_channel, TRUE);

	channel_flags = g_io_channel_get_flags (priv->event_channel);
	status = g_io_channel_set_flags (priv->event_channel,
		channel_flags | G_IO_FLAG_NONBLOCK, NULL);
	g_assert (status);
	priv->event_id = g_io_add_watch (priv->event_channel,
		(EVENT_CONDITIONS | ERROR_CONDITIONS | DISCONNECT_CONDITIONS),
		event_handler, platform);

	/* Allocate netlink caches */
	rtnl_link_alloc_cache (priv->nlh, AF_UNSPEC, &priv->link_cache);
	rtnl_addr_alloc_cache (priv->nlh, &priv->address_cache);
	rtnl_route_alloc_cache (priv->nlh, AF_UNSPEC, 0, &priv->route_cache);
	g_assert (priv->link_cache && priv->address_cache && priv->route_cache);

	return TRUE;
}

static void
nm_linux_platform_finalize (GObject *object)
{
	NMLinuxPlatformPrivate *priv = NM_LINUX_PLATFORM_GET_PRIVATE (object);

	/* Free netlink resources */
	g_source_remove (priv->event_id);
	g_io_channel_unref (priv->event_channel);
	nl_socket_free (priv->nlh);
	nl_socket_free (priv->nlh_event);
	nl_cache_free (priv->link_cache);
	nl_cache_free (priv->address_cache);
	nl_cache_free (priv->route_cache);

	G_OBJECT_CLASS (nm_linux_platform_parent_class)->finalize (object);
}

#define OVERRIDE(function) platform_class->function = function

static void
nm_linux_platform_class_init (NMLinuxPlatformClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	NMPlatformClass *platform_class = NM_PLATFORM_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMLinuxPlatformPrivate));

	/* virtual methods */
	object_class->finalize = nm_linux_platform_finalize;

	platform_class->setup = setup;

	platform_class->sysctl_set = sysctl_set;
	platform_class->sysctl_get = sysctl_get;

	platform_class->link_get_all = link_get_all;
	platform_class->link_add = link_add;
	platform_class->link_delete = link_delete;
	platform_class->link_get_ifindex = link_get_ifindex;
	platform_class->link_get_name = link_get_name;
	platform_class->link_get_type = link_get_type;

	platform_class->link_set_up = link_set_up;
	platform_class->link_set_down = link_set_down;
	platform_class->link_set_arp = link_set_arp;
	platform_class->link_set_noarp = link_set_noarp;
	platform_class->link_is_up = link_is_up;
	platform_class->link_is_connected = link_is_connected;
	platform_class->link_uses_arp = link_uses_arp;

	platform_class->link_supports_carrier_detect = link_supports_carrier_detect;
	platform_class->link_supports_vlans = link_supports_vlans;

	platform_class->link_enslave = link_enslave;
	platform_class->link_release = link_release;
	platform_class->link_get_master = link_get_master;
	platform_class->master_set_option = master_set_option;
	platform_class->master_get_option = master_get_option;
	platform_class->slave_set_option = slave_set_option;
	platform_class->slave_get_option = slave_get_option;

	platform_class->ip4_address_get_all = ip4_address_get_all;
	platform_class->ip6_address_get_all = ip6_address_get_all;
	platform_class->ip4_address_add = ip4_address_add;
	platform_class->ip6_address_add = ip6_address_add;
	platform_class->ip4_address_delete = ip4_address_delete;
	platform_class->ip6_address_delete = ip6_address_delete;
	platform_class->ip4_address_exists = ip4_address_exists;
	platform_class->ip6_address_exists = ip6_address_exists;

	platform_class->ip4_route_get_all = ip4_route_get_all;
	platform_class->ip6_route_get_all = ip6_route_get_all;
	platform_class->ip4_route_add = ip4_route_add;
	platform_class->ip6_route_add = ip6_route_add;
	platform_class->ip4_route_delete = ip4_route_delete;
	platform_class->ip6_route_delete = ip6_route_delete;
	platform_class->ip4_route_exists = ip4_route_exists;
	platform_class->ip6_route_exists = ip6_route_exists;
}
