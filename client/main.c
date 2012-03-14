/*
 * No REST for the wicked!
 *
 * This command line utility provides an interface to the network
 * configuration/information facilities.
 *
 * Copyright (C) 2010-2012 Olaf Kirch <okir@suse.de>
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <mcheck.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <netdb.h>

#include <wicked/netinfo.h>
#include <wicked/logging.h>
#include <wicked/wicked.h>
#include <wicked/addrconf.h>
#include <wicked/route.h>
#include <wicked/bonding.h>
#include <wicked/bridge.h>
#include <wicked/xml.h>
#include <wicked/xpath.h>
#include <wicked/objectmodel.h>
#include <wicked/dbus-errors.h>

#include "client/wicked-client.h"

enum {
	OPT_CONFIGFILE,
	OPT_DEBUG,
	OPT_DRYRUN,
	OPT_ROOTDIR,
	OPT_LINK_TIMEOUT,
	OPT_NOPROGMETER,
};

static struct option	options[] = {
	{ "config",		required_argument,	NULL,	OPT_CONFIGFILE },
	{ "dryrun",		no_argument,		NULL,	OPT_DRYRUN },
	{ "dry-run",		no_argument,		NULL,	OPT_DRYRUN },
	{ "no-progress-meter",	no_argument,		NULL,	OPT_NOPROGMETER },
	{ "debug",		required_argument,	NULL,	OPT_DEBUG },
	{ "root-directory",	required_argument,	NULL,	OPT_ROOTDIR },

	{ NULL }
};

int		opt_global_dryrun = 0;
char *		opt_global_rootdir = NULL;
int		opt_global_progressmeter = 1;

static int		do_show(int, char **);
static int		do_show_xml(int, char **);
extern int		do_ifup(int, char **);
extern int		do_ifdown(int, char **);
extern int		do_lease(int, char **);
extern int		do_check(int, char **);
static int		do_xpath(int, char **);

int
main(int argc, char **argv)
{
	char *cmd;
	int c;

	mtrace();
	while ((c = getopt_long(argc, argv, "+", options, NULL)) != EOF) {
		switch (c) {
		default:
		usage:
			fprintf(stderr,
				"./wicked [options] cmd path\n"
				"This command understands the following options\n"
				"  --config filename\n"
				"        Use alternative configuration file.\n"
				"  --dry-run\n"
				"        Do not change the system in any way.\n"
				"  --debug facility\n"
				"        Enable debugging for debug <facility>.\n"
				"\n"
				"Supported commands:\n"
				"  ifup [--boot] [--file xmlspec] ifname\n"
				"  ifdown [--delete] ifname\n"
				"  show-xml [ifname]\n"
				"  delete ifname\n"
				"  xpath [options] expr ...\n"
			       );
			return 1;

		case OPT_CONFIGFILE:
			ni_set_global_config_path(optarg);
			break;

		case OPT_DRYRUN:
			opt_global_dryrun = 1;
			break;

		case OPT_ROOTDIR:
			opt_global_rootdir = optarg;
			break;

		case OPT_NOPROGMETER:
			opt_global_progressmeter = 0;
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

		}
	}

	if (!isatty(1))
		opt_global_progressmeter = 0;

	if (ni_init() < 0)
		return 1;

	if (optind >= argc) {
		fprintf(stderr, "Missing command\n");
		goto usage;
	}

	cmd = argv[optind];

	if (!strcmp(cmd, "show"))
		return do_show(argc - optind, argv + optind);

	if (!strcmp(cmd, "show-xml"))
		return do_show_xml(argc - optind, argv + optind);

	if (!strcmp(cmd, "ifup"))
		return do_ifup(argc - optind, argv + optind);

	if (!strcmp(cmd, "ifdown"))
		return do_ifdown(argc - optind, argv + optind);

	/* Old wicked style functions follow */
	if (!strcmp(cmd, "xpath"))
		return do_xpath(argc - optind, argv + optind);

	if (!strcmp(cmd, "lease"))
		return do_lease(argc - optind, argv + optind);

	if (!strcmp(cmd, "check"))
		return do_check(argc - optind, argv + optind);

	fprintf(stderr, "Unsupported command %s\n", cmd);
	goto usage;
}

