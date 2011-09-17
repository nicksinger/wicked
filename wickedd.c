/*
 * No REST for the wicked!
 *
 * This command line utility provides a daemon interface to the network
 * configuration/information facilities.
 *
 * It uses a RESTful interface (even though it's a command line utility).
 * The idea is to make it easier to extend this to some smallish daemon
 * with a AF_LOCAL socket interface.
 *
 * Copyright (C) 2010, 2011 Olaf Kirch <okir@suse.de>
 */

#include <sys/poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/addrconf.h>
#include <wicked/logging.h>
#include <wicked/wicked.h>
#include <wicked/xml.h>
#include <wicked/socket.h>
#include <wicked/dhcp.h>
#include <wicked/ipv4ll.h>

enum {
	OPT_CONFIGFILE,
	OPT_DEBUG,
	OPT_FOREGROUND,
	OPT_NOFORK,
	OPT_NORECOVER,
	OPT_DBUS,
};

static struct option	options[] = {
	{ "config",		required_argument,	NULL,	OPT_CONFIGFILE },
	{ "debug",		required_argument,	NULL,	OPT_DEBUG },
	{ "foreground",		no_argument,		NULL,	OPT_FOREGROUND },
	{ "no-fork",		no_argument,		NULL,	OPT_NOFORK },
	{ "no-recovery",	no_argument,		NULL,	OPT_NORECOVER },
	{ "dbus",		no_argument,		NULL,	OPT_DBUS },

	{ NULL }
};

static int		opt_foreground = 0;
static int		opt_nofork = 0;
static int		opt_dbus = 0;
static int		opt_recover_leases = 1;
static ni_dbus_server_t *wicked_dbus_server;

static void		wicked_discover_state(void);
static void		wicked_try_restart_addrconf(ni_interface_t *, ni_afinfo_t *, unsigned int, xml_node_t **);
static int		wicked_accept_connection(ni_socket_t *, uid_t, gid_t);
static void		wicked_interface_event(ni_handle_t *, ni_interface_t *, ni_event_t);
static int		wicked_process_network_restcall(ni_socket_t *);
static void		wicked_register_dbus_services(ni_dbus_server_t *);
static void		wicked_dbus_register_interface(ni_interface_t *);

int
main(int argc, char **argv)
{
	ni_socket_t *sock;
	int c;

	while ((c = getopt_long(argc, argv, "+", options, NULL)) != EOF) {
		switch (c) {
		default:
		usage:
			fprintf(stderr,
				"./wickedd [options]\n"
				"This command understands the following options\n"
				"  --config filename\n"
				"        Read configuration file <filename> instead of system default.\n"
				"  --debug facility\n"
				"        Enable debugging for debug <facility>.\n"
			       );
			return 1;

		case OPT_CONFIGFILE:
			ni_set_global_config_path(optarg);
			break;

		case OPT_DEBUG:
			if (!strcmp(optarg, "help")) {
				printf("Supported debug facilities:\n");
				ni_debug_help(stdout);
				return 0;
			}
			if (ni_enable_debug(optarg) < 0) {
				fprintf(stderr, "Bad debug facility \"%s\"\n", optarg);
				return 1;
			}
			break;

		case OPT_FOREGROUND:
			opt_foreground = 1;
			break;

		case OPT_NOFORK:
			opt_nofork = 1;
			break;

		case OPT_NORECOVER:
			opt_recover_leases = 0;
			break;

		case OPT_DBUS:
			opt_dbus = 1;
			break;
		}
	}

	if (ni_init() < 0)
		return 1;

	ni_addrconf_register(&ni_dhcp_addrconf);
	ni_addrconf_register(&ni_autoip_addrconf);

	if (optind != argc)
		goto usage;

	if (opt_dbus == 0) {
		if ((sock = ni_server_listen()) == NULL)
			ni_fatal("unable to initialize server socket");
		ni_socket_set_accept_callback(sock, wicked_accept_connection);
	} else {
		wicked_dbus_server = ni_server_listen_dbus(WICKED_DBUS_BUS_NAME);
		if (wicked_dbus_server == NULL)
			ni_fatal("unable to initialize dbus service");

		wicked_register_dbus_services(wicked_dbus_server);
	}

	/* open global RTNL socket to listen for kernel events */
	if (ni_server_listen_events(wicked_interface_event) < 0)
		ni_fatal("unable to initialize netlink listener");

	if (!opt_foreground) {
		if (ni_server_background() < 0)
			return 1;
		ni_log_destination_syslog("wickedd");
	}

	wicked_discover_state();

	while (1) {
		long timeout;

		timeout = ni_timer_next_timeout();
		if (ni_socket_wait(timeout) < 0)
			ni_fatal("ni_socket_wait failed");
	}

	exit(0);
}

/*
 * At startup, discover current configuration.
 * If we have any live leases, restart address configuration for them.
 * This allows a daemon restart without losing lease state.
 */
void
wicked_discover_state(void)
{
	ni_handle_t *nih;
	ni_interface_t *ifp;

	nih = ni_global_state_handle();
	if (nih == NULL)
		ni_fatal("Unable to get global state handle");
	if (ni_refresh(nih) < 0)
		ni_fatal("failed to discover interface state");

	if (opt_recover_leases) {
		for (ifp = ni_interfaces(nih); ifp; ifp = ifp->next) {
			xml_node_t *cfg_xml = NULL;
			unsigned int mode;

			for (mode = 0; mode < __NI_ADDRCONF_MAX; ++mode) {
				wicked_try_restart_addrconf(ifp, &ifp->ipv4, mode, &cfg_xml);
				wicked_try_restart_addrconf(ifp, &ifp->ipv6, mode, &cfg_xml);
			}

			if (cfg_xml)
				xml_node_free(cfg_xml);
		}
	}

	if (wicked_dbus_server) {

		for (ifp = ni_interfaces(nih); ifp; ifp = ifp->next)
			wicked_dbus_register_interface(ifp);
	}
}

void
wicked_try_restart_addrconf(ni_interface_t *ifp, ni_afinfo_t *afi, unsigned int mode, xml_node_t **cfg_xml)
{
	ni_addrconf_lease_t *lease;
	ni_addrconf_t *acm;

	if (!ni_afinfo_addrconf_test(afi, mode))
		return;

	/* Don't do anything if we already have a lease for this. */
	if (afi->lease[mode] != NULL)
		return;

	/* Some addrconf modes do not have a backend (like ipv6 autoconf) */
	acm = ni_addrconf_get(mode, afi->family);
	if (acm == NULL)
		return;

	lease = ni_addrconf_lease_file_read(ifp->name, mode, afi->family);
	if (lease == NULL)
		return;

	/* if lease expired, return and remove stale lease file */
	if (!ni_addrconf_lease_is_valid(lease)) {
		ni_debug_wicked("%s: removing stale %s/%s lease file", ifp->name,
				ni_addrconf_type_to_name(lease->type),
				ni_addrfamily_type_to_name(lease->family));
		ni_addrconf_lease_file_remove(ifp->name, mode, afi->family);
		ni_addrconf_lease_free(lease);
		return;
	}

	/* Do not install the lease; let the addrconf mechanism fill in all
	 * the details. */
	ni_addrconf_lease_free(lease);

	/* Recover the original addrconf request data here */
	afi->request[mode] = ni_addrconf_request_file_read(ifp->name, mode, afi->family);
	if (afi->request[mode] == NULL) {
		ni_error("%s: seem to have valid lease, but lost original request", ifp->name);
		return;
	}
	afi->request[mode]->reuse_unexpired = 1;

	if (*cfg_xml == NULL)
		*cfg_xml = ni_syntax_xml_from_interface(ni_default_xml_syntax(),
				ni_global_state_handle(), ifp);

	if (ni_addrconf_acquire_lease(acm, ifp, *cfg_xml) < 0) {
		ni_error("%s: unable to reacquire lease %s/%s", ifp->name,
				ni_addrconf_type_to_name(lease->type),
				ni_addrfamily_type_to_name(lease->family));
		return;
	}

	ni_debug_wicked("%s: initiated recovery of %s/%s lease", ifp->name,
				ni_addrconf_type_to_name(lease->type),
				ni_addrfamily_type_to_name(lease->family));
}