/*
 * Obtain an object handle for Wicked.Interface
 */
ni_dbus_object_t *
wicked_get_interface_object(const char *default_interface)
{
	static const ni_dbus_class_t *netif_list_class = NULL;
	ni_dbus_object_t *root_object, *child;

	if (!(root_object = ni_call_create_client()))
		return NULL;

	if (netif_list_class == NULL) {
		const ni_dbus_service_t *netif_list_service;

		netif_list_service = ni_objectmodel_service_by_name(NI_OBJECTMODEL_NETIFLIST_INTERFACE);
		ni_assert(netif_list_service);

		netif_list_class = netif_list_service->compatible;
	}

	child = ni_dbus_object_create(root_object, "Interface",
			netif_list_class,
			NULL);

	if (!default_interface)
		default_interface = NI_OBJECTMODEL_NETIFLIST_INTERFACE;
	ni_dbus_object_set_default_interface(child, default_interface);

	return child;
}

/*
 * Look up the dbus object for an interface by name.
 * The name can be either a kernel interface device name such as eth0,
 * or a dbus object path such as /com/suse/Wicked/Interfaces/5
 */
static ni_dbus_object_t *
wicked_get_interface(const char *ifname)
{
	static ni_dbus_object_t *interfaces = NULL;
	ni_dbus_object_t *object;

	if (interfaces == NULL) {
		if (!(interfaces = wicked_get_interface_object(NULL)))
			return NULL;

		/* Call ObjectManager.GetManagedObjects to get list of objects and their properties */
		if (!ni_dbus_object_refresh_children(interfaces)) {
			ni_error("Couldn't get list of active network interfaces");
			return NULL;
		}
	}

	if (ifname == NULL)
		return interfaces;

	/* Loop over all interfaces and find the one with matching name */
	for (object = interfaces->children; object; object = object->next) {
		if (ifname[0] == '/') {
			if (ni_string_eq(object->path, ifname))
				return object;
		} else {
			ni_netdev_t *ifp = ni_objectmodel_unwrap_interface(object, NULL);

			if (ifp && ifp->name && !strcmp(ifp->name, ifname))
				return object;
		}
	}

	ni_error("%s: unknown network interface", ifname);
	return NULL;
}

/* Hack */
struct ni_dbus_dict_entry {
	/* key of the dict entry */
	const char *            key;

	/* datum associated with key */
	ni_dbus_variant_t       datum;
};

static void
__dump_fake_xml(const ni_dbus_variant_t *variant, unsigned int indent, const char **dict_elements)
{
	ni_dbus_dict_entry_t *entry;
	unsigned int index;

	if (ni_dbus_variant_is_dict(variant)) {
		const char *dict_element_tag = NULL;

		if (dict_elements && dict_elements[0])
			dict_element_tag = *dict_elements++;
		for (entry = variant->dict_array_value, index = 0; index < variant->array.len; ++index, ++entry) {
			const ni_dbus_variant_t *child = &entry->datum;
			const char *open_tag, *close_tag;
			char namebuf[256];

			if (dict_element_tag) {
				snprintf(namebuf, sizeof(namebuf), "%s name=\"%s\"", dict_element_tag, entry->key);
				open_tag = namebuf;
				close_tag = dict_element_tag;
			} else {
				open_tag = close_tag = entry->key;
			}

			if (child->type != DBUS_TYPE_ARRAY) {
				/* Must be some type of scalar */
				printf("%*.*s<%s>%s</%s>\n",
						indent, indent, "",
						open_tag,
						ni_dbus_variant_sprint(child),
						close_tag);
			} else if(child->array.len == 0) {
				printf("%*.*s<%s />\n", indent, indent, "", open_tag);
			} else if (ni_dbus_variant_is_byte_array(child)) {
				unsigned char value[64];
				unsigned int num_bytes;
				char display_buffer[128];
				const char *display;

				if (!ni_dbus_variant_get_byte_array_minmax(child, value, &num_bytes, 0, sizeof(value))) {
					display = "<INVALID />";
				} else {
					display = ni_format_hex(value, num_bytes, display_buffer, sizeof(display_buffer));
				}
				printf("%*.*s<%s>%s</%s>\n",
						indent, indent, "",
						open_tag,
						display,
						close_tag);
			} else {
				printf("%*.*s<%s>\n", indent, indent, "", open_tag);
				__dump_fake_xml(child, indent + 2, dict_elements);
				printf("%*.*s</%s>\n", indent, indent, "", close_tag);
			}
		}
	} else if (ni_dbus_variant_is_dict_array(variant)) {
		const ni_dbus_variant_t *child;

		for (child = variant->variant_array_value, index = 0; index < variant->array.len; ++index, ++child) {
			printf("%*.*s<e>\n", indent, indent, "");
			__dump_fake_xml(child, indent + 2, NULL);
			printf("%*.*s</e>\n", indent, indent, "");
		}
	} else {
		ni_trace("%s: %s", __func__, ni_dbus_variant_signature(variant));
	}
}