/*
 * Accept an incoming connection.
 * Return value of -1 means close the socket.
 */
static int
wicked_accept_connection(ni_socket_t *sock, uid_t uid, gid_t gid)
{
	if (uid != 0) {
		ni_error("refusing attempted connection by user %u", uid);
		return -1;
	}

	ni_debug_wicked("accepted connection from uid=%u", uid);
	ni_socket_set_request_callback(sock, wicked_process_network_restcall);
	return 0;
}

int
wicked_process_network_restcall(ni_socket_t *sock)
{
	ni_wicked_request_t req;
	int rv;

	/* FIXME: we may want to fork to handle this call. */

	/* Read the request coming in from the socket. */
	ni_wicked_request_init(&req);
	rv = ni_wicked_request_parse(sock, &req);

	/* Process the call */
	if (rv >= 0)
		rv = ni_wicked_call_direct(&req);

	/* ... and send the response back. */
	ni_wicked_response_print(sock, &req, rv);

	ni_wicked_request_destroy(&req);

	return 0;
}

/*
 * Functions to support the DBus binding
 */
static int __wicked_root_dbus_call(ni_dbus_object_t *object, const char *method,
		ni_dbus_message_t *call, ni_dbus_message_t *reply,
		DBusError *error);

void
wicked_register_dbus_services(ni_dbus_server_t *server)
{
	ni_dbus_object_t *root_object = ni_dbus_server_get_root_object(server);

	ni_dbus_object_register_service(root_object, WICKED_DBUS_INTERFACE,
			__wicked_root_dbus_call,
			NULL);
}

static int
__wicked_root_dbus_call(ni_dbus_object_t *object, const char *method,
		ni_dbus_message_t *call, ni_dbus_message_t *reply,
		DBusError *error)
{
	return 0;
}

static int
__wicked_dbus_interface_handler(ni_dbus_object_t *object, const char *method,
				ni_dbus_message_t *call,
				ni_dbus_message_t *reply,
				DBusError *error)
{
	return 0;
}

static int
__wicked_dbus_interface_get_type(const ni_dbus_object_t *object,
				const ni_dbus_property_t *property,
				ni_dbus_variant_t *result,
				DBusError *error)
{
	ni_interface_t *ifp = ni_dbus_object_get_handle(object);

	ni_dbus_variant_set_uint32(result, ifp->type);
	return TRUE;
}

static int
__wicked_dbus_interface_get_status(const ni_dbus_object_t *object,
				const ni_dbus_property_t *property,
				ni_dbus_variant_t *result,
				DBusError *error)
{
	ni_interface_t *ifp = ni_dbus_object_get_handle(object);

	ni_dbus_variant_set_uint32(result, ifp->ifflags);
	return TRUE;
}

static int
__wicked_dbus_interface_get_mtu(const ni_dbus_object_t *object,
				const ni_dbus_property_t *property,
				ni_dbus_variant_t *result,
				DBusError *error)
{
	ni_interface_t *ifp = ni_dbus_object_get_handle(object);

	ni_dbus_variant_set_uint32(result, ifp->mtu);
	return TRUE;
}