static xml_node_t *
__dump_object_xml(const char *object_path, const ni_dbus_variant_t *variant, ni_xs_scope_t *schema, xml_node_t *parent)
{
	xml_node_t *object_node;
	ni_dbus_dict_entry_t *entry;
	unsigned int index;

	if (!ni_dbus_variant_is_dict(variant)) {
		ni_error("%s: dbus data is not a dict", __func__);
		return NULL;
	}

	object_node = xml_node_new("object", parent);
	xml_node_add_attr(object_node, "path", object_path);

	for (entry = variant->dict_array_value, index = 0; index < variant->array.len; ++index, ++entry) {
		const char *interface_name = entry->key;

		/* Ignore well-known interfaces that never have properties */
		if (!strcmp(interface_name, "org.freedesktop.DBus.ObjectManager")
		 || !strcmp(interface_name, "org.freedesktop.DBus.Properties"))
			continue;

		ni_dbus_xml_deserialize_properties(schema, interface_name, &entry->datum, object_node);
	}

	return object_node;
}

static xml_node_t *
__dump_schema_xml(const ni_dbus_variant_t *variant, ni_xs_scope_t *schema)
{
	xml_node_t *root = xml_node_new(NULL, NULL);
	ni_dbus_dict_entry_t *entry;
	unsigned int index;

	if (!ni_dbus_variant_is_dict(variant)) {
		ni_error("%s: dbus data is not a dict", __func__);
		return NULL;
	}

	for (entry = variant->dict_array_value, index = 0; index < variant->array.len; ++index, ++entry) {
		if (!__dump_object_xml(entry->key, &entry->datum, schema, root))
			return NULL;
	}

	return root;
}


int
do_show_xml(int argc, char **argv)
{
	enum  { OPT_RAW, };
	static struct option local_options[] = {
		{ "raw", no_argument, NULL, OPT_RAW },
		{ NULL }
	};
	ni_dbus_object_t *iflist, *object;
	ni_dbus_variant_t result = NI_DBUS_VARIANT_INIT;
	DBusError error = DBUS_ERROR_INIT;
	const char *ifname = NULL;
	int opt_raw = 0;
	int c, rv = 1;

	optind = 1;
	while ((c = getopt_long(argc, argv, "", local_options, NULL)) != EOF) {
		switch (c) {
		case OPT_RAW:
			opt_raw = 1;
			break;

		default:
		usage:
			fprintf(stderr,
				"wicked [options] show-xml [ifname]\n"
				"\nSupported options:\n"
				"  --raw\n"
				"      Show raw dbus reply in pseudo-xml, rather than using the schema\n"
				);
			return 1;
		}
	}

	if (optind < argc)
		ifname = argv[optind++];
	(void)ifname; /* FIXME; not used yet */

	if (optind != argc)
		goto usage;

	if (!(object = ni_call_create_client()))
		return 1;

	if (!(iflist = wicked_get_interface_object(NULL)))
		goto out;

	if (!ni_dbus_object_call_variant(iflist,
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
			0, NULL,
			1, &result, &error)) {
		ni_error("GetManagedObject call failed");
		goto out;
	}

	if (opt_raw) {
		static const char *dict_element_tags[] = {
			"object", "interface", NULL
		};

		__dump_fake_xml(&result, 0, dict_element_tags);
	} else {
		ni_xs_scope_t *schema = ni_objectmodel_init(NULL);
		xml_node_t *tree;

		tree = __dump_schema_xml(&result, schema);
		if (tree == NULL) {
			ni_error("unable to represent properties as xml");
			goto out;
		}

		xml_node_print(tree, NULL);
		xml_node_free(tree);
	}

	rv = 0;

out:
	ni_dbus_variant_destroy(&result);
	return rv;
}

int
do_show(int argc, char **argv)
{
	ni_dbus_object_t *object;

	if (argc != 1 && argc != 2) {
		ni_error("wicked show: missing interface name");
		return 1;
	}

	if (argc == 1) {
		object = wicked_get_interface(NULL);
		if (!object)
			return 1;

		for (object = object->children; object; object = object->next) {
			ni_netdev_t *ifp = object->handle;
			ni_address_t *ap;
			ni_route_t *rp;

			printf("%-12s %-10s %-10s",
					ifp->name,
					(ifp->link.ifflags & NI_IFF_NETWORK_UP)? "up" :
					 (ifp->link.ifflags & NI_IFF_LINK_UP)? "link-up" :
					  (ifp->link.ifflags & NI_IFF_DEVICE_UP)? "device-up" : "down",
					ni_linktype_type_to_name(ifp->link.type));
			printf("\n");

			for (ap = ifp->addrs; ap; ap = ap->next)
				printf("  addr:   %s/%u\n", ni_address_print(&ap->local_addr), ap->prefixlen);

			for (rp = ifp->routes; rp; rp = rp->next) {
				const ni_route_nexthop_t *nh;

				printf("  route: ");

				if (rp->prefixlen)
					printf(" %s/%u", ni_address_print(&rp->destination), rp->prefixlen);
				else
					printf(" default");

				if (rp->nh.gateway.ss_family != AF_UNSPEC) {
					for (nh = &rp->nh; nh; nh = nh->next)
						printf("; via %s", ni_address_print(&nh->gateway));
				}

				printf("\n");
			}
		}
	} else {
		const char *ifname = argv[1];

		object = wicked_get_interface(ifname);
		if (!object)
			return 1;
	}

	return 0;
}

/*
 * xpath
 * This is a utility that can be used by network scripts to extracts bits and
 * pieces of information from an XML file.
 * This is still a bit inconvenient, especially if you need to extract more than
 * one of two elements, since we have to parse and reparse the XML file every
 * time you invoke this program.
 * On the other hand, there's a few rather nifty things you can do. For instance,
 * the following will extract addres/prefixlen pairs for every IPv4 address
 * listed in an XML network config:
 *
 * wicked xpath \
 *	--reference "interface/protocol[@family = 'ipv4']/ip" \
 *	--file vlan.xml \
 *	'%{@address}/%{@prefix}'
 *
 * The "reference" argument tells wicked to look up the <protocol>
 * element with a "family" attribute of "ipv4", and within that,
 * any <ip> elements. For each of these, it obtains the address
 * and prefix attribute, and prints it separated by a slash.
 */