static int
__wicked_dbus_interface_get_hwaddr(const ni_dbus_object_t *object,
				const ni_dbus_property_t *property,
				ni_dbus_variant_t *result,
				DBusError *error)
{
	ni_interface_t *ifp = ni_dbus_object_get_handle(object);

	ni_dbus_variant_set_byte_array(result, ifp->hwaddr.len, ifp->hwaddr.data);
	return TRUE;
}

#define NI_DBUS_PROPERTY_METHODS_RO(fstem, __name) \
	.get = fstem ## _get_ ## __name, .set = NULL
#define NI_DBUS_PROPERTY_METHODS_RW(fstem, __name) \
	.get = fstem ## _get_ ## __name, .set = fstem ## _set_ ## __name
#define __NI_DBUS_PROPERTY(__signature, __name, fstem, rw) \
{ .name = #__name, .id = 0, .signature = __signature, NI_DBUS_PROPERTY_METHODS_##rw(fstem, __name) }
#define NI_DBUS_PROPERTY(type, __name, fstem, rw) \
	__NI_DBUS_PROPERTY(DBUS_TYPE_##type##_AS_STRING, __name, fstem, rw)
#define WICKED_INTERFACE_PROPERTY(type, __name, rw) \
	NI_DBUS_PROPERTY(type, __name, __wicked_dbus_interface, rw)
#define WICKED_INTERFACE_PROPERTY_SIGNATURE(signature, __name, rw) \
	__NI_DBUS_PROPERTY(signature, __name, __wicked_dbus_interface, rw)

static ni_dbus_property_t	wicked_dbus_interface_properties[] = {
	WICKED_INTERFACE_PROPERTY(UINT32, status, RO),
	WICKED_INTERFACE_PROPERTY(UINT32, type, RO),
	WICKED_INTERFACE_PROPERTY(UINT32, mtu, RO),
	WICKED_INTERFACE_PROPERTY_SIGNATURE(
			DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING,
			hwaddr, RO),
	{ NULL }
};

void
wicked_dbus_register_interface(ni_interface_t *ifp)
{
	ni_dbus_object_t *object;
	char object_path[256];

	snprintf(object_path, sizeof(object_path), "Interface/%s", ifp->name);
	object = ni_dbus_server_register_object(wicked_dbus_server, object_path, ifp);
	if (object == NULL)
		ni_fatal("Unable to create dbus object for interface %s", ifp->name);

	ni_dbus_object_register_service(object, WICKED_DBUS_INTERFACE ".Interface",
			__wicked_dbus_interface_handler,
			wicked_dbus_interface_properties);
}

/*
 * Handle network layer events.
 * FIXME: There should be some locking here, which prevents us from
 * calling event handlers on an interface that the admin is currently
 * mucking with manually.
 */
void
wicked_interface_event(ni_handle_t *nih, ni_interface_t *ifp, ni_event_t event)
{
	static const char *evtype[__NI_EVENT_MAX] =  {
		[NI_EVENT_LINK_CREATE]	= "link-create",
		[NI_EVENT_LINK_DELETE]	= "link-delete",
		[NI_EVENT_LINK_UP]	= "link-up",
		[NI_EVENT_LINK_DOWN]	= "link-down",
		[NI_EVENT_NETWORK_UP]	= "network-up",
		[NI_EVENT_NETWORK_DOWN]	= "network-down",
	};
	ni_policy_t *policy;

	if (wicked_dbus_server) {
		switch (event) {
		case NI_EVENT_LINK_CREATE:
			/* Create dbus object and emit event */
			break;

		case NI_EVENT_LINK_DELETE:
			/* Delete dbus object and emit event */
			break;

		default: ;
		}
	}

	if (event >= __NI_EVENT_MAX || !evtype[event])
		return;

	ni_debug_events("%s: %s event", ifp->name, evtype[event]);
	policy = ni_policy_match_event(nih, event, ifp);
	if (policy != NULL) {
		ni_debug_events("matched interface policy; configuring device");
		ni_interface_configure2(nih, ifp, policy->interface);
	}
}