int
do_xpath(int argc, char **argv)
{
	static struct option xpath_options[] = {
		{ "reference", required_argument, NULL, 'r' },
		{ "file", required_argument, NULL, 'f' },
		{ NULL }
	};
	const char *opt_reference = NULL, *opt_file = "-";
	xpath_result_t *input;
	xml_document_t *doc;
	int c;

	optind = 1;
	while ((c = getopt_long(argc, argv, "", xpath_options, NULL)) != EOF) {
		switch (c) {
		case 'r':
			opt_reference = optarg;
			break;

		case 'f':
			opt_file = optarg;
			break;

		default:
			fprintf(stderr,
				"wicked [options] xpath [--reference <expr>] [--file <path>] expr ...\n");
			return 1;
		}
	}

	doc = xml_document_read(opt_file);
	if (!doc) {
		fprintf(stderr, "Error parsing XML document %s\n", opt_file);
		return 1;
	}

	if (opt_reference) {
		xpath_enode_t *enode;

		enode = xpath_expression_parse(opt_reference);
		if (!enode) {
			fprintf(stderr, "Error parsing XPATH expression %s\n", opt_reference);
			return 1;
		}

		input = xpath_expression_eval(enode, doc->root);
		if (!input) {
			fprintf(stderr, "Error evaluating XPATH expression\n");
			return 1;
		}

		if (input->type != XPATH_ELEMENT) {
			fprintf(stderr, "Failed to look up reference node - returned non-element result\n");
			return 1;
		}
		if (input->count == 0) {
			fprintf(stderr, "Failed to look up reference node - returned empty list\n");
			return 1;
		}
		xpath_expression_free(enode);
	} else {
		input = xpath_result_new(XPATH_ELEMENT);
		xpath_result_append_element(input, doc->root);
	}

	while (optind < argc) {
		const char *expression = argv[optind++];
		ni_string_array_t result;
		xpath_format_t *format;
		unsigned int n;

		format = xpath_format_parse(expression);
		if (!format) {
			fprintf(stderr, "Error parsing XPATH format string %s\n", expression);
			return 1;
		}

		ni_string_array_init(&result);
		for (n = 0; n < input->count; ++n) {
			xml_node_t *refnode = input->node[n].value.node;

			if (!xpath_format_eval(format, refnode, &result)) {
				fprintf(stderr, "Error evaluating XPATH expression\n");
				ni_string_array_destroy(&result);
				return 1;
			}
		}

		for (n = 0; n < result.count; ++n)
			printf("%s\n", result.data[n]);

		ni_string_array_destroy(&result);
		xpath_format_free(format);
	}

	return 0;
}

/*
 * Script extensions may trigger some action that take time to complete,
 * and we may wish to notify the caller asynchronously.
 */
int
do_lease(int argc, char **argv)
{
	const char *opt_file, *opt_cmd;
	xml_document_t *doc;
	int c;

	if (argc <= 2)
		goto usage;
	opt_file = argv[1];
	opt_cmd = argv[2];

	optind = 3;
	if (!strcmp(opt_cmd, "add") || !strcmp(opt_cmd, "set")) {
		static struct option add_options[] = {
			{ "address", required_argument, NULL, 'a' },
			{ "route", required_argument, NULL, 'r' },
			{ "netmask", required_argument, NULL, 'm' },
			{ "gateway", required_argument, NULL, 'g' },
			{ "peer", required_argument, NULL, 'p' },
			{ "state", required_argument, NULL, 's' },
			{ NULL }
		};
		char *opt_address = NULL;
		char *opt_route = NULL;
		char *opt_netmask = NULL;
		char *opt_gateway = NULL;
		char *opt_peer = NULL;
		char *opt_state = NULL;
		xml_node_t *node;
		int prefixlen = -1;

		while ((c = getopt_long(argc, argv, "", add_options, NULL)) != EOF) {
			switch (c) {
			case 'a':
				if (opt_address || opt_route)
					goto add_conflict;
				opt_address = optarg;
				break;

			case 'r':
				if (opt_address || opt_route)
					goto add_conflict;
				opt_route = optarg;
				break;

			case 'm':
				opt_netmask = optarg;
				break;

			case 'g':
				opt_gateway = optarg;
				break;

			case 'p':
				opt_peer = optarg;
				break;

			case 's':
				opt_state = optarg;
				break;

			default:
				goto usage;
			}
		}

		if (!opt_address && !opt_route && !opt_state) {
add_conflict:
			ni_error("wicked lease add: need at least one --route, --address or --state option");
			goto usage;
		}

		if (!ni_file_exists(opt_file))
			doc = xml_document_new();
		else {
			doc = xml_document_read(opt_file);
			if (!doc) {
				ni_error("unable to parse XML document %s", opt_file);
				return 1;
			}
		}

		if (opt_netmask) {
			ni_sockaddr_t addr;

			if (ni_address_parse(&addr, opt_netmask, AF_UNSPEC) < 0) {
				ni_error("cannot parse netmask \"%s\"", opt_netmask);
				return 1;
			}
			prefixlen = ni_netmask_bits(&addr);
		}

		node = doc->root;
		if (opt_state) {
			xml_node_t *e;

			if (!(e = xml_node_get_child(node, "state")))
				e = xml_node_new("state", node);
			xml_node_set_cdata(e, opt_state);
		}

		if (opt_address) {
			char *slash, addrbuf[128];
			xml_node_t *list, *e;

			slash = strchr(opt_address, '/');
			if (prefixlen >= 0) {
				if (slash)
					*slash = '\0';
				snprintf(addrbuf, sizeof(addrbuf), "%s/%d", opt_address, prefixlen);
				opt_address = addrbuf;
			}

			if (!(list = xml_node_get_child(node, "addresses")))
				list = xml_node_new("addresses", node);

			e = xml_node_new("e", list);
			xml_node_set_cdata(xml_node_new("local", e), opt_address);
			if (opt_peer)
				xml_node_set_cdata(xml_node_new("peer", e), opt_peer);

			if (opt_gateway)
				ni_warn("ignoring --gateway option");
		}

		if (opt_route) {
			char *slash, addrbuf[128];
			xml_node_t *list, *e;

			slash = strchr(opt_route, '/');
			if (prefixlen >= 0) {
				if (slash)
					*slash = '\0';
				snprintf(addrbuf, sizeof(addrbuf), "%s/%d", opt_route, prefixlen);
				opt_route = addrbuf;
			}

			if (!(list = xml_node_get_child(node, "routes")))
				list = xml_node_new("routes", node);

			e = xml_node_new("e", list);
			xml_node_set_cdata(xml_node_new("destination", e), opt_route);
			if (opt_gateway) {
				e = xml_node_new("nexthop", e);
				xml_node_set_cdata(xml_node_new("gateway", e), opt_gateway);
			}

			if (opt_peer)
				ni_warn("ignoring --peer option");
		}

		xml_document_write(doc, opt_file);
	} else if (!strcmp(opt_cmd, "install")) {
		static struct option install_options[] = {
			{ "device", required_argument, NULL, 'd' },
			{ NULL }
		};
		char *opt_device = NULL;
		ni_dbus_object_t *obj;

		while ((c = getopt_long(argc, argv, "", install_options, NULL)) != EOF) {
			switch (c) {
			case 'd':
				opt_device = optarg;
				break;

			default:
				goto usage;
			}
		}

		if (opt_device == NULL) {
			ni_error("missing --device argument");
			goto usage;
		}

		doc = xml_document_read(opt_file);
		if (!doc) {
			ni_error("unable to parse XML document %s", opt_file);
			return 1;
		}
		if (doc->root == NULL) {
			ni_error("empty lease file");
			goto failed;
		}

		obj = wicked_get_interface(opt_device);
		if (obj == NULL) {
			ni_error("no such device or object: %s", opt_device);
			goto failed;
		}

		if (ni_call_install_lease_xml(obj, doc->root) < 0) {
			ni_error("unable to install addrconf lease");
			goto failed;
		}
	} else {
		ni_error("unsupported command wicked %s %s", argv[0], opt_cmd);
usage:
		fprintf(stderr,
			"Usage: wicked lease <filename> cmd ...\n"
			"Where cmd is one of the following:\n"
			"  add --address <ipaddr> --netmask <ipmask> [--peer <ipaddr>]\n"
			"  add --address <ipaddr>/<prefixlen> [--peer <ipaddr>\n"
			"  add --route <network> --netmask <ipmask> [--gateway <ipaddr>]\n"
			"  add --route <network>/<prefixlen> [--gateway <ipaddr>]\n"
			"  install --device <object-path>\n"
		       );
		return 1;
	}

	if (doc)
		xml_document_free(doc);
	return 0;

failed:
	if (doc)
		xml_document_free(doc);
	return 1;
}

/*
 * Build a getaddrinfo_a request
 */
struct gaicb *
gaicb_new(const char *hostname, int af)
{
	struct addrinfo *hints;
	struct gaicb *cb;

	/* Set up the resolver hints. Note that we explicitly should not
	 * set AI_ADDRCONFIG, as that tests whether one of the interfaces
	 * has an IPv6 address set. Since we may be in the middle of setting
	 * up our networking, we cannot rely on that to always be accurate. */
	hints = calloc(1, sizeof(*hints));
	hints->ai_family = af;

	cb = calloc(1, sizeof(*cb));
	cb->ar_name = hostname;
	cb->ar_request = hints;

	return cb;
}

static void
gaicb_free(struct gaicb *cb)
{
	if (gai_cancel(cb) == EAI_NOTCANCELED) {
		ni_warn("could not cancel getaddrinfo request for %s, leaking memory",
				cb->ar_name);
		return;
	}

	if (cb->ar_request)
		free((struct addrinfo *) cb->ar_request);
	if (cb->ar_result)
		freeaddrinfo(cb->ar_result);
	free(cb);
}

static void
gaicb_list_free(struct gaicb **list, unsigned int nitems)
{
	unsigned int i;

	for (i = 0; i < nitems; ++i)
		gaicb_free(list[i]);
	free(list);
}

/*
 * Use getaddrinfo_a to resolve one or more hostnames
 */
int
gaicb_list_resolve(struct gaicb **greqs, unsigned int nreqs, unsigned int timeout)
{
	unsigned int i;
	int rv;

	if (timeout == 0) {
		rv = getaddrinfo_a(GAI_WAIT, greqs, nreqs, NULL);
		if (rv != 0) {
			ni_error("getaddrinfo_a: %s", gai_strerror(rv));
			return -1;
		}
	} else {
		struct timeval deadline, now;

		rv = getaddrinfo_a(GAI_NOWAIT, greqs, nreqs, NULL);
		if (rv != 0) {
			ni_error("getaddrinfo_a: %s", gai_strerror(rv));
			return -1;
		}

		gettimeofday(&deadline, NULL);
		deadline.tv_sec += timeout;

		while (1) {
			struct timeval delta;
			struct timespec ts;
			int status;

			gettimeofday(&now, NULL);
			if (timercmp(&now, &deadline, >=))
				break;

			timersub(&deadline, &now, &delta);
			ts.tv_sec = delta.tv_sec;
			ts.tv_nsec = 1000 * delta.tv_usec;

			status = gai_suspend((const struct gaicb * const *) greqs, nreqs, &ts);
			if (status == EAI_ALLDONE || status == EAI_AGAIN)
				break;
		}
	}

	for (i = 0, rv = 0; i < nreqs; ++i) {
		struct gaicb *cb = greqs[i];

		switch (gai_cancel(cb)) {
		case EAI_ALLDONE:
			rv++;
			break;

		default: ;
		}
	}

	return rv;
}

static int
gaicb_get_address(struct gaicb *cb, ni_sockaddr_t *addr)
{
	int gerr;
	struct addrinfo *res;
	unsigned int alen;

	if ((gerr = gai_error(cb)) != 0)
		return gerr;

	res = cb->ar_result;
	if ((alen = res->ai_addrlen) > sizeof(*addr))
		alen = sizeof(*addr);
	memcpy(addr, res->ai_addr, alen);
	return 0;
}

int
ni_resolve_hostname_timed(const char *hostname, ni_sockaddr_t *addr, unsigned int timeout)
{
	struct gaicb *cb;
	int gerr;

	cb = gaicb_new(hostname, AF_UNSPEC);
	if (gaicb_list_resolve(&cb, 1, timeout) < 0)
		return -1;

	gerr = gaicb_get_address(cb, addr);
	gaicb_free(cb);

	if (gerr != 0) {
		ni_debug_objectmodel("cannot resolve %s: %s", hostname, gai_strerror(gerr));
		return 0;
	}

	return 1;
}

/*
 * Check for various conditions, such as resolvability and reachability.
 */
static void		write_dbus_error(const char *filename, const char *name, const char *fmt, ...);

int
do_check(int argc, char **argv)
{
	enum { OPT_TIMEOUT, OPT_AF, OPT_WRITE_DBUS_ERROR };
	static struct option options[] = {
		{ "timeout", required_argument, NULL, OPT_TIMEOUT },
		{ "af", required_argument, NULL, OPT_AF },
		{ "write-dbus-error", required_argument, NULL, OPT_WRITE_DBUS_ERROR },
		{ NULL }
	};
	const char *opt_cmd;
	const char *opt_dbus_error_file = NULL;
	unsigned int opt_timeout = 2;
	int opt_af = AF_UNSPEC;
	int c;

	if (argc < 2) {
		ni_error("wicked check: missing arguments");
		goto usage;
	}
	opt_cmd = argv[1];

	optind = 2;
	while ((c = getopt_long(argc, argv, "", options, NULL)) != EOF) {
		switch (c) {
		case OPT_TIMEOUT:
			if (ni_parse_int(optarg, &opt_timeout) < 0)
				ni_fatal("cannot parse timeout value \"%s\"", optarg);
			break;

		case OPT_AF:
			opt_af = ni_addrfamily_name_to_type(optarg);
			if (opt_af < 0)
				ni_fatal("unknown address family \"%s\"", optarg);
			break;

		case OPT_WRITE_DBUS_ERROR:
			opt_dbus_error_file = optarg;
			break;

		default:
			goto usage;
		}
	}

	if (ni_string_eq(opt_cmd, "resolve") || ni_string_eq(opt_cmd, "route")) {
		struct gaicb **greqs = NULL;
		unsigned int i, nreqs;
		int failed = 0;

		nreqs = argc - optind;
		if (nreqs == 0)
			return 0;

		greqs = calloc(nreqs, sizeof(greqs[0]));
		for (i = 0; i < nreqs; ++i)
			greqs[i] = gaicb_new(argv[optind++], opt_af);

		if (gaicb_list_resolve(greqs, nreqs, opt_timeout) < 0)
			return 1;

		for (i = 0; i < nreqs; ++i) {
			struct gaicb *cb = greqs[i];
			const char *hostname;
			ni_sockaddr_t address;
			int gerr;

			hostname = cb->ar_name;

			if ((gerr = gaicb_get_address(cb, &address)) != 0) {
				ni_error("unable to resolve %s: %s", hostname, gai_strerror(gerr));
				failed++;
				if (opt_dbus_error_file) {
					write_dbus_error(opt_dbus_error_file,
							NI_DBUS_ERROR_UNRESOLVABLE_HOSTNAME,
							hostname);
					opt_dbus_error_file = NULL;
				}
				continue;
			}

			if (ni_string_eq(opt_cmd, "resolve")) {
				printf("%s %s\n", hostname, ni_address_print(&address));
				continue;
			}

			if (ni_string_eq(opt_cmd, "route")) {
				/* The check for routability is implemented as a simple
				 * UDP connect, which should return immediately, since no
				 * packets are sent over the wire (except for hostname
				 * resolution).
				 */
				int fd;

				fd = socket(address.ss_family, SOCK_DGRAM, 0);
				if (fd < 0) {
					ni_error("%s: unable to open %s socket", hostname,
							ni_addrfamily_type_to_name(address.ss_family));
					failed++;
					continue;
				}

				if (connect(fd, (struct sockaddr *) &address, sizeof(address)) == 0) {
					printf("%s %s reachable\n", hostname, ni_address_print(&address));
				} else {
					ni_error("cannot connect to %s: %m", hostname);
					failed++;
					if (opt_dbus_error_file) {
						write_dbus_error(opt_dbus_error_file,
								NI_DBUS_ERROR_UNREACHABLE_ADDRESS,
								hostname);
						opt_dbus_error_file = NULL;
					}
				}

				close(fd);
			}
		}
		gaicb_list_free(greqs, nreqs);
	} else {
		ni_error("unsupported command wicked %s %s", argv[0], opt_cmd);
usage:
		fprintf(stderr,
			"Usage: wicked check <cmd> ...\n"
			"Where <cmd> is one of the following:\n"
			"  resolve [options ...] hostname ...\n"
			"  route [options ...] address ...\n"
			"\n"
			"Supported options:\n"
			"  --timeout n\n"
			"        Fail after n seconds.\n"
			"  --af <address-family>\n"
			"        Specify the address family (ipv4, ipv6, ...) to use when resolving hostnames.\n"
		       );
		return 1;
		;
	}

	return 0;
}

/*
 * Write a dbus error message as XML to a file
 */
void
write_dbus_error(const char *filename, const char *name, const char *fmt, ...)
{
	xml_document_t *doc;
	xml_node_t *node;
	char msgbuf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	va_end(ap);

	doc = xml_document_new();
	node = xml_node_new("error", doc->root);
	xml_node_add_attr(node, "name", name);
	xml_node_set_cdata(node, msgbuf);

	if (xml_document_write(doc, filename) < 0)
		ni_fatal("failed to write xml error document");

	xml_document_free(doc);
}
